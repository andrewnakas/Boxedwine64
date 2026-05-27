#!/usr/bin/env python3
"""
Emit a static x86-64 hello-world ELF — no toolchain dependency.

Produces a 166-byte ET_EXEC that issues:
    write(1, "hello, world\n", 13)
    exit(0)
via raw syscalls. No libc, no dynamic linker, no relocations.

Usage:
    python3 tools/buildHelloElf64.py [outfile]

If `outfile` is omitted, writes to ./hello64.elf next to the script.

This file is byte-identical to the embedded payload in
source/emulation/cpu/cpu64RunElf.cpp (buildEmbeddedHelloElf). The
script exists so the bytes can be regenerated from a readable source
of truth, and so a real ELF can be passed via
`Boxedwine --x64-run-elf hello64.elf` for end-to-end testing.
"""

import struct
import sys
import os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
CODE_OFF   = 0x0078
MSG_OFF    = 0x0099
END_OFF    = 0x00A6

MSG = b"hello, world\n"
assert len(MSG) == 13

def build():
    elf = bytearray(END_OFF)

    # --- ELF64 Ehdr (64 bytes) ---
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2     # ELFCLASS64
    ident[5] = 1     # ELFDATA2LSB
    ident[6] = 1     # EV_CURRENT
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    # e_type=ET_EXEC, e_machine=EM_X86_64, e_version=1, e_entry, e_phoff,
    # e_shoff=0, e_flags=0, e_ehsize=64, e_phentsize=56, e_phnum=1,
    # e_shentsize=0, e_shnum=0, e_shstrndx=0
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        2,                              # e_type ET_EXEC
        0x3E,                           # e_machine EM_X86_64
        1,                              # e_version
        LOAD_VADDR + CODE_OFF,          # e_entry
        PHDR_OFF,                       # e_phoff
        0,                              # e_shoff
        0,                              # e_flags
        64,                             # e_ehsize
        56,                             # e_phentsize
        1,                              # e_phnum
        0,                              # e_shentsize
        0,                              # e_shnum
        0,                              # e_shstrndx
    )

    # --- Phdr[0] PT_LOAD (56 bytes) ---
    # p_type=PT_LOAD, p_flags=PF_R|PF_W|PF_X, p_offset, p_vaddr, p_paddr,
    # p_filesz, p_memsz, p_align
    elf[PHDR_OFF:PHDR_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        1,                              # p_type PT_LOAD
        7,                              # p_flags R|W|X
        0,                              # p_offset
        LOAD_VADDR,                     # p_vaddr
        LOAD_VADDR,                     # p_paddr
        END_OFF,                        # p_filesz
        END_OFF,                        # p_memsz
        0x1000,                         # p_align
    )

    # --- Code (33 bytes) ---
    #   48 8D 35 1A 00 00 00     lea rsi, [rip+0x1A]   ; rsi = msg
    #   BF 01 00 00 00           mov edi, 1
    #   BA 0D 00 00 00           mov edx, 13
    #   B8 01 00 00 00           mov eax, 1            ; SYS_write
    #   0F 05                    syscall
    #   B8 3C 00 00 00           mov eax, 60           ; SYS_exit
    #   31 FF                    xor edi, edi
    #   0F 05                    syscall
    code = bytes.fromhex(
        "488D351A000000"
        "BF01000000"
        "BA0D000000"
        "B801000000"
        "0F05"
        "B83C000000"
        "31FF"
        "0F05"
    )
    assert len(code) == 33
    elf[CODE_OFF:CODE_OFF+33] = code

    # --- Message ---
    elf[MSG_OFF:MSG_OFF+13] = MSG

    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "hello64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
