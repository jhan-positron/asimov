#!/bin/sh
# run_followup.sh - the run 3 follow-ups (see follow-up-run-plan.md): the OCM chase rows and the
# CME latency sweep again with three more counters in each group (the refills the hardware
# prefetchers made), a third chain shape for the OCM rows, a translation counter set on the OCM
# rows from CPU 2, and a sequential chain as the prefetcher stimulus. The same script is
# meant to run on more than one boot (the normal boot, then boots with the prefetchers disabled
# or OCM in larger pages, as the platform allows); each boot writes its own --out directory.
#
# Runs as root under asimov-os, POSIX sh. Usage: sh run_followup.sh [--quick] [--dry-run] [--out DIR]
# (options in lib_run.sh). Needs a built tree (make). The CME rows are skipped when
# /proc/cpuinfo has no sme2; the OCM rows when /dev/ocm is absent.
#
# Counter groups (common.h): the core prefetch set is cycles, 0x03 L1D refill, 0x17 L2D refill,
# 0xA2 L3D read refill, plus 0x81BC L1D_CACHE_REFILL_HWPRF, 0x81BD L2D_CACHE_REFILL_HWPRF and
# 0x0111 IMP_L2D_CACHE_REFILL_L1DHWPRF (refills a hardware prefetcher requested, by level and by
# prefetcher). The core TLB set (--events tlb) keeps the first four and adds 0x05 L1D_TLB_REFILL,
# 0x2D L2D_TLB_REFILL and 0x34 DTLB_WALK. The core walk set (--events walk) keeps the first four and
# adds 0x34 DTLB_WALK, 0x8188 DTLB_WALK_BLOCK and 0x818A DTLB_WALK_PAGE (walks by the descriptor
# they ended in; a walk through a contiguous-hint entry is a block walk). The core hit set
# (--events l3) keeps the first four and adds 0x81C5 L2D_CACHE_HIT_RD, 0xA0 L3D_CACHE_RD and 0x81C6
# L3D_CACHE_HIT_RD (the L2 and L3 demand read hits per load; in this RTL 0xA0 leaves out
# hardware-prefetch fills and prefetch-origin walks and keeps the program's own page walks, so hits
# over accesses is reported as an estimate). The CME set is cycles, 0x3246 CME_CYCLES, 0x327D inner
# refills, 0x327E outer refills, plus 0x3280 CME_L1D_CACHE_REFILL_RD (refills by loads), 0x32B2
# CME_L1D_CACHE_REFILL_HWPRF (by the CME prefetcher) and 0x3281 CME_L1D_CACHE_REFILL_WR (by
# stores; the chase has none). An added event that is refused costs its own column only: the
# row keeps the run 3 columns and prints "-" in that one.
#
# Chain shapes (latency.c): dense = run 2's OCM chain, every line of a region (--fresh 3 --flush,
# 4,096 hops a pass); sparse = run 3's, one line per 4 KiB page at one fixed line per chain
# (--sparse --fresh 8 --flush, 64 hops a pass); vary = sparse with the line changing from page to
# page (--sparse-vary); seq = the lines in address order (--seq), the prefetch stimulus.
#
# Phase G (CPU numbers as in run_asimov.sh; CPU 3 offline throughout so CPU 2 owns its DSU's
# L2, L3 and CME):
#   G0  sysinfo       as run_asimov.sh phase 0, plus the kernel command line and the PMU counter count
#   G1  core chase    the run 3 DRAM sweep (4 KiB to 32 MiB, repeated, fresh, flushed) on CPU 2,
#                     then the 8 MiB sequential chain, flushed (the prefetch stimulus); the 64 KiB
#                     to 512 KiB rows again with the hit set (the L2 and L3 hit fractions)
#   G2  core OCM      from CPUs 2 and 8 (1 and 5 crosspoints at most): run 3's 4 KiB control; each
#                     region dense, sparse and vary; all five dense and sparse. From CPU 2 the
#                     per-region rows again with the TLB set (-tlb files), after a 32 MiB random
#                     chain on 4 KiB pages that validates the TLB events (the stimulus on which they are expected to count);
#                     then the sparse per-region rows with the walk set (-walk files), after the same stimulus
#   G3  CME chase     the run 3 CME DRAM sweep and controls (CPU 2 in streaming mode), then the
#                     8 MiB sequential chain through the CME
#   G4  CME OCM       each region dense, sparse and vary; all five dense and sparse
#   G5  write cost    the one-core write rows of 16 KiB to 256 KiB with the run 3 program and with
#                     run 2's (bandwidth_r2, no CME path), on one boot: the per-pass cost question
#   G6  route flits   the dense per-region chase from every CPU to every OCM node (20 cells) under
#                     cmncount, counting the data flits each crosspoint at the two ends sends in each
#                     direction: an observation of outgoing data directions, not the route, and a
#                     second sample of every cell on one boot (CPU 2's and 8's cells already ran in G2)
#   G7  read variants the one-core read rows at 2 MiB and 32 MiB with the plain read loop, with a
#                     software prefetch 4, 8 and 16 lines ahead, and over two streams, each under
#                     the prefetch counters: whether the hardware prefetcher runs on a streaming read
#   G8  L1 pass cost  the one-core read and write rows at 4, 8, 16 and 24 KiB (inside the 32 KiB L1),
#                     each in two forms: the default, one kernel call per pass, and --asm-passes, all
#                     passes inside one call. Time per pass against size, fitted per form, gives that
#                     form's fixed cost per pass (intercept) and the L1 rate without it (slope). The
#                     difference of the two intercepts is the net cost of the default pass machinery
#                     against the assembly's own loop step. Under the prefetch counters, so the L1
#                     refills per line screen the rows for L1 residence
# No bandwidth rows beyond G5, G7 and G8; the mesh counters are used only in G6.
# Chip time: `sh run_followup.sh --dry-run | python3 est_chip_time.py` (est.; see the plan).

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib_run.sh"

