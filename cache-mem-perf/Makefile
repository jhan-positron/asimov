# Asimov build (Jesse's flags): make CC=aarch64-none-linux-gnu-gcc
# Host check build (x86): make CFLAGS="-O2 -Wall -Wextra -std=gnu11 -pthread"
CC ?= gcc
CFLAGS ?= -O2 -mcpu=cortex-a520 -static
CFLAGS += -Wall -Wextra -std=gnu11 -pthread -fno-tree-loop-distribute-patterns

all: latency bandwidth cmncount
latency: latency.c common.h
	$(CC) $(CFLAGS) -o $@ latency.c
bandwidth: bandwidth.c common.h
	$(CC) $(CFLAGS) -o $@ bandwidth.c
cmncount: cmncount.c
	$(CC) $(CFLAGS) -o $@ cmncount.c
clean:
	rm -f latency bandwidth cmncount
.PHONY: all clean
