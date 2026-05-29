#!/bin/bash
# Run every on-disk x86-64 discovery ELF through the runner and assert the
# expected exit status. Lives outside --x64-selftest because the runner is
# its own entry point that the selftest doesn't exercise; this script
# catches regressions to runX64RunElf, the SysV stack builder, and the
# CPU/loader combo when fed real on-disk binaries.
#
# Usage:
#   tools/run_x64_smoke.sh [path/to/Boxedwine]
#
# Defaults to the Debug build under project/mac-xcode/.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Xcode writes into either project/mac-xcode/.../build (when build folder is
# colocated) or ~/Library/Developer/Xcode/DerivedData (the default). Pick the
# freshest binary across BOTH so we never run a stale in-tree binary after a
# DerivedData build (or vice versa). User-supplied path overrides everything.
if [ -n "${1:-}" ]; then
    BOXEDWINE="$1"
else
    # Build a candidate list and pick the freshest *existing* binary. Two
    # quirks we have to dodge here:
    #   1) Either path may not exist (in-tree was deleted; DerivedData was
    #      cleaned). `ls -t` returns non-zero in that case, and combined
    #      with set -euo pipefail the script exits silently with no error
    #      output, which is brutal to debug. We pre-filter to existing
    #      paths so `ls` never sees a missing one.
    #   2) The DerivedData entry is a glob; if no DerivedData build exists
    #      the literal pattern `Boxedwine-*/...` doesn't expand and trips
    #      `ls` on a bogus literal path.
    CANDIDATES=()
    INTREE="$ROOT/project/mac-xcode/Boxedwine/build/Debug/Boxedwine.app/Contents/MacOS/Boxedwine"
    if [ -x "$INTREE" ]; then
        CANDIDATES+=("$INTREE")
    fi
    for p in "$HOME/Library/Developer/Xcode/DerivedData"/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine; do
        if [ -x "$p" ]; then
            CANDIDATES+=("$p")
        fi
    done
    if [ "${#CANDIDATES[@]}" -gt 0 ]; then
        BOXEDWINE="$(ls -t "${CANDIDATES[@]}" | head -1)"
    else
        BOXEDWINE=""
    fi
fi

if [ -z "$BOXEDWINE" ] || [ ! -x "$BOXEDWINE" ]; then
    echo "error: Boxedwine binary not found (tried in-tree build and DerivedData)" >&2
    exit 1
fi
echo "using: $BOXEDWINE"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Run one canned binary and check the exit status the guest sys_exit
# reports (parsed from "CPU64: exit syscall, status=N" in stderr/stdout).
# The host process always exits 0; we care about the guest's status.
run_one() {
    local name="$1"
    local gen_script="$2"
    local expected_status="$3"

    local elf="$TMP/$name.elf"
    python3 "$gen_script" "$elf" >/dev/null
    local out
    out="$("$BOXEDWINE" --x64-run-elf "$elf" 2>&1 || true)"
    local got
    got="$(echo "$out" | grep -oE 'exit syscall, status=[0-9]+' | head -1 | grep -oE '[0-9]+$' || echo "?")"
    if [ "$got" = "$expected_status" ]; then
        printf "  PASS: %-24s (exit=%s)\n" "$name" "$got"
        return 0
    else
        printf "  FAIL: %-24s expected=%s got=%s\n" "$name" "$expected_status" "$got"
        echo "    --- output ---"
        echo "$out" | sed 's/^/    /'
        echo "    --- end ---"
        return 1
    fi
}

# Variant for prebuilt ELFs (real-compiler output). Same exit-check shape
# as run_one but skips the python3 gen step. Used for testdata/*.elf
# artifacts produced by tools/buildHelloRealElf.sh and friends — those
# binaries depend on clang+lld, not python, so we keep them in-tree
# rather than regenerating on every smoke run.
run_one_prebuilt() {
    local name="$1"
    local elf="$2"
    local expected_status="$3"

    if [ ! -f "$elf" ]; then
        printf "  SKIP: %-24s (missing: %s)\n" "$name" "$elf"
        return 0
    fi
    local out
    out="$("$BOXEDWINE" --x64-run-elf "$elf" 2>&1 || true)"
    local got
    got="$(echo "$out" | grep -oE 'exit syscall, status=[0-9]+' | head -1 | grep -oE '[0-9]+$' || echo "?")"
    if [ "$got" = "$expected_status" ]; then
        printf "  PASS: %-24s (exit=%s)\n" "$name" "$got"
        return 0
    else
        printf "  FAIL: %-24s expected=%s got=%s\n" "$name" "$expected_status" "$got"
        echo "    --- output ---"
        echo "$out" | sed 's/^/    /'
        echo "    --- end ---"
        return 1
    fi
}

