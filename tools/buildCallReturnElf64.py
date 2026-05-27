#!/usr/bin/env python3
"""
Emit a tiny x86-64 ELF that exercises CALL / RET / PUSH / POP / register
preservation across a leaf function call — a stripped-down model of how
glibc's _start invokes __libc_start_main and how every C function does
prologue/epilogue.

Layout:
  Single PT_LOAD covering the whole file, RWX, at vaddr 0x400000.
  Entry at vaddr 0x4000B0 (right after Ehdr+Phdr).

Code:
  _start:                                  ; entry
      48 C7 C7 2A 00 00 00     mov rdi, 42      ; arg
      E8 0E 00 00 00           call leaf        ; +14 forward
      ; on return rax == arg + 1 == 43
      48 89 C7                  mov rdi, rax    ; status = 43
      B8 3C 00 00 00            mov eax, 60     ; SYS_exit
      0F 05                     syscall

  leaf:                                    ; +14 from end of call
      55                        push rbp        ; save caller's rbp
      48 89 E5                  mov rbp, rsp    ; frame
      48 89 F8                  mov rax, rdi    ; rax = arg
      48 FF C0                  inc rax         ; rax = arg + 1
      5D                        pop rbp         ; restore
      C3                        ret

Expect: process exits with status 43 (0x2B).

Surfaces any gap in: 1-byte REX-aware MOV r/m (48 89 / 48 8B), call rel32
(E8 disp32), ret (C3), push r64 / pop r64 (50+rd / 58+rd), and inc r/m
with REX.W (48 FF /0).
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    # Assemble code first so we know its length, then size the file.
    main_code  = bytes.fromhex("48C7C72A000000")  # mov rdi, 42
    # call leaf - disp32 to be filled after we know lengths
    # placeholder for E8 + 4-byte disp
    main_code += b"\xE8\x00\x00\x00\x00"
    main_code += bytes.fromhex("4889C7")           # mov rdi, rax
    main_code += bytes.fromhex("B83C000000")       # mov eax, 60
    main_code += bytes.fromhex("0F05")             # syscall

    leaf_code  = bytes.fromhex("55")               # push rbp
    leaf_code += bytes.fromhex("4889E5")           # mov rbp, rsp
    leaf_code += bytes.fromhex("4889F8")           # mov rax, rdi
    leaf_code += bytes.fromhex("48FFC0")           # inc rax
    leaf_code += bytes.fromhex("5D")               # pop rbp
    leaf_code += bytes.fromhex("C3")               # ret

    # disp32 = address-of(leaf) - address-of-after-call-insn
    # Call insn ends 7 (mov rdi,42) + 5 (call) = 12 bytes into main_code,
    # so leaf starts at main_code[12 + len(rest_of_main) :] which is
    # len(main_code) bytes from start. disp = len(main_code) - 12.
    after_call_off = 7 + 5
    leaf_off = len(main_code)
    disp = leaf_off - after_call_off
    main_code = main_code[:8] + struct.pack("<i", disp) + main_code[12:]

    code = main_code + leaf_code

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

    # Phdr[0] PT_LOAD covering whole file (RWX so we don't bother splitting)
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
    out = sys.argv[1] if len(sys.argv) > 1 else "callReturn64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
