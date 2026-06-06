#!/usr/bin/env bash
# split-rootfs.sh — split the large 64-bit rootfs zips into <100 MB chunks for
# GitHub Pages.
#
# Why this exists: the in-browser wine64 build (wasm64-mt, see
# project/emscripten/wine64.html) needs SharedArrayBuffer, which requires the
# page to be cross-origin-isolated (COOP same-origin + COEP require-corp).
# Under COEP require-corp the page can ONLY fetch subresources that are either
# same-origin OR send a Cross-Origin-Resource-Policy header. GitHub Pages cannot
# set response headers, and a GitHub *Release* asset (the obvious place to park a
# 196 MB file that's over Pages' 100 MB/file limit) is served from
# release-assets.githubusercontent.com WITHOUT any CORP/CORS header — so a
# cross-origin-isolated page is blocked from fetching it. Verified 2026-06-06.
#
# Fix: keep everything SAME-ORIGIN on Pages by splitting wine64.zip (196 MB) into
# parts under the 100 MB/file limit, deploy the parts alongside the page, and let
# the launcher fetch + concatenate them back into the original bytes in-browser
# before mounting into the WASM VFS. Same-origin => COEP require-corp is happy and
# no CORP header is needed at all. Total Pages site stays well under the 1 GB cap
# (~217 MB: wine64 parts + glibc 9 MB + prefix 2 MB + wasm 2.8 MB).
#
# Output (written next to the source zip, or to $OUT_DIR): for each input zip
# larger than the threshold, <name>.partNNN files plus a <name>.manifest.json the
# launcher reads to know how many parts there are and the expected total size.
# Small zips (glibc, prefix) are left whole and get a trivial 1-part manifest so
# the launcher can treat every zip uniformly.
#
# Usage:
#   tools/rootfs64/split-rootfs.sh [SRC_DIR] [OUT_DIR]
# Defaults: SRC_DIR = tools/rootfs64/dist, OUT_DIR = SRC_DIR/pages
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${1:-$HERE/dist}"
OUT_DIR="${2:-$SRC_DIR/pages}"

# Chunk size: 90 MiB, comfortably under GitHub's 100 MB/file hard limit (and under
# the 50 MB "you'll get a warning" soft limit is impossible for a 196 MB blob, so
# 90 MiB just minimizes the part count: 196 MB / 90 MiB = 3 parts).
CHUNK_BYTES=$((90 * 1024 * 1024))

# Zips the launcher mounts, in overlay order. Small ones pass through untouched.
ZIPS=(glibc-rootfs64.zip wine64.zip prefix64.zip)

mkdir -p "$OUT_DIR"

filesize() { # portable stat (macOS + Linux)
  stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

for zip in "${ZIPS[@]}"; do
  src="$SRC_DIR/$zip"
  if [ ! -f "$src" ]; then
    echo "split-rootfs: WARNING: $src not found, skipping" >&2
    continue
  fi
  total=$(filesize "$src")
  # Clear any stale parts for this zip from a previous run.
  rm -f "$OUT_DIR/$zip" "$OUT_DIR/$zip".part[0-9]* "$OUT_DIR/$zip.manifest.json"

  if [ "$total" -le "$CHUNK_BYTES" ]; then
    # Small enough to ship whole; copy through and emit a 1-part manifest so the
    # launcher's chunk path is uniform across all three zips.
    cp "$src" "$OUT_DIR/$zip"
    cat > "$OUT_DIR/$zip.manifest.json" <<EOF
{ "name": "$zip", "totalBytes": $total, "chunkBytes": $total, "parts": ["$zip"] }
EOF
    echo "split-rootfs: $zip ($total bytes) <= chunk size, shipped whole (1 part)"
    continue
  fi

  # Split into CHUNK_BYTES pieces named <zip>.partNNN (zero-padded, ordered).
  # `split -b` with a numeric suffix gives us partNNN ordering we can rebuild in
  # JS by sorting. -a 3 => 3-digit suffix (handles up to 1000 parts).
  ( cd "$OUT_DIR" && split -b "$CHUNK_BYTES" -d -a 3 "$src" "$zip.part" )

  parts=("$OUT_DIR/$zip".part[0-9]*)
  # Build the JSON parts array from the actual filenames (basename only).
  part_list=""
  sum=0
  for p in "${parts[@]}"; do
    b="$(basename "$p")"
    part_list="$part_list\"$b\", "
    sum=$((sum + $(filesize "$p")))
  done
  part_list="${part_list%, }"
  if [ "$sum" -ne "$total" ]; then
    echo "split-rootfs: ERROR: parts of $zip sum to $sum, expected $total" >&2
    exit 1
  fi
  cat > "$OUT_DIR/$zip.manifest.json" <<EOF
{ "name": "$zip", "totalBytes": $total, "chunkBytes": $CHUNK_BYTES, "parts": [ $part_list ] }
EOF
  echo "split-rootfs: $zip ($total bytes) -> ${#parts[@]} parts in $OUT_DIR"
done

echo "split-rootfs: done. Pages-ready assets in $OUT_DIR"
ls -la "$OUT_DIR"
