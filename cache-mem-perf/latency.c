/*
 * latency.c - single-thread dependent-load pointer chase for Asimov.
 *
 * The buffer holds one 8-byte pointer per 64-byte line. The pointers form one
 * random cycle through all lines (Sattolo's algorithm, a Fisher-Yates shuffle
 * that swaps element i only with some j < i, which yields a single n-cycle).
 * Each load depends on the previous one, so a stride prefetcher cannot run
 * ahead, and elapsed / loads is the load-to-use latency at that footprint.
 *
 * Two timing modes:
 *   repeated (default): one cycle is walked --loads times in total per rep.
 *     Good for footprints that fit a cache: the second and later walks hit.
 *   --fresh K: K independent cycles share the buffer (chain c uses word c of
 *     each line). Chain 0 is walked twice to make the data resident, then each
 *     of chains 1..K-1 is walked exactly once and timed. A prefetcher that
 *     learned the address sequence of chain 0 gains nothing on chain 1.
 * --flush: before every timed walk, every line is cleaned and invalidated from
 *   the core caches (DC CIVAC), and the walk is exactly one pass. Every load
 *   then goes to the home node (OCM or SLC) or DRAM. This is how OCM latency is
 *   measured, since one 256 KiB region fits inside the core's L2 + L3.
 * --perf: cycles and L1D/L2D/L3D refills are counted around each timed walk
 *   and printed per load, so each latency step can be attributed to a level.
 */
#include "common.h"

#define MAX_SIZES 64

static void *volatile sink;
static uint64_t rng_state;

