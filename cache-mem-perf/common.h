/*
 * common.h - shared pieces of the Asimov cache tests: buffers in DRAM, OCM
 * or SPM, timing, cache flush, CPU pinning and perf counters.
 *
 * A buffer is a list of segments (base, bytes). A DRAM buffer is one segment.
 * An OCM buffer is one segment per mapped region (256 KiB each by default).
 * Line index i of a buffer lives in segment i / lines_per_segment.
 */
#ifndef ASIMOV_COMMON_H
#define ASIMOV_COMMON_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LINE_BYTES 64
#define HUGE_BYTES (2UL << 20)
#define MAX_SEGS 16

enum mem_kind { MEM_DRAM, MEM_OCM, MEM_SPM };

struct seg {
    uint8_t *base;
    uint64_t bytes;
};

struct buf {
    enum mem_kind kind;
    int nseg;
    struct seg seg[MAX_SEGS];
    uint64_t bytes;     /* sum over segments */
    uint64_t seg_bytes; /* every segment has this size */
    const char *backing; /* "hugetlb", "thp", "4k", "ocm", "spm" */
};

/* Device defaults from Jesse Zhang's 2026-10-30 notes. All overridable. */
static struct {
    const char *ocm_dev;
    uint64_t ocm_base, ocm_stride, ocm_size;
    const char *spm_dev;
    uint64_t spm_off;
    int no_hugetlb; /* skip MAP_HUGETLB, go straight to THP madvise */
    int no_thp;     /* with no_hugetlb: ask for 4 KiB pages (MADV_NOHUGEPAGE), so only touched pages exist */
} memcfg = { "/dev/ocm", 0xE0000000ULL, 0x100000ULL, 0x40000ULL, "/dev/asimov-spm", 0, 0, 0 };

static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Accepts "4096", "16K", "1.5M", "2G" (K/M/G are powers of 1024). */
static inline uint64_t parse_bytes(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    double mul = 1;
    switch (*end) {
    case 'k': case 'K': mul = 1UL << 10; end++; break;
    case 'm': case 'M': mul = 1UL << 20; end++; break;
    case 'g': case 'G': mul = 1UL << 30; end++; break;
    }
    if (end == s || *end != '\0' || v <= 0) {
        fprintf(stderr, "bad size: %s\n", s);
        exit(2);
    }
    return (uint64_t)(v * mul);
}

static inline enum mem_kind parse_mem(const char *s)
{
    if (!strcmp(s, "dram")) return MEM_DRAM;
    if (!strcmp(s, "ocm")) return MEM_OCM;
    if (!strcmp(s, "spm")) return MEM_SPM;
    fprintf(stderr, "bad --mem: %s (dram|ocm|spm)\n", s);
    exit(2);
}

static inline void pin_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        perror("sched_setaffinity");
        exit(2);
    }
}

/* DRAM: MAP_HUGETLB first (needs reserved 2 MiB pages), else THP madvise on
 * a 2 MiB-aligned anonymous mapping, or 4 KiB pages with --no-thp (a control
 * row then touches only its own bytes, not a whole 2 MiB page). Records which
 * one was obtained. */
static inline void alloc_dram(struct buf *b, uint64_t bytes)
{
    size_t len = (bytes + HUGE_BYTES - 1) & ~(HUGE_BYTES - 1);
    uint8_t *p = MAP_FAILED;
    if (!memcfg.no_hugetlb) {
        p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        b->backing = "hugetlb";
    }
    if (p == MAP_FAILED) {
        uint8_t *raw = mmap(NULL, len + HUGE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            perror("mmap");
            exit(2);
        }
        p = (uint8_t *)(((uintptr_t)raw + HUGE_BYTES - 1) & ~(uintptr_t)(HUGE_BYTES - 1));
        if (memcfg.no_thp)
            b->backing = madvise(p, len, MADV_NOHUGEPAGE) == 0 ? "4k" : "4k?";
        else
            b->backing = madvise(p, len, MADV_HUGEPAGE) == 0 ? "thp" : "4k";
    }
    b->kind = MEM_DRAM;
    b->nseg = 1;
    b->seg[0].base = p;
    b->seg[0].bytes = bytes;
    b->bytes = b->seg_bytes = bytes;
}

