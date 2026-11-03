# Asimov cache latency and bandwidth tests (run 2)

Short version: three plain-C programs and one shell script measure, on one
hemisphere of Asimov under asimov-os, the load-to-use latency and the streaming
bandwidth of L1, L2, L3, the mesh cache (SLC), the CMN OCM regions and DRAM, and
count the requests each mesh home node receives. Build with your usual flags,
run `sh run_asimov.sh` as root, and send back the whole results directory. Chip
time is about 4 s (est., `est_chip_time.py`; half of it is an allowance of
14 ms per program launch), so about 3 h of wall time at 2,779 s per chip
second (run 1).

Run 2 differs from run 1 (commit 9c6b8e7) in these places, all from the run 1
results:

- Every flushed latency row walks a chain that was never walked before.
- The flushed DRAM rows shrink to one DRAM reference row.
- The OCM write row is a streaming write over all five regions instead of a
  store-then-flush. The OCM copy row and the 4-core OCM write row are dropped.
  All three timed the DC CIVAC instruction (run 1 report, section 5a).
- The OCM latency rows run from one core per DSU, and every OCM row runs under
  the mesh counters, with one control row per core for the background counts.
- The SLC-regime fresh chase (2 MiB, DRAM-backed) runs from CPUs 4, 6 and 8 as
  well as CPU 2.
- Writer placement rows with two and four writers.
- The scratchpad phase stays in the script but is skipped while
  `/dev/asimov-spm` is absent.

## Words used here

- C1-Nano: the Arm core type of the ten CPUs of one hemisphere.
- DSU: DynamIQ Shared Unit, a 2-core cluster. CPUs 2 and 3 form one DSU, and
  so do 4 and 5, 6 and 7, 8 and 9. CPUs 0 and 1 run the kernel.
- L1, L2, L3: per-core 32 KiB data cache, per-DSU 128 KiB and 256 KiB caches
  shared by the two cores of a DSU.