echo "=== x64 runtime smoke tests ==="

fail=0
run_one hello          "$ROOT/tools/buildHelloElf64.py"        0          || fail=$((fail+1))
run_one pieReloc       "$ROOT/tools/buildPieRelocElf64.py"     5592471    || fail=$((fail+1))
run_one multiSegment   "$ROOT/tools/buildMultiSegmentElf64.py" 0          || fail=$((fail+1))
run_one callReturn     "$ROOT/tools/buildCallReturnElf64.py"   43         || fail=$((fail+1))
run_one loop           "$ROOT/tools/buildLoopElf64.py"         55         || fail=$((fail+1))
run_one stackString    "$ROOT/tools/buildStackStringElf64.py"  3          || fail=$((fail+1))
run_one strEq          "$ROOT/tools/buildStrEqElf64.py"        17         || fail=$((fail+1))
run_one scalarFp       "$ROOT/tools/buildScalarFpElf64.py"     8          || fail=$((fail+1))
run_one div            "$ROOT/tools/buildDivElf64.py"          135        || fail=$((fail+1))
run_one repMovsb       "$ROOT/tools/buildRepMovsbElf64.py"     13         || fail=$((fail+1))
run_one shiftBranch    "$ROOT/tools/buildShiftBranchElf64.py"  80         || fail=$((fail+1))
run_one arraySum       "$ROOT/tools/buildArraySumElf64.py"     36         || fail=$((fail+1))
run_one indirectCall   "$ROOT/tools/buildIndirectCallElf64.py" 119        || fail=$((fail+1))
run_one tlsImage       "$ROOT/tools/buildTlsImageElf64.py"     171        || fail=$((fail+1))
run_one integration    "$ROOT/tools/buildIntegrationElf64.py"  75         || fail=$((fail+1))
run_one relro          "$ROOT/tools/buildRelroElf64.py"        90         || fail=$((fail+1))
run_one_prebuilt helloReal   "$ROOT/tools/testdata/hello_real.elf"   98  || fail=$((fail+1))
run_one_prebuilt helloWide   "$ROOT/tools/testdata/hello_wide.elf"   242 || fail=$((fail+1))
run_one_prebuilt helloFp     "$ROOT/tools/testdata/hello_fp.elf"     45  || fail=$((fail+1))
# Float-formatting probe — Milestone C exit-criterion kernel: turn a double
# into a decimal string via repeated MULSD/SUBSD/CVTTSD2SI/CVTSI2SD (the
# arithmetic glibc's __printf_fp performs). Exits 6 (digit count); the
# decimal string is tee'd to host stdout. Last-place drift is correct
# IEEE-754 behaviour, not an emulator bug.
run_one_prebuilt helloFmt    "$ROOT/tools/testdata/hello_fmt.elf"    6   || fail=$((fail+1))
# REAL glibc 2.36 — a gcc-compiled, statically-linked glibc hello-world (not
# hand-crafted, not -nostdlib). Runs full __libc_start_main → malloc arena
# init → printf → exit(42), ~127K instructions. This is the Milestone D
# foundation: real glibc startup runs end-to-end. Exit 42. Guard against
# regressing the PUNPCKLQDQ / ENDBR64 / runner-brk fixes that unblocked it.
run_one_prebuilt glibcStatic "$ROOT/tools/testdata/hello_glibc_static.elf" 42 || fail=$((fail+1))
# Signal-delivery probe — installs SIGUSR1 handler, raises via tgkill, the
# handler writes a sentinel, sigreturn restores, main exits with sentinel.
# Exit 77 means the entire deliverSignalSync → handler → restorer → rt_sigreturn
# loop worked end-to-end. This is the closest reachable proof of Milestone B's
# signal-delivery primitive (clone is still ENOSYS pending KThread64).
run_one_prebuilt helloSignal "$ROOT/tools/testdata/hello_signal.elf" 77  || fail=$((fail+1))
# Multi-signal probe — two distinct handlers (SIGUSR1=10, SIGUSR2=12), each
# verifies it received the correct sig arg in RDI, two sequential
# deliver→handle→sigreturn cycles. Exit 22 (=10+12) proves sig-arg passing
# and sigreturn-restore generalize beyond the single-signal case.
run_one_prebuilt helloSig2   "$ROOT/tools/testdata/hello_sig2.elf"   22  || fail=$((fail+1))
# SSE3/SSSE3 vector-intrinsics probe — real clang emits PSHUFB/PALIGNR/
# PABSD/PHADDD/PSIGND; exits 42 if they all decode+run without tripping the
# unimpl-tracer. The opcodes' exact semantics are pinned separately by the
# cpu64SelfTest PABS/PHADD/PSIGN cases; this asserts the compiler-emitted
# forms decode end-to-end.
run_one_prebuilt helloVec    "$ROOT/tools/testdata/hello_vec.elf"    42  || fail=$((fail+1))
# Second vector probe — SSE3 FP-horizontal (HADDPS/HADDPD/MOVSHDUP/
# MOVSLDUP) + SSE4.1 packed dword (PMULLD/PMINSD). Exits 24 clean if all
# decode+run. PMULLD/PMINSD/PMAXSD/MOVSHDUP/MOVSLDUP semantics pinned by
# cpu64SelfTest; HADDPS/HADDPD covered by this probe's end-to-end run.
run_one_prebuilt helloVec2   "$ROOT/tools/testdata/hello_vec2.elf"   24  || fail=$((fail+1))