static inline uint8_t *map_dev(const char *dev, uint64_t off, uint64_t bytes)
{
    int fd = open(dev, O_RDWR); /* MAP_SHARED below: the SPM driver refuses MAP_PRIVATE */
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        exit(2);
    }
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap %s offset 0x%llx bytes 0x%llx: %s\n", dev,
                (unsigned long long)off, (unsigned long long)bytes, strerror(errno));
        exit(2);
    }
    close(fd);
    return p;
}

/* OCM: one segment per region number in regions[] (region n is at
 * ocm_base + n * ocm_stride, ocm_size bytes). */
static inline void alloc_ocm(struct buf *b, const int *regions, int nregions, uint64_t bytes_per_region)
{
    if (nregions < 1 || nregions > MAX_SEGS || bytes_per_region > memcfg.ocm_size) {
        fprintf(stderr, "bad OCM request: %d regions of %llu bytes (region size %llu)\n", nregions,
                (unsigned long long)bytes_per_region, (unsigned long long)memcfg.ocm_size);
        exit(2);
    }
    b->kind = MEM_OCM;
    b->nseg = nregions;
    for (int i = 0; i < nregions; i++) {
        b->seg[i].base = map_dev(memcfg.ocm_dev, memcfg.ocm_base + (uint64_t)regions[i] * memcfg.ocm_stride, memcfg.ocm_size);
        b->seg[i].bytes = bytes_per_region;
    }
    b->seg_bytes = bytes_per_region;
    b->bytes = bytes_per_region * (uint64_t)nregions;
    b->backing = "ocm";
}

static inline void alloc_spm(struct buf *b, uint64_t bytes)
{
    b->kind = MEM_SPM;
    b->nseg = 1;
    b->seg[0].base = map_dev(memcfg.spm_dev, memcfg.spm_off, bytes);
    b->seg[0].bytes = bytes;
    b->bytes = b->seg_bytes = bytes;
    b->backing = "spm";
}

static inline uint8_t *line_addr(const struct buf *b, uint64_t line)
{
    uint64_t per = b->seg_bytes / LINE_BYTES;
    return b->seg[line / per].base + (line % per) * LINE_BYTES;
}

/* Writes every byte with aligned 8-byte stores. This is the first touch of
 * DRAM pages, the required write-before-read of OCM (ECC), and the only
 * store form allowed on SPM (no memset: it may use unaligned or DC ZVA
 * stores). The volatile pointer stops gcc from turning the loop into memset. */
static inline void fill_buf(struct buf *b, uint64_t v)
{
    for (int s = 0; s < b->nseg; s++) {
        volatile uint64_t *p = (volatile uint64_t *)b->seg[s].base;
        for (uint64_t i = 0; i < b->seg[s].bytes / 8; i++)
            p[i] = v + i;
    }
}

/* Clean and invalidate every line of the buffer from all core caches, then
 * wait. After this a load must go to the home node (OCM, SLC) or DRAM. On
 * SPM (non-cacheable) it is a no-op. */
