/*
 * bandwidth.c - streaming bandwidth for Asimov: read, write, copy; one or
 * more pinned threads; buffers in DRAM, OCM regions or SPM.
 *
 * Every thread owns its buffers (mapped and first-written by that thread).
 * All threads start a rep on a barrier, run --passes whole passes over their
 * buffer, and record bytes moved and their own elapsed time. GB = 1e9 bytes.
 *
 * Kernels (aarch64 inline asm, so the compiler cannot change the access
 * pattern; each iteration moves 256 B):
 *   read / write / copy : ldp/stp of q-register pairs (two 16 B accesses each)
 *   ldr16 / str16       : single ldr/str q (16 B, aligned), for SPM, which
 *                         accepts only aligned 8, 16 or 32 B accesses
 * --flush-before: DC CIVAC the buffer before each timed rep, so reads come
 *   from the home node (OCM / SLC), not from L1/L2/L3.
 * --flush-after: DC CIVAC inside the timed region after the passes, so a
 *   write test includes the write-back to the home node ("write+writeback").
 */
#include "common.h"
#include <pthread.h>

#define MAX_CPUS 64
#define MAX_REPS 32

enum kernel { K_READ, K_WRITE, K_COPY, K_LDR16, K_STR16, K_COUNT };
static const char *kernel_name[K_COUNT] = { "read", "write", "copy", "ldr16", "str16" };

static struct {
    int threads, ncpus, cpus[MAX_CPUS];
    uint64_t bytes;
    int run[K_COUNT];
    int passes, reps, flush_before, flush_after;
    enum mem_kind kind;
    int regions[MAX_SEGS], nregions;
} cfg = { .threads = 1, .bytes = 1 << 20, .passes = 4, .reps = 3, .kind = MEM_DRAM };

struct result { uint64_t bytes; double elapsed_s; };

struct thread {
    int idx, cpu;
    struct buf a, b;
    struct result res[K_COUNT][MAX_REPS];
    pthread_t tid;
};

static pthread_barrier_t bar;

static void parse_cpus(char *list)
{
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        char *e, *e2;
        long lo = strtol(tok, &e, 10), hi = lo, step = 1;
        if (e != tok && *e == '-') {
            hi = strtol(e + 1, &e2, 10);
            e = e2 == e + 1 ? tok : e2;
            if (e != tok && *e == ':') { step = strtol(e + 1, &e2, 10); e = e2 == e + 1 ? tok : e2; }
        }
        if (e == tok || *e != '\0' || lo < 0 || hi < lo || hi >= CPU_SETSIZE || step < 1) {
            fprintf(stderr, "bad cpu list: %s\n", tok);
            exit(2);
        }
        for (long c = lo; c <= hi; c += step) {
            if (cfg.ncpus == MAX_CPUS) { fprintf(stderr, "too many cpus\n"); exit(2); }
            cfg.cpus[cfg.ncpus++] = (int)c;
        }
    }
}

#if defined(__aarch64__)
#define HAVE_ASM 1
/* v8-v15 are callee-saved in AAPCS64: naming them as clobbers makes gcc save
 * d8-d15 once per call, outside the loop. */
#define QCLOBBER "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "memory", "cc"

__attribute__((noinline)) static void k_read(const uint8_t *p, uint64_t bytes)
{
    const uint8_t *end = p + bytes;
    __asm__ volatile(
        "1:\n\t"
        "ldp q0, q1, [%[p]]\n\t"       "ldp q2, q3, [%[p], #32]\n\t"
        "ldp q4, q5, [%[p], #64]\n\t"  "ldp q6, q7, [%[p], #96]\n\t"
        "ldp q8, q9, [%[p], #128]\n\t" "ldp q10, q11, [%[p], #160]\n\t"
        "ldp q12, q13, [%[p], #192]\n\t" "ldp q14, q15, [%[p], #224]\n\t"
        "add %[p], %[p], #256\n\t" "cmp %[p], %[end]\n\t" "b.lo 1b\n\t"
        : [p] "+r"(p) : [end] "r"(end) : QCLOBBER);
}