say "# run_followup.sh start $(date -u +%FT%TZ) quick=$QUICK out=$OUT sme2=$HAVE_SME2 ocm=$HAVE_OCM"

# ---- G0. sysinfo ----
if [ "$DRY" != 1 ]; then
{
    echo "## date"; date -u +%FT%TZ; cat /proc/uptime
    echo "## kernel"; cat /proc/version 2>/dev/null
    echo "## cmdline"; cat /proc/cmdline 2>/dev/null
    echo "## cpuinfo"; cat /proc/cpuinfo 2>/dev/null
    echo "## cache topology (cpu$CPU)"
    for d in /sys/devices/system/cpu/cpu$CPU/cache/index*; do
        [ -d "$d" ] || continue
        echo "$d: level=$(cat $d/level) type=$(cat $d/type) size=$(cat $d/size) ways=$(cat $d/ways_of_associativity 2>/dev/null) line=$(cat $d/coherency_line_size 2>/dev/null) shared_cpu_list=$(cat $d/shared_cpu_list)"
    done
    echo "## meminfo"; grep -E 'MemTotal|MemFree|HugePages_|Hugepagesize|AnonHugePages' /proc/meminfo
    echo "## thp"; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
    echo "## dmesg timer/memory/sme/pmu"; dmesg 2>/dev/null | grep -iE 'arch_timer|timer\(s\) running|Memory:|hugepage|ocm|spm|cmn|sme|prefetch|perfevents|counters available' | head -40
    echo "## devices"; ls -l /dev/ocm /dev/asimov-spm 2>&1
    echo "## perf events"; ls /sys/bus/event_source/devices/ 2>/dev/null
    echo "## arm_cmn_0 event names"; ls /sys/bus/event_source/devices/arm_cmn_0/events 2>/dev/null | tr '\n' ' '; echo
    echo "## arm-cmn mesh map (needs CONFIG_DEBUG_FS)"
    grep -q ' /sys/kernel/debug debugfs' /proc/mounts 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>&1
    cat /sys/kernel/debug/arm-cmn/map* 2>&1
} > "$OUT/sysinfo.txt" 2>&1
say "sysinfo written"
say "ocm=$HAVE_OCM sme2=$HAVE_SME2 hugepages_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')"
fi

cpu_set_online "$OFFLINE_CPU" 0