static inline void flush_buf(const struct buf *b)
{
#if defined(__aarch64__)
    for (int s = 0; s < b->nseg; s++)
        for (uint64_t off = 0; off < b->seg[s].bytes; off += LINE_BYTES)
            __asm__ volatile("dc civac, %0" : : "r"(b->seg[s].base + off) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
#else
    (void)b;
#endif
}

static inline uint64_t read_cntfrq(void)
{
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 0;
#endif
}

/* ---- perf counters: cycles + six raw events, one group. The C1-Nano PMU has 6 or 20 event
 * counters (TRM, "Number of PMU event counters"); cycles take the dedicated cycle counter, so
 * six raw events fit either size, as long as nothing else holds a counter (perf_cov catches
 * the case where something does). The CRB (Neoverse V3AE) exposes 6 + cycles. ---- */
#define PERF_N 7
#define PERF_REQUIRED 4   /* the run 3 set: cycles and three refill events; the rest are optional */
struct perfgrp {
    int fd[PERF_N];
    int pos[PERF_N];          /* position of event i in the group read (the open events in order), or -1 when refused */
    int ok;
    int n;                    /* events open: PERF_N, or fewer when optional events were refused */
    const char *const *names; /* column names of the open set */
    int ctl_err;              /* errno of the last failed reset/enable/disable ioctl, else 0 */
    uint64_t t_enabled, t_running; /* ns, this row: the change since the previous read (the kernel keeps
                                      the lifetime totals across PERF_EVENT_IOC_RESET, which resets counts only) */
    uint64_t prev_enabled, prev_running; /* lifetime totals at the previous read, the baseline of the next row */
};
static const char *perf_name[PERF_N] = { "cycles", "l1d_refill", "l2d_refill", "l3d_refill_rd", "l1d_refill_hwprf", "l2d_refill_hwprf", "l2d_refill_l1dhwprf" };
static const char *perf_name_tlb[PERF_N] = { "cycles", "l1d_refill", "l2d_refill", "l3d_refill_rd", "l1d_tlb_refill", "l2d_tlb_refill", "dtlb_walk" };
static const char *perf_name_l3[PERF_N] = { "cycles", "l1d_refill", "l2d_refill", "l3d_refill_rd", "l2d_hit_rd", "l3d_access_rd", "l3d_hit_rd" };
static const char *perf_name_walk[PERF_N] = { "cycles", "l1d_refill", "l2d_refill", "l3d_refill_rd", "dtlb_walk", "dtlb_walk_block", "dtlb_walk_page" };

static inline int perf_open_one(struct perf_event_attr *a, int group)
{
    a->size = sizeof *a;
    a->disabled = group < 0;
    a->exclude_kernel = 1;
    a->exclude_hv = 1;
    a->read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return (int)syscall(__NR_perf_event_open, a, 0, -1, group, 0);
}

/* The CME set: the same core PMU, with the C1-SME2 events that the core PMU
 * exposes (asimov-bench dpa/stress counters.c and cme-perf open them this way).
 * Numbers and names from the Arm C1-SME2 TRM 107831 chapter 12:
 *   0x3246 CME_CYCLES              clock cycles of the CME (its own clock, not the timer)
 *   0x327D CME_L1D_CACHE_REFILL_INNER  CME L1 refills whose data came from a cache inside the cluster (the DSU L3)
 *   0x327E CME_L1D_CACHE_REFILL_OUTER  CME L1 refills whose data came from outside the cluster (SLC, OCM, DRAM)
 *   0x3280 CME_L1D_CACHE_REFILL_RD     CME L1 refills caused by a load that missed
 *   0x32B2 CME_L1D_CACHE_REFILL_HWPRF  CME L1 refills caused by the CME's hardware prefetcher
 *   0x3281 CME_L1D_CACHE_REFILL_WR     CME L1 refills caused by a store that missed (none expected: the chase has no stores)
 * A load that hit the CME L1 counts in no refill event. INNER + OUTER is every refill. The TRM
 * lists four causes (RD, WR, HWPRF and 0x32B1 software preload); the chase kernel has no stores
 * and no preloads, so for it RD + HWPRF should equal INNER + OUTER. That is a consistency
 * hypothesis for this kernel, checked per load, not a TRM identity. "Inner" means a cache inside
 * the immediate cluster of the core; on Asimov that is the DSU's L3 or the other core's caches. */
static const char *perf_name_cme[PERF_N] = { "cycles", "cme_cycles", "cme_l1d_refill_inner", "cme_l1d_refill_outer", "cme_l1d_refill_rd", "cme_l1d_refill_hwprf", "cme_l1d_refill_wr" };
static const char *perf_name_cme_l3[PERF_N] = { "cycles", "cme_cycles", "cme_l1d_refill_inner", "cme_l1d_refill_outer", "cme_l3d_hit", "cme_l3d_miss" , "cme_l3d_refill" };

static inline void perf_open_set(struct perfgrp *g, const uint64_t raw[PERF_N], const char *const names[PERF_N])
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_CPU_CYCLES;
    memset(g, 0, sizeof *g); /* fds, errno, coverage baseline: a stack-allocated group starts clean too */
    g->names = names;
    g->fd[0] = perf_open_one(&a, -1);
    if (g->fd[0] < 0) {
        printf("# perf: unavailable (%s)\n", strerror(errno));
        return;
    }
    g->n = 1;
    g->pos[0] = 0;
    for (int i = 1; i < PERF_N; i++) {
        memset(&a, 0, sizeof a);
        a.type = PERF_TYPE_RAW;
        a.config = raw[i];
        g->fd[i] = perf_open_one(&a, g->fd[0]);
        if (g->fd[i] < 0) {
            printf("# perf: event %s (raw 0x%llx) unavailable (%s)\n", names[i],
                   (unsigned long long)raw[i], strerror(errno));
            if (i < PERF_REQUIRED)
                return;
            g->pos[i] = -1; /* an optional event: its column prints "-"; the events after it are still tried */
            continue;
        }
        g->pos[i] = g->n++;
    }
    g->ok = 1;
}