__attribute__((noinline)) static void k_write(uint8_t *p, uint64_t bytes, uint64_t v)
{
    const uint8_t *end = p + bytes;
    __asm__ volatile(
        "dup v0.2d, %[v]\n\t"
        "1:\n\t"
        "stp q0, q0, [%[p]]\n\t"       "stp q0, q0, [%[p], #32]\n\t"
        "stp q0, q0, [%[p], #64]\n\t"  "stp q0, q0, [%[p], #96]\n\t"
        "stp q0, q0, [%[p], #128]\n\t" "stp q0, q0, [%[p], #160]\n\t"
        "stp q0, q0, [%[p], #192]\n\t" "stp q0, q0, [%[p], #224]\n\t"
        "add %[p], %[p], #256\n\t" "cmp %[p], %[end]\n\t" "b.lo 1b\n\t"
        : [p] "+r"(p) : [end] "r"(end), [v] "r"(v) : "v0", "memory", "cc");
}

__attribute__((noinline)) static void k_copy(uint8_t *d, const uint8_t *s, uint64_t bytes)
{
    const uint8_t *end = s + bytes;
    __asm__ volatile(
        "1:\n\t"
        "ldp q0, q1, [%[s]]\n\t"       "ldp q2, q3, [%[s], #32]\n\t"
        "ldp q4, q5, [%[s], #64]\n\t"  "ldp q6, q7, [%[s], #96]\n\t"
        "ldp q8, q9, [%[s], #128]\n\t" "ldp q10, q11, [%[s], #160]\n\t"
        "ldp q12, q13, [%[s], #192]\n\t" "ldp q14, q15, [%[s], #224]\n\t"
        "stp q0, q1, [%[d]]\n\t"       "stp q2, q3, [%[d], #32]\n\t"
        "stp q4, q5, [%[d], #64]\n\t"  "stp q6, q7, [%[d], #96]\n\t"
        "stp q8, q9, [%[d], #128]\n\t" "stp q10, q11, [%[d], #160]\n\t"
        "stp q12, q13, [%[d], #192]\n\t" "stp q14, q15, [%[d], #224]\n\t"
        "add %[s], %[s], #256\n\t" "add %[d], %[d], #256\n\t"
        "cmp %[s], %[end]\n\t" "b.lo 1b\n\t"
        : [s] "+r"(s), [d] "+r"(d) : [end] "r"(end) : QCLOBBER);
}

__attribute__((noinline)) static void k_ldr16(const uint8_t *p, uint64_t bytes)
{
    const uint8_t *end = p + bytes;
    __asm__ volatile(
        "1:\n\t"
        "ldr q0, [%[p]]\n\t"        "ldr q1, [%[p], #16]\n\t"  "ldr q2, [%[p], #32]\n\t"  "ldr q3, [%[p], #48]\n\t"
        "ldr q4, [%[p], #64]\n\t"   "ldr q5, [%[p], #80]\n\t"  "ldr q6, [%[p], #96]\n\t"  "ldr q7, [%[p], #112]\n\t"
        "ldr q8, [%[p], #128]\n\t"  "ldr q9, [%[p], #144]\n\t" "ldr q10, [%[p], #160]\n\t" "ldr q11, [%[p], #176]\n\t"
        "ldr q12, [%[p], #192]\n\t" "ldr q13, [%[p], #208]\n\t" "ldr q14, [%[p], #224]\n\t" "ldr q15, [%[p], #240]\n\t"
        "add %[p], %[p], #256\n\t" "cmp %[p], %[end]\n\t" "b.lo 1b\n\t"
        : [p] "+r"(p) : [end] "r"(end) : QCLOBBER);
}