# ---- G1. the core DRAM sweep (cpu $CPU), with the prefetch-refill columns ----
# Run 3's rows: the 192 and 256 KiB rows mix L2, L3 and mesh service; the new columns say how
# many of their L1 and L2 fills the prefetchers made. The 4 KiB repeated row is the drift control
# against run 3's latency-dram-small.csv (same command). The sequential 8 MiB chain is the
# candidate prefetch stimulus: each prefetch event is validated separately, on a boot with the
# prefetchers enabled, when the activity it counts is established on this row; an event that
# stays zero is unvalidated, not broken. On a prefetch-off boot the readbacks and the previously
# validated counts assess the change; the latency change is an outcome, with no required direction.
say "## G1. core DRAM latency (cpu $CPU)"
run latency-dram-small.csv "$LAT" --cpu $CPU --perf --loads 64K --reps 3 \
    --sizes 4K,8K,16K,24K,32K,48K,64K,96K,128K,192K,256K,384K
# the L2-to-L3 rows again with the hit set: run 3 inferred a 42 % L2 hit fraction at 192 KiB from the
# refills; these rows count the L2 and L3 demand read hits per load. 64 KiB (in the L2) and 512 KiB
# (beyond the L3) bracket the sweep.
run latency-dram-small-l3.csv "$LAT" --cpu $CPU --perf --events l3 --loads 64K --reps 3 \
    --sizes 64K,96K,128K,192K,256K,384K,512K
if [ "$QUICK" = 1 ]; then
    run latency-dram-large.csv "$LAT" --cpu $CPU --perf --fresh 3 --sizes 512K,1M,2M,4M
    run latency-dram-flushed.csv "$LAT" --cpu $CPU --perf --fresh 3 --flush --sizes 2M
    run latency-dram-seq.csv "$LAT" --cpu $CPU --perf --seq --fresh 3 --flush --sizes 2M
else
    run latency-dram-large.csv "$LAT" --cpu $CPU --perf --fresh 3 \
        --sizes 512K,768K,1M,1.5M,2M,3M,4M,6M,8M,16M,32M
    run latency-dram-flushed.csv "$LAT" --cpu $CPU --perf --fresh 3 --flush --sizes 8M
    run latency-dram-seq.csv "$LAT" --cpu $CPU --perf --seq --fresh 3 --flush --sizes 8M
fi