/* Whether event i of the group is open (its column is a number, not "-"). */
/* One per-load or per-line counter value as a CSV field: "%.4f", or "%.3e" for a nonzero count that
 * "%.4f" would print as 0.0000, so a small count stays distinct from an exact zero (and from "-", a
 * refused event). Shared by latency.c (per load) and bandwidth.c (per 64 B line). */
static inline void print_rate(uint64_t count, uint64_t per)
{
    double v = (double)count / (double)per;
    if (count != 0 && v < 0.00005)
        printf(",%.3e", v);
    else
        printf(",%.4f", v);
}

static inline int perf_have(const struct perfgrp *g, int i)
{
    return g->ok && g->pos[i] >= 0;
}

static inline void perf_open(struct perfgrp *g)
{
    /* Arm PMU events (C1-Nano TRM event list): 0x03 L1D_CACHE_REFILL (refills caused by the
     * program's loads and stores), 0x17 L2D_CACHE_REFILL (every L2 refill: data and instruction
     * misses, translation walks, L1 write-backs, prefetches), 0xA2 L3D_CACHE_REFILL_RD (L3 fills
     * from outside due to a read), 0x81BC L1D_CACHE_REFILL_HWPRF (L1D refills a hardware
     * prefetcher requested), 0x81BD L2D_CACHE_REFILL_HWPRF (L2 refills by prefetchers targeting
     * L2) and 0x0111 IMP_L2D_CACHE_REFILL_L1DHWPRF (L2 refills by the L1D prefetcher's requests).
     * The three prefetch events are reported as their own rates; 0x17 minus them is "L2 refills
     * not accounted for by these events", not a demand count. Not 0x2A: the C1-Nano opens it
     * without error but it always reads 0 (Jesse, 2026-10-30). Whether an event counts on the
     * emulator is shown by its counts on a stimulus where its activity is established, not by the open succeeding. */
    static const uint64_t raw[PERF_N] = { 0, 0x03, 0x17, 0xA2, 0x81BC, 0x81BD, 0x0111 };
    perf_open_set(g, raw, perf_name);
}

/* The translation set (--events tlb): the run 3 refill events plus 0x05 L1D_TLB_REFILL,
 * 0x2D L2D_TLB_REFILL and 0x34 DTLB_WALK (C1-Nano TRM event list). 0x05 counts refills caused by
 * hardware prefetches too (TRM), so it is translation activity, not demand misses only. */
static inline void perf_open_tlb(struct perfgrp *g)
{
    static const uint64_t raw[PERF_N] = { 0, 0x03, 0x17, 0xA2, 0x05, 0x2D, 0x34 };
    perf_open_set(g, raw, perf_name_tlb);
}

/* The walk set (--events walk): the run 3 refill events plus 0x34 DTLB_WALK and its two parts by
 * the descriptor the walk ended in, 0x8188 DTLB_WALK_BLOCK (a block descriptor at level 1 or 2) and
 * 0x818A DTLB_WALK_PAGE (a page descriptor), C1-Nano TRM event list. On 4 KiB pages every walk is
 * a page walk. A walk through a contiguous-hint entry counts as a block walk (nevis0_tlb_pagewalk.sv
 * line 3147: block_entry or contig; nevis0_dpu_pmu.sv line 4304 routes it to 0x8188), which is what
 * the boot C and D rows ask: whether the walker used the larger translation, not only whether the
 * kernel wrote the hint bit. A zero 0x8188 is unvalidated until the event has read nonzero on some
 * row (the script has no block-walk stimulus). */
static inline void perf_open_walk(struct perfgrp *g)
{
    static const uint64_t raw[PERF_N] = { 0, 0x03, 0x17, 0xA2, 0x34, 0x8188, 0x818A };
    perf_open_set(g, raw, perf_name_walk);
}

