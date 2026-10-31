# Asimov cache latency and bandwidth tests

Short version: two plain-C programs and one shell script measure, on one
hemisphere of Asimov under asimov-os, the load-to-use latency and the streaming
bandwidth of L1, L2, L3, the mesh cache (SLC), the CMN OCM regions, the
scratchpad (SPM) and DRAM. Build with your usual flags, run `sh run_asimov.sh`
as root, and send back the whole results directory. Chip time is about 2 s
(est.), so about 1.5 h of wall time at 46 min per chip second (est.).

## Words used here

- L1, L2, L3: per-core 32 KiB data cache; per-DSU 128 KiB and 256 KiB caches
  shared by the two cores of a DSU (DynamIQ Shared Unit, a 2-core cluster).
- SLC: System Level Cache, 5 MiB inside the CMN mesh, 1 MiB per home node.
- OCM: On-Chip Memory, 4 locked SLC ways per home node: 5 regions of 256 KiB at
  physical 0xE0000000 + n * 0x100000, reached through `/dev/ocm`.
- SPM: the 16 MB scratchpad memory, reached through `/dev/asimov-spm`,
  mapped non-cacheable, aligned 8/16/32-byte accesses only.
- Latency: nanoseconds from issuing a load to using its result, measured with a
  pointer chase (each load's address comes from the previous load).
- Bandwidth: bytes moved per second by a streaming loop of 16-byte vector loads
  or stores. GB means 1e9 bytes.
- Fresh chain: a random cycle through the buffer that has never been walked
  before the timed pass, so a prefetcher that learned an earlier walk cannot
  help.
- Flushed: every line of the buffer is cleaned and invalidated from L1, L2 and
  L3 (`DC CIVAC` + `DSB SY`) before the timed pass, so each load goes to the
  home node (OCM or SLC) or to DRAM.

## Files

| File | What it is |
|---|---|
| `common.h` | buffers in DRAM (MAP_HUGETLB, else THP), OCM regions, SPM; flush; perf counters |
| `latency.c` | single-thread pointer chase; `--fresh`, `--flush`, `--perf` |
| `bandwidth.c` | pinned threads, read / write / copy with `ldp`/`stp` q-register pairs; `ldr16`/`str16` for SPM |
| `run_asimov.sh` | the whole plan, phases 0 and A to E; writes `results/<UTC time>/` |
| `Makefile` | `make CC=<your aarch64 gcc>`; default flags `-O2 -mcpu=cortex-a520 -static` |

## How to build and run

```
make CC=aarch64-none-linux-gnu-gcc        # or your SDK compiler; flags are yours to change
sh run_asimov.sh                          # as root; CPUs 0-1 kernel, 2-9 online and isolated
```

The script takes CPU 3 offline for the single-core phases and brings it back for
the multi-core phase, by writing `/sys/devices/system/cpu/cpu3/online`. If that
file is not writable it says so and continues with CPU 3 online but idle.

Before the run, the script records `HugePages_Free`. The DRAM buffers use
`MAP_HUGETLB` when pages are reserved (256 pages of 2 MiB cover every phase),
and fall back to `madvise(MADV_HUGEPAGE)` otherwise. Each result file states
which one it got (`backing=hugetlb|thp|4k`).

`sh run_asimov.sh --quick` is a small DRAM-only self-test for any Linux host.
It is not an Asimov measurement.

## What to send back

The whole `results/<UTC time>/` directory. It holds `sysinfo.txt`, one CSV per
test, `all.txt` with every command and its output in order, and `log.txt`.
Total size is a few tens of kilobytes.

## What each phase measures

| Phase | Test | Footprints | Expected to show |
|---|---|---|---|
| 0 | sysinfo | | cache topology from sysfs, kernel config, timer frequency, memory map |
| A | DRAM latency, CPU 2, repeated walk + perf counters | 4 KiB to 384 KiB | L1 (up to 32 KiB), L2 (to 128 KiB), L3 (to 384 KiB) latency steps; refills per load say which level served the loads |
| A | DRAM latency, fresh chains | 512 KiB to 32 MiB | SLC step (to about 3.75 MiB), then DRAM |
| A | DRAM latency, flushed single pass | 256 KiB to 8 MiB | latency when every load leaves the core caches |
| B | OCM latency, flushed, one region at a time | 256 KiB | per-region latency; region 1 (one hop from DSU 1) expected fastest |
| B | OCM latency, all five regions | 1.25 MiB | OCM latency across home nodes, flushed and fresh |
| D | one-core bandwidth read / write / copy | 16 KiB, 48 KiB, 96 KiB, 256 KiB, 2 MiB, 32 MiB | L1, L2, L3, SLC and DRAM bandwidth of one core |
| D | OCM bandwidth | 256 KiB per region, 1.25 MiB all | read from OCM (flushed before), write + write-back, copy region 1 to 2 |
| E | shared-L2 pair, CPUs 2 and 3 | 16 KiB, 48 KiB, 160 KiB each | whether two cores sharing L2 and L3 halve each other's bandwidth |
| E | one core per DSU (2, 4, 6, 8) and all eight (2 to 9) | 2 MiB and 16 MiB per thread | aggregate SLC and DRAM bandwidth of the hemisphere |
| E | OCM from four cores | one region per core | aggregate OCM read and write |
| C (runs last) | SPM latency and bandwidth | 64 KiB to 1 MiB | non-cacheable scratchpad latency; 16-byte load and store bandwidth |

## Output format

`latency` prints one CSV row per timed walk:

```
mem,size_bytes,mode,rep,loads,ns_per_load,cycles_per_load,l1d_refill_per_load,l2d_refill_per_load,l3d_refill_rd_per_load
dram,131072,repeat,0,65536,3.75,10.5,1.0000,0.0100,0.0001
```

`ns_per_load` is the latency. The refill columns come from the Arm PMU events
0x03 (L1D refill), 0x17 (L2D refill) and 0xA2 (L3D refill on reads) around the
timed walk, divided by the number of loads. 0xA2 is used instead of 0x2A because
the C1-Nano opens 0x2A without error but it always reads 0 (Jesse, 2026-10-30).
A level with about 1.0 refills per load is the one that missed. A `best` row
per size carries the minimum.

`bandwidth` prints one row per rep:

```
mem,kernel,threads,bytes_per_thread,rep,total_bytes,elapsed_s,aggregate_GBps,per_thread_min_GBps,per_thread_max_GBps
```

`aggregate_GBps` is total bytes over the slowest thread's time. Copy counts
both the bytes read and the bytes written.

## Assumptions to check on the first boot

- `/dev/ocm` accepts `mmap(NULL, 0x40000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, <region physical base>)`
  and maps the region Normal write-back. Region n is at 0xE0000000 + n * 0x100000.
  Override with `--ocm-dev`, `--ocm-base`, `--ocm-stride`, `--ocm-size` if it differs.
- `/dev/asimov-spm` accepts `mmap` with the file offset as the byte offset into
  the scratchpad, starting at 0. Override with `--spm-dev`, `--spm-off`.
- The pointer chase writes every byte of a buffer with aligned 8-byte stores
  before reading it (OCM ECC rule), and never calls memset or memcpy on OCM or SPM.
- `DC CIVAC` from user space is allowed and keeps OCM data. Every timed OCM pass,
  repeats included, starts with a flush of the whole buffer, and every region gets
  an untimed write and walk first, so 4 KiB page walks stay out of the timed passes.
- SPM: the whole range is written (aligned 8-byte volatile stores) before any timed
  pass, so the driver's per-page first-touch mapping is done. The mapping is
  write-combining, so `str16` bandwidth measures merged bursts, not single stores.
  The SPM phase runs last, because a wrong-size access would revoke the mapping and
  later passes would get SIGBUS.
- The PMU raw events 0x03, 0x17, 0xA2 count on the C1-Nano (0xA2 is wired in the PMU
  RTL but missing from the kernel's named list, per Jesse). If an event cannot be
  opened the program prints `# perf: ... unavailable` and continues without counters.
- `clock_gettime(CLOCK_MONOTONIC)` gives chip time at 1 ns resolution.
  Each program also prints `cntfrq_hz` from `CNTFRQ_EL0`.

## Chip-time estimate (est.)

The DRAM fresh-chain sweep dominates: building and walking the 16 MiB and
32 MiB cycles is about 1.2 s of chip time at an assumed 120 ns per DRAM load
(est.). Everything else together is under 0.5 s (est.). If a run must be
shorter, drop `32M` from the `latency-dram-large` line in `run_asimov.sh`.
