#!/bin/sh
# run_asimov.sh - the whole Asimov cache measurement, one run (run 2 form).
#
# Runs as root under asimov-os. POSIX sh (busybox ash is enough).
# Usage: sh run_asimov.sh [--quick] [--dry-run] [--out DIR]   (options in lib_run.sh)
# The CME test is run_cme.sh; run_all.sh runs both into one results directory.
#
# Phases (CPU numbers from Jesse's 2026-10-30 notes: kernel on 0-1, test on 2):
#   0  sysinfo         cache topology, kernel config, timer, memory map, mesh map, mesh PMU
#   A  DRAM latency    pointer chase 4 KiB .. 32 MiB on CPU 2, perf counters
#   B  OCM latency     each region and all five, fresh chain + flush, from CPUs 2, 4, 6, 8,
#                      with the mesh counters per home node
#   D  1-core bandwidth read/write/copy at L1, L2, L3, SLC, DRAM footprints; OCM read per
#                      region, OCM streaming write over all five regions
#   E  multi-core      shared-L2 pair (2,3), one core per DSU (2,4,6,8), all 8,
#                      writer placement rows (2 and 4 writers), OCM read from four cores
#   C  SPM (last)      skipped while /dev/asimov-spm is absent (driver postponed)
# CPU 3 is taken offline for A-D (CPU 2 then owns the DSU-1 L2 and L3) and
# brought back for E. CPUs 5, 7 and 9 stay online and idle during B; Jesse:
# an idle isolated core does not touch the caches. Hot-plug happens outside
# timed sections only.
#
# Changes from run 1 (commit 9c6b8e7), from the run 1 results:
#   - every flushed latency row is "--fresh 3 --flush": a chain that was never
#     walked, flushed before its one timed pass. Run 1's "--flush --reps 3"
#     re-walked a learned chain and 9 to 23 % of its lines were prefetched.
#   - (after run 2) the OCM chase rows are "--sparse --fresh 8 --flush": one member
#     line per 4 KiB page per chain. Run 2's dense 256 KiB rows still counted 1.1143 to
#     1.4468 L2 refills and 0.7627 to 0.9500 L3 refills per load (40 passes); a fresh chain
#     changes the walk order, not the set of lines, so prefetched neighbours were still
#     members. The sparse chain is an experimental mitigation; its dense twin is the control.
#   - the flushed DRAM rows shrink to 8 MiB: DC CIVAC removes DRAM-backed lines
#     from the SLC too, so those rows measure DRAM (52.7 ns in run 1).
#   - the OCM write rows with --flush-after are gone: they timed the DC CIVAC
#     instructions. OCM write is now a streaming write over all five regions
#     (1.25 MiB, more than the 384 KiB of L2 + L3) with --flush-before.
#   - the OCM copy row is gone (copy needs two five-region buffers).
#   - the OCM rows run from CPUs 2, 4, 6, 8 and under cmncount, which counts
#     requests received at each of the five home nodes (ids from the address map).
#
# Chip-time estimate (est.): about 2 s. Run 1 measured 1.24 s for 34 commands;
# process start was about 14 ms per command (est.), which dominates the small rows.
# Run 2 has 63 commands, 47 of them under cmncount (one more program launch each),
# so about 110 launches. cmncount records each counted command's lifetime and its
# own lifetime from main entry to exit in cmn-counts.csv (chip ns), and the CPU 2
# control runs counted and uncounted, so the counters' cost (setup, read and
# teardown, plus any effect on the command) is measured. The exec and load of
# cmncount itself, the 16 uncounted commands, the shell and sysinfo are not timed;
# the run's total minus the summed helper lifetimes is unaccounted time, not a
# launch cost. The uptime stamp on each command line in log.txt is a coarse
# (10 ms) start time.
# Phase F adds 33 commands, 48 launches and about 1.3 s (est.): the CME chase pays about 22 ns
# per hop even from its L1 (asimov-bench, 2026-10-22), ten times the core's 2 ns, so its chase
# rows cost ten times A's, and half of its estimate is the launch allowance. It lives in
# run_cme.sh. The whole run: `sh run_all.sh --dry-run | python3 est_chip_time.py`, about 4.1 s
# (est.), about 3 h of wall time at 2,779 s per chip second; this script alone about 2.8 s
# (est.). The launch allowance is not measured by anything in the run.

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib_run.sh"