/* The hit set (--events l3): the run 3 refill events plus 0x81C5 L2D_CACHE_HIT_RD (L2 hits by
 * demand reads), 0xA0 L3D_CACHE_RD (reads that reach the L3; the TRM says every memory read
 * counted by L3D_CACHE, 0x2B; in this RTL the event takes requests of type ReadOnce or
 * ReadNotSharedDirty not sourced by the hardware prefetcher, and drops requests whose translation
 * origin is a prefetcher, a TLB prefetch or the CME (origin_not_ldst), so hardware-prefetch fills and
 * prefetch-origin walks are out while the program's own page walks stay in: nevis0_l2_tagctl.sv
 * lines 10297 to 10304, nevis0_tlb_pagewalk.sv line 3267; Jesse 2026-11-04, codex R20) and 0x81C6 L3D_CACHE_HIT_RD (demand
 * reads counted by L3D_CACHE_RD that hit in the L3), C1-Nano TRM table 17-1. The columns are per
 * program load and are reported as independent counts. Derived ratios (l2d_hit_rd / l1d_refill as
 * the L2 demand hit fraction, l3d_hit_rd / (l1d_refill - l2d_hit_rd) as the L3 demand hit fraction)
 * are estimates that assume a load-only chase's L1 refills (0x03: refills by speculative loads or
 * stores) are its demand reads reaching the L2; the TRM states no such identity, so the plan checks
 * it on the 64 KiB row. l3d_hit_rd / l3d_access_rd is closer to a demand hit rate than the TRM
 * wording suggests, and is still reported as an estimate: the RTL population keeps the program's
 * page walks and software-prefetch (PLD) reads, and is not a count of the program's data reads. 0x2A
 * L3D_CACHE_REFILL is not in this set: the TRM lists it as not implemented (PMCEID1_EL0 ID10
 * resets to 0) and it read 0 on the emulator (Jesse, 2026-10-30); 0xA2 stays the L3 refill count. */
static inline void perf_open_l3(struct perfgrp *g)
{
    static const uint64_t raw[PERF_N] = { 0, 0x03, 0x17, 0xA2, 0x81C5, 0xA0, 0x81C6 };
    perf_open_set(g, raw, perf_name_l3);
}

static inline void perf_open_cme(struct perfgrp *g)
{
    static const uint64_t raw[PERF_N] = { 0, 0x3246, 0x327D, 0x327E, 0x3280, 0x32B2, 0x3281 };
    perf_open_set(g, raw, perf_name_cme);
}

/* The CME L3 set (--cme --events cme-l3, run 6): the two refill-source events plus 0x328E
 * CME_L3D_CACHE_HIT (L3 accesses by the CME that hit), 0x328F CME_L3D_CACHE_MISS (L3 demand
 * accesses by the CME that missed) and 0x3290 CME_L3D_CACHE_REFILL (L3 refills caused by the CME),
 * C1-SME2 TRM 107831 event table. The three are collected as exploratory counts. Two checks are
 * made and neither validates them: hit plus miss against inner plus outer on the same row compares
 * two totals whose populations the TRM defines differently (L3 accesses, translation-resolution
 * accesses included, against L1 line refills by source), so their agreement is a workload
 * hypothesis, and it says nothing about how the first total divides into hits and misses; a hit
 * count of 0 on a boot whose L3 holds no data is a necessary check that no run 6 boot provides.
 * Until an independent control validates the hit and miss interpretation and the populations, no
 * fit or attribution reads these columns (Insufficient data); the rows are reported as counts.
 * The read-refill event is not in this set (six counters). */
static inline void perf_open_cme_l3(struct perfgrp *g)
{
    static const uint64_t raw[PERF_N] = { 0, 0x3246, 0x327D, 0x327E, 0x328E, 0x328F, 0x3290 };
    perf_open_set(g, raw, perf_name_cme_l3);
}

/* Reads the group. Returns 1 when the counts are valid: the group is open,
 * every ioctl since perf_start succeeded, the read returned the whole record
 * and the group ran for the whole time it was enabled during this row (no
 * multiplexing). Otherwise returns 0 with the reason in *why; the caller
 * prints "-" columns. The enabled and running times the kernel returns are
 * lifetime totals (IOC_RESET clears the counts, not the times), so the row's
 * coverage is the change since the previous read; the baseline moves on every
 * complete read, valid or not, so one multiplexed row does not taint the next. */