# ---- G2. core OCM chase, dense, sparse and vary, from CPUs 2 and 8 ----
# Dense per region: run 2's row (fresh=3, two timed passes of 4,096 hops), which counted 0.76 to
# 0.92 L3 read refills per load (best pass of each of the 20 CPU-region cells). Sparse per
# region: run 3's row (fresh=8, seven timed passes of 64 hops), which counted 0.50 to 0.86 by the
# same selection, fewer. One reading, a hypothesis, is that a prefetcher lands on sparse members
# by carrying the fixed line offset from page to page. Vary: the sparse chain with the line
# changing per page, which tests the sensitivity to that offset (on boots A and B). The prefetch
# columns say which fills the prefetchers made; the -tlb rows from CPU 2 count the translation
# activity of the same three chains. CPU 2 sees the regions at 1 and 3 crosspoints, CPU 8 at
# 1, 3 and 5. The control is run 3's command, so its file compares directly.
if [ "$HAVE_OCM" = 1 ]; then
    for c in 2 8; do
        [ "$QUICK" = 1 ] && c=$CPU
        say "## G2. OCM latency (cpu $c)"
        run latency-control-cpu$c.csv "$LAT" --cpu $c --perf --no-thp --sizes 4K --loads 8K --reps 1
        for r in 0 1 2 3 4; do
            run latency-ocm-region$r-dense-cpu$c.csv "$LAT" --cpu $c --perf --mem ocm --ocm-regions $r --sizes 256K --fresh 3 --flush
            run latency-ocm-region$r-cpu$c.csv "$LAT" --cpu $c --perf --mem ocm --ocm-regions $r --sizes 256K --sparse --fresh 8 --flush
            run latency-ocm-region$r-vary-cpu$c.csv "$LAT" --cpu $c --perf --mem ocm --ocm-regions $r --sizes 256K --sparse-vary --fresh 8 --flush
        done
        run latency-ocm-all5-dense-cpu$c.csv "$LAT" --cpu $c --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --fresh 3 --flush
        run latency-ocm-all5-cpu$c.csv "$LAT" --cpu $c --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --sparse --fresh 8 --flush
        if [ "$c" = "$CPU" ]; then
            say "## G2. OCM latency, TLB counter set (cpu $c)"
            # the translation-miss stimulus that validates the TLB events: a random 32 MiB chain on 4 KiB
            # pages (8,192 pages, more than the L2 TLB holds), so every hop can miss both TLB levels and walk
            run latency-dram-tlb-cpu$c.csv "$LAT" --cpu $c --perf --events tlb --no-thp --no-hugetlb --fresh 3 --sizes 32M
            for r in 0 1 2 3 4; do
                run latency-ocm-region$r-dense-tlb-cpu$c.csv "$LAT" --cpu $c --perf --events tlb --mem ocm --ocm-regions $r --sizes 256K --fresh 3 --flush
                run latency-ocm-region$r-tlb-cpu$c.csv "$LAT" --cpu $c --perf --events tlb --mem ocm --ocm-regions $r --sizes 256K --sparse --fresh 8 --flush
                run latency-ocm-region$r-vary-tlb-cpu$c.csv "$LAT" --cpu $c --perf --events tlb --mem ocm --ocm-regions $r --sizes 256K --sparse-vary --fresh 8 --flush
            done
            say "## G2. OCM latency, walk counter set (cpu $c)"
            # the walks split by the descriptor they ended in. On 4 KiB pages (boots A and B) every walk is
            # a page walk, so the same 32 MiB stimulus validates 0x818A and shows 0x8188 at zero. On boots C
            # and D a walk through a contiguous-hint entry counts as a block walk (nevis0_tlb_pagewalk.sv line
            # 3147), which says what the walker used, where the map lines say only what the kernel wrote. A
            # zero block count is unvalidated until 0x8188 has read nonzero on some row (no block stimulus
            # here). Sparse rows only: one member line per 4 KiB page, the shape with the most walks.
            run latency-dram-walk-cpu$c.csv "$LAT" --cpu $c --perf --events walk --no-thp --no-hugetlb --fresh 3 --sizes 32M
            for r in 0 1 2 3 4; do
                run latency-ocm-region$r-walk-cpu$c.csv "$LAT" --cpu $c --perf --events walk --mem ocm --ocm-regions $r --sizes 256K --sparse --fresh 8 --flush
            done
        fi
        [ "$QUICK" = 1 ] && break
    done
else
    say "## G2. OCM rows skipped: no /dev/ocm"
fi

# ---- G3, G4. the CME rows (cpu $CPU in streaming mode, CPU 3 offline) ----
# Run 3's CME sweep read 1.65 to 3.43 outer refills per load at 8 KiB and up to 16.9 near 128 to
# 256 KiB, more than one line fill per load. The new columns say whether those fills were
# loads (demand), the CME prefetcher's, or stores (none expected). The sequential chain is the
# CME prefetcher's stimulus, read as in G1.
if [ "$HAVE_SME2" = 1 ]; then
    say "## G3. CME DRAM latency (cpu $CPU)"
    run cme-latency-dram-small.csv "$LAT" --cme --cpu $CPU --perf --loads 64K --reps 3 \
        --sizes 4K,8K,16K,24K,32K,48K,64K,96K,128K,192K,256K,384K
    if [ "$QUICK" = 1 ]; then
        run cme-latency-dram-large.csv "$LAT" --cme --cpu $CPU --perf --fresh 3 --sizes 512K,1M,2M,4M
        run cme-latency-dram-flushed.csv "$LAT" --cme --cpu $CPU --perf --fresh 3 --flush --sizes 2M
        run cme-latency-dram-seq.csv "$LAT" --cme --cpu $CPU --perf --seq --fresh 3 --flush --sizes 2M
    else
        run cme-latency-dram-large.csv "$LAT" --cme --cpu $CPU --perf --fresh 3 \
            --sizes 512K,768K,1M,1.5M,2M,3M,4M,6M,8M,16M,32M
        run cme-latency-dram-flushed.csv "$LAT" --cme --cpu $CPU --perf --fresh 3 --flush --sizes 8M
        run cme-latency-dram-seq.csv "$LAT" --cme --cpu $CPU --perf --seq --fresh 3 --flush --sizes 8M
    fi
    if [ "$HAVE_OCM" = 1 ]; then
        say "## G4. CME OCM latency (cpu $CPU)"
        for r in 0 1 2 3 4; do
            run cme-latency-ocm-region$r-dense.csv "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions $r --sizes 256K --fresh 3 --flush
            run cme-latency-ocm-region$r.csv "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions $r --sizes 256K --sparse --fresh 8 --flush
            run cme-latency-ocm-region$r-vary.csv "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions $r --sizes 256K --sparse-vary --fresh 8 --flush
        done
        run cme-latency-ocm-all5-dense.csv "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --fresh 3 --flush
        run cme-latency-ocm-all5.csv "$LAT" --cme --cpu $CPU --perf --mem ocm --ocm-regions 0,1,2,3,4 --sizes 1280K --sparse --fresh 8 --flush
    fi