say "# run_asimov.sh start $(date -u +%FT%TZ) quick=$QUICK out=$OUT"

# ---- 0. sysinfo ----
[ "$DRY" = 1 ] || {
    echo "## date"; date -u
    echo "## uname"; uname -a
    echo "## cmdline"; cat /proc/cmdline 2>/dev/null
    echo "## online cpus"; cat /sys/devices/system/cpu/online 2>/dev/null
    echo "## cpuinfo"; cat /proc/cpuinfo 2>/dev/null
    echo "## cache topology (cpu$CPU)"
    for d in /sys/devices/system/cpu/cpu$CPU/cache/index*; do
        [ -d "$d" ] || continue
        echo "$d: level=$(cat $d/level) type=$(cat $d/type) size=$(cat $d/size) ways=$(cat $d/ways_of_associativity 2>/dev/null) line=$(cat $d/coherency_line_size 2>/dev/null) shared_cpu_list=$(cat $d/shared_cpu_list)"
    done
    echo "## meminfo"; grep -E 'MemTotal|MemFree|HugePages_|Hugepagesize|AnonHugePages' /proc/meminfo
    echo "## thp"; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
    echo "## config"; zcat /proc/config.gz 2>/dev/null | grep -E 'HUGEPAGE|HUGETLB|PERF_EVENTS|ARM_PMU|ARM_CMN|DEBUG_FS|DEVMEM|HZ=|PREEMPT'
    echo "## dmesg timer/memory"; dmesg 2>/dev/null | grep -iE 'arch_timer|timer\(s\) running|Memory:|hugepage|ocm|spm|cmn' | head -20
    echo "## iomem"; cat /proc/iomem 2>/dev/null
    echo "## devices"; ls -l /dev/ocm /dev/asimov-spm 2>&1
    echo "## perf events"; ls /sys/bus/event_source/devices/ 2>/dev/null
    echo "## arm_cmn_0"
    P=/sys/bus/event_source/devices/arm_cmn_0
    if [ -d "$P" ]; then
        echo "type=$(cat $P/type) cpumask=$(cat $P/cpumask) events=$(ls $P/events | wc -l)"
        for e in hns_pocq_reqs_recvd_all hns_mc_reqs_local_all hns_cache_miss_all hns_slc_sf_cache_access_all hns_cache_fill_all; do
            printf '%s: ' "$e"; cat "$P/events/$e" 2>&1
        done
        echo "## arm_cmn_0 events (all)"; ls "$P/events"
    else
        echo "absent"
    fi
    echo "## arm-cmn mesh map (needs CONFIG_DEBUG_FS)"
    grep -q ' /sys/kernel/debug debugfs' /proc/mounts 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>&1
    cat /sys/kernel/debug/arm-cmn/map* 2>&1
} > "$OUT/sysinfo.txt" 2>&1
say "sysinfo written"

say "ocm=$HAVE_OCM spm=$HAVE_SPM sme2=$HAVE_SME2 hugepages_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')"

cpu_set_online "$OFFLINE_CPU" 0

# ---- A. DRAM latency on one core ----
say "## A. DRAM latency (cpu $CPU)"
run latency-dram-small.csv "$LAT" --cpu $CPU --perf --loads 64K --reps 3 \
    --sizes 4K,8K,16K,24K,32K,48K,64K,96K,128K,192K,256K,384K