static inline int perf_read(struct perfgrp *g, uint64_t out[PERF_N], const char **why)
{
    uint64_t v[3 + PERF_N]; /* nr, time_enabled, time_running, values */
    memset(out, 0, sizeof(uint64_t) * PERF_N);
    *why = NULL;
    if (!g->ok) { *why = "group not open"; return 0; }
    size_t want = sizeof(uint64_t) * (3 + (size_t)g->n);
    if (read(g->fd[0], v, want) != (ssize_t)want) { *why = "short read"; return 0; }
    if (v[0] != (uint64_t)g->n) { *why = "wrong event count"; return 0; }
    g->t_enabled = v[1] - g->prev_enabled;
    g->t_running = v[2] - g->prev_running;
    g->prev_enabled = v[1];
    g->prev_running = v[2];
    for (int i = 0; i < PERF_N; i++)
        out[i] = g->pos[i] >= 0 ? v[3 + g->pos[i]] : 0; /* a refused event reads as 0; print_counters prints "-" for it */
    if (g->ctl_err) { *why = "counter ioctl failed"; return 0; }
    if (g->t_running == 0 || g->t_running != g->t_enabled) { *why = "multiplexed"; return 0; } /* ran for part of the row */
    return 1;
}

static inline void perf_start(struct perfgrp *g)
{
    if (g->ok) {
        g->ctl_err = 0;
        if (ioctl(g->fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0) g->ctl_err = errno;
        if (ioctl(g->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) g->ctl_err = errno;
    }
}

static inline void perf_stop(struct perfgrp *g)
{
    if (g->ok && ioctl(g->fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0)
        g->ctl_err = errno;
}

/* Shared option parsing for --mem, --ocm-*, --spm-*, --no-hugetlb, --no-thp.
 * Returns the number of argv entries consumed (0 if not a memory option). */
static inline int parse_mem_opt(int argc, char **argv, int i, enum mem_kind *kind, int *regions, int *nregions)
{
    const char *a = argv[i];
    int more = i + 1 < argc;
    if (!strcmp(a, "--no-hugetlb")) { memcfg.no_hugetlb = 1; return 1; }
    if (!strcmp(a, "--no-thp")) { memcfg.no_hugetlb = 1; memcfg.no_thp = 1; return 1; }
    if (!more) return 0;
    if (!strcmp(a, "--mem")) { *kind = parse_mem(argv[i + 1]); return 2; }
    if (!strcmp(a, "--ocm-dev")) { memcfg.ocm_dev = argv[i + 1]; return 2; }
    if (!strcmp(a, "--ocm-base")) { memcfg.ocm_base = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--ocm-stride")) { memcfg.ocm_stride = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--ocm-size")) { memcfg.ocm_size = parse_bytes(argv[i + 1]); return 2; }
    if (!strcmp(a, "--spm-dev")) { memcfg.spm_dev = argv[i + 1]; return 2; }
    if (!strcmp(a, "--spm-off")) { memcfg.spm_off = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--ocm-regions")) {
        *nregions = 0;
        char *list = argv[i + 1];
        for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
            if (*nregions == MAX_SEGS) { fprintf(stderr, "too many regions\n"); exit(2); }
            regions[(*nregions)++] = atoi(tok);
        }
        return 2;
    }
    return 0;
}

/* Allocate, fill, and describe the buffer of `bytes` for the chosen kind.
 * OCM: bytes is split evenly over the listed regions. */
static inline void alloc_buf(struct buf *b, enum mem_kind kind, uint64_t bytes, const int *regions, int nregions)
{
    memset(b, 0, sizeof *b);
    switch (kind) {
    case MEM_DRAM: alloc_dram(b, bytes); break;
    case MEM_OCM:
        if (nregions == 0) { fprintf(stderr, "--mem ocm needs --ocm-regions\n"); exit(2); }
        if (bytes % (uint64_t)nregions) { fprintf(stderr, "bytes not divisible by %d regions\n", nregions); exit(2); }
        alloc_ocm(b, regions, nregions, bytes / (uint64_t)nregions);
        break;
    case MEM_SPM: alloc_spm(b, bytes); break;
    }
    fill_buf(b, 0x9E3779B97F4A7C15ULL);
}

static inline const char *mem_name(enum mem_kind k)
{
    return k == MEM_DRAM ? "dram" : k == MEM_OCM ? "ocm" : "spm";
}
/* ---- CME (the Arm C1-SME2 unit): streaming-mode kernels in cme_kern.S ---- */
#include <sys/auxv.h>
#ifndef HWCAP2_SME
#define HWCAP2_SME (1UL << 23)
#endif
#ifndef HWCAP2_SME2
#define HWCAP2_SME2 (1UL << 37)
#endif
#if defined(__aarch64__)
uint64_t cme_svl(void);
uint64_t cme_chase(uint64_t p, uint64_t hops);
uint64_t cme_roundtrip(uint64_t x, uint64_t n);
uint64_t cme_read4(const void *p, uint64_t bytes);
void cme_write4(void *p, uint64_t bytes, uint64_t v);
void cme_copy4(void *d, const void *s, uint64_t bytes);
uint64_t cme_read1(const void *p, uint64_t bytes);
void cme_write1(void *p, uint64_t bytes, uint64_t v);
uint64_t cme_read4_n(const void *p, uint64_t bytes, uint64_t passes);        /* the single-call forms: all passes in one span */
void cme_write4_n(void *p, uint64_t bytes, uint64_t v, uint64_t passes);
void cme_write4_dsb(void *p, uint64_t bytes, uint64_t v);                   /* cme_write4 with DSB ST before SMSTOP */
uint64_t cme_read4_nofold(const void *p, uint64_t bytes);                   /* cme_read4 without the end-of-call fold */
extern uint64_t cme_span[3];   /* CNTVCT_EL0 read inside the last call's span (after SMSTART, before SMSTOP) and the running sum of end - start (cme_kern.S) */
#else
/* Stand-ins so the harness builds on a non-Arm host; --cme refuses to run there. */
static inline uint64_t cme_svl(void) { return 0; }
static inline uint64_t cme_chase(uint64_t p, uint64_t hops) { (void)hops; return p; }
static inline uint64_t cme_roundtrip(uint64_t x, uint64_t n) { (void)n; return x; }
static inline uint64_t cme_read4(const void *p, uint64_t bytes) { (void)p; (void)bytes; return 0; }
static inline void cme_write4(void *p, uint64_t bytes, uint64_t v) { (void)p; (void)bytes; (void)v; }
static inline void cme_copy4(void *d, const void *s, uint64_t bytes) { (void)d; (void)s; (void)bytes; }
static inline uint64_t cme_read1(const void *p, uint64_t bytes) { (void)p; (void)bytes; return 0; }
static inline void cme_write1(void *p, uint64_t bytes, uint64_t v) { (void)p; (void)bytes; (void)v; }
static inline uint64_t cme_read4_n(const void *p, uint64_t bytes, uint64_t passes) { (void)p; (void)bytes; (void)passes; return 0; }
static inline void cme_write4_n(void *p, uint64_t bytes, uint64_t v, uint64_t passes) { (void)p; (void)bytes; (void)v; (void)passes; }
static inline void cme_write4_dsb(void *p, uint64_t bytes, uint64_t v) { (void)p; (void)bytes; (void)v; }
static inline uint64_t cme_read4_nofold(const void *p, uint64_t bytes) { (void)p; (void)bytes; return 0; }
static uint64_t cme_span[3];
#endif
/* The last call's in-span time in seconds (cme_span over the counter frequency); 0 on a host without the kernels.
 * cme_span_reset and cme_span_sum_s bracket a timed region from outside it: the kernels add each call's span to
 * cme_span[2] themselves, so no C span collection runs inside the caller's clock reads. */
static inline double cme_span_s(void)
{
    uint64_t f = read_cntfrq();
    return f ? (double)(cme_span[1] - cme_span[0]) / (double)f : 0.0;
}
static inline void cme_span_reset(void) { cme_span[2] = 0; }
static inline double cme_span_sum_s(void)
{
    uint64_t f = read_cntfrq();
    return f ? (double)cme_span[2] / (double)f : 0.0;
}

/* Exit unless this CPU has SME2 and the streaming vector length is 64 B (the
 * Asimov CME). Prints the reason: the row then says why it is missing. */
static inline void require_cme(void)
{
    unsigned long hw2 = getauxval(AT_HWCAP2);
    if (!(hw2 & HWCAP2_SME) || !(hw2 & HWCAP2_SME2)) {
        printf("# cme: SME2 not supported on this CPU (hwcap2 0x%lx)\n", hw2);
        exit(2);
    }
    uint64_t svl = cme_svl();
    if (svl != 64) {
        printf("# cme: streaming vector length is %llu B, the kernels assume 64\n", (unsigned long long)svl);
        exit(2);
    }
}
#endif
