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
- Vary chain (`--sparse-vary`): the sparse chain with the member's line changing
  from page to page (a hash of the page number picks the slot and the line;
  chains never share a line). Run 3's sparse rows counted fewer L3 read
  refills per load than run 2's dense rows (best pass of each of the 20
  CPU-region cells: 0.50 to 0.86 against 0.76 to 0.92). One reading, a
  hypothesis, is that a prefetcher lands on sparse members by carrying the fixed
  line offset from page to page; this chain tests the sensitivity to that
  offset. A difference between the two chains identifies the offset pattern as
  a factor, not which prefetcher reacts to it.
- Sequential chain (`--seq`): the lines in address order, a chain a stride
  prefetcher can run ahead of. It is the candidate stimulus for validating the
  prefetch refill events: each event is validated separately, on a boot with
  the prefetchers enabled, when the activity it counts is established on this
  row; an event that stays zero without such evidence is unvalidated, not
  broken. On a prefetch-disabled boot the register readbacks and the previously
  validated counts assess the change; the latency change is reported as an
  outcome, with no required direction and no required equality with the random
  chain.

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
| `latency.c` | single-thread pointer chase; `--fresh`, `--flush`, `--sparse`, `--sparse-vary`, `--seq`, `--perf`, `--events` |
| `bandwidth.c` | pinned threads, read / write / copy with `ldp`/`stp` q-register pairs; `ldr16`/`str16` for SPM; `read-pf4`/`-pf8`/`-pf16` (the read loop with a software prefetch hint that many lines ahead) and `read-2s` (two streams); `--asm-passes` (read and write: all passes of a rep inside one assembly call, for the per-pass cost; with `--cme`, one streaming span); `--dsb-st` (CME write with `DSB ST` before `SMSTOP`); `--no-fold` (CME read without its end-of-call fold); `--perf` counts the prefetch set per line on one thread |
| `bandwidth_r2.c` | run 2's bandwidth.c (commit 8ae533c) kept verbatim, built as `bandwidth_r2`: the control of run_followup.sh phase G5 for the per-pass write cost that run 3 added |
| `bandwidth_r3.c` | run 3's bandwidth.c (commit ba834c0) kept verbatim, built as `bandwidth_r3`: the run 3 side of phase G5, since run 5 changed bandwidth.c |
| `cmncount.c` | runs one command with mesh PMU events open (system-wide, on the PMU's CPU), appends one CSV line per event and one with the command's lifetime |
| `cme_kern.S` | the chase and the read / write / copy loops again as streaming-mode SVE (`LD1D` + `LASTB`; `LD1B` / `ST1B` of four vectors and of one), used by `latency --cme` and `bandwidth --cme`; plus the register round trip and `RDSVL`. From run 6 every span reads `CNTVCT_EL0` after `SMSTART` and before `SMSTOP` into `cme_span` and sums the spans there, and the single-call forms (`cme_read4_n`, `cme_write4_n`), the `DSB ST` write (`cme_write4_dsb`) and the read without its fold (`cme_read4_nofold`) exist |
| `cme_test.c` | self-test of those kernels on any SME2 machine: chase, round trip, read fold, write pattern, copy, the single-call forms, the `DSB ST` write, the no-fold read, the span stamps and their running sum; `make cme_test && ./cme_test` |
| `run_followup.sh` | the run 3 follow-up (follow-up-run-plan.md): the OCM chase rows dense, sparse and vary from CPUs 2 and 8, the TLB and walk counter sets on the CPU 2 rows, each with its own validation row, the CME latency sweep, and a sequential chain as the prefetch stimulus, all with the prefetch-refill counters, plus the small one-core write rows with run 3's and run 2's bandwidth programs (G5) and the 20 dense OCM cells under the crosspoint flit counters (G6), the 64 KiB to 512 KiB chase rows with the L2 and L3 hit set (G1, run 5) and the one-core read rows with software prefetch distances and two streams under the prefetch counters (G7, run 5), and the one-core L1 rows at 4 to 24 KiB in the per-pass and `--asm-passes` forms (G8, run 5c); meant to run once per boot (normal, prefetchers off, larger OCM pages, both) |
| `test_lat_rows.py` | test of the latency CSV reader in gen_run3.py over run 2, run 3 and run 4 row shapes, refused and invalid counters; `python3 test_lat_rows.py` |
| `perf_cov_test.c` | host test that a multiplexed counter row is rejected and the next row is not; `make perf_cov_test && taskset -c 2 ./perf_cov_test` on an Arm machine |
| `est_chip_time.py` | sizes a run from `sh run_asimov.sh --dry-run`: exact counts per command, labelled time assumptions |
| `run_all.sh` | **the script to run**: the cache and mesh test, then the CME test, into one `results/<UTC time>/` directory |
| `run_asimov.sh` | the cache and mesh test: phases 0, A, B, D, E and C (last, skipped without `/dev/asimov-spm`) |
| `run_cme.sh` | the CME test: phase F and, from run 6, F2 (the per-call cost rows); skipped when `/proc/cpuinfo` has no `sme2`; takes CPU 3 offline for its rows and brings it back |
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
| F2 (run 6) | CME read and write in the looped form (one call per pass) and the single-call form (`--asm-passes`: one streaming span per rep), 5 reps; the single-call form again at the sweep's 48 KiB to 256 KiB sizes, 3 reps | 4, 8, 16, 24 KiB (1,024, 512, 256, 176 passes); 48, 96, 160, 256 KiB | each form's fitted cost per pass from a line through its four rows (as the core's G8 rows) and their net difference, which holds every per-call difference between the forms (call, return, save, restore, the switch, the predicate and accumulator setup, the fold), not one part of it; every `--cme` row also prints its in-span time, so elapsed minus span is the instrumented outside-span interval of its calls |
| F2 (run 6) | CME read without its end-of-call fold (`--no-fold`), looped form, 5 reps | 4, 8, 16, 24 KiB | the matched control for the fold: the looped read's fitted cost minus this row's is the net effect of removing the final reduction and its return dependency, overlap and completion waiting included, not the execution cost of the removed instructions |
| F2 (run 6) | CME write with `DSB ST` before `SMSTOP` (`--dsb-st`), looped form, 5 reps | 4, 8, 16, 24 KiB | the barrier's measured effect on the per-call cost of writes; the posted-store reading of asimov-cme-perf.html stays a hypothesis either way |
| F2 (run 6) | per-call write rows, 2, 4 and 8 KiB per call, beside the 1 KiB row | 2 to 8 KiB (2,048 to 512 passes) | at which call size the per-call cost of a short write call (23 ns est. at 1 KiB on run 3) disappears |
| F2 (run 6) | the CME chase sizes under the CME L3 events (`--events cme-l3`: 0x328E hit, 0x328F miss, 0x3290 refill, with the inner and outer refills) | 4 KiB to 32 MiB | exploratory counts: whether the three events count on this platform, and how hit plus miss compares with inner plus outer (two totals with different documented populations, so agreement is a workload hypothesis and does not validate the hit and miss split); no fit or attribution reads them until an independent control validates the hit and miss interpretation and the populations (Insufficient data in run 6) |
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
state); the timer and counter reads sit outside the span. From run 6 each
kernel also reads `CNTVCT_EL0` (a system register, legal in streaming mode)
just after `SMSTART` and just before `SMSTOP`, keeps a running sum of the
spans, and the rows print that in-span time beside the bracket time. The
span is instrumented: it holds the first stamp's store and address setup and
the closing `ISB`; the call holds the second stamp's address setup, store and
the sum (eight instructions) as well. So bracket minus span is the
instrumented outside-span interval of a call, not the switch alone, and work
inside the span is in both numbers, so their difference does not show it.
Whether that closing `ISB` waits for the span's loads to return is
Insufficient data (the CME executes out of order and the core stalls on a
CME result in a general-purpose register), so a kernel that ends on a result
of its loads (the folded read) and one that does not (`--no-fold`) end their
spans differently. `--cme` takes one worker, since the stamps are one global
pair, and core workers never touch them. A signal cannot be
ruled out by the program: a handler runs outside streaming mode and the return
restores the state, so the span survives, but the elapsed time then includes
the handler. The run's isolated CPUs make that unlikely, and the reps show it
as an outlier. The first SMSTART of a thread traps to the kernel once, which
the untimed warm-up pass absorbs.

## Output format

`latency` prints one CSV row per timed walk:

```
mem,size_bytes,mode,rep,loads,ns_per_load,cycles_per_load,l1d_refill_per_load,l2d_refill_per_load,l3d_refill_rd_per_load,l1d_refill_hwprf_per_load,l2d_refill_hwprf_per_load,l2d_refill_l1dhwprf_per_load,perf_cov
dram,4096,repeat,0,65536,2.003,4.01,0.0003,0.0002,0.0002,0.0000,0.0000,0.0000,1.0000
```

`ns_per_load` is the latency. The refill columns come from Arm PMU events
counted around the timed walk and divided by the number of loads. The first
three are run 2's: 0x03 L1D_CACHE_REFILL (L1 refills the program's own loads and
stores caused), 0x17 L2D_CACHE_REFILL (every L2 refill: data and instruction
misses, translation walks, L1 write-backs and prefetches) and 0xA2
L3D_CACHE_REFILL_RD (L3 fills from outside due to a read). 0xA2 is used instead
of 0x2A because the C1-Nano opens 0x2A without error but it always reads 0
(Jesse, 2026-10-30). Since the run 3 follow-up the default set adds three
hardware-prefetch refill events: 0x81BC L1D_CACHE_REFILL_HWPRF, 0x81BD
L2D_CACHE_REFILL_HWPRF (prefetchers targeting L2) and 0x0111
IMP_L2D_CACHE_REFILL_L1DHWPRF (the L1D prefetcher's requests). Each is its own
rate; the L2 column minus the two L2 prefetch columns is "L2 refills not
accounted for by these events", not a demand count. `--events tlb` swaps the
three prefetch events for 0x05 L1D_TLB_REFILL (which also counts refills that
prefetches caused), 0x2D L2D_TLB_REFILL and 0x34 DTLB_WALK; `--events walk`
swaps them for 0x34 DTLB_WALK, 0x8188 DTLB_WALK_BLOCK and 0x818A DTLB_WALK_PAGE
(walks by the descriptor they ended in; a walk through a contiguous-hint entry
is a block walk, nevis0_tlb_pagewalk.sv line 3147; a zero block count is
unvalidated until the event has read nonzero); `--events l3` swaps
them for 0x81C5 L2D_CACHE_HIT_RD (L2 hits by demand reads), 0xA0 L3D_CACHE_RD
(reads that reach the L3; in this RTL without hardware-prefetch fills and
prefetch-origin walks, with the program's own page walks kept, nevis0_l2_tagctl.sv
lines 10297 to 10304 and nevis0_tlb_pagewalk.sv line 3267) and 0x81C6 L3D_CACHE_HIT_RD (demand reads that hit in the L3), so the
L2 and L3 demand hits per program load are counted rather than inferred from
the refills; the header names the columns. Hits over accesses (0x81C6 over
0xA0) is reported as an estimate: 0xA0's population is the RTL's reading, and
it still includes the program's page walks and software-prefetch reads. The
columns are independent counts per load; a demand hit fraction derived from
them (l3d_hit_rd over l1d_refill minus l2d_hit_rd) is an estimate that assumes
a load-only chase's L1 refills are its demand reads, which the plan checks. A level with about 1.0 refills per load is the one that filled one
line per load. The events after the first three are optional: if the kernel
refuses one, the row prints `-` in that column and keeps the others (a `# perf:
event ... unavailable` line says so once per file). An event can also open and
never count (on the CRB 0x81BC, 0x81BD and 0x0111 read 0 on a `--seq` chain
that some prefetcher plainly sped up); the `--seq` chain is the stimulus on
which each event is validated separately, and an event that stays zero on it
is unvalidated rather than broken. A
nonzero count too small for four decimals prints in exponent form (`4.6e-05`),
so `0.0000` is an exact zero. A `best` row per size carries the minimum. In
`--fresh 3` mode the timed rows are `fresh1` and `fresh2`, one pass each over a
chain that was never walked.

The last counter column, `perf_cov`, is the group's running time over its
enabled time; 1.0000 means the counters covered the whole walk. When they did
not (the group failed to open, a counter ioctl failed, the read came back
short, or the kernel multiplexed the group), the row prints `-` in every
counter column and a `# perf: counters of the row above are invalid: <reason>`
line follows it. The timing of such a row is still valid; its cache
attribution is not. With `--cme` the counter columns are
`cme_cycles_per_load`, `cme_l1d_refill_inner_per_load`,
`cme_l1d_refill_outer_per_load`, `cme_l1d_refill_rd_per_load`,
`cme_l1d_refill_hwprf_per_load` and `cme_l1d_refill_wr_per_load` (events
0x3246, 0x327D, 0x327E, 0x3280, 0x32B2, 0x3281: inner plus outer is every CME
L1 refill, by where the data came from; rd, hwprf and wr are refills by cause,
a load that missed, the CME prefetcher, a store that missed. The TRM also lists
software preloads; the chase has none, so rd plus hwprf equalling inner plus
outer is a consistency check for this kernel, not an identity), and the first
rows of the file are the `cme-roundtrip` control with `size_bytes` 0, then
the `cme-call` rows: 64 complete one-hop calls timed together, per call
(`loads` 64). A call holds the register save, `SMSTART`, one register hop,
`SMSTOP` and the restore, plus the C loop around it. A sparse chase pass pays
the same entry and exit once, so a short pass is read beside this row, not
corrected by it. From run 6 every `--cme` row has one more column after
`ns_per_load`, `ns_per_load_span`: the time the call spent inside its
streaming span (`CNTVCT_EL0` after `SMSTART` and before `SMSTOP`) per load, so
`ns_per_load` minus it is the instrumented outside-span interval of that call
(register save, `SMSTART`, `SMSTOP`, restore, the call and return, the second
stamp's instructions) spread over its loads; the `cme-call` rows sum the
spans of their 64 calls. The `best` rows keep their six fields. With
`--events cme-l3` the last three counter columns are `cme_l3d_hit_per_load`,
`cme_l3d_miss_per_load` and `cme_l3d_refill_per_load` (0x328E, 0x328F,
0x3290), to be validated on the run 6 boots before they are read.

`bandwidth` prints one row per rep:

```
mem,kernel,threads,bytes_per_thread,rep,total_bytes,elapsed_s,aggregate_GBps,per_thread_min_GBps,per_thread_max_GBps
```

`aggregate_GBps` is total bytes over the slowest thread's time. Copy counts
both the bytes read and the bytes written.

With `--perf` (one thread) the row carries the core prefetch counter set of
`latency` per 64 B line of data moved in the timed passes
(`cycles_per_line`, `l1d_refill_per_line`, ..., `perf_cov`), with the same `-`
and `# perf:` conventions; the `best` row prints `-` in those columns. The
`read-pf4`, `read-pf8` and `read-pf16` kernels are the read loop with a `PRFM
PLDL1KEEP` on every line that many lines ahead of the loads; `read-2s` reads
the two halves of the buffer as two streams, 128 B from each per iteration.
Neither has a CME form. `--asm-passes` (read and write only, one segment)
runs all `--passes` passes of a rep inside one assembly call, so a rep pays
one call, one return and one save of the callee-saved vector registers
instead of one per pass. The inner loop's exit still runs once per pass. The
header line records it as `asm_passes=1`. The default form and this form at
the same size give the net cost of the default pass machinery (call, return,
register save, the per-pass dispatch and accounting) against the assembly's
own loop step (run_followup.sh G8). With `--cme` (run 6) the single-call form
holds all passes of a rep in one streaming span, one `SMSTART` and `SMSTOP`
per rep. Every `--cme` row carries two more columns after
`per_thread_max_GBps`: `span_s`, the sum over the rep's kernel calls of the
time each spent inside its streaming span, and `span_GBps`, the bytes over
that time; `elapsed_s` minus `span_s` is the instrumented outside-span
interval of the rep's calls. The `best` row prints `-` and the `span_GBps` of
the same rep as its `aggregate_GBps`, so the two stay paired. `--dsb-st` (CME
write, looped form; header `dsb_st=1`) ends each call with `DSB ST` before
`SMSTOP`, so the call and its span wait for the stores to complete, which is
the point at which every observer sees them, not the data reaching memory.
`--no-fold` (CME read, looped form; header `no_fold=1`) runs the read kernel
without its end-of-call fold and returns 0 (run_cme.sh F2). The header line
of every `bandwidth` file now carries `dsb_st=` and `no_fold=`; the data rows
and columns of non-CME rows are unchanged.

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
launch allowance (0.64 s). Phase F2 (run 6) adds 37 commands and 37 launches,
about 1.0 s (est.): the two chase sweeps under the CME L3 events are half of
it, the rest is launch time; `sh run_cme.sh --dry-run | python3
est_chip_time.py` gives 70 commands, 85 launches, 2.4 s (est.) for the CME
script. `sh run_asimov.sh --dry-run | python3
est_chip_time.py` lists every command with exact counts and labelled
assumptions: 95 commands, 156 launches, about 4 s (est.) in all, half of it
the launch allowance, which nothing in the run measures.
