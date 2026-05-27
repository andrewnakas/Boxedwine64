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
    BOXEDWINE="$(ls -t \
        "$ROOT/project/mac-xcode/Boxedwine/build/Debug/Boxedwine.app/Contents/MacOS/Boxedwine" \
        "$HOME/Library/Developer/Xcode/DerivedData"/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine \
        2>/dev/null | head -1)"
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

echo "=== summary: $((13 - fail))/13 passed ==="
exit $fail
