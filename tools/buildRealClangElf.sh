#!/bin/bash
# Build a real-compiler static ELF probe for the cpu64 tracer.
# Generalised version of buildHelloRealElf.sh — takes a basename and
# builds tools/testdata/<name>.c into tools/testdata/<name>.elf.
#
# Why this script exists: every hand-coded discovery ELF
# (tools/build*Elf64.py) emits a tiny, hand-picked opcode set. Real
# compiler output exercises an unpredictable cross-section — the
# register allocator picks differently than a human, the optimizer
# folds loops, prologue/epilogue use opcodes we didn't think to add.
# Feeding a real .c into the emulator is the cheapest discovery
# probe we have for "what's the next opcode we're missing?".
#
# No libc, no glibc startup: each probe's _start calls sys_write/
# sys_exit via inline asm. The probes stay static-syscall-only —
# full libc startup lives behind Milestone A3 (real libc.so.6).
#
# Usage:
#   tools/buildRealClangElf.sh hello_real   # builds testdata/hello_real.{c,elf}
#   tools/buildRealClangElf.sh hello_wide
#   tools/buildRealClangElf.sh hello_wide /custom/out.elf

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ $# -lt 1 ]; then
    echo "usage: $0 <name> [out.elf]" >&2
    exit 1
fi
NAME="$1"
SRC="$ROOT/tools/testdata/$NAME.c"
OUT="${2:-$ROOT/tools/testdata/$NAME.elf}"

if [ ! -f "$SRC" ]; then
    echo "error: missing source: $SRC" >&2
    exit 1
fi

# Apple's ld64 only emits Mach-O; we need an ELF linker. Homebrew lld
# provides ld.lld which speaks the GNU ld dialect well enough.
LD_LLD="/opt/homebrew/opt/lld@21/bin/ld.lld"
if [ ! -x "$LD_LLD" ]; then
    LD_LLD="$(command -v ld.lld 2>/dev/null || true)"
fi
if [ -z "${LD_LLD:-}" ] || [ ! -x "$LD_LLD" ]; then
    echo "error: ld.lld not found (brew install lld)" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

clang -target x86_64-linux-gnu -nostdlib -fno-stack-protector \
      -O2 -c -o "$TMP/$NAME.o" "$SRC"

"$LD_LLD" -static -nostdlib -e _start -o "$OUT" "$TMP/$NAME.o"

chmod +x "$OUT"
echo "wrote $OUT ($(stat -f %z "$OUT") bytes)"
file "$OUT"
