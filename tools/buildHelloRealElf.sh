#!/bin/bash
# Build a real-compiler static ELF hello-world for the cpu64 tracer.
# Uses Apple clang (cross-targeting x86_64-linux-gnu) + Homebrew lld.
#
# Why this script exists: every hand-coded discovery ELF
# (tools/build*Elf64.py) emits a tiny, hand-picked opcode set. Real
# compiler output exercises an unpredictable cross-section — the
# register allocator picks differently than a human, the optimizer
# folds loops, prologue/epilogue use opcodes we didn't think to add.
# Feeding a real .c into the emulator is the cheapest discovery
# probe we have for "what's the next opcode we're missing?".
#
# No libc, no glibc startup: _start calls sys_write/sys_exit via
# inline asm. This makes the ELF a *static* probe that exercises
# only emulator + loader + raw syscall path — same shape as the
# hand-coded ELFs but with compiler-chosen instructions.
#
# Usage:
#   tools/buildHelloRealElf.sh [out.elf]
# Output defaults to tools/testdata/hello_real.elf.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/testdata/hello_real.c"
OUT="${1:-$ROOT/tools/testdata/hello_real.elf}"

if [ ! -f "$SRC" ]; then
    echo "error: missing source: $SRC" >&2
    exit 1
fi

# Locate Homebrew lld (Apple's ld64 doesn't emit ELF).
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
      -O2 -c -o "$TMP/hello_real.o" "$SRC"

"$LD_LLD" -static -nostdlib -e _start -o "$OUT" "$TMP/hello_real.o"

chmod +x "$OUT"
echo "wrote $OUT ($(stat -f %z "$OUT") bytes)"
file "$OUT"
