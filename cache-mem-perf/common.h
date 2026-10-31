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
} memcfg = { "/dev/ocm", 0xE0000000ULL, 0x100000ULL, 0x40000ULL, "/dev/asimov-spm", 0, 0 };

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
 * a 2 MiB-aligned anonymous mapping. Records which one was obtained. */
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

/* ---- perf counters: cycles + L1D, L2D, L3D refills, one group ---- */
#define PERF_N 4
struct perfgrp {
    int fd[PERF_N];
    int ok;
};
static const char *perf_name[PERF_N] = { "cycles", "l1d_refill", "l2d_refill", "l3d_refill_rd" };

static inline int perf_open_one(struct perf_event_attr *a, int group)
{
    a->size = sizeof *a;
    a->disabled = group < 0;
    a->exclude_kernel = 1;
    a->exclude_hv = 1;
    a->read_format = PERF_FORMAT_GROUP;
    return (int)syscall(__NR_perf_event_open, a, 0, -1, group, 0);
}

static inline void perf_open(struct perfgrp *g)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_CPU_CYCLES;
    g->ok = 0;
    g->fd[0] = perf_open_one(&a, -1);
    if (g->fd[0] < 0) {
        printf("# perf: unavailable (%s)\n", strerror(errno));
        return;
    }
    /* Arm PMU events: 0x03 L1D_CACHE_REFILL, 0x17 L2D_CACHE_REFILL, 0xA2 L3D_CACHE_REFILL_RD.
     * Not 0x2A: the C1-Nano opens it without error but it always reads 0 (Jesse, 2026-10-30). */
    static const uint64_t raw[PERF_N] = { 0, 0x03, 0x17, 0xA2 };
    for (int i = 1; i < PERF_N; i++) {
        memset(&a, 0, sizeof a);
        a.type = PERF_TYPE_RAW;
        a.config = raw[i];
        g->fd[i] = perf_open_one(&a, g->fd[0]);
        if (g->fd[i] < 0) {
            printf("# perf: event %s (raw 0x%llx) unavailable (%s)\n", perf_name[i],
                   (unsigned long long)raw[i], strerror(errno));
            return;
        }
    }
    g->ok = 1;
}

static inline void perf_read(const struct perfgrp *g, uint64_t out[PERF_N])
{
    uint64_t v[1 + PERF_N];
    memset(out, 0, sizeof(uint64_t) * PERF_N);
    if (!g->ok)
        return;
    if (read(g->fd[0], v, sizeof v) != (ssize_t)sizeof v) {
        perror("perf read");
        return;
    }
    memcpy(out, v + 1, sizeof(uint64_t) * PERF_N);
}

static inline void perf_start(const struct perfgrp *g)
{
    if (g->ok) {
        ioctl(g->fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(g->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
}

static inline void perf_stop(const struct perfgrp *g)
{
    if (g->ok)
        ioctl(g->fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

/* Shared option parsing for --mem, --ocm-*, --spm-*, --no-hugetlb.
 * Returns the number of argv entries consumed (0 if not a memory option). */
static inline int parse_mem_opt(int argc, char **argv, int i, enum mem_kind *kind, int *regions, int *nregions)
{
    const char *a = argv[i];
    int more = i + 1 < argc;
    if (!strcmp(a, "--no-hugetlb")) { memcfg.no_hugetlb = 1; return 1; }
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
#endif
