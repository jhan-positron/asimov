/*
 * cme_test.c - self-test of the streaming-mode kernels in cme_kern.S.
 *
 * Runs on any AArch64 Linux with SME2 and SVL 64 B (the Asimov emulator, or
 * QEMU user mode with -cpu max,sme512=on). Each kernel's result is compared
 * with a scalar reference, so a wrong predicate, offset or register would show
 * before a run spends chip time. Exit 0 when every check passes.
 */
#include "common.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t fold_ref(const uint8_t *p, uint64_t bytes)
{
    const uint64_t *q = (const uint64_t *)p;
    uint64_t f = 0;
    for (uint64_t i = 0; i < bytes / 8; i++)
        f ^= q[i];
    return f;
}

/* --sme1: check only the kernels that need SME, not SME2 (chase, round trip,
 * single-vector read and write), on a machine or emulator without SME2. */
int main(int argc, char **argv)
{
    int sme1 = argc > 1 && !strcmp(argv[1], "--sme1");
    if (sme1) {
        unsigned long hw2 = getauxval(AT_HWCAP2);
        if (!(hw2 & HWCAP2_SME) || cme_svl() != 64) {
            printf("# cme: SME with SVL 64 B not available (hwcap2 0x%lx, svl %llu)\n", hw2, (unsigned long long)cme_svl());
            return 2;
        }
    } else
        require_cme();
    printf("svl=%llu hwcap2=0x%lx sme1_only=%d\n", (unsigned long long)cme_svl(), getauxval(AT_HWCAP2), sme1);

    enum { N = 64 * 1024 };
    uint8_t *a = aligned_alloc(4096, N), *b = aligned_alloc(4096, N);
    if (!a || !b) { perror("alloc"); return 2; }
    for (uint64_t i = 0; i < N / 8; i++)
        ((uint64_t *)a)[i] = 0x9E3779B97F4A7C15ULL * (i + 1);

    /* round trip: the value passes through Z and back unchanged */
    CHECK(cme_roundtrip(0x1234567890ABCDEFULL, 1) == 0x1234567890ABCDEFULL, "roundtrip 1 hop");
    CHECK(cme_roundtrip(7, 1000) == 7, "roundtrip 1000 hops");

    /* chase: a ring of 64 B lines, pointer in the first 8 B, walked exactly once and once more */
    const uint64_t lines = N / 64;
    for (uint64_t i = 0; i < lines; i++)
        *(uint64_t *)(a + i * 64) = (uint64_t)(uintptr_t)(a + ((i * 7 + 3) % lines) * 64);
    uint64_t p = (uint64_t)(uintptr_t)a, q = p;
    for (int i = 0; i < 100; i++) q = *(uint64_t *)(uintptr_t)q;
    CHECK(cme_chase(p, 100) == q, "chase 100 hops");
    uint64_t r = cme_chase(p, 1);
    CHECK(r == *(uint64_t *)(uintptr_t)p, "chase 1 hop");

    /* chase with the pointer in word 2 of each line, as a --fresh chain stores it */
    for (uint64_t i = 0; i < lines; i++)
        *(uint64_t *)(a + i * 64 + 16) = (uint64_t)(uintptr_t)(a + ((i * 5 + 1) % lines) * 64 + 16);
    p = (uint64_t)(uintptr_t)(a + 16); q = p;
    for (int i = 0; i < 100; i++) q = *(uint64_t *)(uintptr_t)q;
    CHECK(cme_chase(p, 100) == q, "chase 100 hops, word 2");

    /* reads: the fold equals the scalar XOR of every 8-byte word */
    for (uint64_t i = 0; i < N / 8; i++)
        ((uint64_t *)a)[i] = 0xC2B2AE3D27D4EB4FULL * (i + 11);
    if (!sme1) CHECK(cme_read4(a, N) == fold_ref(a, N), "read4 fold over 64 KiB");
    if (!sme1) CHECK(cme_read4(a, 1024) == fold_ref(a, 1024), "read4 fold over 1 KiB");
    CHECK(cme_read1(a, N) == fold_ref(a, N), "read1 fold over 64 KiB");
    CHECK(cme_read1(a, 1024) == fold_ref(a, 1024), "read1 fold over 1 KiB");

    /* writes: every 8-byte word becomes v, and nothing past the end changes */
    int ok = 1;
    if (!sme1) {
        memset(b, 0xEE, N);
        cme_write4(b, N - 1024, 0x0123456789ABCDEFULL);
        for (uint64_t i = 0; i < (N - 1024) / 8; i++) ok &= ((uint64_t *)b)[i] == 0x0123456789ABCDEFULL;
        for (uint64_t i = (N - 1024) / 8; i < N / 8; i++) ok &= ((uint64_t *)b)[i] == 0xEEEEEEEEEEEEEEEEULL;
        CHECK(ok, "write4 pattern and end guard");
    }
    memset(b, 0xEE, N);
    cme_write1(b, N - 1024, 0xFEDCBA9876543210ULL);
    ok = 1;
    for (uint64_t i = 0; i < (N - 1024) / 8; i++) ok &= ((uint64_t *)b)[i] == 0xFEDCBA9876543210ULL;
    for (uint64_t i = (N - 1024) / 8; i < N / 8; i++) ok &= ((uint64_t *)b)[i] == 0xEEEEEEEEEEEEEEEEULL;
    CHECK(ok, "write1 pattern and end guard");

    /* copy: b equals a over the copied span, untouched past it */
    if (!sme1) {
        memset(b, 0xEE, N);
        cme_copy4(b, a, N - 2048);
        CHECK(memcmp(a, b, N - 2048) == 0, "copy4 data");
        ok = 1;
        for (uint64_t i = (N - 2048) / 8; i < N / 8; i++) ok &= ((uint64_t *)b)[i] == 0xEEEEEEEEEEEEEEEEULL;
        CHECK(ok, "copy4 end guard");
    }

    /* run 6: the single-call forms. An odd number of read passes folds to the
     * scalar XOR, an even number to zero (each word enters the fold once per pass). */
    if (!sme1) {
        CHECK(cme_read4_n(a, N, 3) == fold_ref(a, N), "read4_n fold, 3 passes over 64 KiB");
        CHECK(cme_read4_n(a, 1024, 2) == 0, "read4_n fold, 2 passes over 1 KiB");
        CHECK(cme_read4_n(a, N, 1) == cme_read4(a, N), "read4_n 1 pass equals read4");
        CHECK(cme_read4_nofold(a, N) == 0 && cme_read4(a, N) == fold_ref(a, N), "read4_nofold returns 0 and leaves read4's fold intact");
        memset(b, 0xEE, N);
        cme_write4_n(b, N - 1024, 0x5A5A5A5A5A5A5A5AULL, 3);
        ok = 1;
        for (uint64_t i = 0; i < (N - 1024) / 8; i++) ok &= ((uint64_t *)b)[i] == 0x5A5A5A5A5A5A5A5AULL;
        for (uint64_t i = (N - 1024) / 8; i < N / 8; i++) ok &= ((uint64_t *)b)[i] == 0xEEEEEEEEEEEEEEEEULL;
        CHECK(ok, "write4_n pattern and end guard, 3 passes");
        memset(b, 0xEE, N);
        cme_write4_dsb(b, N - 1024, 0xA5A5A5A5A5A5A5A5ULL);
        ok = 1;
        for (uint64_t i = 0; i < (N - 1024) / 8; i++) ok &= ((uint64_t *)b)[i] == 0xA5A5A5A5A5A5A5A5ULL;
        for (uint64_t i = (N - 1024) / 8; i < N / 8; i++) ok &= ((uint64_t *)b)[i] == 0xEEEEEEEEEEEEEEEEULL;
        CHECK(ok, "write4_dsb pattern and end guard");
    }

    /* run 6: the in-span stamps. Every call leaves cme_span[1] at or after
     * cme_span[0], a longer chain takes a longer span, and the span is shorter
     * than the whole call as the caller's clock sees it. */
    uint64_t s0 = cme_span[0], s1 = cme_span[1];
    CHECK(s1 >= s0, "span stamps ordered after the last call");
    double tb0 = now_s();
    (void)cme_roundtrip(1, 1 << 16);
    double tb1 = now_s();
    uint64_t short_span, long_span = cme_span[1] - cme_span[0];
    CHECK(cme_span[1] >= cme_span[0] && cme_span[0] != s0, "span stamps rewritten by the call");
    CHECK(cme_span_s() <= tb1 - tb0, "span inside the caller's bracket");
    (void)cme_roundtrip(1, 1);
    short_span = cme_span[1] - cme_span[0];
    CHECK(long_span > short_span, "65536-hop span longer than a 1-hop span");
    CHECK(read_cntfrq() > 0, "counter frequency readable");
    cme_span_reset();
    uint64_t sum = 0;
    for (int i = 0; i < 5; i++) { (void)cme_roundtrip(1, 1); sum += cme_span[1] - cme_span[0]; }
    CHECK(cme_span[2] == sum && sum > 0, "running span sum equals the sum of the five calls' spans");

    /* the kernels leave the caller's state alone: a second pass gives the same fold */
    CHECK(cme_read1(a, N) == fold_ref(a, N), "read1 fold, second pass");

    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    free(a); free(b);
    return fails ? 1 : 0;
}
