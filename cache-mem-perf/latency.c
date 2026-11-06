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
 * --sparse: each chain uses one line per 4 KiB page, at a different line
 *   offset per chain (line 8c + 4 of every page for chain c), so no two chain
 *   members share a page and no member has a member as a neighbour. It is an
 *   experimental mitigation for the reuse of prefetched adjacent lines: run 2's
 *   dense 256 KiB rows counted 1.1143 to 1.4468 L2 refills and 0.7627 to
 *   0.9500 L3 refills per load (40 passes), while the 1.25 MiB and 8 MiB rows
 *   counted 1.00 of each. With a sparse chain a line fetched beside a member
 *   is not a member, and a chain visits each page once; whether any prefetch
 *   still lands on a member is not something the counters can rule out. A
 *   sparse chain has bytes / 4096 hops, so --fresh 8 gives seven timed passes,
 *   each a complete-call time divided by its hop count.
 * --sparse-vary: as --sparse, but the member's line within the page also changes
 *   from page to page (a hash of the page number picks one of 8 slots of 8 lines
 *   for each chain, and a line within the slot; chains never share a line). Run 3's
 *   sparse rows counted fewer L3 read refills per load than run 2's dense rows
 *   (best pass of each of the 20 CPU-region cells: 0.50 to 0.86 against 0.76 to
 *   0.92). One reading (a hypothesis) is that a prefetcher lands on sparse members
 *   by carrying the one fixed line offset from page to page; this chain tests the
 *   sensitivity to that offset. It does not by itself name the prefetcher.
 * --seq: the chain visits the lines in address order instead of a random cycle
 *   (member i points to member i + 1). A candidate prefetch stimulus for the
 *   prefetch counters: a stride prefetcher can run ahead of it. Each event is
 *   validated separately, on a boot with the prefetchers enabled, when the
 *   activity it counts is established; an event that stays zero without such
 *   evidence is unvalidated, not broken. On a prefetch-disabled boot the
 *   readbacks and the previously validated counts assess the change; the
 *   latency change is reported as an outcome, with no required direction.
 * --perf: cycles and six raw events are counted around each timed walk and
 *   printed per load (common.h names the sets): the run 3 refill events plus
 *   the three hardware-prefetch refill events, or with --events tlb the run 3
 *   events plus the L1D and L2D TLB refills and the TLB walks, or with
 *   --events l3 the run 3 events plus the L2 demand read hits, the L3 read
 *   accesses and the L3 demand read hits, or with --events walk the run 3
 *   events plus the TLB walks and their split by the descriptor the walk
 *   ended in (block or page).
 * --cme: the same chase through the CME (cme_kern.S: LD1D then LASTB in
 *   streaming mode), so every load takes the core -> CME -> core path. The
 *   first row, "cme-roundtrip", is a DUP then LASTB chain with no memory
 *   access: a control with the same register return path, not a measured
 *   part of a chase hop. --perf then counts the CME set (cycles, CME_CYCLES,
 *   CME L1 refills from inside the cluster, from outside the cluster, by reads,
 *   by the CME's hardware prefetcher). Every --cme row carries one more
 *   column (run 6), ns_per_load_span: the time the call spent inside its
 *   streaming span (CNTVCT_EL0 read after SMSTART and before SMSTOP, in
 *   cme_kern.S) per load. ns_per_load minus it is the instrumented
 *   outside-span interval of the call (register save, SMSTART, SMSTOP,
 *   restore, the call and return, and the second stamp's eight instructions),
 *   spread over the loads; it is not the switch alone. The cme-call rows sum
 *   the 64 spans of their 64 calls (the kernels keep the sum in cme_span[2];
 *   no C span collection inside the bracket). --events cme-l3 with --cme swaps the
 *   three refill-by-cause events for the CME's L3 hit, miss and refill
 *   events (0x328E, 0x328F, 0x3290), to be validated on the run 6 boots.
 * Counter columns: a row prints "-" for every counter, and a "# perf:" line
 *   with the reason, when the group did not count the whole walk (open
 *   failure, ioctl failure, short read, multiplexing). The timing stays valid.
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

static int sparse; /* --sparse: one member line per 4 KiB page per chain (1), --sparse-vary: the line varies per page (2) */
static int seq;    /* --seq: the chain is the lines in address order */
static int ev_tlb; /* --events tlb: the translation counter set instead of the prefetch set */
static int ev_l3;  /* --events l3: the L2 and L3 hit counter set instead of the prefetch set */
static int ev_walk; /* --events walk: the TLB walk set (walks by descriptor type) instead of the prefetch set */
static int ev_cme_l3; /* --events cme-l3 (with --cme): the CME L3 hit, miss and refill events instead of the refill-by-cause events */
#define LINES_PER_PAGE (4096 / LINE_BYTES)

/* Line index of member k of chain `slot`: every line in order, or one line per
 * page at an offset that differs per chain (line 8 slot + 4), or (--sparse-vary)
 * one line per page in slot (slot + h) mod 8 at line h' of that slot, where h and
 * h' are bits of a hash of the page number. Chains differ in slot, so no two chains
 * share a line in any mode. */
static uint64_t member_line(uint64_t k, int slot)
{
    if (!sparse)
        return k;
    uint64_t s = (uint64_t)slot, off = 4;
    if (sparse == 2) {
        uint64_t h = (k + 1) * 0x9E3779B97F4A7C15ULL;
        s = (s + (h >> 61)) & 7;
        off = (h >> 58) & 7;
    }
    return k * LINES_PER_PAGE + 8 * s + off;
}

/* First the identity, then a Sattolo shuffle of member indices, then index ->
 * address. Word `slot` of each member line carries this chain and the stored
 * pointer targets the same slot of the next member, so chase() needs no offset.
 * With --seq the shuffle is skipped and member i points to member i + 1. */
static void build_cycle(const struct buf *b, uint64_t n, int slot, uint64_t seed)
{
#define AT(i) (*(uint64_t *)(line_addr(b, member_line((i), slot)) + (slot) * 8))
    rng_state = seed;
    for (uint64_t i = 0; i < n; i++)
        AT(i) = seq ? (i + 1) % n : i;
    for (uint64_t i = n - 1; i > 0 && !seq; i--) {
        uint64_t j = xorshift64() % i;
        uint64_t tmp = AT(i);
        AT(i) = AT(j);
        AT(j) = tmp;
    }
    for (uint64_t i = 0; i < n; i++)
        AT(i) = (uint64_t)(uintptr_t)(line_addr(b, member_line(AT(i), slot)) + slot * 8);
#undef AT
}

static void *chain_start(const struct buf *b, int slot)
{
    return line_addr(b, member_line(0, slot)) + slot * 8;
}

static int use_cme;

__attribute__((noinline)) static void *chase(void *p, uint64_t loads)
{
    if (loads == 0)
        return p;
    if (use_cme)
        return (void *)(uintptr_t)cme_chase((uint64_t)(uintptr_t)p, loads);
    uint64_t i = 0;
    for (; i + 8 <= loads; i += 8) {
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
    }
    for (; i < loads; i++) /* a sparse chain can have fewer than 8 members, or a count that is not a multiple of 8 */
        p = *(void **)p;
    return p;
}

static struct {
    int cpu, reps, fresh, flush, use_perf;
    uint64_t loads;
    enum mem_kind kind;
    int regions[MAX_SEGS], nregions;
} cfg = { .cpu = -1, .reps = 3, .loads = 1 << 16, .kind = MEM_DRAM };

static struct perfgrp pg;

/* The counter columns of one row: per-load values and the coverage
 * (time_running / time_enabled, 1.0000 when the group counted the whole
 * walk), or "-" in every column and a comment line with the reason. */
static void print_counters(const uint64_t c[PERF_N], uint64_t loads, int valid, const char *why)
{
    if (pg.ok) {
        if (valid) {
            printf(",%.2f", (double)c[0] / (double)loads);
            for (int i = 1; i < PERF_N; i++) {
                if (perf_have(&pg, i)) print_rate(c[i], loads) /* common.h: %.4f, or %.3e for a small nonzero count */;
                else printf(",-"); /* an optional event this kernel refused to open */
            }
            printf(",%.4f", (double)pg.t_running / (double)pg.t_enabled);
        } else {
            for (int i = 0; i <= PERF_N; i++)
                printf(",-");
        }
    }
    printf("\n");
    if (pg.ok && !valid)
        printf("# perf: counters of the row above are invalid: %s\n", why);
}

/* One timed walk. Prints the CSV row and returns ns per load. */
static double timed_walk(const struct buf *b, void *start, uint64_t loads, const char *label, int rep, uint64_t n)
{
    uint64_t c[PERF_N];
    const char *why;
    if (cfg.flush)
        flush_buf(b);
    perf_start(&pg);
    double t0 = now_s();
    void *q = chase(start, loads);
    double t1 = now_s();
    perf_stop(&pg);
    sink = q;
    int valid = perf_read(&pg, c, &why);
    double ns = (t1 - t0) * 1e9 / (double)loads;
    printf("%s,%llu,%s%s,%d,%llu,%.3f", mem_name(b->kind), (unsigned long long)b->bytes, use_cme ? "cme-" : "",
           label, rep, (unsigned long long)loads, ns);
    if (use_cme)
        printf(",%.3f", cme_span_s() * 1e9 / (double)loads);
    print_counters(c, loads, valid, why);
    (void)n;
    return ns;
}

static void run_size(uint64_t bytes)
{
    uint64_t unit = sparse ? 2 * 4096 : 8 * LINE_BYTES;
    if (bytes % unit != 0) {
        fprintf(stderr, "size %llu is not a multiple of %llu bytes\n", (unsigned long long)bytes, (unsigned long long)unit);
        exit(2);
    }
    struct buf b;
    alloc_buf(&b, cfg.kind, bytes, cfg.regions, cfg.nregions);
    uint64_t n = sparse ? b.bytes / 4096 : b.bytes / LINE_BYTES; /* members per chain */
    int chains = cfg.fresh ? cfg.fresh : 1;
    for (int c = 0; c < chains; c++)
        build_cycle(&b, n, c, 0x9E3779B97F4A7C15ULL * (uint64_t)(c + 1) + 12345);
    printf("# size=%llu lines=%llu backing=%s segments=%d sparse=%d\n", (unsigned long long)b.bytes,
           (unsigned long long)n, b.backing, b.nseg, sparse);

    double best = 1e30;
    void *p = chase(chain_start(&b, 0), n); /* warm-up: data resident, TLB filled */
    p = chase(p, n);
    sink = p;
    if (cfg.fresh) {
        for (int c = 1; c < chains; c++) {
            char label[16];
            snprintf(label, sizeof label, "fresh%d", c);
            double ns = timed_walk(&b, chain_start(&b, c), n, label, c, n);
            if (ns < best) best = ns;
        }
    } else {
        /* --flush walks exactly one pass; otherwise --loads rounded up to whole cycles */
        uint64_t loads = cfg.flush ? n : n * ((cfg.loads + n - 1) / n);
        for (int rep = 0; rep < cfg.reps; rep++) {
            double ns = timed_walk(&b, chain_start(&b, 0), loads, cfg.flush ? "flushed" : "repeat", rep, n);
            if (ns < best) best = ns;
        }
    }
    printf("%s,%llu,%sbest,-,-,%.3f\n", mem_name(b.kind), (unsigned long long)b.bytes, use_cme ? "cme-" : "", best);
    fflush(stdout);
    for (int s = 0; s < b.nseg; s++)
        munmap(b.seg[s].base, b.kind == MEM_OCM ? memcfg.ocm_size : b.seg[s].bytes);
}

/* The DUP + LASTB control: a chain of --loads hops through a Z register with
 * no memory access. Printed like a size row with size 0, mode cme-roundtrip. */
static void run_roundtrip(void)
{
    uint64_t c[PERF_N];
    const char *why;
    double best = 1e30;
    uint64_t x = cme_roundtrip(1, cfg.loads); /* warm-up: the first SMSTART of a thread traps once */
    for (int rep = 0; rep < cfg.reps; rep++) {
        perf_start(&pg);
        double t0 = now_s();
        x = cme_roundtrip(x, cfg.loads);
        double t1 = now_s();
        perf_stop(&pg);
        int valid = perf_read(&pg, c, &why);
        double ns = (t1 - t0) * 1e9 / (double)cfg.loads;
        if (ns < best) best = ns;
        printf("%s,0,cme-roundtrip,%d,%llu,%.3f,%.3f", mem_name(cfg.kind), rep, (unsigned long long)cfg.loads, ns,
               cme_span_s() * 1e9 / (double)cfg.loads);
        print_counters(c, cfg.loads, valid, why);
    }
    sink = (void *)(uintptr_t)x;
    printf("%s,0,cme-roundtrip-best,-,-,%.3f\n", mem_name(cfg.kind), best);
    /* A complete one-hop control: 64 calls of cme_roundtrip with one DUP + LASTB
     * hop each, timed together and printed per call (loads = 64). Each call
     * holds the register save, SMSTART, the predicate setup, one register hop,
     * SMSTOP and the restore, plus the C loop around it. The span column (the
     * sum of the 64 in-span times, per call) holds the predicate setup and the
     * hop; ns_per_load minus it is the instrumented outside-span interval of a
     * call, not the switch alone. A sparse 64-hop chase pass pays the same
     * entry and exit once, so a short pass is read beside this row. */
    best = 1e30;
    for (int rep = 0; rep < cfg.reps; rep++) {
        perf_start(&pg);
        cme_span_reset();
        double t0 = now_s();
        for (int i = 0; i < 64; i++)
            x = cme_roundtrip(x, 1);
        double t1 = now_s();
        perf_stop(&pg);
        double span = cme_span_sum_s();
        int valid = perf_read(&pg, c, &why);
        double ns = (t1 - t0) * 1e9 / 64.0;
        if (ns < best) best = ns;
        printf("%s,0,cme-call,%d,64,%.3f,%.3f", mem_name(cfg.kind), rep, ns, span * 1e9 / 64.0);
        print_counters(c, 64, valid, why);
    }
    sink = (void *)(uintptr_t)x;
    printf("%s,0,cme-call-best,-,-,%.3f\n", mem_name(cfg.kind), best);
    fflush(stdout);
}

/* The fixed cost of one timed bracket (perf start, two clock reads, perf
 * stop) with no loads inside, printed once so a short pass (a sparse chain
 * has 64 hops per 256 KiB) can be read against it. Median of 32. */
static void print_bracket_overhead(void)
{
    double t[32];
    uint64_t c[PERF_N];
    const char *why;
    for (int i = 0; i < 32; i++) {
        perf_start(&pg);
        double t0 = now_s();
        double t1 = now_s();
        perf_stop(&pg);
        perf_read(&pg, c, &why);
        t[i] = t1 - t0;
    }
    for (int i = 1; i < 32; i++) /* insertion sort, 32 values */
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { double x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
    printf("# bracket_overhead_ns=%.1f (median of 32 empty timed brackets: the two clock reads; the perf start and stop sit outside them)\n",
           t[16] * 1e9);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: latency --sizes a,b,c [--cpu N] [--mem dram|ocm|spm] [--ocm-regions 0,1,..]\n"
            "               [--loads N] [--reps R] [--fresh K] [--flush] [--sparse | --sparse-vary] [--seq]\n"
            "               [--perf] [--events prefetch|tlb|l3|walk|cme-l3] [--cme] [--no-hugetlb] [--no-thp]\n"
            "               [--ocm-dev P --ocm-base A --ocm-stride S --ocm-size B] [--spm-dev P --spm-off O]\n"
            "  sizes accept K/M/G suffixes. --mem ocm splits each size evenly over the listed regions.\n"
            "  --fresh K (2..8): K chains per line, time one pass over each of chains 1..K-1.\n"
            "  --flush: DC CIVAC the buffer before each timed pass; the pass is one cycle.\n"
            "  --sparse: one member line per 4 KiB page per chain (sizes in multiples of 8 KiB), so a\n"
            "    prefetched neighbour is never a member; a pass is bytes/4096 hops, use --fresh 8.\n"
            "  --sparse-vary: as --sparse, with the member's line changing from page to page.\n"
            "  --seq: the chain is the lines in address order (a prefetch stimulus for the prefetch counters).\n"
            "  --events tlb: count the TLB refills and walks instead of the prefetch refills (core sets only).\n"
            "  --events l3: count the L2 and L3 demand read hits and the L3 read accesses instead (core sets only).\n"
            "  --events walk: count the TLB walks and their block and page parts instead (core sets only).\n"
            "  --events cme-l3: with --cme, count the CME's L3 hit, miss and refill events instead of its refill-by-cause events.\n"
            "  --cme: chase through the CME in streaming mode (needs SME2, SVL 64 B); adds a cme-roundtrip row.\n");
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
        else if (!strcmp(a, "--cme")) use_cme = 1;
        else if (!strcmp(a, "--sparse")) sparse = 1;
        else if (!strcmp(a, "--sparse-vary")) sparse = 2;
        else if (!strcmp(a, "--seq")) seq = 1;
        else if (!strcmp(a, "--events") && more) {
            const char *e = argv[++i];
            if (!strcmp(e, "tlb")) ev_tlb = 1;
            else if (!strcmp(e, "l3")) ev_l3 = 1;
            else if (!strcmp(e, "walk")) ev_walk = 1;
            else if (!strcmp(e, "cme-l3")) ev_cme_l3 = 1;
            else if (strcmp(e, "prefetch")) usage();
        }
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
    if (use_cme)
        require_cme();
    if (ev_tlb + ev_l3 + ev_walk + ev_cme_l3 > 1) { fprintf(stderr, "--events tlb, l3, walk and cme-l3 are exclusive\n"); usage(); }
    if (ev_cme_l3 && !use_cme) { fprintf(stderr, "--events cme-l3 needs --cme\n"); usage(); }
    if (cfg.use_perf) {
        if (use_cme) { if (ev_cme_l3) perf_open_cme_l3(&pg); else perf_open_cme(&pg); }
        else if (ev_tlb) perf_open_tlb(&pg);
        else if (ev_l3) perf_open_l3(&pg);
        else if (ev_walk) perf_open_walk(&pg);
        else perf_open(&pg);
    }

    printf("# latency: ns_per_load = elapsed_ns / loads; cpu=%d mem=%s cntfrq_hz=%llu loads=%llu reps=%d fresh=%d flush=%d sparse=%d seq=%d cme=%d events=%s\n",
           cfg.cpu, mem_name(cfg.kind), (unsigned long long)read_cntfrq(), (unsigned long long)cfg.loads,
           cfg.reps, cfg.fresh, cfg.flush, sparse, seq, use_cme, use_cme ? (ev_cme_l3 ? "cme-l3" : "cme") : ev_tlb ? "tlb" : ev_l3 ? "l3" : ev_walk ? "walk" : "prefetch");
    printf("mem,size_bytes,mode,rep,loads,ns_per_load");
    if (use_cme)
        printf(",ns_per_load_span"); /* run 6: the call's time inside its streaming span, per load */
    if (pg.ok) {
        for (int i = 0; i < PERF_N; i++)
            printf(",%s_per_load", pg.names[i]);
        printf(",perf_cov");
    }
    printf("\n");
    print_bracket_overhead();
    if (use_cme)
        run_roundtrip();
    for (int i = 0; i < nsizes; i++)
        run_size(sizes[i]);
    return 0;
}