# Dynamic-link probe — exits with status from libtiny.so::tiny_compute(10).
# Requires BOXEDWINE64_LIBPATH to point at testdata/ so the in-tree
# fetcher in cpu64RunElf.cpp finds libtiny.so. This is the closest
# proof of Milestone A's exit criterion (real dynamic ELF, real
# DT_NEEDED resolution, real R_X86_64_JUMP_SLOT against a resolved
# symbol) inside the smoke matrix.
run_one_dynamic() {
    local name="$1"
    local elf="$2"
    local libpath="$3"
    local expected_status="$4"

    if [ ! -f "$elf" ]; then
        printf "  SKIP: %-24s (missing: %s)\n" "$name" "$elf"
        return 0
    fi
    local out
    out="$(BOXEDWINE64_LIBPATH="$libpath" "$BOXEDWINE" --x64-run-elf "$elf" 2>&1 || true)"
    local got
    got="$(echo "$out" | grep -oE 'exit syscall, status=[0-9]+' | head -1 | grep -oE '[0-9]+$' || echo "?")"
    if [ "$got" = "$expected_status" ]; then
        printf "  PASS: %-24s (exit=%s)\n" "$name" "$got"
        return 0
    else
        printf "  FAIL: %-24s expected=%s got=%s\n" "$name" "$expected_status" "$got"
        echo "    --- output ---"
        echo "$out" | sed 's/^/    /'
        echo "    --- end ---"
        return 1
    fi
}
run_one_dynamic helloDynlink "$ROOT/tools/testdata/hello_dynlink.elf" "$ROOT/tools/testdata" 125 || fail=$((fail+1))

# Two-hop dynamic chain — exe -> libchainA -> libchainB. Proves the flat
# symbol table resolves cross-DSO calls *between libraries* (libchainA
# imports chain_leaf from libchainB; the exe never references libchainB
# directly). chain_compute(5) = chain_leaf(5) + 5*2 = (5*7+3) + 10 = 48.
# A wrong exit status here means either DT_NEEDED-of-a-DSO didn't recurse
# or the flat symbol table missed leaf-DSO exports during A's relocation.
run_one_dynamic helloChain "$ROOT/tools/testdata/hello_chain.elf" "$ROOT/tools/testdata" 48 || fail=$((fail+1))

echo "=== summary: $((27 - fail))/27 passed ==="
exit $fail
