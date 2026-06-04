#!/bin/bash
# loop_bug2_refwatch.sh [N]
# Run the bug #2 repro up to N times (default 12), stopping as soon as REFWATCH
# traps the release_object double-release. The crash has three faces (refcount
# assert / corrupted double-linked list / unaligned tcache) — only the refcount
# face routes through release_object where REFWATCH fires, so we may need a few
# tries to land on it. Restores the committed prefix between runs.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-12}"
for i in $(seq 1 "$N"); do
    echo "===== attempt $i/$N ====="
    git -C "$ROOT_DIR" checkout -- tools/rootfs64/root/home/username/.wine/ 2>/dev/null || true
    pkill -9 -f 'Boxedwine.app/Contents/MacOS/Boxedwine' 2>/dev/null || true
    LOG="/tmp/bug2_refwatch_$i.log"
    "$ROOT_DIR/tools/repro_bug2_refwatch.sh" > "$LOG" 2>&1
    face=$(grep -oE "Assertion .obj->refcount.|malloc\(\): [a-z ]*(tcache|double linked list|chunk)[a-z ]*|corrupted [a-z ]*list[a-z ]*" "$LOG" | head -1)
    if grep -q "DOUBLE-RELEASE" "$LOG"; then
        echo ">>> REFWATCH TRAPPED on attempt $i (face: ${face:-?}) — log: $LOG"
        grep -E "REFWATCH:" "$LOG"
        exit 0
    fi
    echo "attempt $i: no refcount trap (face: ${face:-<no crash / other>})"
done
echo "no REFWATCH trap in $N attempts — the crash kept taking the malloc-metadata face."
echo "logs: /tmp/bug2_refwatch_*.log"
exit 1
