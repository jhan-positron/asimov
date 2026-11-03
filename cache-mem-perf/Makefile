# Asimov build (Jesse's flags): make CC=aarch64-none-linux-gnu-gcc
# Host check build (x86): make CFLAGS="-O2 -Wall -Wextra -std=gnu11 -pthread"
CC ?= gcc
CFLAGS ?= -O2 -mcpu=cortex-a520 -static
CFLAGS += -Wall -Wextra -std=gnu11 -pthread -fno-tree-loop-distribute-patterns
# The CME kernels are AArch64 assembly (streaming SVE mode); a non-Arm host build uses the stand-ins in common.h.
ifneq ($(findstring aarch64,$(shell $(CC) -dumpmachine)),)
CME_KERN = cme_kern.S
endif

all: latency bandwidth cmncount
latency: latency.c common.h $(CME_KERN)
	$(CC) $(CFLAGS) -o $@ latency.c $(CME_KERN)
bandwidth: bandwidth.c common.h $(CME_KERN)
	$(CC) $(CFLAGS) -o $@ bandwidth.c $(CME_KERN)
cmncount: cmncount.c
	$(CC) $(CFLAGS) -o $@ cmncount.c
# self-test of the CME kernels; needs an SME2 machine (or QEMU user mode) to run
cme_test: cme_test.c common.h $(CME_KERN)
	$(CC) $(CFLAGS) -o $@ cme_test.c $(CME_KERN)
# host test of the per-row counter coverage; needs an Arm machine with a PMU
perf_cov_test: perf_cov_test.c common.h $(CME_KERN)
	$(CC) $(CFLAGS) -o $@ perf_cov_test.c $(CME_KERN)
clean:
	rm -f latency bandwidth cmncount cme_test perf_cov_test
.PHONY: all clean
