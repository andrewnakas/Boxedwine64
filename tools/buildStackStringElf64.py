#!/usr/bin/env python3
"""
Emit an ET_EXEC that builds a string ON THE STACK (not in the data
segment), then writes it via the write() syscall. Models how glibc
startup builds short error messages without referencing read-only data.

Code:
  ; Build "ok\n" on the stack: 3 bytes = "ok\n" + null pad to 8.
  ; "ok\n" in little-endian qword (low byte first): 0x000A6B6F
  mov rax, 0x000A6B6F          ; 48 B8 6F 6B 0A 00 00 00 00 00
  push rax                     ; 50
  mov rax, 1                   ; SYS_write (48 C7 C0 01 00 00 00)
  mov rdi, 1                   ; stdout    (48 C7 C7 01 00 00 00)
  mov rsi, rsp                 ; buf       (48 89 E6)
  mov rdx, 3                   ; len       (48 C7 C2 03 00 00 00)
  syscall                      ; 0F 05
  ; rax now contains bytes written (==3 on success); use as exit status
  mov rdi, rax                 ; 48 89 C7
  mov rax, 60                  ; SYS_exit  (48 C7 C0 3C 00 00 00)
  syscall                      ; 0F 05

Expect: prints "ok\n" to host stdout AND exits with status 3.

Tests:
  - 48 B8 MOV r64, imm64 (already in hello)
  - PUSH r64 with imm64 still in rax (50)
  - 48 89 E6 MOV r64, RSP (the stack-pointer source for mov r/m64,r64)
  - syscall return value flows back through rax (sys_write returns bytes)
  - exit status non-zero pulled from rax (== bytes written)

The "ok\n" pattern reads from stack memory through SI (the syscall buf),
which means sys_write64's readGuestCString-equivalent has to handle a
stack-pointed buffer. Easy to break with a regression that assumes args
point into mmapped data segments.
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x00B0

def build():
    # Need a stack — but the runner builds one for us at the canonical high
    # vaddr. So we just push/pop and trust the runner's SysV frame setup.
    code  = bytes.fromhex("48B8") + struct.pack("<Q", 0x000A6B6F)  # mov rax, "ok\n"
    code += bytes.fromhex("50")                                    # push rax
    code += bytes.fromhex("48C7C001000000")                        # mov rax, 1
    code += bytes.fromhex("48C7C701000000")                        # mov rdi, 1
    code += bytes.fromhex("4889E6")                                # mov rsi, rsp
    code += bytes.fromhex("48C7C203000000")                        # mov rdx, 3
    code += bytes.fromhex("0F05")                                  # syscall
    code += bytes.fromhex("4889C7")                                # mov rdi, rax
    code += bytes.fromhex("48C7C03C000000")                        # mov rax, 60
    code += bytes.fromhex("0F05")                                  # syscall

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
    out = sys.argv[1] if len(sys.argv) > 1 else "stackString64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
