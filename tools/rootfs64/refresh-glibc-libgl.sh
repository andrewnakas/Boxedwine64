#!/usr/bin/env bash
# refresh-glibc-libgl.sh — put the current guest OpenGL shims (both arches) into
# glibc-rootfs64.zip, the small (~9 MB) zip mounted FIRST.
#
# Why this works: zip mounts are first-wins (fszip.cpp skips a path an earlier
# zip already registered), and the launcher mounts glibc-rootfs64.zip before
# wine64.zip and wine32.zip. So a shim change (tools/rootfs64/build-libgl64.sh)
# ships by re-uploading this one small zip instead of the ~200 MB wine64/wine32
# rootfs parts that also carry (now shadowed) copies.
#
# Usage: tools/rootfs64/refresh-glibc-libgl.sh [path/to/glibc-rootfs64.zip]
# Default: tools/rootfs64/dist/glibc-rootfs64.zip (updated in place).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIP="$(cd "$(dirname "${1:-$HERE/dist/glibc-rootfs64.zip}")" && pwd)/$(basename "${1:-$HERE/dist/glibc-rootfs64.zip}")"
[ -f "$ZIP" ] || { echo "error: $ZIP missing" >&2; exit 1; }
for f in "$HERE/libgl64/libGL.so.1" "$HERE/libgl32/libGL.so.1"; do
    [ -f "$f" ] || { echo "error: $f missing (run build-libgl64.sh)" >&2; exit 1; }
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/lib/x86_64-linux-gnu" "$STAGE/usr/lib/x86_64-linux-gnu" "$STAGE/usr/lib/i386-linux-gnu"
cp "$HERE/libgl64/libGL.so.1" "$STAGE/lib/x86_64-linux-gnu/libGL.so.1"
cp "$HERE/libgl64/libGL.so.1" "$STAGE/usr/lib/x86_64-linux-gnu/libGL.so.1"
cp "$HERE/libgl32/libGL.so.1" "$STAGE/usr/lib/i386-linux-gnu/libGL.so.1"
(cd "$STAGE" && zip -q "$ZIP" lib/x86_64-linux-gnu/libGL.so.1 \
    usr/lib/x86_64-linux-gnu/libGL.so.1 usr/lib/i386-linux-gnu/libGL.so.1)
unzip -l "$ZIP" | grep libGL.so.1
echo "updated $ZIP ($(wc -c < "$ZIP" | tr -d ' ') bytes)"
