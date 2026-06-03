#!/bin/bash
# build-wine64-zip.sh — assemble the x86_64 Wine + glibc rootfs zips that
# Boxedwine64 mounts to run wine64 headless. Produces two layered zips that
# FsZip mounts at "/" (later layers override earlier):
#
#   glibc-rootfs64.zip   base: glibc dynamic linker + libc + wine64's lib deps
#   wine64.zip           overlay: the wine64 loader, wineserver, and the
#                        x86_64-unix / x86_64-windows Wine module trees
#
# At least one path in each trips FsZip::guestIs64 (x86_64-linux-gnu/, /lib64/,
# x86_64-unix/, x86_64-windows/) so the launcher treats the guest as 64-bit.
#
# The heavy apt-get is done ONCE into a committed image
# (boxedwine64/wine64-debian:bookworm); this script only stages + zips from it,
# so it is fast and repeatable. To rebuild the image from scratch:
#
#   docker run --name wine64build --platform linux/amd64 debian:bookworm sh -c \
#     'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && \
#      apt-get install -y -qq wine64'
#   docker commit wine64build boxedwine64/wine64-debian:bookworm
#   docker rm wine64build
#
# Output zips land in tools/rootfs64/dist/ (gitignored — 600MB+, build artifact).
set -euo pipefail

IMAGE="boxedwine64/wine64-debian:bookworm"
HERE="$(cd "$(dirname "$0")" && pwd)"
DIST="$HERE/dist"
mkdir -p "$DIST"

echo "=== staging wine64 rootfs from $IMAGE (linux/amd64) ==="

STAGEHOST="$DIST/stage"
rm -rf "$STAGEHOST"; mkdir -p "$STAGEHOST"

# Run a container that stages a clean guest "/" tree into the mounted volume,
# computing the full runtime dependency closure of the wine64 loader,
# wineserver, and every x86_64-unix module via ldd, so the guest ld.so resolves
# every DT_NEEDED. We stage in the container (Linux paths/symlinks) but ZIP on
# the macOS host afterward (the image has no `zip`; it does have python3, but
# host zip keeps the artifact build off the slow emulated container).
docker run --rm --platform linux/amd64 -v "$DIST":/dist "$IMAGE" bash -c '
set -euo pipefail
STAGE=/dist/stage
mkdir -p "$STAGE"