else
    say "## G3, G4. CME rows skipped: no sme2 in /proc/cpuinfo"
fi

# ---- G5. the per-pass cost of the write rows: run 3's program beside run 2's ----
# Run 3's one-core write rows of 16 to 256 KiB read 7.7 to 8.0 ns more per pass than run 2's
# (asimov-c1nano-perf.html, table 4.3 and section 11). The hypothesis is the CME switch run 3 added
# to the bandwidth program's pass loop. The same rows with both programs on one boot decide it:
# if the difference is the program, bandwidth_r2 reads run 2's numbers and bandwidth_r3 run 3's.
# bandwidth_r3 is run 3's bandwidth.c (ba834c0) kept verbatim: run 5 changed bandwidth.c, so the
# current binary is not run 3's program. The G5 rows run two preserved programs, not the current one.
say "## G5. one-core write rows, run 3 program and run 2 program (cpu $CPU)"
for sp in 16K:256 48K:128 96K:64 256K:32; do
    s=${sp%%:*}; p=${sp##*:}
    run bandwidth-write-small.csv "$BW3" --threads 1 --cpus $CPU --bytes $s --kernel write --passes $p --reps 3
    run bandwidth-write-small-r2prog.csv "$BW2" --threads 1 --cpus $CPU --bytes $s --kernel write --passes $p --reps 3
done

# ---- G6. the route of each CPU-to-node pair: crosspoint data flits by direction ----
# The per-cell OCM latencies at one crosspoint count spread by up to 16 % in runs 2 and 3, and a
# fit of the 20 cells attributes none of it to the node or the CPU (asimov-c1nano-perf.html,
# section 11). The hop count assumes a Manhattan route. This phase counts, for each cell, the data
# flits that the crosspoint of the CPU's DSU and the crosspoint of the home node transmit on each of
# their four mesh directions (arm-cmn XP events mxp_<dir>_dat_txflit_valid, dir in e w n s), over the
# whole dense chase command: the outgoing data directions at both ends. These counts include the
# chain build, the flush write-backs and unrelated traffic; read requests travel on the request
# channel (mxp_<dir>_req_txflit_valid, not counted here), so they do not identify the read-request
# route or the read response's final link. The hop counts stay coordinate assumptions. Eight counters per command, which is the CMN's DTC counter count, so no
# multiplexing is expected; cmncount flags any. The event names are the arm-cmn driver's and are
# not verified on this image: if one is absent the chase still runs, cmncount writes a '# cmncount:'
# line, and G0's list of event names says what the image offers. XP node id = 64 x + 8 y.
XP_EVENTS="mxp_e_dat_txflit_valid,mxp_w_dat_txflit_valid,mxp_n_dat_txflit_valid,mxp_s_dat_txflit_valid"
xp() { # xp <label> <xp node ids> <cmd...>: cmncount with the four direction events on the listed crosspoints
    l="$1"; n="$2"; shift 2
    "$CMN" --out "$CMNOUT" --label "$l" --nodes "$n" --events "$XP_EVENTS" -- "$@"
}
if [ "$HAVE_OCM" = 1 ]; then
    say "## G6. route flits: dense per-region chase from CPUs 2, 4, 6, 8 under the crosspoint counters"
    for c in 2 4 6 8; do
        case $c in 2) dsu_xp=152 ;; 4) dsu_xp=208 ;; 6) dsu_xp=280 ;; 8) dsu_xp=336 ;; esac   # XP (2,3) (3,2) (4,3) (5,2)
        for r in 0 1 2 3 4; do
            case $r in 0) node_xp=88 ;; 1) node_xp=160 ;; 2) node_xp=216 ;; 3) node_xp=288 ;; 4) node_xp=344 ;; esac   # XP (1,3) (2,4) (3,3) (4,4) (5,3)
            run latency-ocm-region$r-route-cpu$c.csv xp "route-ocm$r-cpu$c" "$dsu_xp,$node_xp" "$LAT" --cpu $c --perf --mem ocm --ocm-regions $r --sizes 256K --fresh 3 --flush
        done
        [ "$QUICK" = 1 ] && break
    done
