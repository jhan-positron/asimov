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
#   F2 (run 6) the per-call cost rows: looped and single-call forms at 4 to 24 KiB, the no-fold
#              read and DSB ST write controls, per-call writes at 2 to 8 KiB, single-call rows at
#              48 KiB to 256 KiB, the chase sizes under the CME L3 events; every --cme row prints
#              its in-span time
# Chip time about 2.4 s (est.), half of it the 14 ms launch allowance; 70 commands, 85 launches
# (`sh run_cme.sh --dry-run | python3 est_chip_time.py`). Wall time about 1.8 h at 2,779 s per
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
    # F2 (run 6): the per-call cost of the CME bandwidth kernels, measured three ways. Every --cme
    # row above now also prints its in-span time (span_s: CNTVCT_EL0 read after SMSTART and before
    # SMSTOP in each call, summed by the kernels), so elapsed minus span is the instrumented
    # outside-span interval of the calls (save, SMSTART, SMSTOP, restore, call and return, the
    # second stamp's instructions), not the switch alone. These rows add, at the G8 sizes with 5
    # reps as G8: the looped form (one call per pass, cme-bandwidth-l1-passes.csv) and the
    # single-call form (all passes of a rep in one streaming span, --asm-passes,
    # cme-bandwidth-l1-asmpasses.csv), read and write, whose fitted intercepts give each form's
    # fixed cost per pass; the looped read without its end-of-call fold (--no-fold,
    # cme-bandwidth-l1-nofold.csv), whose difference from the looped read is the net effect of
    # removing the final reduction and its return dependency; a write row with DSB ST
    # before SMSTOP (--dsb-st, cme-bandwidth-l1-dsb.csv), which makes each call wait for its stores
    # to complete; per-call write rows at 2, 4 and 8 KiB beside the 1 KiB row above, to see at which
    # size the per-call cost of a short write call disappears; and the single-call form at the 48 KiB
    # to 256 KiB sizes of the sweep above (cme-bandwidth-dram-1core-asmpasses.csv), so every
    # read and write row below 256 KiB has its single-call twin.
    for sp in 4K:1024 8K:512 16K:256 24K:176; do
        s=${sp%%:*}; p=${sp##*:}
        for k in read write; do
            run cme-bandwidth-l1-passes.csv "$BW" --cme --threads 1 --cpus $CPU --bytes $s --kernel $k --passes $p --reps 5
            run cme-bandwidth-l1-asmpasses.csv "$BW" --cme --asm-passes --threads 1 --cpus $CPU --bytes $s --kernel $k --passes $p --reps 5
        done
        run cme-bandwidth-l1-nofold.csv "$BW" --cme --no-fold --threads 1 --cpus $CPU --bytes $s --kernel read --passes $p --reps 5
        run cme-bandwidth-l1-dsb.csv "$BW" --cme --dsb-st --threads 1 --cpus $CPU --bytes $s --kernel write --passes $p --reps 5
    done
    for sp in 2K:2048 4K:1024 8K:512; do
        s=${sp%%:*}; p=${sp##*:}
        run cme-bandwidth-call-cost.csv "$BW" --cme --threads 1 --cpus $CPU --bytes $s --kernel write --passes $p --reps 3
    done
    for sp in 48K:128 96K:64 160K:32 256K:32; do
        s=${sp%%:*}; p=${sp##*:}
        for k in read write; do
            run cme-bandwidth-dram-1core-asmpasses.csv "$BW" --cme --asm-passes --threads 1 --cpus $CPU --bytes $s --kernel $k --passes $p --reps 3
        done
    done
    # F2 (run 6): the CME chase sizes again under the CME's L3 events (0x328E hit, 0x328F miss,
    # 0x3290 refill, with the inner and outer refills), to validate the three events and, on the
    # prefetcher-off boot, to read which level served the loads beside the refill-source counts.
    run cme-latency-dram-small-l3.csv "$LAT" --cme --cpu $CPU --perf --events cme-l3 --loads 64K --reps 3 \
        --sizes 4K,8K,16K,24K,32K,48K,64K,96K,128K,192K,256K,384K
    if [ "$QUICK" = 1 ]; then
        run cme-latency-dram-large-l3.csv "$LAT" --cme --cpu $CPU --perf --events cme-l3 --fresh 3 --sizes 512K,1M,2M,4M
    else
        run cme-latency-dram-large-l3.csv "$LAT" --cme --cpu $CPU --perf --events cme-l3 --fresh 3 \
            --sizes 512K,768K,1M,1.5M,2M,3M,4M,6M,8M,16M,32M
    fi
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
