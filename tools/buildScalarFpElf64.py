#!/usr/bin/env python3
"""
Emit an ET_EXEC that exercises SSE2 scalar double-precision FP through
an on-disk binary — the same path glibc's __monstartup, libm, and any
C program using doubles takes. Independent of the in-memory selftest
that built the C1 SSE2 dispatch.

Computes: int(3.5 + 1.25 * 4.0) = int(8.5) = 8, exits with that as
status. Goes through:
  - MOVQ xmm, r64        (66 48 0F 6E /r) — load int->FP register
  - CVTSI2SD             (F2 48 0F 2A /r) — convert int to double
  - MULSD                (F2 0F 59 /r)
  - ADDSD                (F2 0F 58 /r)
  - CVTTSD2SI            (F2 48 0F 2C /r) — convert double->int (truncating)

Layout: single PT_LOAD at 0x400000, entry 0x4000B0.

Code:
  ; load 3 into rax, cvtsi2sd to xmm0
  mov rax, 3
  cvtsi2sd xmm0, rax          ; F2 48 0F 2A C0 — xmm0 = 3.0
  ; load 1 into rax, cvtsi2sd to xmm1, but we want 1.25 — use a direct
  ; load via a stack-built qword instead. Simpler: 1.25 = 0x3FF4000000000000.
  mov rax, 0x3FF4000000000000  ; bit pattern for 1.25
  movq xmm1, rax              ; 66 48 0F 6E C8 — xmm1 = 1.25
  ; load 4 into rax, cvtsi2sd to xmm2
  mov rax, 4
  cvtsi2sd xmm2, rax          ; F2 48 0F 2A D0 — xmm2 = 4.0
  ; xmm1 *= xmm2  (1.25 * 4.0 = 5.0)
  mulsd xmm1, xmm2            ; F2 0F 59 CA
  ; xmm0 += xmm1  (3.0 + 5.0 = 8.0)... wait, we wanted 3.5
  ; Use 0.5 instead. Bit pattern for 0.5 = 0x3FE0000000000000.
  ; Redo: xmm0 = 3.0, then add 0.5, then add (1.25*4.0)=5.0 → 8.5
  mov rax, 0x3FE0000000000000  ; 0.5
  movq xmm3, rax              ; 66 48 0F 6E D8 — xmm3 = 0.5
  addsd xmm0, xmm3            ; F2 0F 58 C3 — xmm0 = 3.5
  addsd xmm0, xmm1            ; F2 0F 58 C1 — xmm0 = 3.5 + 5.0 = 8.5
  ; convert truncating to int — should give 8
  cvttsd2si rax, xmm0         ; F2 48 0F 2C C0
  mov rdi, rax
  mov eax, 60
  syscall

Expect: exit status 8.
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    code  = bytes.fromhex("48B8") + struct.pack("<Q", 3)
    code += bytes.fromhex("F2480F2AC0")                        # cvtsi2sd xmm0, rax
    code += bytes.fromhex("48B8") + struct.pack("<Q", 0x3FF4000000000000)
    code += bytes.fromhex("66480F6EC8")                        # movq xmm1, rax
    code += bytes.fromhex("48B8") + struct.pack("<Q", 4)
    code += bytes.fromhex("F2480F2AD0")                        # cvtsi2sd xmm2, rax
    code += bytes.fromhex("F20F59CA")                          # mulsd xmm1, xmm2
    code += bytes.fromhex("48B8") + struct.pack("<Q", 0x3FE0000000000000)
    code += bytes.fromhex("66480F6ED8")                        # movq xmm3, rax
    code += bytes.fromhex("F20F58C3")                          # addsd xmm0, xmm3
    code += bytes.fromhex("F20F58C1")                          # addsd xmm0, xmm1
    code += bytes.fromhex("F2480F2CC0")                        # cvttsd2si rax, xmm0
    code += bytes.fromhex("4889C7")                            # mov rdi, rax
    code += bytes.fromhex("B83C000000")                        # mov eax, 60
    code += bytes.fromhex("0F05")                              # syscall

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
    out = sys.argv[1] if len(sys.argv) > 1 else "scalarFp64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