__attribute__((noinline)) static void k_str16(uint8_t *p, uint64_t bytes, uint64_t v)
{
    const uint8_t *end = p + bytes;
    __asm__ volatile(
        "dup v0.2d, %[v]\n\t"
        "1:\n\t"
        "str q0, [%[p]]\n\t"        "str q0, [%[p], #16]\n\t"  "str q0, [%[p], #32]\n\t"  "str q0, [%[p], #48]\n\t"
        "str q0, [%[p], #64]\n\t"   "str q0, [%[p], #80]\n\t"  "str q0, [%[p], #96]\n\t"  "str q0, [%[p], #112]\n\t"
        "str q0, [%[p], #128]\n\t"  "str q0, [%[p], #144]\n\t" "str q0, [%[p], #160]\n\t" "str q0, [%[p], #176]\n\t"
        "str q0, [%[p], #192]\n\t"  "str q0, [%[p], #208]\n\t" "str q0, [%[p], #224]\n\t" "str q0, [%[p], #240]\n\t"
        "add %[p], %[p], #256\n\t" "cmp %[p], %[end]\n\t" "b.lo 1b\n\t"
        : [p] "+r"(p) : [end] "r"(end), [v] "r"(v) : "v0", "memory", "cc");
}
#else
/* Portable stand-ins so the harness can be tested on a non-Arm host. They
 * are not the kernels that produce the Asimov numbers. */
#define HAVE_ASM 0
static volatile uint64_t c_sink;
__attribute__((noinline)) static void k_read(const uint8_t *p, uint64_t bytes)
{
    const uint64_t *q = (const uint64_t *)p;
    uint64_t s[8] = { 0 };
    for (uint64_t i = 0; i < bytes / 8; i += 8)
        for (int j = 0; j < 8; j++) s[j] += q[i + j];
    c_sink = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
}
__attribute__((noinline)) static void k_write(uint8_t *p, uint64_t bytes, uint64_t v)
{
    volatile uint64_t *q = (volatile uint64_t *)p;
    for (uint64_t i = 0; i < bytes / 8; i++) q[i] = v;
}
__attribute__((noinline)) static void k_copy(uint8_t *d, const uint8_t *s, uint64_t bytes)
{
    volatile uint64_t *q = (volatile uint64_t *)d;
    const uint64_t *r = (const uint64_t *)s;
    for (uint64_t i = 0; i < bytes / 8; i++) q[i] = r[i];
}
static void k_ldr16(const uint8_t *p, uint64_t bytes) { k_read(p, bytes); }
static void k_str16(uint8_t *p, uint64_t bytes, uint64_t v) { k_write(p, bytes, v); }
#endif

/* One pass over all segments of the thread's buffer(s); returns bytes moved. */
static uint64_t pass(enum kernel k, struct thread *t, uint64_t v)
{
    for (int s = 0; s < t->a.nseg; s++) {
        uint8_t *a = t->a.seg[s].base;
        uint64_t n = t->a.seg[s].bytes;
        switch (k) {
        case K_READ: k_read(a, n); break;
        case K_WRITE: k_write(a, n, v); break;
        case K_COPY: k_copy(t->b.seg[s].base, a, n); break;
        case K_LDR16: k_ldr16(a, n); break;
        case K_STR16: k_str16(a, n, v); break;
        default: break;
        }
    }
    return k == K_COPY ? 2 * cfg.bytes : cfg.bytes;
}

static int need_b(void) { return cfg.run[K_COPY]; }

/* Thread t's buffers: DRAM: private mappings. OCM: regions taken in order
 * from --ocm-regions, ceil(bytes / region size) per buffer. SPM: consecutive
 * slices of the SPM window starting at --spm-off. */
