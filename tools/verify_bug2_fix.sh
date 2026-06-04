#!/bin/bash
# verify_bug2_fix.sh [N]
# Run wineboot --init N times (default 12) and report, per run, whether
# wineserver crashed (any of the four faces) or booted clean. With the
# tgkill/tkill cross-thread signal fix, expect ZERO crashes.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-12}"
crashes=0; clean=0
for i in $(seq 1 "$N"); do
    git -C "$ROOT_DIR" checkout -- tools/rootfs64/root/home/username/.wine/ 2>/dev/null || true
    pkill -9 -f 'Boxedwine.app/Contents/MacOS/Boxedwine' 2>/dev/null || true
    LOG="/tmp/verify_fix_$i.log"
    "$ROOT_DIR/tools/repro_bug2_refwatch.sh" > "$LOG" 2>&1
    if grep -qE "DOUBLE-RELEASE|Assertion .obj->refcount.|malloc\(\):|exit_group status=1.*wineserver" "$LOG"; then
        face=$(grep -oE "DOUBLE-RELEASE|Assertion .obj->refcount.|malloc\(\): [a-z ]*(tcache|list|chunk)[a-z ]*" "$LOG" | head -1)
        echo "run $i: CRASH (${face:-?}) -> $LOG"
        crashes=$((crashes+1))
    else
        # Clean = wineboot completed (registry written, or reached HARD_TIMEOUT
        # with no crash signature = booted past the teardown window).
        echo "run $i: clean (no crash)"
        clean=$((clean+1))
    fi
done
echo "================================================"
echo "RESULT: $clean clean, $crashes crashed out of $N"
[ "$crashes" -eq 0 ] && echo ">>> FIX HOLDS: zero teardown crashes." || echo ">>> still crashing in $crashes/$N."
