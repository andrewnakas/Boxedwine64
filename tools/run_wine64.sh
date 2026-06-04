#!/bin/bash
# run_wine64.sh — launch a wine64 program headless through the full Boxedwine64
# kernel, layering the staged rootfs zips + the nss/prefix overlay so wine finds
# a populated WINEPREFIX, NSS files, and a valid TZ. The in-process X11 wire
# server (source/x11wire) answers winex11's display connection.
#
# Usage: tools/run_wine64.sh [guest-exe] [args...]
#   default guest-exe: notepad.exe (PE, launched via the wine64 loader)
#
# Env passthrough: set BW64_SCDUMP=1 / BW64_EPDUMP=1 before invoking to enable
# the socket-connect / epoll-spin diagnostics.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RF="$ROOT_DIR/tools/rootfs64"
DIST="$RF/dist"
GLIBC_ZIP="$DIST/glibc-rootfs64.zip"
WINE_ZIP="$DIST/wine64.zip"
OVERLAY="$RF/nss-overlay"
BASE_ROOT="$RF/root"

# Prefer the signing-disabled Release build we drive from the agent; fall back
# to any DerivedData Debug build.
BOX="$ROOT_DIR/project/mac-xcode/build_dd/Build/Products/Release/Boxedwine.app/Contents/MacOS/Boxedwine"
if [ ! -x "$BOX" ]; then
    for p in "$HOME/Library/Developer/Xcode/DerivedData"/Boxedwine-*/Build/Products/*/Boxedwine.app/Contents/MacOS/Boxedwine; do
        [ -x "$p" ] && BOX="$p"
    done
fi
[ -x "$BOX" ] || { echo "error: no Boxedwine build found" >&2; exit 1; }

GUEST_PE="${1:-/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/notepad.exe}"
shift || true

# The guest rootfs is a WRITABLE native overlay, so a crashed/incomplete run
# leaves wineserver's O_EXCL registry temp files (regf*.tmp) and per-boot
# server-1-XXX socket dirs behind. On the next launch wineserver's temp-create
# loops forever on EEXIST and eventually corrupts its own heap ("malloc():
# unsorted double linked list corrupted"), which kills the GUI bring-up before
# winex11 loads. Wipe that transient state so every run starts from the clean
# committed prefix.
PREFIX="$BASE_ROOT/home/username/.wine"
rm -f "$PREFIX"/regf*.tmp 2>/dev/null || true
# Pin wine's prefix-update check OFF. DELETING .update-timestamp (or leaving it
# "0") makes wineboot run a full `wineboot --update` every launch — the
# "The Wine configuration in … is being updated, please wait…" dialog. The
# committed prefix is already initialized, so write the literal "disable" that
# wine honors to skip the update (programs/wineboot/wineboot.c). Set
# BW64_FORCE_UPDATE=1 to allow the update (e.g. after a wine version bump).
if [ "${BW64_FORCE_UPDATE:-0}" = "1" ]; then
    rm -f "$PREFIX/.update-timestamp" 2>/dev/null || true
else
    printf 'disable\n' > "$PREFIX/.update-timestamp" 2>/dev/null || true
fi
find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
    ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true

# Self-heal an incomplete prefix. wineboot --init creates the registry +
# dosdevices symlinks AND the drive_c Windows skeleton (windows/system32,
# syswow64, ...). A prefix that has registry but no drive_c (e.g. an earlier
# partial commit) makes wine spin forever on open("dosdevices/c:") -> ENOENT
# because C:\ resolves to a non-existent target, and notepad never loads
# winex11. If drive_c is absent, run a one-shot --init before the real launch.
if [ ! -d "$PREFIX/drive_c/windows/system32" ]; then
    echo "prefix: drive_c missing -> running wineboot --init"
    "$BOX" \
        -root "$BASE_ROOT" -zip "$GLIBC_ZIP" -zip "$WINE_ZIP" -novideo \
        -env "HOME=/home/username" -env "USER=username" \
        -env "WINEPREFIX=/home/username/.wine" \
        -env "WINELOADER=/usr/lib/wine/wine64" \
        -env "WINESERVER=/usr/lib/wine/wineserver64" \
        -env "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine" \
        /usr/lib/wine/wine64 wineboot --init >/dev/null 2>&1 || true
    rm -f "$PREFIX"/regf*.tmp 2>/dev/null || true
    # Just initialized the prefix — pin the update check off so subsequent
    # launches don't re-run wineboot --update (the "being updated" dialog).
    [ "${BW64_FORCE_UPDATE:-0}" = "1" ] || printf 'disable\n' > "$PREFIX/.update-timestamp" 2>/dev/null || true
    find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
        ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true
fi

echo "using:   $BOX"
echo "glibc:   $GLIBC_ZIP"
echo "wine:    $WINE_ZIP"
echo "overlay: $OVERLAY"
echo "guest:   wine64 $GUEST_PE $*"

# Layer order (later overrides earlier): base root (with the nss/prefix overlay
# already merged in) -> glibc zip -> wine zip. The base root is the writable
# native tree, so wine can create $HOME/.wine/* and the wineserver socket.
"$BOX" \
    -root "$BASE_ROOT" \
    -zip "$GLIBC_ZIP" \
    -zip "$WINE_ZIP" \
    -novideo \
    -env "HOME=/home/username" \
    -env "USER=username" \
    -env "WINEPREFIX=/home/username/.wine" \
    -env "WINELOADER=/usr/lib/wine/wine64" \
    -env "WINESERVER=/usr/lib/wine/wineserver64" \
    -env "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine" \
    -env "WINEDEBUG=+x11drv,+win,+message" \
    -env "DISPLAY=:0" \
    /usr/lib/wine/wine64 "$GUEST_PE" "$@" 2>&1 \
    | grep -avE "pixel format|redundant|new pixel|failed to choose|software renderer|Number which|Number of"
