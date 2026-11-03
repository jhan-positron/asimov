#!/bin/sh
# run_all.sh - the whole measurement for Jesse: the cache and mesh test (run_asimov.sh,
# phases 0, A, B, D, E, C) and then the CME test (run_cme.sh, phase F), into one results
# directory. Usage: sh run_all.sh [--quick] [--dry-run] [--out DIR]
# Chip time about 4.1 s (est.) in all: `sh run_all.sh --dry-run | python3 est_chip_time.py`.
# Needs: CPUs 2 to 9 online and isolated, 256 huge pages of 2 MiB, /dev/ocm; /dev/asimov-spm
# if present (phase C). Send back the whole results directory.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=""
ARGS=""
while [ $# -gt 0 ]; do
    case "$1" in
    --out) shift; OUT="$1" ;;
    *) ARGS="$ARGS $1" ;;
    esac
    shift
done
[ -n "$OUT" ] || OUT="$HERE/results/$(date -u +%Y%m%dT%H%M%SZ)"
# shellcheck disable=SC2086
sh "$HERE/run_asimov.sh" --out "$OUT" $ARGS
rc1=$?
# shellcheck disable=SC2086
sh "$HERE/run_cme.sh" --out "$OUT" $ARGS
rc2=$?
echo "# run_all.sh done: run_asimov.sh exit $rc1, run_cme.sh exit $rc2 (nonzero: a measurement command failed, see log.txt), results in $OUT"
[ "$rc1" = 0 ] && [ "$rc2" = 0 ]