static uint64_t xorshift64(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* First the identity, then a Sattolo shuffle of line indices, then index ->
 * address. Word `slot` of each line carries this chain and the stored pointer
 * targets the same slot of the next line, so chase() needs no offset. */
static void build_cycle(const struct buf *b, uint64_t n, int slot, uint64_t seed)
{
#define AT(i) (*(uint64_t *)(line_addr(b, (i)) + (slot) * 8))
    rng_state = seed;
    for (uint64_t i = 0; i < n; i++)
        AT(i) = i;
    for (uint64_t i = n - 1; i > 0; i--) {
        uint64_t j = xorshift64() % i;
        uint64_t tmp = AT(i);
        AT(i) = AT(j);
        AT(j) = tmp;
    }
    for (uint64_t i = 0; i < n; i++)
        AT(i) = (uint64_t)(uintptr_t)(line_addr(b, AT(i)) + slot * 8);
#undef AT
}

__attribute__((noinline)) static void *chase(void *p, uint64_t loads)
{
    for (uint64_t i = 0; i < loads; i += 8) {
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
    }
    return p;
}

static struct {
    int cpu, reps, fresh, flush, use_perf;
    uint64_t loads;
    enum mem_kind kind;
    int regions[MAX_SEGS], nregions;
} cfg = { .cpu = -1, .reps = 3, .loads = 1 << 16, .kind = MEM_DRAM };

static struct perfgrp pg;

/* One timed walk. Prints the CSV row and returns ns per load. */
static double timed_walk(const struct buf *b, void *start, uint64_t loads, const char *label, int rep, uint64_t n)
{
    uint64_t c[PERF_N];
    if (cfg.flush)
        flush_buf(b);
    perf_start(&pg);
    double t0 = now_s();
    void *q = chase(start, loads);
    double t1 = now_s();
    perf_stop(&pg);
    sink = q;
    perf_read(&pg, c);
    double ns = (t1 - t0) * 1e9 / (double)loads;
    printf("%s,%llu,%s,%d,%llu,%.3f", mem_name(b->kind), (unsigned long long)b->bytes, label, rep,
           (unsigned long long)loads, ns);
    if (pg.ok)
        printf(",%.2f,%.4f,%.4f,%.4f", (double)c[0] / (double)loads, (double)c[1] / (double)loads,
               (double)c[2] / (double)loads, (double)c[3] / (double)loads);
    printf("\n");
    (void)n;
    return ns;
}

static void run_size(uint64_t bytes)
{
    if (bytes % (8 * LINE_BYTES) != 0) {
        fprintf(stderr, "size %llu is not a multiple of 512 bytes\n", (unsigned long long)bytes);
        exit(2);
    }
    struct buf b;
    alloc_buf(&b, cfg.kind, bytes, cfg.regions, cfg.nregions);
    uint64_t n = b.bytes / LINE_BYTES;
    int chains = cfg.fresh ? cfg.fresh : 1;
    for (int c = 0; c < chains; c++)
        build_cycle(&b, n, c, 0x9E3779B97F4A7C15ULL * (uint64_t)(c + 1) + 12345);
    printf("# size=%llu lines=%llu backing=%s segments=%d\n", (unsigned long long)b.bytes,
           (unsigned long long)n, b.backing, b.nseg);

    double best = 1e30;
    void *p = chase(b.seg[0].base, n); /* warm-up: data resident, TLB filled */
    p = chase(p, n);
    sink = p;
    if (cfg.fresh) {
        for (int c = 1; c < chains; c++) {
            char label[16];
            snprintf(label, sizeof label, "fresh%d", c);
            double ns = timed_walk(&b, b.seg[0].base + c * 8, n, label, c, n);
            if (ns < best) best = ns;
        }
    } else {
        /* --flush walks exactly one pass; otherwise --loads rounded up to whole cycles */
        uint64_t loads = cfg.flush ? n : n * ((cfg.loads + n - 1) / n);
        for (int rep = 0; rep < cfg.reps; rep++) {
            double ns = timed_walk(&b, b.seg[0].base, loads, cfg.flush ? "flushed" : "repeat", rep, n);
            if (ns < best) best = ns;
        }
    }
    printf("%s,%llu,best,-,-,%.3f\n", mem_name(b.kind), (unsigned long long)b.bytes, best);
    fflush(stdout);
    for (int s = 0; s < b.nseg; s++)
        munmap(b.seg[s].base, b.kind == MEM_OCM ? memcfg.ocm_size : b.seg[s].bytes);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: latency --sizes a,b,c [--cpu N] [--mem dram|ocm|spm] [--ocm-regions 0,1,..]\n"
            "               [--loads N] [--reps R] [--fresh K] [--flush] [--perf] [--no-hugetlb]\n"
            "               [--ocm-dev P --ocm-base A --ocm-stride S --ocm-size B] [--spm-dev P --spm-off O]\n"
            "  sizes accept K/M/G suffixes. --mem ocm splits each size evenly over the listed regions.\n"
            "  --fresh K (2..8): K chains per line, time one pass over each of chains 1..K-1.\n"
            "  --flush: DC CIVAC the buffer before each timed pass; the pass is one cycle.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    uint64_t sizes[MAX_SIZES];
    int nsizes = 0;
    for (int i = 1; i < argc;) {
        int used = parse_mem_opt(argc, argv, i, &cfg.kind, cfg.regions, &cfg.nregions);
        if (used) { i += used; continue; }
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--flush")) cfg.flush = 1;
        else if (!strcmp(a, "--perf")) cfg.use_perf = 1;
        else if (!strcmp(a, "--cpu") && more) cfg.cpu = atoi(argv[++i]);
        else if (!strcmp(a, "--loads") && more) cfg.loads = parse_bytes(argv[++i]);
        else if (!strcmp(a, "--reps") && more) cfg.reps = atoi(argv[++i]);
        else if (!strcmp(a, "--fresh") && more) { cfg.fresh = atoi(argv[++i]); if (cfg.fresh < 2 || cfg.fresh > 8) usage(); }
        else if (!strcmp(a, "--sizes") && more) {
            for (char *tok = strtok(argv[++i], ","); tok; tok = strtok(NULL, ",")) {
                if (nsizes == MAX_SIZES) usage();
                sizes[nsizes++] = parse_bytes(tok);
            }
        } else usage();
        i++;
    }
    if (nsizes == 0 || cfg.reps < 1 || cfg.loads < 8)
        usage();
    if (cfg.cpu >= 0)
        pin_cpu(cfg.cpu);
    if (cfg.use_perf)
        perf_open(&pg);

    printf("# latency: ns_per_load = elapsed_ns / loads; cpu=%d mem=%s cntfrq_hz=%llu loads=%llu reps=%d fresh=%d flush=%d\n",
           cfg.cpu, mem_name(cfg.kind), (unsigned long long)read_cntfrq(), (unsigned long long)cfg.loads,
           cfg.reps, cfg.fresh, cfg.flush);
    printf("mem,size_bytes,mode,rep,loads,ns_per_load%s\n",
           pg.ok ? ",cycles_per_load,l1d_refill_per_load,l2d_refill_per_load,l3d_refill_per_load" : "");
    for (int i = 0; i < nsizes; i++)
        run_size(sizes[i]);
    return 0;
}
