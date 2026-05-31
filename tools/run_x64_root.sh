#!/bin/bash
# Run a 64-bit guest program through the FULL Boxedwine kernel against the
# tools/rootfs64/root glibc rootfs, headless (-novideo). This exercises the
# DYNAMIC-linking path (real ld-linux-x86-64.so.2 + libc.so.6), unlike
# tools/run_x64_smoke.sh which uses the bare --x64-run-elf runner (no FS).
#
# Usage: tools/run_x64_root.sh /bin/hello_glibc [args...]
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GUEST_ROOT="$ROOT_DIR/tools/rootfs64/root"
CANDIDATES=()
for p in "$HOME/Library/Developer/Xcode/DerivedData"/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine; do
    [ -x "$p" ] && CANDIDATES+=("$p")
done
[ "${#CANDIDATES[@]}" -gt 0 ] || { echo "error: no Boxedwine build found" >&2; exit 1; }
BOX="$(ls -t "${CANDIDATES[@]}" | head -1)"
GUEST="${1:?usage: run_x64_root.sh <guestPath> [args...]}"; shift || true
echo "using: $BOX"
echo "root:  $GUEST_ROOT"
echo "guest: $GUEST $*"
"$BOX" -root "$GUEST_ROOT" -novideo "$GUEST" "$@" 2>&1 \
    | grep -avE "pixel format|redundant|new pixel|failed to choose|software renderer|Number which|Number of"