static void alloc_thread_bufs(struct thread *t)
{
    int per = (int)((cfg.bytes + memcfg.ocm_size - 1) / memcfg.ocm_size);
    int nb = need_b() ? 2 : 1;
    switch (cfg.kind) {
    case MEM_DRAM:
        alloc_dram(&t->a, cfg.bytes);
        if (need_b()) alloc_dram(&t->b, cfg.bytes);
        break;
    case MEM_OCM: {
        int first = t->idx * per * nb;
        if (first + per * nb > cfg.nregions) {
            fprintf(stderr, "thread %d needs OCM regions %d..%d but only %d listed\n", t->idx, first,
                    first + per * nb - 1, cfg.nregions);
            exit(2);
        }
        alloc_ocm(&t->a, cfg.regions + first, per, cfg.bytes / (uint64_t)per);
        if (need_b()) alloc_ocm(&t->b, cfg.regions + first + per, per, cfg.bytes / (uint64_t)per);
        break;
    }
    case MEM_SPM: {
        uint64_t save = memcfg.spm_off;
        memcfg.spm_off = save + (uint64_t)t->idx * cfg.bytes * (uint64_t)nb;
        alloc_spm(&t->a, cfg.bytes);
        if (need_b()) { memcfg.spm_off += cfg.bytes; alloc_spm(&t->b, cfg.bytes); }
        memcfg.spm_off = save;
        break;
    }
    }
    uint64_t v = ((uint64_t)t->idx + 1) * 0x9E3779B97F4A7C15ULL;
    fill_buf(&t->a, v);
    if (need_b()) fill_buf(&t->b, v);
}

