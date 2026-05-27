#!/usr/bin/env python3
"""
Emit an ET_EXEC that exercises REP MOVSB (F3 A4) — the byte-by-byte
copy primitive that glibc's memcpy/memmove fall back to for sizes
the compiler-inlined version can't handle, and that ld.so uses to
copy program headers into TLS images.

Strategy: build a 13-byte string "Hello, copy!\n" on the stack via
two pushes, then REP MOVSB it to a different stack slot, then
sys_write the destination. Validates DF=0 forward direction, RCX
counter, RSI/RDI auto-increment, and reads from stack memory through
SI/DI (not just from data segment).

Layout: single PT_LOAD at 0x400000 RWX, entry 0x4000B0.

Code:
  ; Build "Hello, copy!\n\0\0\0" on stack: 16 bytes via two pushes.
  ; Low 8 bytes (pushed second so on top of stack): "Hello, c"
  ; High 8 bytes (pushed first):                    "opy!\n\0\0\0\0"
  ; In little-endian qword order, low-byte first:
  ;   q1 = 0x00006f632c6f6c6c6548 — wait that's 9 chars, need 8.
  ;   "Hello, c" (8 chars) = 0x63202c6f6c6c6548 (little-end low byte 'H'=0x48)
  ;   "opy!\n\0\0\0" (8 chars padded) = 0x0000000a216f706f wait — 'o'=0x6f,'p'=0x70,'y'=0x79,'!'=0x21,'\n'=0x0a
  ; Let me just spell out the bytes:
  ;   "Hello, copy!\n" = 48 65 6c 6c 6f 2c 20 63 6f 70 79 21 0a (13 bytes)
  ; Pushed as two qwords (push uses 8-byte slots, low addr last after both pushes):
  ;   first push: bytes 8..15 → 0x0000_000a_2179_706f
  ;   second push: bytes 0..7 → 0x636f_2c20_6f6c_6c48 wait 'H'=0x48 should be low
  ; Easier: compute in Python.
  ;
  ;   bytes[0..7]  = "Hello, c"
  ;   bytes[8..15] = "opy!\n" + 3*\0
  ;
  ; push order: push high-qword first (ends up at higher address after RSP grew),
  ; then push low-qword (ends up at RSP). REP MOVSB needs source at low addr,
  ; so RSI = RSP. Length = 13.
  mov rax, <high_qword>     ; bytes[8..15] padded — push to put at [rsp+8]
  push rax
  mov rax, <low_qword>      ; bytes[0..7]
  push rax
  ; Reserve another 16 bytes on stack for the destination
  sub rsp, 16               ; 48 83 EC 10
  mov rdi, rsp              ; 48 89 E7 — dst
  lea rsi, [rsp+16]         ; 48 8D 74 24 10 — src = the strings we just pushed
  mov ecx, 13               ; B9 0D 00 00 00
  cld                       ; FC — DF=0, forward direction
  rep movsb                 ; F3 A4
  ; sys_write(1, dst, 13)
  mov eax, 1                ; B8 01 00 00 00
  mov edi, 1                ; BF 01 00 00 00
  mov rsi, rsp              ; 48 89 E6 — dst (now on top of stack)
  mov edx, 13               ; BA 0D 00 00 00
  syscall
  ; rax now contains bytes written (13). exit(rax).
  mov rdi, rax              ; 48 89 C7
  mov eax, 60               ; B8 3C 00 00 00
  syscall

Expect: prints "Hello, copy!\n" to host stdout AND exits status 13.

Tests:
  - F3 A4   REP MOVSB (string move, ECX counter, RSI/RDI auto-increment)
  - FC      CLD (clear direction flag — needed for forward copy)
  - 48 83 EC ib   SUB r/m64, imm8 (stack-pointer adjust)
  - LEA [rsp+disp8]  with SIB byte (encoding: 48 8D 74 24 10)
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    msg = b"Hello, copy!\n"
    assert len(msg) == 13
    padded = msg + b"\x00" * (16 - len(msg))
    low_q  = struct.unpack("<Q", padded[0:8])[0]
    high_q = struct.unpack("<Q", padded[8:16])[0]

    code  = bytes.fromhex("48B8") + struct.pack("<Q", high_q)   # mov rax, high
    code += bytes.fromhex("50")                                 # push rax
    code += bytes.fromhex("48B8") + struct.pack("<Q", low_q)    # mov rax, low
    code += bytes.fromhex("50")                                 # push rax
    code += bytes.fromhex("4883EC10")                           # sub rsp, 16
    code += bytes.fromhex("4889E7")                             # mov rdi, rsp
    code += bytes.fromhex("488D742410")                         # lea rsi, [rsp+16]
    code += bytes.fromhex("B90D000000")                         # mov ecx, 13
    code += bytes.fromhex("FC")                                 # cld
    code += bytes.fromhex("F3A4")                               # rep movsb
    code += bytes.fromhex("B801000000")                         # mov eax, 1
    code += bytes.fromhex("BF01000000")                         # mov edi, 1
    code += bytes.fromhex("4889E6")                             # mov rsi, rsp
    code += bytes.fromhex("BA0D000000")                         # mov edx, 13
    code += bytes.fromhex("0F05")                               # syscall
    code += bytes.fromhex("4889C7")                             # mov rdi, rax
    code += bytes.fromhex("B83C000000")                         # mov eax, 60
    code += bytes.fromhex("0F05")                               # syscall

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
    out = sys.argv[1] if len(sys.argv) > 1 else "repMovsb64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
