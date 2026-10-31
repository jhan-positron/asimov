#!/bin/sh
# run_asimov.sh - the whole Asimov cache measurement, one run.
#
# Runs as root under asimov-os. POSIX sh (busybox ash is enough).
# Usage: sh run_asimov.sh [--quick] [--out DIR]
#   --quick  host self-test: small DRAM-only subset, no CPU hot-plug, no devices
#
# Phases (CPU numbers from Jesse's 2026-10-30 notes: kernel on 0-1, test on 2):
#   0  sysinfo         cache topology, kernel config, timer, memory map
#   A  DRAM latency    pointer chase 4 KiB .. 32 MiB on CPU 2, perf counters
#   B  OCM latency     each region flushed, all five regions, SLC reference
#   C  SPM             latency and ldr16/str16 bandwidth (non-cacheable)
#   D  1-core bandwidth read/write/copy at L1, L2, L3, SLC, DRAM footprints; OCM
#   E  multi-core      shared-L2 pair (2,3), one core per DSU (2,4,6,8), all 8
# CPU 3 is taken offline for A-D (CPU 2 then owns the DSU-1 L2 and L3) and
# brought back for E. Hot-plug happens outside timed sections only.
#
# Chip-time estimate (est.): about 2 s in total, dominated by the 16-32 MiB
# DRAM chases. Wall time at 46 min per chip second: about 1.5 h (est.).

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
[ -x "$LAT" ] && [ -x "$BW" ] || { echo "build first: make" >&2; exit 2; }

if [ "$QUICK" = 1 ]; then
    CPU=3; PAIR="3,5"; PERDSU="3,5"; ALL="3,5"; OFFLINE_CPU=""
else
    CPU=2; PAIR="2,3"; PERDSU="2,4,6,8"; ALL="2-9"; OFFLINE_CPU=3
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
    echo "## config"; zcat /proc/config.gz 2>/dev/null | grep -E 'HUGEPAGE|HUGETLB|PERF_EVENTS|ARM_PMU|ARM_CMN|DEVMEM|HZ=|PREEMPT' 
    echo "## dmesg timer/memory"; dmesg 2>/dev/null | grep -iE 'arch_timer|timer\(s\) running|Memory:|hugepage|ocm|spm' | head -20
    echo "## iomem"; cat /proc/iomem 2>/dev/null
    echo "## devices"; ls -l /dev/ocm /dev/asimov-spm 2>&1
    echo "## perf events"; ls /sys/bus/event_source/devices/ 2>/dev/null
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
else
    run latency-dram-large.csv "$LAT" --cpu $CPU --perf --fresh 3 \
        --sizes 512K,768K,1M,1.5M,2M,3M,4M,6M,8M,16M,32M
fi
# flushed one-pass walks: every load leaves the core caches (SLC or DRAM)
run latency-dram-flushed.csv "$LAT" --cpu $CPU --perf --flush --reps 3 --sizes 256K,1M,2M,3M,8M

# ---- B. OCM latency ----
if [ "$HAVE_OCM" = 1 ]; then
    say "## B. OCM latency (cpu $CPU)"
    for r in 0 1 2 3 4; do
        run latency-ocm-region$r.csv "$LAT" --cpu $CPU --perf --mem ocm --ocm-regions $r --sizes 256K --flush --reps 3
    done
    run latency-ocm-all5-flushed.csv "$LAT" --cpu $CPU --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --flush --reps 3
    run latency-ocm-all5-fresh.csv "$LAT" --cpu $CPU --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --fresh 3
fi

# ---- C. SPM ----
if [ "$HAVE_SPM" = 1 ]; then
    say "## C. SPM latency and bandwidth (cpu $CPU)"
    run latency-spm.csv "$LAT" --cpu $CPU --perf --mem spm --loads 32K --reps 3 --sizes 64K,256K,1M
    run bandwidth-spm.csv "$BW" --threads 1 --cpus $CPU --mem spm --bytes 1M --kernel ldr16 --passes 2 --reps 3
    run bandwidth-spm.csv "$BW" --threads 1 --cpus $CPU --mem spm --bytes 1M --kernel str16 --passes 2 --reps 3
fi

# ---- D. one-core bandwidth ----
say "## D. one-core bandwidth (cpu $CPU)"
for sp in 16K:256 48K:128 96K:64 256K:32 2M:4 32M:2; do
    s=${sp%%:*}; p=${sp##*:}
    run bandwidth-dram-1core.csv "$BW" --threads 1 --cpus $CPU --bytes $s --kernel all --passes $p --reps 3
done
if [ "$HAVE_OCM" = 1 ]; then
    for r in 0 1 2 3 4; do
        run bandwidth-ocm-region$r.csv "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions $r --bytes 256K --kernel read --flush-before --passes 1 --reps 3
    done
    run bandwidth-ocm-write.csv "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions 1 --bytes 256K --kernel write --flush-after --passes 1 --reps 3
    run bandwidth-ocm-copy.csv "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions 1,2 --bytes 256K --kernel copy --flush-before --flush-after --passes 1 --reps 3
    run bandwidth-ocm-all5.csv "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions 0,1,2,3,4 --bytes 1280K --kernel read --passes 4 --reps 3
    run bandwidth-ocm-all5.csv "$BW" --threads 1 --cpus $CPU --mem ocm --ocm-regions 0,1,2,3,4 --bytes 1280K --kernel read --flush-before --passes 1 --reps 3
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
if [ "$HAVE_OCM" = 1 ]; then
    run bandwidth-ocm-4core.csv "$BW" --threads 4 --cpus 2,4,6,8 --mem ocm --ocm-regions 1,2,3,4 --bytes 256K --kernel read --flush-before --passes 1 --reps 3
    run bandwidth-ocm-4core.csv "$BW" --threads 4 --cpus 2,4,6,8 --mem ocm --ocm-regions 1,2,3,4 --bytes 256K --kernel write --flush-after --passes 1 --reps 3
fi

mv "$OUT/all.txt.tmp" "$OUT/all.txt" 2>/dev/null
say "# run_asimov.sh done $(date -u +%FT%TZ)"
say "# send back the whole directory: $OUT"