if [ "$QUICK" = 1 ]; then
    run latency-dram-large.csv "$LAT" --cpu $CPU --perf --fresh 3 --sizes 512K,1M,2M,4M
    run latency-dram-flushed.csv "$LAT" --cpu $CPU --perf --fresh 3 --flush --sizes 2M
else
    run latency-dram-large.csv "$LAT" --cpu $CPU --perf --fresh 3 \
        --sizes 512K,768K,1M,1.5M,2M,3M,4M,6M,8M,16M,32M
    # DRAM reference: flushed fresh chain. DC CIVAC removes the lines from the SLC too (run 1).
    run latency-dram-flushed.csv "$LAT" --cpu $CPU --perf --fresh 3 --flush --sizes 8M
fi

# ---- B. OCM latency: fresh chain + flush, per region and all five, from one core per DSU ----
if [ "$HAVE_OCM" = 1 ]; then
    for c in $OCMCPUS; do
        say "## B. OCM latency (cpu $c)"
        # control: the same program with a 4 KiB DRAM buffer on 4 KiB pages (--no-thp: no 2 MiB page is
        # allocated or zeroed), so the counters show the per-node background of program load, kernel
        # and output; an OCM row's home node must rise well above it. On CPU 2 the control also runs
        # once without counters, which gives the cost of the seven counters themselves.
        run latency-control-cpu$c.csv cmn "control-cpu$c" "$LAT" --cpu $c --perf --no-thp --sizes 4K --loads 8K --reps 1
        [ "$c" = "$CPU" ] && run latency-control-cpu$c.csv cmn0 "control-nocount-cpu$c" "$LAT" --cpu $c --perf --no-thp --sizes 4K --loads 8K --reps 1
        # sparse chains (one member line per 4 KiB page per chain, seven timed passes of 64 hops):
        # run 2's dense 256 KiB rows counted 1.1143 to 1.4468 L2 refills and 0.7627 to 0.9500 L3
        # refills per load over 40 passes (the reading: prefetched neighbours found later by the
        # chain, a hypothesis). The dense all-five row (1.0003 to 1.0011 of each in run 2) stays as
        # the control beside the sparse all-five row.
        for r in 0 1 2 3 4; do
            run latency-ocm-region$r-cpu$c.csv cmn "ocm-region$r-cpu$c" "$LAT" --cpu $c --perf --mem ocm --ocm-regions $r --sizes 256K --sparse --fresh 8 --flush
        done
        run latency-ocm-all5-cpu$c.csv cmn "ocm-all5-cpu$c" "$LAT" --cpu $c --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --sparse --fresh 8 --flush
        run latency-ocm-all5-dense-cpu$c.csv cmn "ocm-all5-dense-cpu$c" "$LAT" --cpu $c --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --fresh 3 --flush
        # the SLC-regime fresh chase (DRAM-backed, 2 MiB) from the same core: the per-core SLC average,
        # to set beside the all-five OCM row; CPU 2 already has it in latency-dram-large.csv
        [ "$c" = "$CPU" ] || run latency-dram-slc-cpu$c.csv cmn "dram-slc-2M-cpu$c" "$LAT" --cpu $c --perf --fresh 3 --sizes 2M
    done
fi

