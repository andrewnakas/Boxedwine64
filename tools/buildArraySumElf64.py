#!/usr/bin/env python3
"""
Emit an ET_EXEC that allocates an 8-element int32 array on the stack,
fills it 1..8, then sums via an indexed-addressing loop with a
pointer that walks. Models the most common shape of inner loop in
real code: `for (i=0; i<n; i++) sum += arr[i];`.

Exercises indexed addressing modes (SIB byte), 32-bit memory loads,
zero-extension of EAX into RAX on add, and the dec-jnz countdown
pattern that GCC emits when it can choose its own loop counter.

Layout: PT_LOAD RWX at 0x400000, entry 0x4000B0.

Code:
  ; Build array of 8 int32 on the stack via two 8-byte pushes.
  ; Array order (low addr first after both pushes): [1, 2, 3, 4, 5, 6, 7, 8]
  ; Two qwords: low = (2<<32) | 1, high = (4<<32) | 3, etc.
  ;
  ; push high-to-low so RSP ends at [1, 2, 3, 4, 5, 6, 7, 8].
  ; Four pushes total: push (8|7<<32), push (6|5<<32), push (4|3<<32), push (2|1<<32)
  mov rax, q4   ; bytes for [7, 8] — push first, lands at highest addr
  push rax
  mov rax, q3   ; [5, 6]
  push rax
  mov rax, q2   ; [3, 4]
  push rax
  mov rax, q1   ; [1, 2]
  push rax
  ; rsi = &arr[0]
  mov rsi, rsp
  xor eax, eax              ; sum = 0
  xor ecx, ecx              ; i = 0
loop_top:
  add eax, [rsi + rcx*4]    ; sum += arr[i]
  inc rcx                   ; i++
  cmp rcx, 8                ; i < 8 ?
  jl loop_top
  ; sum = 1+2+...+8 = 36
  mov rdi, rax              ; status = 36
  mov eax, 60
  syscall

Expect: exit status 36.

Tests:
  - SIB byte with index and scale-4: ModRM=00_000_100, SIB=10_001_110 (scale=2 i.e. *4, index=rcx, base=rsi). Encoding: 03 04 8E
  - INC r/m REX.W on rcx
  - 32-bit ADD-from-memory (no REX) on eax — should NOT zero-extend
    eax into rax (no REX.W); but the `add eax, ...` reads a 32-bit
    value into an opcode that has REX-less ax form. We use 03 /r.
  - JL backward branch (7C rel8)
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    q1 = (1 | (2 << 32))   # arr[0]=1, arr[1]=2
    q2 = (3 | (4 << 32))
    q3 = (5 | (6 << 32))
    q4 = (7 | (8 << 32))

    code  = bytes.fromhex("48B8") + struct.pack("<Q", q4)
    code += bytes.fromhex("50")
    code += bytes.fromhex("48B8") + struct.pack("<Q", q3)
    code += bytes.fromhex("50")
    code += bytes.fromhex("48B8") + struct.pack("<Q", q2)
    code += bytes.fromhex("50")
    code += bytes.fromhex("48B8") + struct.pack("<Q", q1)
    code += bytes.fromhex("50")
    code += bytes.fromhex("4889E6")              # mov rsi, rsp
    code += bytes.fromhex("31C0")                # xor eax, eax
    code += bytes.fromhex("31C9")                # xor ecx, ecx
    loop_top = len(code)
    # add eax, [rsi + rcx*4]  → 03 04 8E
    #   opcode 03 (ADD r32, r/m32)
    #   ModRM=00_000_100 → reg=eax, mod=00, rm=100 (SIB follows)
    #   SIB=10_001_110 → scale=10 (*4), index=001 (rcx), base=110 (rsi)
    code += bytes.fromhex("03048E")
    code += bytes.fromhex("48FFC1")              # inc rcx
    code += bytes.fromhex("4883F908")            # cmp rcx, 8
    # jl loop_top
    after_jl = len(code) + 2
    disp = loop_top - after_jl
    assert -128 <= disp <= 127
    code += b"\x7C" + struct.pack("<b", disp)
    code += bytes.fromhex("4889C7")              # mov rdi, rax
    code += bytes.fromhex("B83C000000")          # mov eax, 60
    code += bytes.fromhex("0F05")

    file_size = CODE_OFF + len(code)
    elf = bytearray(file_size)

    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2; ident[5] = 1; ident[6] = 1
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        2, 0x3E, 1,
        LOAD_VADDR + CODE_OFF,
        PHDR_OFF, 0, 0,
        64, 56, 1, 0, 0, 0,
    )

    elf[PHDR_OFF:PHDR_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        1, 7, 0,
        LOAD_VADDR, LOAD_VADDR,
        file_size, file_size,
        0x1000,
    )

    elf[CODE_OFF:CODE_OFF+len(code)] = code
    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "arraySum64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