- CMN: Arm Coherent Mesh Network, the on-chip interconnect.
- SLC: System Level Cache, 5 MiB inside the CMN mesh, 1 MiB per home node.
- Home node (HN-S): the mesh node that owns an address. OCM0 to OCM4 are homed
  at the nodes with ids 88, 160, 216, 288 and 344 (the address map's HNS_CTRL,
  HNS_X2 to HNS_X5). CPU 2's DSU port is one mesh hop from the homes of OCM0 to
  OCM2 and three hops from the homes of OCM3 and OCM4 (Jesse's run 1 notes).
- OCM: On-Chip Memory, 4 SLC ways per home node locked by the SCP firmware in
  CMN OCM mode (`program_ocm` in the SCP's `asimov_physical_sam.c` sets the lock
  ways, the lock bases and the OCM enable bit). 5 regions of 256 KiB at physical
  0xE0000000 + n * 0x100000, reached through `/dev/ocm` in 4 KiB pages. The
  address map puts no memory behind OCM. A DC CIVAC on an OCM line keeps the
  data (run 1: every flushed OCM row read 18.8 to 26.0 ns, not the 52 ns of
  DRAM).
- SPM: the scratchpad memory (16 MB, Jesse's figure), reached through
  `/dev/asimov-spm`. Postponed by Jesse on 2026-10-31 because its driver cannot
  load on this emulator image.
- CME: Compute Matrix Engine, Positron's name for the Arm C1-SME2 unit (Arm
  code name Griffin). One per DSU, shared by its two cores. It executes SME
  and SME2 instructions and, in streaming mode, every SVE load and store too,
  through its own 64 KiB L1 data cache, then the DSU L3, then the mesh. The
  core's L2 is not on that path.
- Streaming mode: the CPU state (`SMSTART SM`) in which SVE instructions run on
  the CME at the streaming vector length instead of on the core. Entering or
  leaving it zeroes the vector registers. A system call inside it clears the
  streaming state, so a timed span contains none; a signal is survived (the
  state is restored on return) but its handler time lands in the elapsed time.
- SVL: streaming vector length, 64 B on Asimov (the core's own SVE length is
  16 B). A multi-vector `LD1B` or `ST1B` moves four vectors, 256 B.
- CME counters: three C1-SME2 events read through the core PMU (asimov-bench
  reads them the same way): `CME_CYCLES` 0x3246 (clock cycles of the CME on
  its own clock), `CME_L1D_CACHE_REFILL_INNER` 0x327D (CME L1 refills whose
  line came from a cache inside the cluster: the DSU L3, or the partner core's
  L2) and `CME_L1D_CACHE_REFILL_OUTER` 0x327E (refills whose line came from
  outside the cluster: SLC, OCM or DRAM). They count refills by source, demand
  and prefetch alike; a load that hit the CME L1 counts in neither. Event
  numbers and names from the Arm C1-SME2 TRM 107831 chapter 12.
- Round-trip control: a chain of `DUP` (general register to Z register) then
  `LASTB` (back) hops in streaming mode, with no memory access. It shares the
  register return path with a CME chase hop but is not a measured part of one;
  "chase minus control" is a derived number, not the memory latency.
- PMU: performance monitoring unit, a block of hardware counters. The core PMU
  gives the refill counts; the mesh PMU `arm_cmn_0` gives the home-node counts.
- THP: transparent huge pages, 2 MiB pages the kernel assigns on `madvise`.
- Latency: nanoseconds from issuing a load to using its result, measured with a
  pointer chase (each load's address comes from the previous load).
- Bandwidth: bytes moved per second by a streaming loop of 16-byte vector loads
  or stores. GB means 1e9 bytes.
- Fresh chain: a random cycle through the buffer that has never been walked
  before the timed pass, so a prefetcher that learned an earlier walk cannot
  help. In run 1 a flushed row that re-walked its warm-up chain had 9 to 23 % of
  its lines prefetched. A fresh chain removes that.
- Sparse chain (`--sparse`): a chain whose members are one line per 4 KiB page,
  at a line offset that differs per chain (line 8c + 4 of every page for chain
  c). No member has a member as a neighbour and a pass visits each page once,
  so a line the prefetcher brings in beside a member is never used by the
  chain. It is an experimental mitigation: run 2's dense 256 KiB per-region
  rows counted 1.1143 to 1.4468 L2 refills and 0.7627 to 0.9500 L3 refills per
  load over their 40 passes (the reading, a hypothesis: prefetched neighbours
  found later by the chain) while the 1.25 MiB and 8 MiB rows counted 1.00 of
  each. The counters cannot show that no member was prefetched, so the sparse
  rows are read beside their dense controls. A sparse pass has bytes / 4096
  hops, so `--fresh 8` gives seven timed passes, each reported as the
  complete-call time divided by its hop count; the `lines=` value in the size
  line is then the members per chain. A 64-hop pass has a timing step of
  1/64 ns per 1 ns of clock step and a counter step of 1/64 per event.
- Flushed: every line of the buffer is cleaned and invalidated from L1, L2 and
  L3 (`DC CIVAC` + `DSB SY`) before the timed pass. For a DRAM-backed buffer this
  also removes the lines from the SLC (run 1: flushed rows read the DRAM
  latency), so a flushed DRAM row stands for DRAM, not for the SLC. For OCM the
  data stays, so a flushed OCM row reads the home node.
- Mesh counters: the CMN performance counters, read by `cmncount` around one
  command. `hns_pocq_reqs_recvd_all` per home node is the number of requests
  that node received. `hns_mc_reqs_local_all` is the number of requests all home
  nodes sent on to memory. `hns_cache_miss_all` is the SLC miss count over all
  nodes.

## Files

| File | What it is |
|---|---|
| `common.h` | buffers in DRAM (MAP_HUGETLB, else THP, or 4 KiB pages with `--no-thp`), OCM regions, SPM; flush; core perf counters |
| `latency.c` | single-thread pointer chase; `--fresh`, `--flush`, `--perf` |
| `bandwidth.c` | pinned threads, read / write / copy with `ldp`/`stp` q-register pairs; `ldr16`/`str16` for SPM |
| `cmncount.c` | runs one command with mesh PMU events open (system-wide, on the PMU's CPU), appends one CSV line per event and one with the command's lifetime |
| `cme_kern.S` | the chase and the read / write / copy loops again as streaming-mode SVE (`LD1D` + `LASTB`; `LD1B` / `ST1B` of four vectors and of one), used by `latency --cme` and `bandwidth --cme`; plus the register round trip and `RDSVL` |
| `cme_test.c` | self-test of those kernels on any SME2 machine: chase, round trip, read fold, write pattern, copy; `make cme_test && ./cme_test` |
| `perf_cov_test.c` | host test that a multiplexed counter row is rejected and the next row is not; `make perf_cov_test && taskset -c 2 ./perf_cov_test` on an Arm machine |
| `est_chip_time.py` | sizes a run from `sh run_asimov.sh --dry-run`: exact counts per command, labelled time assumptions |
| `run_all.sh` | **the script to run**: the cache and mesh test, then the CME test, into one `results/<UTC time>/` directory |
| `run_asimov.sh` | the cache and mesh test: phases 0, A, B, D, E and C (last, skipped without `/dev/asimov-spm`) |
| `run_cme.sh` | the CME test: phase F (skipped when `/proc/cpuinfo` has no `sme2`); takes CPU 3 offline for its rows and brings it back |
| `lib_run.sh` | shared by both scripts: options, output directory, mesh counter settings, CPU sets, the `run` / `cmn` helpers |
| `Makefile` | `make CC=<your aarch64 gcc>`; default flags `-O2 -mcpu=cortex-a520 -static`; links `cme_kern.S` when the compiler targets AArch64 |

## How to build and run

```
make CC=aarch64-none-linux-gnu-gcc        # or your SDK compiler; flags are yours to change
sh run_all.sh                             # as root; CPUs 0-1 kernel, 2-9 online and isolated
```

`run_all.sh` runs `run_asimov.sh` (the cache and mesh test) and then `run_cme.sh` (the CME
test) into one results directory; each can also run alone with the same options. A
measurement command that fails is logged with its exit status (`(exit N)` in `log.txt`), the
other rows still run, and the suite exits 1 at its end after restoring the CPU state;
`run_all.sh` prints both suites' exit codes and exits 0 only when both are 0.

The script takes CPU 3 offline for the single-core phases and brings it back for
the multi-core phase, by writing `/sys/devices/system/cpu/cpu3/online`. If that
file is not writable it says so and continues with CPU 3 online but idle. CPUs
5, 7 and 9 stay online and idle while their partners 4, 6 and 8 run the OCM
rows (Jesse: an idle isolated core does not touch the caches).

Before the run, the script records `HugePages_Free`. The DRAM buffers use
`MAP_HUGETLB` when pages are reserved (256 pages of 2 MiB cover every phase; the
largest single command, the 8-thread 16 MiB row, maps 128 pages), and fall back
to `madvise(MADV_HUGEPAGE)` otherwise. Each result file states which one it got
(`backing=hugetlb|thp|4k`).

The sysinfo step mounts debugfs if it is not mounted and prints
`/sys/kernel/debug/arm-cmn/map`, the mesh map (crosspoint grid with every
node). It needs `CONFIG_DEBUG_FS`. Without it the step prints an error line and
the run continues.

`sh run_all.sh --dry-run | python3 est_chip_time.py` prints every
measurement command of the full run and sizes it: exact counts of chase hops,
bytes, flushes and launches from the command list, and an estimated chip time
from labelled assumptions. Add `--phase F` to size one phase.

`sh run_all.sh --quick` is a small DRAM-only self-test for any Linux host.
On an x86 host build it with `make CFLAGS="-O2 -Wall -Wextra -std=gnu11
-pthread"`. It is not an Asimov measurement.

## What to send back

The whole `results/<UTC time>/` directory. It holds `sysinfo.txt`, one CSV per
test, `cmn-counts.csv` (the mesh counters, one line per event per counted
command), `all.txt` with every command and its output in order, and `log.txt`.
A counted command appears in `all.txt` and `log.txt` as `cmn <label> <command>`.
Every command line in `log.txt` ends with the chip's `/proc/uptime` at launch, a
coarse (10 ms) start stamp. The lifetime of every command run through
`cmncount`, and the lifetime of `cmncount` itself from the entry of its `main`
to its exit, are in `cmn-counts.csv` at 1 ns resolution.
Total size is a few tens of kilobytes.

## What each phase measures

| Phase | Test | Footprints | Expected to show |
|---|---|---|---|
| 0 | sysinfo | | cache topology from sysfs, kernel config, timer, memory map, mesh map, `arm_cmn_0` events |
| A | DRAM latency, CPU 2, repeated walk + perf counters | 4 KiB to 384 KiB | L1 (up to 32 KiB) and L2 (to 128 KiB) latency steps; refills per load say which level served the loads |
| A | DRAM latency, fresh chains | 512 KiB to 32 MiB | SLC step (to about 3.75 MiB), then DRAM |
| A | DRAM latency, fresh chain + flush | 8 MiB | the DRAM reference (the flush removes the lines from the SLC too) |
| B | OCM latency, sparse chain + flush (`--sparse --fresh 8`), one region at a time, from CPUs 2, 4, 6, 8 | 256 KiB, 64 hops per pass, 7 passes | per-region latency from each DSU with the adjacent-line reuse of run 2's dense rows (5 to 24 % of loads found their line in L3) mitigated; one-hop regions faster than three-hop regions; the counters show which home node each region is at; read beside the dense all-five control |
| B | control row per core: a 4 KiB DRAM chase on 4 KiB pages (`--no-thp`) under the counters; on CPU 2 once more without counters | 4 KiB | the per-node background of program load, kernel and output, to set against the OCM rows; the `helper_elapsed_ns` rows of the counted and uncounted pair differ by the cost of the seven counters (setup, read, teardown and any effect on the command) |
| B | OCM latency, all five regions, from CPUs 2, 4, 6, 8: sparse chain, and the dense `--fresh 3` chain as the control | 1.25 MiB (320 hops per sparse pass) | OCM latency averaged over the five home nodes, per core; the dense row counted 1.00 refills of each kind in run 2, and the sparse row is read beside it as a diagnostic comparison (within-sparse spread and sparse-against-dense agreement reported separately); a sparse row that differs is reported with both numbers, not discarded |
| B | SLC-regime fresh chase, 2 MiB DRAM-backed, from CPUs 4, 6, 8 | 2 MiB | the per-core SLC average, beside the all-five OCM row (CPU 2's is in the phase A sweep) |
| D | one-core bandwidth read / write / copy | 16 KiB, 48 KiB, 96 KiB, 256 KiB, 2 MiB, 32 MiB | L1, L2, L3, SLC and DRAM bandwidth of one core |
| D | one-core 2 MiB read under the counters | 2 MiB | how a DRAM buffer spreads over the five home nodes |
| D | OCM read, flushed before | 256 KiB per region, 1.25 MiB all | read bandwidth from each home node and from all five |
| D | OCM streaming write over all five regions | 1.25 MiB, 8 passes | whether OCM sinks one core's stores at the core's 32 GB/s store rate, and how the requests spread over the five nodes (the request count cannot tell a data fetch from a dataless ownership request, so it says nothing about the write protocol) |
| F | DUP + LASTB control, CPU 2 (printed first in every `--cme` latency file) | none | ns per hop of a register chain through the CME with no memory access; the CME chase rows are read beside it, and "chase minus control" is reported as a derived number only |
| F | CME chase: the phase A rows again with `--cme` (repeated walk, fresh chains, fresh + flush) | 4 KiB to 32 MiB | latency of a dependent load that runs on the CME, across its 64 KiB L1, the DSU L3, the SLC and DRAM; the CME refill-source counters say whether the lines came from the CME L1, from inside the cluster or from outside it, and which outside level is a hypothesis from the footprint. asimov-bench measured 21.6 ns from the CME L1 and 26.6 ns from the DSU L3 on 2026-10-22, against 2.0 ns and 6.1 ns on the core |
| F | CME chase, fresh + flush, 8 MiB, under the mesh counters | 8 MiB | the DRAM reference through the CME; `cme_l1d_refill_outer` about 1 per load is the evidence that the loads left the cluster; the mesh counts cover the whole command (fill, chains, warm-up, timed walks), so they corroborate the traffic and do not tell SLC from DRAM for the timed walk |
| F | CME OCM chase, sparse chains (`--sparse --fresh 8 --flush`), per region and all five, CPU 2; plus the dense all-five row as a control | 256 KiB (64 hops per pass), 1.25 MiB (320) | OCM latency through the CME with adjacent-line reuse mitigated; the refill-source counters and the sparse-against-dense pair are consistency checks, not proof that no member was prefetched; the `cme-call` row bounds the per-call cost a 64-hop pass pays; beside this run's phase B rows from CPU 2 |
| F | CME bandwidth read / write / copy, 256 B per instruction | 16 KiB to 32 MiB per buffer (adds 160 KiB and 1 MiB); copy touches two buffers, so its working set is twice the size | throughput of the whole kernel loop (loads, the fold, stores, one `SMSTART`/`SMSTOP` per pass) one core gets through the CME, against phase D's 9 to 32 GB/s; asimov-bench measured 34.5 GB/s reading DRAM through one CME with the same load form |
| F | CME bandwidth with single-vector (64 B) loads and stores | 16 KiB, 2 MiB, 32 MiB | the 64 B forms against the 256 B forms, as whole-kernel throughput; the fold differs too (8 against 16 accumulators), so a gap is not assigned to the load width alone |
| F | per-call cost control: 1 KiB read and write calls, 4096 passes | 1 KiB | elapsed / 4096 is an upper bound on the `SMSTART`, register save, `SMSTOP` cost that every bandwidth pass pays (it includes 1 KiB of data and loop work); a 16 KiB pass pays it once per 16 KiB |
| F | CME OCM read per region and all five, OCM streaming write | 256 KiB, 1.25 MiB | the phase D OCM rows through the CME |
| E | shared-L2 pair, CPUs 2 and 3 | 16 KiB, 48 KiB, 160 KiB each | whether two cores sharing L2 and L3 halve each other's bandwidth |
| E | one core per DSU (2, 4, 6, 8) and all eight (2 to 9) | 2 MiB and 16 MiB per thread | aggregate SLC and DRAM bandwidth of the hemisphere |
| E | writer placement: 2 writers on CPUs 2+4, 2+8, 2+3; 4 writers on 2+4+6+8 (the control), 2+3+4+5, 2+3+8+9 | 16 MiB each | whether the hemisphere write limit (run 1: 86.8 GB/s at 16 MiB per writer, 88.8 at 2 MiB) depends on where the writers sit; a placement differs from the control only if the gap exceeds the spread of the repeats |
| E | OCM read from four cores | one region per core | aggregate OCM read |
| C (runs last) | SPM latency and bandwidth | 64 KiB to 1 MiB | skipped while `/dev/asimov-spm` is absent |

The per-region OCM rows run on 4 KiB pages and the DRAM rows on 2 MiB pages, so
an OCM-versus-SLC latency comparison carries a TLB asymmetry of a few ns (est.).

Phase F runs only when `/proc/cpuinfo` lists `sme2`. Each `--cme` program also
checks the SME2 hwcaps and that the streaming vector length is 64 B, and exits
with a `# cme:` line otherwise, so a missing row says why. The CME rows run
from CPU 2 with CPU 3 offline: the two cores of a DSU share one CME, and an
idle partner core keeps the CME to CPU 2. Inside a streaming span the program
makes no system call (a system call clears streaming mode and its Z and P
state); the timer and counter reads sit outside the span. A signal cannot be
ruled out by the program: a handler runs outside streaming mode and the return
restores the state, so the span survives, but the elapsed time then includes
the handler. The run's isolated CPUs make that unlikely, and the reps show it
as an outlier. The first SMSTART of a thread traps to the kernel once, which
the untimed warm-up pass absorbs.

## Output format

`latency` prints one CSV row per timed walk:

```
mem,size_bytes,mode,rep,loads,ns_per_load,cycles_per_load,l1d_refill_per_load,l2d_refill_per_load,l3d_refill_rd_per_load
dram,4096,repeat,0,65536,2.003,4.01,0.0003,0.0002,0.0002
```

`ns_per_load` is the latency. The refill columns come from the Arm PMU events
0x03 (L1D refill), 0x17 (L2D refill) and 0xA2 (L3D refill on reads) around the
timed walk, divided by the number of loads. 0xA2 is used instead of 0x2A because
the C1-Nano opens 0x2A without error but it always reads 0 (Jesse, 2026-10-30).
A level with about 1.0 refills per load is the one that missed. A `best` row
per size carries the minimum. In `--fresh 3` mode the timed rows are `fresh1`
and `fresh2`, one pass each over a chain that was never walked.

The last counter column, `perf_cov`, is the group's running time over its
enabled time; 1.0000 means the counters covered the whole walk. When they did
not (the group failed to open, a counter ioctl failed, the read came back
short, or the kernel multiplexed the group), the row prints `-` in every
counter column and a `# perf: counters of the row above are invalid: <reason>`
line follows it. The timing of such a row is still valid; its cache
attribution is not. With `--cme` the counter columns are
`cme_cycles_per_load`, `cme_l1d_refill_inner_per_load` and
`cme_l1d_refill_outer_per_load` (events 0x3246, 0x327D, 0x327E), and the first
rows of the file are the `cme-roundtrip` control with `size_bytes` 0, then
the `cme-call` rows: 64 complete one-hop calls timed together, per call
(`loads` 64). A call holds the register save, `SMSTART`, one register hop,
`SMSTOP` and the restore, plus the C loop around it; the parts are not
separated. A sparse chase pass pays the same entry and exit once, so a short
pass is read beside this row, not corrected by it.

`bandwidth` prints one row per rep:

```
mem,kernel,threads,bytes_per_thread,rep,total_bytes,elapsed_s,aggregate_GBps,per_thread_min_GBps,per_thread_max_GBps
```

`aggregate_GBps` is total bytes over the slowest thread's time. Copy counts
both the bytes read and the bytes written.

`cmncount` appends to `cmn-counts.csv`:

```
label,event,nodeid,count,enabled_ns,running_ns,scaled_count
ocm-region1-cpu2,hns_pocq_reqs_recvd_all,160,20480,8123456,8123456,20480
ocm-region1-cpu2,child_elapsed_ns,-,8130000,8130000,8130000,8130000
ocm-region1-cpu2,helper_elapsed_ns,-,8410000,8410000,8410000,8410000
```

One line per event per counted command, plus two timing lines on the chip's
monotonic clock (the same value in the four numeric columns): `child_elapsed_ns`,
the command's lifetime from fork to exit, and `helper_elapsed_ns`, the lifetime
of `cmncount` from the entry of its `main` to the point where every preceding
record is flushed to this file, which includes the sysfs reads, the counter
open, enable, disable, read and close, and the output. Counted and uncounted
runs flush at the same point, so the two are comparable; writing the
`helper_elapsed_ns` line itself is outside the interval. The difference of the
two rows is the helper's own in-process work; the exec and load of `cmncount`
before `main` are not timed. `nodeid` is `-` for an event summed
over all nodes. The counts cover the whole command (buffer fill, chain build,
warm-up, flushes and timed passes), so for an OCM row the question is which
node's count rose, not the absolute number. Expected sizes (est.): a per-region
row raises its home node by a few times 4,096 requests above the other four;
the 2 MiB DRAM read row puts about 40,000 requests on each node if the buffer
spreads evenly over the five. The control row of each core (a 4 KiB chase on 4 KiB
pages, so no 2 MiB page is zeroed inside the count) shows the background per
node (program load, kernel, output); an OCM row's home node should exceed the
other four by well over its own control value. Five nodes that look like the
control under a region row would mean the node ids are wrong. `running_ns`
below `enabled_ns` means the kernel multiplexed the counters and `scaled_count`
is the estimate; a counter whose running time is zero gets a comment line, not
a numeric row. A `# cmncount:` comment line also records an event that could not
be opened or enabled, or that the command ran uncounted because the PMU was
absent or an event name was unknown.

## Assumptions to check on the first boot

- `/dev/ocm` accepts `mmap(NULL, 0x40000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, <region physical base>)`
  and maps the region Normal write-back. Region n is at 0xE0000000 + n * 0x100000.
  Override with `--ocm-dev`, `--ocm-base`, `--ocm-stride`, `--ocm-size` if it differs.
  (Run 1: yes.)
- The pointer chase writes every byte of a buffer with aligned 8-byte stores
  before reading it, and never calls memset or memcpy on OCM or SPM. (Jesse:
  OCM that was never written reads as zero, so the write is not required by the
  hardware. The code keeps it.)
- Every timed OCM pass, repeats included, starts with a flush of the whole
  buffer, and every region gets an untimed write and walk first, so 4 KiB page
  walks stay out of the timed passes. In run 2 this holds for every OCM row:
  the latency rows are `--fresh 3 --flush`, the read rows `--flush-before`, and
  the streaming write row `--flush-before`.
- The mesh PMU `arm_cmn_0` lists 280 events in sysfs (Jesse, 2026-11-01).
  Opening them with `perf_event_open` is untested before run 2. The script
  needs `hns_pocq_reqs_recvd_all`, `hns_mc_reqs_local_all` and
  `hns_cache_miss_all`. If the PMU is absent, or a name is missing, or an open
  fails, the command still runs and `cmn-counts.csv` gets a comment line.
- The five home-node ids 88, 160, 216, 288 and 344 are the ids the arm-cmn
  driver uses. Jesse: they are the firmware's ids, not yet tried with the
  driver. If they are wrong every per-node line reads `unavailable (Invalid
  argument)` and the mesh map in `sysinfo.txt` gives the right ones.
- The PMU raw events 0x03, 0x17, 0xA2 count on the C1-Nano. (Run 1: yes.)
- `clock_gettime(CLOCK_MONOTONIC)` gives chip time at 1 ns resolution. The chip
  has no real-time clock, so dates in the results read 1970.

## Chip-time estimate (est.)

Run 1 took 1.237 s of chip time for 34 commands, about 14 ms of which per
command was process start (est.). Run 2 has 63 commands, 47 of them under
`cmncount`, which is one more program launch each, so about 110 launches and
about 1.5 s of launch cost (est.) on top of the measurement work. The
`child_elapsed_ns` rows measure each counted command's lifetime, the
`helper_elapsed_ns` rows measure `cmncount`'s own lifetime around it, and the
CPU 2 control runs counted and uncounted, so the difference of its two helper
lifetimes is the counters' cost. The exec and load of `cmncount` before `main`,
the 16 uncounted commands, the shell and the sysinfo collection are not timed;
the run's total minus the summed helper lifetimes is unaccounted time, not a
launch cost, and the launch cost stays an estimate. Run 2
drops the 256 KiB to 3 MiB
flushed DRAM rows, the unflushed OCM rows and the OCM write-then-flush rows
(about 20 ms together). It adds 15 per-region and 3 all-five OCM latency rows
from CPUs 4, 6 and 8 (each builds 3 chains and walks 4 passes over 256 KiB or
1.25 MiB at 20 to 25 ns per load: about 2 ms and 10 ms of work, plus process
start), the 2 MiB read row (about 1.5 ms), the OCM streaming write (3 reps of 8
passes over 1.25 MiB, about 5 ms), six writer-placement rows (2 reps of 2 passes
of 16 MiB per writer, about 5 ms each), and a 3-chain 8 MiB DRAM reference row
(about 0.13 s), the five control rows and the three 2 MiB SLC chases (about 15 ms
each). Phase F adds 33 commands and 48 launches: the phase A chase sizes once
more through the CME, whose hops cost about ten times the core's from L1
(21.6 ns against 2.0 ns, asimov-bench 2026-10-22), about 0.45 s, plus the
control hops, chain builds, CME bandwidth and OCM rows (about 0.15 s) and the
launch allowance (0.64 s). `sh run_asimov.sh --dry-run | python3
est_chip_time.py` lists every command with exact counts and labelled
assumptions: 95 commands, 156 launches, about 4 s (est.) in all, half of it
the launch allowance, which nothing in the run measures.
