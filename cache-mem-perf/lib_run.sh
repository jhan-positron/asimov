# lib_run.sh - shared part of run_asimov.sh (the cache and mesh test) and
# run_cme.sh (the CME test): options, output directory, binaries, mesh
# counter settings, CPU sets, and the run / cmn / cmn0 / cpu_set_online
# helpers. Sourced with ". "$HERE/lib_run.sh"" after the caller set HERE.
# Both scripts write into one results directory when run_all.sh runs them.
#
# Options (parsed here from the caller's arguments):
#   --quick    host self-test: small DRAM-only subset, no CPU hot-plug, no devices
#   --dry-run  print every measurement command line (as if OCM were present and the CPU
#              had SME2) and run nothing; est_chip_time.py reads this to size the run
#   --out DIR  results directory (default results/<UTC time>)

set -u
QUICK=0
DRY=0
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
    --quick) QUICK=1 ;;
    --dry-run) DRY=1 ;;
    --out) shift; OUT="$1" ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$OUT" ] || OUT="$HERE/results/$(date -u +%Y%m%dT%H%M%SZ)"
[ "$DRY" = 1 ] || mkdir -p "$OUT"
LOG="$OUT/log.txt"
[ "$DRY" = 1 ] && LOG=/dev/null
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
# run <outfile> <cmd...>: echo the command, run it, append stdout to the file and to all.txt.
# A failing command is logged with its exit status and counted in FAILS; the suite goes on
# with the other rows and finish_suite makes the script exit 1 at the end.
FAILS=0
run() {
    f="$OUT/$1"; shift
    if [ "$DRY" = 1 ]; then echo "+ $*"; return 0; fi
    say "+ $* [uptime $(cut -d' ' -f1 /proc/uptime 2>/dev/null)]"  # coarse (10 ms) start stamp; command lifetimes are in cmn-counts.csv
    if "$@" > "$OUT/.last" 2>> "$LOG"; then :; else rc=$?; say "  (exit $rc)"; FAILS=$((FAILS + 1)); fi
    cat "$OUT/.last" >> "$f"
    { echo "## $*"; cat "$OUT/.last"; } >> "$OUT/all.txt"
    rm -f "$OUT/.last"
}
# finish_suite <name>: the last lines of a suite; exit 1 when any measurement command failed
# (call it after the CPU state is restored).
finish_suite() {
    say "# $1 done $(date -u +%FT%TZ): $FAILS measurement command(s) failed"
    say "# send back the whole directory: $OUT"
    [ "$FAILS" = 0 ] || exit 1
}
# cmn <label> <cmd...>: run cmd under the mesh counters. cmncount always runs
# the command: with the PMU absent or an event name unknown it runs it uncounted
# and writes one "# cmncount:" comment line to cmn-counts.csv. It also writes the
# command's lifetime (fork to exit, chip ns) as a child_elapsed_ns row and its own
# lifetime (main entry to exit, which includes the counter setup, read and
# teardown) as a helper_elapsed_ns row. Labels carry no comma.
cmn() {
    l="$1"; shift
    "$CMN" --out "$CMNOUT" --label "$l" --nodes "$HNS" --events "$CMN_PER_NODE" --global "$CMN_GLOBAL" -- "$@"
}
# cmn0 <label> <cmd...>: the same without any counter. The helper_elapsed_ns rows
# of a counted and an uncounted run of one command differ by the cost of the seven
# counters: open, enable, disable, read and close, plus any effect on the command
# (visible in the child_elapsed_ns rows). The child rows alone do not hold the
# counter setup and read, which happen outside the child's lifetime.
cmn0() {
    l="$1"; shift
    "$CMN" --out "$CMNOUT" --label "$l" -- "$@"
}
cpu_set_online() { # cpu 0|1
    [ -n "$OFFLINE_CPU" ] || return 0
    [ "$DRY" = 1 ] && return 0
    f="/sys/devices/system/cpu/cpu$1/online"
    [ -w "$f" ] || { say "no $f, skipping hot-plug"; return 0; }
    echo "$2" > "$f" && say "cpu$1 online=$2" || say "cpu$1: hot-plug failed"
}

HAVE_OCM=0; HAVE_SPM=0; HAVE_SME2=0
[ "$QUICK" = 1 ] || { [ -e /dev/ocm ] && HAVE_OCM=1; [ -e /dev/asimov-spm ] && HAVE_SPM=1; }
grep -qw sme2 /proc/cpuinfo 2>/dev/null && HAVE_SME2=1
[ "$DRY" = 1 ] && { HAVE_OCM=1; HAVE_SME2=1; }