static void *worker(void *arg)
{
    struct thread *t = arg;
    uint64_t v = ((uint64_t)getpid() + (uint64_t)t->idx + 1) * 0x9E3779B97F4A7C15ULL;
    if (cfg.ncpus)
        pin_cpu(t->cpu);
    alloc_thread_bufs(t);
    pthread_barrier_wait(&bar); /* all buffers exist before anyone starts timing */

    for (int k = 0; k < K_COUNT; k++) {
        if (!cfg.run[k])
            continue;
        for (int rep = 0; rep < cfg.reps; rep++) {
            pass(k, t, v); /* warm-up: TLB filled, data resident where it fits */
            if (cfg.flush_before)
                flush_buf(&t->a);
            pthread_barrier_wait(&bar);
            double t0 = now_s();
            uint64_t bytes = 0;
            for (int p = 0; p < cfg.passes; p++)
                bytes += pass(k, t, v + (uint64_t)p);
            if (cfg.flush_after)
                flush_buf(k == K_COPY ? &t->b : &t->a);
            double t1 = now_s();
            t->res[k][rep].bytes = bytes;
            t->res[k][rep].elapsed_s = t1 - t0;
            pthread_barrier_wait(&bar);
        }
    }
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: bandwidth [--threads T --cpus LIST] [--bytes B] [--kernel read|write|copy|all|ldr16|str16]\n"
            "                 [--passes P] [--reps R] [--mem dram|ocm|spm] [--ocm-regions 0,1,..]\n"
            "                 [--flush-before] [--flush-after] [--no-hugetlb] [--ocm-*/--spm-* overrides]\n"
            "  bytes is per thread and must be a multiple of 256; cpus: \"2,4,6\" or \"2-9\" or \"2-8:2\"\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int threads_given = 0;
    cfg.run[K_READ] = cfg.run[K_WRITE] = cfg.run[K_COPY] = 1;
    for (int i = 1; i < argc;) {
        int used = parse_mem_opt(argc, argv, i, &cfg.kind, cfg.regions, &cfg.nregions);
        if (used) { i += used; continue; }
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--flush-before")) cfg.flush_before = 1;
        else if (!strcmp(a, "--flush-after")) cfg.flush_after = 1;
        else if (!strcmp(a, "--threads") && more) { cfg.threads = atoi(argv[++i]); threads_given = 1; }
        else if (!strcmp(a, "--cpus") && more) parse_cpus(argv[++i]);
        else if (!strcmp(a, "--bytes") && more) cfg.bytes = parse_bytes(argv[++i]);
        else if (!strcmp(a, "--passes") && more) cfg.passes = atoi(argv[++i]);
        else if (!strcmp(a, "--reps") && more) cfg.reps = atoi(argv[++i]);
        else if (!strcmp(a, "--kernel") && more) {
            const char *k = argv[++i];
            memset(cfg.run, 0, sizeof cfg.run);
            if (!strcmp(k, "all")) cfg.run[K_READ] = cfg.run[K_WRITE] = cfg.run[K_COPY] = 1;
            else {
                int found = 0;
                for (int j = 0; j < K_COUNT; j++)
                    if (!strcmp(k, kernel_name[j])) { cfg.run[j] = 1; found = 1; }
                if (!found) usage();
            }
        } else usage();
        i++;
    }
    if (threads_given && cfg.ncpus == 0) { fprintf(stderr, "--threads requires --cpus\n"); usage(); }
    if (cfg.threads < 1 || cfg.threads > MAX_CPUS || cfg.reps < 1 || cfg.reps > MAX_REPS || cfg.passes < 1 ||
        cfg.bytes == 0 || cfg.bytes % 256 != 0)
        usage();
    if (cfg.ncpus && cfg.threads > cfg.ncpus)
        fprintf(stderr, "warning: %d threads on %d cpus\n", cfg.threads, cfg.ncpus);
    if (cfg.kind == MEM_SPM && (cfg.run[K_READ] || cfg.run[K_WRITE] || cfg.run[K_COPY]))
        fprintf(stderr, "warning: SPM accepts only aligned 8/16/32 B accesses; ldr16/str16 are the safe kernels\n");

    struct thread *th = calloc((size_t)cfg.threads, sizeof *th);
    if (!th || pthread_barrier_init(&bar, NULL, (unsigned)cfg.threads + 1) != 0) { perror("setup"); return 2; }
    printf("# bandwidth: GB = 1e9 bytes; asm_kernels=%d threads=%d bytes_per_thread=%llu passes=%d reps=%d mem=%s "
           "flush_before=%d flush_after=%d cntfrq_hz=%llu\n", HAVE_ASM, cfg.threads,
           (unsigned long long)cfg.bytes, cfg.passes, cfg.reps, mem_name(cfg.kind), cfg.flush_before,
           cfg.flush_after, (unsigned long long)read_cntfrq());
    for (int i = 0; i < cfg.threads; i++) {
        th[i].idx = i;
        th[i].cpu = cfg.ncpus ? cfg.cpus[i % cfg.ncpus] : -1;
        if (pthread_create(&th[i].tid, NULL, worker, &th[i]) != 0) { perror("pthread_create"); return 2; }
    }
    pthread_barrier_wait(&bar); /* buffers allocated */
    printf("# backing=%s\n", th[0].a.backing);
    printf("mem,kernel,threads,bytes_per_thread,rep,total_bytes,elapsed_s,aggregate_GBps,per_thread_min_GBps,per_thread_max_GBps\n");
    for (int k = 0; k < K_COUNT; k++) {
        if (!cfg.run[k]) continue;
        double best = 0;
        for (int rep = 0; rep < cfg.reps; rep++) {
            pthread_barrier_wait(&bar); /* start */
            pthread_barrier_wait(&bar); /* end */
            uint64_t total = 0;
            double longest = 0, mn = 1e30, mx = 0;
            for (int i = 0; i < cfg.threads; i++) {
                struct result *r = &th[i].res[k][rep];
                double g = (double)r->bytes / r->elapsed_s / 1e9;
                total += r->bytes;
                if (r->elapsed_s > longest) longest = r->elapsed_s;
                if (g < mn) mn = g;
                if (g > mx) mx = g;
            }
            double agg = (double)total / longest / 1e9;
            if (agg > best) best = agg;
            printf("%s,%s,%d,%llu,%d,%llu,%.6f,%.3f,%.3f,%.3f\n", mem_name(cfg.kind), kernel_name[k], cfg.threads,
                   (unsigned long long)cfg.bytes, rep, (unsigned long long)total, longest, agg, mn, mx);
        }
        printf("%s,%s,%d,%llu,best,-,-,%.3f,-,-\n", mem_name(cfg.kind), kernel_name[k], cfg.threads,
               (unsigned long long)cfg.bytes, best);
        fflush(stdout);
    }
    for (int i = 0; i < cfg.threads; i++)
        pthread_join(th[i].tid, NULL);
    return 0;
}
