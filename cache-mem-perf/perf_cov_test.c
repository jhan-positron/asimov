/*
 * perf_cov_test.c - host test of the per-row counter coverage in common.h.
 *
 * Opens the core counter group, then crowds the same task with more event
 * groups than the PMU has counters, so the kernel multiplexes them. A timed
 * walk under that load must come back invalid ("multiplexed", running below
 * enabled for the row). The extra groups are then closed and a second walk
 * must come back valid, which fails if the coverage were cumulative over the
 * group's lifetime. Needs an Arm machine with a PMU and root or
 * perf_event_paranoid <= 2. Exit 0 when both rows behave, 1 on a failure, 2 when the
 * machine cannot run the test (no group, or too few extra groups to force multiplexing).
 */
#include "common.h"

static volatile uint64_t sink;

static void work(void)
{
    uint64_t x = 1;
    for (uint64_t i = 0; i < 20000000; i++) /* about 10 ms */
        x = x * 6364136223846793005ULL + i;
    sink = x;
}

int main(void)
{
    struct perfgrp g = { 0 };
    uint64_t c[PERF_N];
    const char *why;
    int fails = 0;
    perf_open(&g);
    if (!g.ok) { printf("SKIP: core counter group not available\n"); return 2; }

    /* crowd: 12 more groups of the same four events, task-bound, enabled */
    struct perfgrp extra[12];
    memset(extra, 0, sizeof extra);
    int nextra = 0;
    for (int i = 0; i < 12; i++) {
        perf_open(&extra[i]);
        if (!extra[i].ok) break;
        perf_start(&extra[i]);
        nextra++;
    }
    if (nextra < 6) { printf("SKIP: only %d extra groups opened, not enough to force multiplexing\n", nextra); return 2; }
    perf_start(&g);
    work();
    perf_stop(&g);
    int valid = perf_read(&g, c, &why);
    printf("crowded row (%d extra groups): valid=%d why=%s enabled=%llu running=%llu\n", nextra, valid, why ? why : "-",
           (unsigned long long)g.t_enabled, (unsigned long long)g.t_running);
    if (valid || !why || strcmp(why, "multiplexed") != 0 || !(g.t_running < g.t_enabled)) {
        fails++;
        printf("FAIL: the crowded row must be rejected as multiplexed with running below enabled\n");
    }

    for (int i = 0; i < nextra; i++) { perf_stop(&extra[i]); for (int k = 0; k < PERF_N; k++) close(extra[i].fd[k]); }

    perf_start(&g);
    work();
    perf_stop(&g);
    valid = perf_read(&g, c, &why);
    printf("quiet row: valid=%d why=%s enabled=%llu running=%llu cycles=%llu\n", valid, why ? why : "-",
           (unsigned long long)g.t_enabled, (unsigned long long)g.t_running, (unsigned long long)c[0]);
    if (!valid) { fails++; printf("FAIL: the quiet row after a crowded one was rejected\n"); }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
