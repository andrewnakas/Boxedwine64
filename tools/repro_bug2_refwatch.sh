#!/bin/bash
# repro_bug2_refwatch.sh — drive the deterministic wineserver teardown crash
# (bug #2) under the BW64_REFWATCH refcount double-release canary.
#
# Bug #2: wineserver aborts during teardown with
#   server/object.c:443: release_object: Assertion `obj->refcount' failed
# (corrupted double-linked list / unaligned tcache are the same root surfacing
# in glibc). REFWATCH traps release_object's entry one instruction before the
# assert, while %rdi still names the dying object and [%rsp] still holds the
# CALLER — the exact wineserver call site that released a dead object.
#
# Output to look for:
#   REFWATCH: armed — release_object confirmed at 0x...
#   REFWATCH: *** DOUBLE-RELEASE *** pid=.. obj=0x.. refcount=0 ops=0x.. caller=0x.. (caller-PIE=0x..)
# Feed caller-PIE into:  tools/sym_bug2.sh 0x<caller-PIE>
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RF="$ROOT_DIR/tools/rootfs64"
DIST="$RF/dist"
GLIBC_ZIP="$DIST/glibc-rootfs64.zip"
WINE_ZIP="$DIST/wine64.zip"
BASE_ROOT="$RF/root"

# Use the signing-disabled REFWATCH build by default.
BOX="${BW64_BIN:-$ROOT_DIR/project/mac-xcode/build_refwatch/Build/Products/Release/Boxedwine.app/Contents/MacOS/Boxedwine}"
[ -x "$BOX" ] || { echo "error: REFWATCH build not found at $BOX (set BW64_BIN)" >&2; exit 1; }

# Force a fresh prefix every run so wineboot --init does the full teardown that
# trips bug #2 (the crash is in the boot-helper exit right after first-window-map
# / registry flush). Wipe transient wineserver state.
PREFIX="$BASE_ROOT/home/username/.wine"
rm -f "$PREFIX"/regf*.tmp "$PREFIX/.update-timestamp" 2>/dev/null || true
find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
    ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true

echo "using: $BOX"
echo "armed: BW64_REFWATCH + BW64_CRASHRING"

# After wineserver aborts, the parent emulator does NOT exit — surviving guest
# threads spin at 100% CPU forever. So run it detached, tail its output, and kill
# it as soon as the crash (any of the three faces) or the REFWATCH trap appears.
# Hard cap at HARD_TIMEOUT seconds so a no-crash run can't wedge the loop.
HARD_TIMEOUT="${HARD_TIMEOUT:-150}"
OUT="$(mktemp /tmp/bug2_refwatch_run.XXXXXX)"

BW64_REFWATCH=1 BW64_CRASHRING=1 BW64_WSBT=1 "$BOX" \
    -root "$BASE_ROOT" -zip "$GLIBC_ZIP" -zip "$WINE_ZIP" -novideo \
    -env "HOME=/home/username" -env "USER=username" \
    -env "WINEPREFIX=/home/username/.wine" \
    -env "WINELOADER=/usr/lib/wine/wine64" \
    -env "WINESERVER=/usr/lib/wine/wineserver64" \
    -env "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine" \
    /usr/lib/wine/wine64 wineboot --init > "$OUT" 2>&1 &
BOX_PID=$!

start=$(date +%s)
# Match every face: glibc malloc aborts all begin "malloc():", the wine assert is
# "Assertion", REFWATCH prints DOUBLE-RELEASE, and wineserver's failure exit is
# status=1. Broad on purpose — a missed signature wedges the run to HARD_TIMEOUT.
crash_re='DOUBLE-RELEASE|Assertion|malloc\(\):|tcache|corrupted|exit_group status=1.*wineserver'
while kill -0 "$BOX_PID" 2>/dev/null; do
    if grep -qE "$crash_re" "$OUT" 2>/dev/null; then
        # Give the abort path a beat to finish flushing CRASHRING/REFWATCH/WSBT.
        sleep 2
        kill -9 "$BOX_PID" 2>/dev/null
        break
    fi
    if [ $(( $(date +%s) - start )) -ge "$HARD_TIMEOUT" ]; then
        echo "(repro: HARD_TIMEOUT ${HARD_TIMEOUT}s — no crash signature; killing)" >> "$OUT"
        kill -9 "$BOX_PID" 2>/dev/null
        break
    fi
    sleep 1
done
wait "$BOX_PID" 2>/dev/null
cat "$OUT"
rm -f "$OUT"
