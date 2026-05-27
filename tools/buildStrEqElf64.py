#!/usr/bin/env python3
"""
Emit an ET_EXEC that builds two short strings on the stack and compares
them byte-by-byte using CMP + JNE in a counted loop. Models how glibc's
__strcmp_sse2 fallback path walks env strings and how ld.so's section
matching tests SHT_DYNSYM names.

Code (entry at 0x4000B0):
  ; build s1 = "abc" on stack at [rsp-8]
  mov rax, 0x00636261          ; "abc\0" little-endian
  push rax                     ; pushes 8 bytes — low 3 are 'a','b','c'
  ; build s2 = "abc" on stack at [rsp-8] (different slot)
  mov rax, 0x00636261
  push rax
  ; setup pointers
  mov rsi, rsp                 ; rsi -> s2
  lea rdi, [rsp+8]             ; rdi -> s1 (one qword higher)
  mov ecx, 3                   ; counter = 3 bytes to compare
loop_top:
  mov al, [rsi]                ; load byte from s2
  cmp al, [rdi]                ; compare against s1
  jne not_equal                ; if differ, jump out
  inc rsi
  inc rdi
  dec ecx
  jnz loop_top
  ; strings equal — exit 17
  mov edi, 17
  mov eax, 60
  syscall
not_equal:
  mov edi, 99
  mov eax, 60
  syscall

Expect: exit status 17 (both strings "abc" — equal).

Tests:
  - 8A 06       MOV r8, m8 (load byte from memory)
  - 3A 07       CMP r8, m8 (8-bit compare against memory)
  - 75 rel8     JNZ (forward branch on inequality)
  - 48 8D ... LEA r64, m (RIP-relative would be different; this is [rsp+8])
  - 48 FF C6 / 48 FF C7  INC r/m for RSI/RDI
  - FF C9       DEC ECX (no REX, 32-bit form)
  - 75 rel8     JNZ backward (loop-bottom branch)
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    code = b""
    # mov rax, "abc\0"
    code += bytes.fromhex("48B8") + struct.pack("<Q", 0x00636261)
    code += bytes.fromhex("50")                              # push rax (s1)
    code += bytes.fromhex("48B8") + struct.pack("<Q", 0x00636261)
    code += bytes.fromhex("50")                              # push rax (s2)
    code += bytes.fromhex("4889E6")                          # mov rsi, rsp (s2)
    # lea rdi, [rsp+8] — 48 8D 7C 24 08 (REX.W, modrm=01_111_100, sib=00_100_100, disp8=8)
    code += bytes.fromhex("488D7C2408")
    code += bytes.fromhex("B903000000")                      # mov ecx, 3

    loop_top_off = len(code)
    code += bytes.fromhex("8A06")                            # mov al, [rsi]
    code += bytes.fromhex("3A07")                            # cmp al, [rdi]
    # jne not_equal — placeholder, fix disp8 later
    jne_off = len(code)
    code += b"\x75\x00"
    code += bytes.fromhex("48FFC6")                          # inc rsi
    code += bytes.fromhex("48FFC7")                          # inc rdi
    code += bytes.fromhex("FFC9")                            # dec ecx
    # jnz loop_top (backward)
    after_jnz = len(code) + 2
    disp_back = loop_top_off - after_jnz
    assert -128 <= disp_back <= 127
    code += b"\x75" + struct.pack("<b", disp_back)
    # equal path: exit 17
    code += bytes.fromhex("BF11000000")                      # mov edi, 17
    code += bytes.fromhex("B83C000000")                      # mov eax, 60
    code += bytes.fromhex("0F05")                            # syscall
    # not_equal lands here
    not_equal_off = len(code)
    disp_fwd = not_equal_off - (jne_off + 2)
    assert 0 <= disp_fwd <= 127
    code = code[:jne_off+1] + struct.pack("<b", disp_fwd) + code[jne_off+2:]
    code += bytes.fromhex("BF63000000")                      # mov edi, 99
    code += bytes.fromhex("B83C000000")                      # mov eax, 60
    code += bytes.fromhex("0F05")                            # syscall

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
    out = sys.argv[1] if len(sys.argv) > 1 else "strEq64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
