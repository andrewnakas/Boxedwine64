#!/bin/bash
# repro_sleepprobe_sockdtor.sh — drive the deterministic "partial write" socket
# teardown bug under BW64_SOCKDTOR + BW64_WRITECLOSED, then surface the decisive
# SOCKCLOSE/SOCKDTOR/WRITECLOSED lines so we can read the use_count and decide
# model A (shared object, last-ref destruction) vs model B (peer-EOF cascade).
#
# sleepprobe.exe is a tiny 64-bit app that boots wine then does a Sleep/select;
# the bug kills it on that FIRST blocking write to wineserver:
#   WRITECLOSED: ... con=NULL connected=1   (live client, peer gone)
#   -> wine "partial write" / "wine client error"
#
# After the failure the emulator can spin at 100% CPU (surviving guest threads),
# so run detached, tail, kill on the failure signature, HARD_TIMEOUT cap.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RF="$ROOT_DIR/tools/rootfs64"
DIST="$RF/dist"
GLIBC_ZIP="$DIST/glibc-rootfs64.zip"
WINE_ZIP="$DIST/wine64.zip"
BASE_ROOT="$RF/root"

BOX="${BW64_BIN:-$ROOT_DIR/project/mac-xcode/build_dd/Build/Products/Release/Boxedwine.app/Contents/MacOS/Boxedwine}"
[ -x "$BOX" ] || { echo "error: Boxedwine build not found at $BOX (set BW64_BIN)" >&2; exit 1; }

GUEST="${1:-/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/sleepprobe.exe}"

# Clean transient wineserver state; keep the committed prefix (do NOT wipe .reg).
PREFIX="$BASE_ROOT/home/username/.wine"
rm -f "$PREFIX"/regf*.tmp 2>/dev/null || true
# Pin wine's prefix-update check off (literal "disable") so the run doesn't get
# the "Wine configuration is being updated" dialog; deleting it forces an update.
printf 'disable\n' > "$PREFIX/.update-timestamp" 2>/dev/null || true
find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
    ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true

echo "using: $BOX"
echo "armed: BW64_SOCKDTOR + BW64_WRITECLOSED"
echo "guest: $GUEST"

HARD_TIMEOUT="${HARD_TIMEOUT:-90}"
OUT="$(mktemp /tmp/sleepprobe_sockdtor.XXXXXX)"

BW64_SOCKDTOR=1 BW64_WRITECLOSED=1 "$BOX" \
    -root "$BASE_ROOT" -zip "$GLIBC_ZIP" -zip "$WINE_ZIP" -novideo \
    -env "HOME=/home/username" -env "USER=username" \
    -env "WINEPREFIX=/home/username/.wine" \
    -env "WINELOADER=/usr/lib/wine/wine64" \
    -env "WINESERVER=/usr/lib/wine/wineserver64" \
    -env "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine" \
    /usr/lib/wine/wine64 "$GUEST" > "$OUT" 2>&1 &
BOX_PID=$!

# The failure is a WRITECLOSED (the fatal write) and/or a wine client error.
fail_re='WRITECLOSED|partial write|wine client error|exit_group status=1'
start=$(date +%s)
while kill -0 "$BOX_PID" 2>/dev/null; do
    if grep -qE "$fail_re" "$OUT" 2>/dev/null; then
        sleep 2   # let the SOCKCLOSE/SOCKDTOR lines flush
        kill -9 "$BOX_PID" 2>/dev/null
        break
    fi
    if [ $(( $(date +%s) - start )) -ge "$HARD_TIMEOUT" ]; then
        echo "(repro: HARD_TIMEOUT ${HARD_TIMEOUT}s — no failure signature; killing)" >> "$OUT"
        kill -9 "$BOX_PID" 2>/dev/null
        break
    fi
    sleep 1
done
wait "$BOX_PID" 2>/dev/null

echo "================ DECISIVE LINES ================"
grep -nE 'SOCKCLOSE|SOCKDTOR|WRITECLOSED|partial write|wine client error' "$OUT" | tail -40
echo "================ (full log: $OUT) ================"
# keep $OUT for inspection
