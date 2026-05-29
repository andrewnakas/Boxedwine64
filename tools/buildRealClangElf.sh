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
#   tools/buildRealClangElf.sh --shared libtiny  # builds testdata/libtiny.so
#   tools/buildRealClangElf.sh --link-against libtiny.so hello_dynlink
#       (links against testdata/libtiny.so during the final ld step;
#        produces hello_dynlink.elf with a DT_NEEDED entry pointing at
#        libtiny.so — runnable via $BOXEDWINE64_LIBPATH)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Parse optional flags.
MODE="exe"
LINK_AGAINST=()
while [ $# -gt 0 ]; do
    case "$1" in
        --shared)
            MODE="shared"
            shift ;;
        --link-against)
            LINK_AGAINST+=("$ROOT/tools/testdata/$2")
            shift 2 ;;
        *)
            break ;;
    esac
done

if [ $# -lt 1 ]; then
    echo "usage: $0 [--shared] [--link-against <lib.so>] <name> [out]" >&2
    exit 1
fi
NAME="$1"
SRC="$ROOT/tools/testdata/$NAME.c"
if [ "$MODE" = "shared" ]; then
    OUT="${2:-$ROOT/tools/testdata/$NAME.so}"
else
    OUT="${2:-$ROOT/tools/testdata/$NAME.elf}"
fi

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

clang -target x86_64-linux-gnu -nostdlib -fno-stack-protector -fPIC \
      -O2 -c -o "$TMP/$NAME.o" "$SRC"

if [ "$MODE" = "shared" ]; then
    # A DSO may itself depend on other DSOs (DT_NEEDED *of* a DSO). When
    # --link-against is passed for a shared build, link the named libs in
    # so the resulting .so gets its own DT_NEEDED entries. This is what
    # makes a two-hop dynamic chain (exe -> libA -> libB) verifiable.
    "$LD_LLD" -shared -nostdlib -soname="$NAME.so" -o "$OUT" \
              "$TMP/$NAME.o" "${LINK_AGAINST[@]}"
else
    # exe: dynamic if --link-against deps were given, else static.
    if [ ${#LINK_AGAINST[@]} -gt 0 ]; then
        "$LD_LLD" -nostdlib -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
                  -e _start -o "$OUT" "$TMP/$NAME.o" "${LINK_AGAINST[@]}"
    else
        "$LD_LLD" -static -nostdlib -e _start -o "$OUT" "$TMP/$NAME.o"
    fi
fi

chmod +x "$OUT"
echo "wrote $OUT ($(stat -f %z "$OUT") bytes)"
file "$OUT"
