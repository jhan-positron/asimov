# Asimov build (Jesse's flags): make CC=aarch64-none-linux-gnu-gcc
# Host check build: make
CC ?= gcc
CFLAGS ?= -O2 -mcpu=cortex-a520 -static
CFLAGS += -Wall -Wextra -std=gnu11 -pthread -fno-tree-loop-distribute-patterns

all: latency bandwidth
latency: latency.c common.h
	$(CC) $(CFLAGS) -o $@ latency.c
bandwidth: bandwidth.c common.h
	$(CC) $(CFLAGS) -o $@ bandwidth.c
clean:
	rm -f latency bandwidth
.PHONY: all clean
