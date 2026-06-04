#!/bin/bash
# sym_bug2.sh 0x<vaddr> [0x<vaddr> ...]
# Map a wineserver64 file-vaddr (PIE-relative; i.e. caller-PIE from REFWATCH,
# which already subtracts the 0x400000000 load base) to the disassembly around
# it. The binary is stripped, so there are no function names — but the
# instructions just before a return address show the call, and nearby
# __assert_fail call sites carry "server/<file>.c:<line>" via their leaq operands,
# which pins the source file/function.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WS="$ROOT_DIR/tools/rootfs64/dist/stage/usr/lib/wine/wineserver64"
[ -f "$WS" ] || { echo "no wineserver64 at $WS" >&2; exit 1; }

ASM=/tmp/ws64.asm
if [ ! -f "$ASM" ] || [ "$WS" -nt "$ASM" ]; then
    echo "(disassembling $WS -> $ASM)" >&2
    objdump -d "$WS" > "$ASM"
fi

for A in "$@"; do
    # Normalize: strip a 0x400000000 base if the user pasted the absolute addr.
    v=$(python3 - "$A" <<'PY'
import sys
a=int(sys.argv[1],0)
if a>=0x400000000: a-=0x400000000
print(hex(a))
PY
)
    hexv=${v#0x}
    echo "================ $A  (file-vaddr $v) ================"
    # objdump lines look like "   30164: e8 .. callq ...". The return address
    # points to the instruction AFTER a call, so show a window before it.
    line=$(grep -nE "^[[:space:]]+${hexv}:" "$ASM" | head -1 | cut -d: -f1)
    if [ -z "$line" ]; then
        echo "  (exact address not an instruction boundary; showing nearest below)"
        # find the greatest instruction addr <= target
        line=$(awk -v t=$((16#$hexv)) '
            match($0,/^[[:space:]]+([0-9a-f]+):/,m){
                a=strtonum("0x" m[1]); if(a<=t && a>best){best=a; bl=NR}
            } END{print bl}' "$ASM")
    fi
    [ -z "$line" ] && { echo "  not found"; continue; }
    sed -n "$((line>12?line-12:1)),$((line+2))p" "$ASM"
done
