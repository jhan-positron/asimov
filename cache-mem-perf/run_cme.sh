#!/bin/sh
# run_cme.sh - the CME test: the loads and stores of run_asimov.sh's phases A, B and D
# again through the CME (the DSU's Arm C1-SME2 unit) from CPU 2 in streaming mode.
#
# Runs as root under asimov-os, POSIX sh. Usage: sh run_cme.sh [--quick] [--dry-run] [--out DIR]
# (options in lib_run.sh). run_all.sh runs run_asimov.sh and then this script into one
# results directory; on its own this script needs a built tree (make) and writes its own
# results/<UTC time>/ directory. It is skipped when /proc/cpuinfo has no sme2.
#
# Phase F (CPU numbers as in run_asimov.sh: kernel on 0-1, test on 2; CPU 3 taken offline here
# so the DSU-1 CME is not shared, and brought back at the end):
#   control    cme-roundtrip (DUP + LASTB chain, no memory) and cme-call (64 one-hop calls)
#   chase      the phase A sizes through the CME, repeated walk and fresh chains; the flushed
#              8 MiB row under the mesh counters
#   OCM chase  sparse chains per region and over all five, with the dense all-five control
#   bandwidth  read / write / copy with 256 B per instruction at the phase D footprints plus
#              160 KiB and 1 MiB; single-vector 64 B forms; the 1 KiB per-call cost rows;
#              OCM read per region and all five, OCM streaming write
# Chip time about 1.3 s (est.), half of it the 14 ms launch allowance; 33 commands, 48 launches
# (`sh run_cme.sh --dry-run | python3 est_chip_time.py`). Wall time about 1 h at 2,779 s per
# chip second, plus boot.

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib_run.sh"

say "# run_cme.sh start $(date -u +%FT%TZ) quick=$QUICK out=$OUT sme2=$HAVE_SME2 ocm=$HAVE_OCM"
cpu_set_online "$OFFLINE_CPU" 0

