#!/bin/bash
# sse4_diff.sh [BoxedwineBinary] — differential SSE3/SSSE3/SSE4 test (M20).
# Builds tools/testdata/sse4_diff.c as an x86_64-linux ELF (run under CPU64)
# and as an x86_64 macOS binary (run under Rosetta = reference), then diffs.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TD="$ROOT/tools/testdata"
BOX="${1:-$ROOT/project/mac-xcode/dd/Build/Products/Release/Boxedwine.app/Contents/MacOS/Boxedwine}"
LLD="/opt/homebrew/opt/lld@21/bin/ld.lld"; [ -x "$LLD" ] || LLD="$(command -v ld.lld)"
OUT="${TMPDIR:-/tmp}/sse4_diff.$$"; mkdir -p "$OUT"
clang -target x86_64-linux-gnu -O1 -msse4.2 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -c "$TD/sse4_diff.c" -o "$OUT/sse4_diff.o" || exit 1
"$LLD" -static -e _start "$OUT/sse4_diff.o" -o "$TD/sse4_diff.elf" || exit 1
clang -arch x86_64 -O1 -msse4.2 "$TD/sse4_diff.c" -o "$OUT/sse4_ref" || exit 1
arch -x86_64 "$OUT/sse4_ref" > "$OUT/ref.txt"
BW64_INSN_LIMIT=4000000000 "$BOX" --x64-run-elf "$TD/sse4_diff.elf" 2>&1 \
  | sed -n 's/^\[guest fd=1[^]]*\] //p' > "$OUT/cpu64.txt"
grep -a "unimpl" <(BW64_INSN_LIMIT=4000000000 "$BOX" --x64-run-elf "$TD/sse4_diff.elf" 2>&1) | head -3
if diff "$OUT/ref.txt" "$OUT/cpu64.txt" > "$OUT/diff.txt"; then
    echo "sse4_diff: PASS ($(grep -c . "$OUT/ref.txt") lines identical)"
else
    echo "sse4_diff: FAIL"; cat "$OUT/diff.txt" | head -60
fi