# ---- D. one-core bandwidth ----
say "## D. one-core bandwidth (cpu $CPU)"
for sp in 16K:256 48K:128 96K:64 256K:32 2M:4 32M:2; do
    s=${sp%%:*}; p=${sp##*:}
    run bandwidth-dram-1core.csv "$BW" --threads 1 --cpus $CPU --bytes $s --kernel all --passes $p --reps 3
done
# which home nodes a 2 MiB DRAM buffer is spread over: requests per node around one read row
run bandwidth-dram-spread.csv cmn "dram-read-2M-cpu$CPU" "$BW" --threads 1 --cpus $CPU --bytes 2M --kernel read --passes 4 --reps 1
if [ "$HAVE_OCM" = 1 ]; then
    for r in 0 1 2 3 4; do
        run bandwidth-ocm-region$r.csv cmn "ocm-read-region$r" "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions $r --bytes 256K --kernel read --flush-before --passes 1 --reps 3
    done
    run bandwidth-ocm-all5.csv cmn "ocm-read-all5" "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions 0,1,2,3,4 --bytes 1280K --kernel read --flush-before --passes 1 --reps 3
    # streaming write into OCM over all five regions. One core's store rate is 32 GB/s at every DRAM
    # footprint (run 1), so this row shows whether OCM sinks one core's stores at that rate, and the
    # counters show how the requests spread over the five nodes. The request count cannot tell a data
    # fetch from a dataless ownership request, so it says nothing about the write protocol. Up to
    # 384 KiB of each 10 MiB rep may still be dirty in the core at the end (under 4 %).
    run bandwidth-ocm-write.csv cmn "ocm-write-all5" "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions 0,1,2,3,4 --bytes 1280K --kernel write --flush-before --passes 8 --reps 3
fi

# ---- E. multi-core bandwidth ----
cpu_set_online "$OFFLINE_CPU" 1
say "## E. multi-core bandwidth"
for sp in 16K:256 48K:128 160K:32; do
    s=${sp%%:*}; p=${sp##*:}
    run bandwidth-pair.csv "$BW" --threads 2 --cpus $PAIR --bytes $s --kernel all --passes $p --reps 3
done
NPD=$(echo "$PERDSU" | tr ',' '\n' | wc -l)
for sp in 2M:4 16M:2; do
    s=${sp%%:*}; p=${sp##*:}
    run bandwidth-perdsu.csv "$BW" --threads $NPD --cpus $PERDSU --bytes $s --kernel all --passes $p --reps 3
    [ "$QUICK" = 1 ] || run bandwidth-all8.csv "$BW" --threads 8 --cpus $ALL --bytes $s --kernel all --passes $p --reps 3
done
if [ "$QUICK" = 0 ]; then
    # writer placement on the mesh. Two writers: adjacent DSUs, end DSUs, the shared-L2 pair
    # (2 x 32 GB/s stays under the hemisphere write limit of run 1, 86.8 GB/s at 16 MiB per writer,
    # so these show the per-DSU path). Four writers at the limit: one per DSU (the control, same
    # options as the other two rows), two full DSUs side by side, two full DSUs at the ends. A
    # placement is different from the control only if the gap exceeds the spread of the repeats.
    for cp in 2,4 2,8 2,3 2,4,6,8 2,3,4,5 2,3,8,9; do
        n=$(echo "$cp" | tr ',' '-')
        t=$(echo "$cp" | tr ',' '\n' | wc -l)
        run bandwidth-write-cpus$n.csv cmn "write-cpus$n" "$BW" --threads $t --cpus $cp --bytes 16M --kernel write --passes 1 --reps 2
    done
fi
if [ "$HAVE_OCM" = 1 ]; then
    run bandwidth-ocm-4core.csv cmn "ocm-read-4core" "$BW" --threads 4 --cpus 2,4,6,8 --mem ocm --ocm-regions 1,2,3,4 --bytes 256K --kernel read --flush-before --passes 1 --reps 3
fi

# ---- C. SPM (last: a wrong-size access would revoke the mapping and SIGBUS later passes) ----
if [ "$HAVE_SPM" = 1 ]; then
    say "## C. SPM latency and bandwidth (cpu $CPU)"
    run latency-spm.csv "$LAT" --cpu $CPU --perf --mem spm --loads 32K --reps 3 --sizes 64K,256K,1M
    run bandwidth-spm.csv "$BW" --threads 1 --cpus $CPU --mem spm --bytes 1M --kernel ldr16 --passes 2 --reps 3
    run bandwidth-spm.csv "$BW" --threads 1 --cpus $CPU --mem spm --bytes 1M --kernel str16 --passes 2 --reps 3
fi

[ "$DRY" = 1 ] && exit 0
finish_suite run_asimov.sh