# Collect the dependency closure: start from the wine64 ELF binaries + unix
# modules, ldd each, and copy every resolved host library (dedup via sort -u).
seeds=(
  /usr/lib/wine/wine64
  /usr/lib/wine/wineserver64
  /lib64/ld-linux-x86-64.so.2
)
for so in /usr/lib/x86_64-linux-gnu/wine/x86_64-unix/*.so; do seeds+=("$so"); done

# Resolve the transitive .so closure.
libs=$(
  for f in "${seeds[@]}"; do
    ldd "$f" 2>/dev/null | awk "/=>/ {print \$3} /ld-linux/ {print \$1}"
  done | grep -E "^/" | sort -u
)

copy() {  # copy a file preserving its absolute path under $STAGE, deref symlinks
  local src="$1"
  [ -e "$src" ] || return 0
  local real; real="$(readlink -f "$src")"
  local dstdir="$STAGE$(dirname "$src")"
  mkdir -p "$dstdir"
  cp -aL "$real" "$STAGE$src"
}

echo "--- copying glibc + lib closure ---"
for l in $libs; do copy "$l"; done
copy /lib64/ld-linux-x86-64.so.2
copy /lib/x86_64-linux-gnu/libc.so.6
# libgcc_s.so.1 is dlopen'd LAZILY by glibc for stack unwinding on pthread_exit,
# so ldd never lists it and it gets dropped — without it, every wine thread that
# exits aborts with "libgcc_s.so.1 must be installed for pthread_exit to work",
# which cascades into wineserver heap corruption during the registry save.
copy /lib/x86_64-linux-gnu/libgcc_s.so.1
copy /usr/lib/x86_64-linux-gnu/libgcc_s.so.1
# libfreetype: wine probes it for font rendering ("Wine cannot find the FreeType
# font library"); harmless to omit but cheap to ship.
copy /usr/lib/x86_64-linux-gnu/libfreetype.so.6
mkdir -p "$STAGE/etc"; : > "$STAGE/etc/ld.so.cache"

echo "--- copying wine64 loader + wineserver + module trees ---"
copy /usr/lib/wine/wine64
copy /usr/lib/wine/wineserver64
copy /usr/lib/wine/wineserver
copy /usr/lib/wine/wineapploader
mkdir -p "$STAGE/usr/lib/x86_64-linux-gnu/wine"
cp -aL /usr/lib/x86_64-linux-gnu/wine/x86_64-unix    "$STAGE/usr/lib/x86_64-linux-gnu/wine/"
cp -aL /usr/lib/x86_64-linux-gnu/wine/x86_64-windows "$STAGE/usr/lib/x86_64-linux-gnu/wine/"
# i386-windows is the 32-bit PE side; not needed for a pure wine64 boot.

# Wine data: NLS locale tables (wineserver loads l_intl.nls), fonts, etc.
# wine resolves these as <bindir>/../../share/wine, i.e. /usr/share/wine.
mkdir -p "$STAGE/usr/share"
cp -aL /usr/share/wine "$STAGE/usr/share/"

# Convenience launch symlinks at canonical wine paths.
mkdir -p "$STAGE/usr/bin"
ln -sf /usr/lib/wine/wine64      "$STAGE/usr/bin/wine64"
ln -sf /usr/lib/wine/wineserver64 "$STAGE/usr/bin/wineserver"

# A minimal HOME so wine has somewhere to create its prefix.
mkdir -p "$STAGE/root"

# Standard writable scratch dirs a Linux rootfs must have. wineserver creates
# its socket tmpdir under /tmp (falling back from /run/user/<uid>), and wine
# chdir/stat the prefix HOME. Without these the guest mkdir(/tmp/wine-XXX)
# fails ENOENT (no parent) and wineserver setup aborts. Ship them in the zip so
# the parent node always exists; the kernel overlays the writable native root
# on top for the actual file creation.
# HOME defaults to /home/username (the writable-prefix path the kernel's
# getMode() allows); it must exist so wine can create $HOME/.wine and the
# dosdevices/c: symlink (under /winePrefix those symlinks fail EACCES).
mkdir -p "$STAGE/tmp" "$STAGE/run/user/1000" "$STAGE/home/username" "$STAGE/var/tmp"
# /etc/localtime: wine/glibc reads this as a TZif file on every time op AND
# RtlQueryTimeZoneInformation re-reads it from its idle loop. It must be a
# VALID TZif2 file — a v1 block followed by a v2 block ending in a "\nUTC0\n"
# POSIX footer — or glibc __tzfile_read keeps rejecting it and wine never
# settles. A bare `ln -sf /usr/share/zoneinfo/UTC` produces a DANGLING symlink
# (zoneinfo isn't shipped in the guest) and the container's own UTC file is a
# truncated 57-byte stub, so embed a known-good 114-byte UTC TZif inline.
printf %s \
  "VFppZjIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAQAAAAAAABVVEMAVFppZjIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAQAAAAAAABVVEMAClVUQzAK" \
  | base64 -d > "$STAGE/etc/localtime"

echo "--- staged tree ready ---"
du -sh "$STAGE"
'

# Stage our custom guest libGL.so.1 (the 64-bit OpenGL shim wine dlopens; it
# traps gl*/glX* to the Boxedwine64 host — see source/opengl/gl64bridge*). It's a
# freestanding prebuilt artifact (tools/rootfs64/build-libgl64.sh) copied in on
# the host AFTER the container stages glibc, so wine's loader finds libGL.so.1 on
# the standard path and 3D apps don't hit "GLX is missing, disabling OpenGL".
LIBGL_PREBUILT="$HERE/libgl64/libGL.so.1"
if [ -f "$LIBGL_PREBUILT" ]; then
    echo "--- staging custom guest libGL.so.1 ---"
    mkdir -p "$STAGEHOST/lib/x86_64-linux-gnu" "$STAGEHOST/usr/lib/x86_64-linux-gnu"
    cp "$LIBGL_PREBUILT" "$STAGEHOST/lib/x86_64-linux-gnu/libGL.so.1"
    # also drop a copy on the usr path some wine builds search first
    cp "$LIBGL_PREBUILT" "$STAGEHOST/usr/lib/x86_64-linux-gnu/libGL.so.1"
else
    echo "WARNING: $LIBGL_PREBUILT missing — run tools/rootfs64/build-libgl64.sh first;" \
         "wine64 will run but with OpenGL disabled." >&2
fi

echo "=== zipping on host (macOS zip preserves the symlinks) ==="
rm -f "$DIST/glibc-rootfs64.zip" "$DIST/wine64.zip"
# Base: glibc + deps. Overlay: wine trees + launchers. -y keeps symlinks as links.
( cd "$STAGEHOST" && zip -qry9 "$DIST/glibc-rootfs64.zip" lib64 lib etc tmp run home var )
( cd "$STAGEHOST" && zip -qry9 "$DIST/wine64.zip" usr root )
echo "=== zip sizes ==="
ls -lh "$DIST"/*.zip
echo "=== done. zips in $DIST ==="
