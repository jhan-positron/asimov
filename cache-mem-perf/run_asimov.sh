#!/bin/sh
# run_asimov.sh - the whole Asimov cache measurement, one run (run 2 form).
#
# Runs as root under asimov-os. POSIX sh (busybox ash is enough).
# Usage: sh run_asimov.sh [--quick] [--out DIR]
#   --quick  host self-test: small DRAM-only subset, no CPU hot-plug, no devices
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
#   - the flushed DRAM rows shrink to 8 MiB: DC CIVAC removes DRAM-backed lines
#     from the SLC too, so those rows measure DRAM (52.7 ns in run 1).
#   - the OCM write rows with --flush-after are gone: they timed the DC CIVAC
#     instructions. OCM write is now a streaming write over all five regions
#     (1.25 MiB, more than the 384 KiB of L2 + L3) with --flush-before.
#   - the OCM copy row is gone (copy needs two five-region buffers).
#   - the OCM rows run from CPUs 2, 4, 6, 8 and under cmncount, which counts
#     requests received at each of the five home nodes (ids from the address map).
#
# Chip-time estimate (est.): about 1.5 s. Run 1 measured 1.24 s for 34 commands;
# run 2 has 55 commands, and process start was about 14 ms per command in run 1,
# which dominates the small OCM rows. Wall time at 2,779 s per chip second: about 1.2 h.

set -u
QUICK=0
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
    --quick) QUICK=1 ;;
    --out) shift; OUT="$1" ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
HERE=$(cd "$(dirname "$0")" && pwd)
[ -n "$OUT" ] || OUT="$HERE/results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"
LOG="$OUT/log.txt"
LAT="$HERE/latency"
BW="$HERE/bandwidth"
CMN="$HERE/cmncount"
CMNOUT="$OUT/cmn-counts.csv"
[ -x "$LAT" ] && [ -x "$BW" ] && [ -x "$CMN" ] || { echo "build first: make" >&2; exit 2; }

# Home-node ids of OCM0..4 (asimov-address-map profiles/sam_map.yaml: HNS_CTRL, HNS_X2..X5).
HNS="88,160,216,288,344"
# Mesh events: requests received per home node, plus two sums over all nodes.
CMN_PER_NODE="hns_pocq_reqs_recvd_all"
CMN_GLOBAL="hns_mc_reqs_local_all,hns_cache_miss_all"

if [ "$QUICK" = 1 ]; then
    CPU=3; PAIR="3,5"; PERDSU="3,5"; ALL="3,5"; OFFLINE_CPU=""; OCMCPUS="3"
else
    CPU=2; PAIR="2,3"; PERDSU="2,4,6,8"; ALL="2-9"; OFFLINE_CPU=3; OCMCPUS="2 4 6 8"
fi

say() { echo "$*" | tee -a "$LOG"; }
# run <outfile> <cmd...>: echo the command, run it, append stdout to the file and to all.txt
run() {
    f="$OUT/$1"; shift
    say "+ $*"
    "$@" > "$OUT/.last" 2>> "$LOG" || say "  (exit $?)"
    cat "$OUT/.last" >> "$f"
    { echo "## $*"; cat "$OUT/.last"; } >> "$OUT/all.txt.tmp"
    rm -f "$OUT/.last"
}
# cmn <label> <cmd...>: run cmd under the mesh counters. cmncount always runs
# the command: with the PMU absent or an event name unknown it runs it uncounted
# and writes one "# cmncount:" comment line to cmn-counts.csv. Labels carry no comma.
cmn() {
    l="$1"; shift
    "$CMN" --out "$CMNOUT" --label "$l" --nodes "$HNS" --events "$CMN_PER_NODE" --global "$CMN_GLOBAL" -- "$@"
}
cpu_set_online() { # cpu 0|1
    [ -n "$OFFLINE_CPU" ] || return 0
    f="/sys/devices/system/cpu/cpu$1/online"
    [ -w "$f" ] || { say "no $f, skipping hot-plug"; return 0; }
    echo "$2" > "$f" && say "cpu$1 online=$2" || say "cpu$1: hot-plug failed"
}

say "# run_asimov.sh start $(date -u +%FT%TZ) quick=$QUICK out=$OUT"

# ---- 0. sysinfo ----
{
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

HAVE_OCM=0; HAVE_SPM=0
[ "$QUICK" = 1 ] || { [ -e /dev/ocm ] && HAVE_OCM=1; [ -e /dev/asimov-spm ] && HAVE_SPM=1; }
say "ocm=$HAVE_OCM spm=$HAVE_SPM hugepages_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')"

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
        for r in 0 1 2 3 4; do
            run latency-ocm-region$r-cpu$c.csv cmn "ocm-region$r-cpu$c" "$LAT" --cpu $c --perf --mem ocm --ocm-regions $r --sizes 256K --fresh 3 --flush
        done
        run latency-ocm-all5-cpu$c.csv cmn "ocm-all5-cpu$c" "$LAT" --cpu $c --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --fresh 3 --flush
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
    # footprint (run 1), so this row can only show whether OCM sinks one core's stores at that rate;
    # the counters tell the path: about 9 x 4,096 x 5 requests per rep at the five nodes means
    # full-line streaming writes, about twice that means fetch-then-write-back. Up to 384 KiB of
    # each 10 MiB rep may still be dirty in the core at the end (under 4 %).
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
    # (2 x 32 GB/s stays under the 88 GB/s hemisphere write limit of run 1, so these show the
    # per-DSU path). Four writers at the limit: one per DSU (run 1: 88 GB/s), two full DSUs
    # side by side, two full DSUs at the ends; a placement that reads below 88 GB/s names a
    # bottleneck in front of the home nodes.
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

mv "$OUT/all.txt.tmp" "$OUT/all.txt" 2>/dev/null
say "# run_asimov.sh done $(date -u +%FT%TZ)"
say "# send back the whole directory: $OUT"