# ---- F. the same loads and stores through the CME (cpu $CPU, CPU 3 still offline: the CME is not shared) ----
# The chase rows at the phase A sizes, the OCM chase rows and the bandwidth rows at the phase D
# footprints have a C1-Nano twin with the same footprint, chain and flush rule, so those CSVs
# compare line for line; the cme-roundtrip control, the 160K and 1M bandwidth rows, the
# single-vector rows and the call-cost rows are CME-only. A CME load leaves the core, runs on the
# CME (its own 64 KiB L1, then the DSU L3, then the mesh; the core's L2 is not on this path) and
# its value comes back to the core. The cme-roundtrip row is a DUP + LASTB chain with no load, a
# control beside the chase rows. The CME counter set (through the core PMU, as asimov-bench does):
# CME_CYCLES, CME L1 refills from inside the cluster, CME L1 refills from outside the cluster.
if [ "$HAVE_SME2" = 1 ]; then
    say "## F. CME loads and stores (cpu $CPU)"
    run cme-latency-dram-small.csv "$LAT" --cme --cpu $CPU --perf --loads 64K --reps 3 \
        --sizes 4K,8K,16K,24K,32K,48K,64K,96K,128K,192K,256K,384K
    # the flushed row runs under the mesh counters. cme_l1d_refill_outer about 1 per load is the
    # evidence that the loads left the cluster; the mesh counts span the whole command (fill,
    # chains, warm-up and timed walks), so they corroborate the traffic and cannot tell the SLC
    # from DRAM for the timed walk (the flush removes DRAM-backed lines from the SLC, run 1)
    if [ "$QUICK" = 1 ]; then
        run cme-latency-dram-large.csv "$LAT" --cme --cpu $CPU --perf --fresh 3 --sizes 512K,1M,2M,4M
        run cme-latency-dram-flushed.csv cmn "cme-dram-flushed-2M-cpu$CPU" "$LAT" --cme --cpu $CPU --perf --fresh 3 --flush --sizes 2M
    else
        run cme-latency-dram-large.csv "$LAT" --cme --cpu $CPU --perf --fresh 3 \
            --sizes 512K,768K,1M,1.5M,2M,3M,4M,6M,8M,16M,32M
        run cme-latency-dram-flushed.csv cmn "cme-dram-flushed-8M-cpu$CPU" "$LAT" --cme --cpu $CPU --perf --fresh 3 --flush --sizes 8M
    fi
    # OCM chase rows: sparse chains (one member line per 4 KiB page per chain, --fresh 8: seven
    # timed passes of 64 hops per region, 320 over all five). Run 2's dense 256 KiB rows counted
    # 1.1143 to 1.4468 L2 refills and 0.7627 to 0.9500 L3 refills per load over 40 passes (the
    # reading: prefetched neighbours found later by the chain, a hypothesis). A sparse chain has
    # no member neighbours and visits each page once; an experimental mitigation, not a proof.
    # The dense all-five row (1.25 MiB, counters 1.00) is kept beside the sparse one as the control.
    if [ "$HAVE_OCM" = 1 ]; then
        for r in 0 1 2 3 4; do
            run cme-latency-ocm-region$r.csv cmn "cme-ocm-region$r-cpu$CPU" "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions $r --sizes 256K --sparse --fresh 8 --flush
        done
        run cme-latency-ocm-all5.csv cmn "cme-ocm-all5-cpu$CPU" "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --sparse --fresh 8 --flush
        run cme-latency-ocm-all5-dense.csv cmn "cme-ocm-all5-dense-cpu$CPU" "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --fresh 3 --flush
    fi
    # bandwidth: read, write, copy with 256 B per instruction (LD1B/ST1B of four vectors), then the
    # single-vector 64 B forms (ldr16/str16 names, printed as cme-ldr16/cme-str16), at the D footprints
    # plus 160K (DSU L3) and 1M (SLC)
    for sp in 16K:256 48K:128 96K:64 160K:32 256K:32 1M:8 2M:4 32M:2; do
        s=${sp%%:*}; p=${sp##*:}
        run cme-bandwidth-dram-1core.csv "$BW" --cme --threads 1 --cpus $CPU --bytes $s --kernel all --passes $p --reps 3
    done
    for sp in 16K:256 2M:4 32M:2; do
        s=${sp%%:*}; p=${sp##*:}
        run cme-bandwidth-dram-1core.csv "$BW" --cme --threads 1 --cpus $CPU --bytes $s --kernel ldr16 --passes $p --reps 3
        run cme-bandwidth-dram-1core.csv "$BW" --cme --threads 1 --cpus $CPU --bytes $s --kernel str16 --passes $p --reps 3
    done
    # per-call cost control: every bandwidth pass is one kernel call with its own SMSTART,
    # register save and SMSTOP. A 1 KiB call (the minimum) over 4096 passes is almost all
    # that per-call cost; elapsed / 4096 bounds what a 16 KiB pass pays on top of its data.
    run cme-bandwidth-call-cost.csv "$BW" --cme --threads 1 --cpus $CPU --bytes 1K --kernel read --passes 4096 --reps 3
    run cme-bandwidth-call-cost.csv "$BW" --cme --threads 1 --cpus $CPU --bytes 1K --kernel write --passes 4096 --reps 3
    if [ "$HAVE_OCM" = 1 ]; then
        for r in 0 1 2 3 4; do
            run cme-bandwidth-ocm-region$r.csv cmn "cme-ocm-read-region$r" "$BW" --cme --threads 1 --cpus $CPU --mem ocm --ocm-regions $r --bytes 256K --kernel read --flush-before --passes 1 --reps 3
        done
        run cme-bandwidth-ocm-all5.csv cmn "cme-ocm-read-all5" "$BW" --cme --threads 1 --cpus $CPU --mem ocm --ocm-regions 0,1,2,3,4 --bytes 1280K --kernel read --flush-before --passes 1 --reps 3
        run cme-bandwidth-ocm-write.csv cmn "cme-ocm-write-all5" "$BW" --cme --threads 1 --cpus $CPU --mem ocm --ocm-regions 0,1,2,3,4 --bytes 1280K --kernel write --flush-before --passes 8 --reps 3
    fi
else
    say "## F. CME rows skipped: no sme2 in /proc/cpuinfo"
fi


cpu_set_online "$OFFLINE_CPU" 1
[ "$DRY" = 1 ] && exit 0
finish_suite run_cme.sh
