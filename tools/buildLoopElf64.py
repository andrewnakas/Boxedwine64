#!/usr/bin/env python3
"""
Emit a tiny x86-64 ELF that exercises a counted loop with conditional
branches — a stripped-down model of every `for (i=0; i<n; i++)` in real
code. Surfaces gaps in flag-setting arithmetic and conditional jumps.

Computes sum(1..10) = 55 and exits with that value.

Code (rip starts at vaddr 0x4000B0):
  xor eax, eax              ; sum = 0   (31 C0 — clears RAX zero-extending)
  mov ecx, 10               ; counter   (B9 0A 00 00 00)
loop_top:
  add rax, rcx              ; sum += counter (48 01 C8)
  dec rcx                   ; counter--      (48 FF C9)
  jnz loop_top              ; (75 fb — relative back -5)
  mov rdi, rax              ; status = sum   (48 89 C7)
  mov eax, 60               ; SYS_exit       (B8 3C 00 00 00)
  syscall                   ; (0F 05)

Expect: process exits with status 55.

Tests:
  - 31 C0    XOR EAX,EAX (clears flags, zero-extends to RAX)
  - B9       MOV r32, imm32 (no REX)
  - 48 01    ADD r64,r/m64 (REX.W ADD r/m, r reg variant)
  - 48 FF /1 DEC r/m (REX.W)
  - 75       JNZ rel8 (taken/not-taken decision based on DEC's ZF)
  - 48 89    MOV r/m, r (REX.W)
  - syscall machinery
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    code  = bytes.fromhex("31C0")            # xor eax, eax
    code += bytes.fromhex("B90A000000")      # mov ecx, 10
    # loop_top starts here (offset = 2 + 5 = 7)
    loop_top_off_in_code = len(code)
    code += bytes.fromhex("4801C8")          # add rax, rcx
    code += bytes.fromhex("48FFC9")          # dec rcx
    # jnz back to loop_top
    after_jmp_off = len(code) + 2            # JNZ is 2 bytes
    disp8 = loop_top_off_in_code - after_jmp_off
    assert -128 <= disp8 <= 127
    code += b"\x75" + struct.pack("<b", disp8)
    code += bytes.fromhex("4889C7")          # mov rdi, rax
    code += bytes.fromhex("B83C000000")      # mov eax, 60
    code += bytes.fromhex("0F05")            # syscall

    file_size = CODE_OFF + len(code)
    elf = bytearray(file_size)

    # Ehdr
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

    # Phdr[0] PT_LOAD
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
    out = sys.argv[1] if len(sys.argv) > 1 else "loop64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
