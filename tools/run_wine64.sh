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