else
    say "## G6. route rows skipped: no /dev/ocm"
fi

# ---- G7. one-core streaming read: software prefetch distances and two streams, under the counters ----
# Run 3's one-core read reads 9.2 GB/s at 2 MiB and 32 MiB against 58 GB/s at 16 KiB, with 3.5 to 7.5
# lines in flight by Little's law (asimov-c1nano-perf.html, table 4.3). The hypothesis is that the
# plain ldp loop does not keep enough lines in flight: a streaming read depends on the hardware
# prefetcher running ahead of it. The rows here are the plain read loop (the control: run 3 ran it
# under --kernel all, here it is --kernel read plus --perf), the same loop with a PRFM PLDL1KEEP on
# every line 4, 8 and 16 lines ahead, and the loop over two streams. The prefetch columns say whether the hardware prefetcher fills lines on the
# plain row (0x81BC, 0x81BD, 0x0111 per line) and how the software hints change the refill mix.
say "## G7. one-core read, software prefetch and two streams, prefetch counters (cpu $CPU)"
for sp in 2M:4 32M:2; do
    s=${sp%%:*}; p=${sp##*:}
    for k in read read-pf4 read-pf8 read-pf16 read-2s; do
        run bandwidth-read-prefetch.csv "$BW" --threads 1 --cpus $CPU --bytes $s --kernel $k --passes $p --reps 3 --perf
    done
    [ "$QUICK" = 1 ] && break
done

# ---- G8. the L1 rows: per-pass cost by size, and with the pass loop inside the assembly ----
# Run 3's 16 KiB read row (58.1 GB/s) is 91 % of the core's two 128-bit load pipes at 2 GHz (64 GB/s).
# A 16 KiB pass is 512 cycles of loads; the row reads 564 cycles per pass, 52 cycles over. The boot A
# write sweep (G5) fits a fixed 21 to 30 ns per pass. Four sizes fit the same line for read and write,
# and the --asm-passes form replaces the default pass machinery (call, return, register save, the
# per-pass dispatch and accounting) with the assembly's own loop step, so each form's fit gives its own
# fixed cost per pass. Passes keep about 4 MiB per rep. 5 reps, because on the CRB (21 reps, two cores)
# the --asm-passes read rows at 16 KiB had their best rep at the pipe limit and medians 4 to 10 % under
# it, and the best rep is the number. 24 KiB is the largest size used: a 32 KiB buffer would share the
# 32 KiB L1's sets with the stack and the harness's own lines.
say "## G8. one-core L1 rows, 4 to 24 KiB, per-pass form and --asm-passes form (cpu $CPU)"
for sp in 4K:1024 8K:512 16K:256 24K:176; do
    s=${sp%%:*}; p=${sp##*:}
    for k in read write; do
        run bandwidth-l1-passes.csv "$BW" --threads 1 --cpus $CPU --bytes $s --kernel $k --passes $p --reps 5 --perf
        run bandwidth-l1-asmpasses.csv "$BW" --threads 1 --cpus $CPU --bytes $s --kernel $k --passes $p --reps 5 --perf --asm-passes
    done
    [ "$QUICK" = 1 ] && break
done

cpu_set_online "$OFFLINE_CPU" 1
[ "$DRY" = 1 ] && exit 0
finish_suite run_followup.sh
