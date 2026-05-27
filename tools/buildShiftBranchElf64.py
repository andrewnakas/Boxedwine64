#!/usr/bin/env python3
"""
Emit an ET_EXEC that exercises variable-count shifts (SHL/SHR/SAR
with CL operand) plus a CMP+JL conditional — the same pattern glibc's
__ctype_b_loc lookup hits (shift the codepoint into table index) and
the same pattern hashtable bucket selection uses (shift then mask).

Computes: SHL(7, 4) → 0x70 = 112; SAR(-256 = 0xFFFFFFFFFFFFFF00, 3)
arithmetic right → 0xFFFFFFFFFFFFFFE0 = -32. Add → 80. Exit 80.

But also pulls in a CMP+JL: test if result > 50 (it is, 80>50), so
take the "result is large" branch which returns the value; otherwise
return 0. Validates JL (signed less-than, 7C rel8) reads SF/OF/ZF
relationship correctly.

Code:
  mov rax, 7                ; 48 C7 C0 07 00 00 00
  mov cl, 4                 ; B1 04
  shl rax, cl               ; 48 D3 E0 — SHL r/m64, CL
  mov rbx, rax              ; 48 89 C3 — save 0x70 = 112
  mov rax, -256             ; 48 C7 C0 00 FF FF FF (sign-extends)
  mov cl, 3                 ; B1 03
  sar rax, cl               ; 48 D3 F8 — SAR r/m64, CL → -32
  add rax, rbx              ; 48 01 D8 — rax = -32 + 112 = 80
  ; if (rax < 50) goto small; else fall through to return rax
  cmp rax, 50               ; 48 83 F8 32 — CMP r/m64, imm8 sign-extended
  jl small                  ; 7C rel8
  ; large path: exit rax
  mov rdi, rax              ; 48 89 C7
  mov eax, 60               ; B8 3C 00 00 00
  syscall                   ; 0F 05
small:
  mov edi, 0                ; BF 00 00 00 00
  mov eax, 60               ; B8 3C 00 00 00
  syscall

Expect: exit status 80.

Tests:
  - 48 D3 /4 (SHL r/m64, CL)  — variable-count left shift
  - 48 D3 /7 (SAR r/m64, CL)  — variable-count arithmetic right shift
  - 48 83 F8 ib (CMP r/m64, imm8 sign-extended)
  - 7C rel8 (JL — signed less, branches on SF != OF)
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    code  = bytes.fromhex("48C7C007000000")    # mov rax, 7
    code += bytes.fromhex("B104")              # mov cl, 4
    code += bytes.fromhex("48D3E0")            # shl rax, cl
    code += bytes.fromhex("4889C3")            # mov rbx, rax
    code += bytes.fromhex("48C7C0") + struct.pack("<i", -256)  # mov rax, -256
    code += bytes.fromhex("B103")              # mov cl, 3
    code += bytes.fromhex("48D3F8")            # sar rax, cl
    code += bytes.fromhex("4801D8")            # add rax, rbx
    code += bytes.fromhex("4883F832")          # cmp rax, 50
    # jl small — placeholder
    jl_off = len(code)
    code += b"\x7C\x00"
    # large path: exit rax
    code += bytes.fromhex("4889C7")            # mov rdi, rax
    code += bytes.fromhex("B83C000000")        # mov eax, 60
    code += bytes.fromhex("0F05")              # syscall
    # small path
    small_off = len(code)
    code += bytes.fromhex("BF00000000")        # mov edi, 0
    code += bytes.fromhex("B83C000000")        # mov eax, 60
    code += bytes.fromhex("0F05")              # syscall

    disp = small_off - (jl_off + 2)
    assert -128 <= disp <= 127
    code = code[:jl_off+1] + struct.pack("<b", disp) + code[jl_off+2:]

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
    out = sys.argv[1] if len(sys.argv) > 1 else "shiftBranch64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
