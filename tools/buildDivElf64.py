#!/usr/bin/env python3
"""
Emit an ET_EXEC that exercises unsigned 64-bit DIV and signed IDIV
through F7 /6 and F7 /7 — the same path printf's %u/%d hits when
formatting integers, and the same path glibc uses to compute object
sizes from byte-counts. Independent of any in-memory selftest.

Computes: (1000 / 7) + (-21 / 3) = 142 + (-7) = 135, exits 135.

Code layout (entry 0x4000B0):
  ; rax = 1000, rcx = 7, then DIV rcx → rax=quotient(142), rdx=rem(6)
  mov rax, 1000              ; 48 C7 C0 E8 03 00 00
  xor rdx, rdx               ; 48 31 D2 (clear high half for DIV)
  mov rcx, 7                 ; 48 C7 C1 07 00 00 00
  div rcx                    ; 48 F7 F1     — unsigned 128/64
  mov rbx, rax               ; 48 89 C3     — save quotient (142)
  ; rax = -21 (signed), rcx = 3, CQO then IDIV
  mov rax, -21               ; 48 C7 C0 EB FF FF FF (32-bit imm sign-extends)
  cqo                        ; 48 99        — sign-extend RAX→RDX:RAX
  mov rcx, 3                 ; 48 C7 C1 03 00 00 00
  idiv rcx                   ; 48 F7 F9     — signed 128/64
  ; rax = -7 (signed), rbx = 142; add to get 135
  add rax, rbx               ; 48 01 D8
  mov rdi, rax               ; 48 89 C7
  mov eax, 60                ; B8 3C 00 00 00
  syscall                    ; 0F 05

Expect: exit status 135.

Tests:
  - F7 /6 (DIV r/m64)  with REX.W — unsigned 128/64 divide
  - F7 /7 (IDIV r/m64) with REX.W — signed 128/64 divide
  - 48 99 (CQO) — sign-extend RAX into RDX:RAX
  - 48 31 D2 (XOR RDX,RDX) — zero high half for unsigned divide
  - 48 C7 C0 with negative imm32 (-21) → sign-extends to RAX
  - 48 01 D8 (ADD RBX←RAX wait, /r encoding: ADD r/m64, r64)
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    code  = bytes.fromhex("48C7C0E8030000")    # mov rax, 1000
    code += bytes.fromhex("4831D2")            # xor rdx, rdx
    code += bytes.fromhex("48C7C107000000")    # mov rcx, 7
    code += bytes.fromhex("48F7F1")            # div rcx
    code += bytes.fromhex("4889C3")            # mov rbx, rax  (save quotient)
    code += bytes.fromhex("48C7C0") + struct.pack("<i", -21)  # mov rax, -21
    code += bytes.fromhex("4899")              # cqo
    code += bytes.fromhex("48C7C103000000")    # mov rcx, 3
    code += bytes.fromhex("48F7F9")            # idiv rcx
    code += bytes.fromhex("4801D8")            # add rax, rbx
    code += bytes.fromhex("4889C7")            # mov rdi, rax
    code += bytes.fromhex("B83C000000")        # mov eax, 60
    code += bytes.fromhex("0F05")              # syscall

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
    out = sys.argv[1] if len(sys.argv) > 1 else "div64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
