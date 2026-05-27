#!/usr/bin/env python3
"""
Emit an ET_EXEC x86-64 ELF with two PT_LOAD segments — code (RX) and
data+BSS (RW) — and exit with a status read from the BSS region.

This is the on-disk runtime twin of the A13 self-test inside
cpu64SelfTest.cpp. The self-test exercises the in-memory loader path; this
script lets `Boxedwine --x64-run-elf path/to/it` exercise the same path
through the runner entry point (`runX64RunElf` in cpu64RunElf.cpp), which
covers the file-read + parseBuffer + mapSegmentsFromBuffer + CPU64 chain
end-to-end against a real on-disk binary.

Exit status: 0 on success — the entry code reads a qword from a vaddr
inside the BSS gap of segment 1 and writes its low 8 bits as the exit
status. The loader's MAP_ANONYMOUS zero-fill semantics in
KMemory64::mmapAnonymousFixed guarantee that gap is zero, so the qword
read returns 0 and exit(0) is observed.

If a future loader regression breaks BSS zero-fill, the exit status will
be whatever garbage was in those host bytes — easy signal.

Layout:
  0x0000 Ehdr (64)
  0x0040 Phdr[0] PT_LOAD                          ; RX, vaddr 0x400000
  0x0078 Phdr[1] PT_LOAD                          ; RW, vaddr 0x401000
  0x00B0 code
  0x00D0 data (sentinel qword 0xCAFEF00DDEADBEEF — proves file copy works)
  0x00D8 end of file
                                                  ; BSS extends in mem to
                                                  ; vaddr 0x401040 (memsz=0x40)

Code at vaddr 0x400000 + 0xB0 = 0x4000B0:
  48 A1 imm64        mov rax, [0x401010]   ; BSS read
  48 89 C7           mov rdi, rax          ; status = BSS value
  B8 3C 00 00 00     mov eax, 60           ; SYS_exit
  0F 05              syscall

Usage:  python3 tools/buildMultiSegmentElf64.py [outfile]
"""

import struct, sys, os

SEG0_VADDR    = 0x400000
SEG1_VADDR    = 0x401000
BSS_READ_ADDR = 0x401010
EHDR_OFF      = 0x0000
PHDR0_OFF     = 0x0040
PHDR1_OFF     = 0x0078
CODE_OFF      = 0x00B0
DATA_OFF      = 0x00D0
END_OFF       = 0x00D8

PT_LOAD = 1
SENTINEL = 0xCAFEF00DDEADBEEF

def build():
    elf = bytearray(END_OFF)

    # Ehdr
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2; ident[5] = 1; ident[6] = 1
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        2,                       # e_type ET_EXEC
        0x3E,                    # e_machine EM_X86_64
        1,                       # e_version
        SEG0_VADDR + CODE_OFF,   # e_entry
        PHDR0_OFF,               # e_phoff
        0, 0,                    # e_shoff, e_flags
        64, 56, 2, 0, 0, 0,      # ehsize, phentsize, phnum, ...
    )

    # Phdr[0] PT_LOAD: code segment (RX). Covers file [0, DATA_OFF).
    elf[PHDR0_OFF:PHDR0_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        PT_LOAD, 5,              # p_type, p_flags = R|X
        0,                       # p_offset
        SEG0_VADDR, SEG0_VADDR,  # p_vaddr, p_paddr
        DATA_OFF, DATA_OFF,      # p_filesz, p_memsz
        0x1000,                  # p_align
    )

    # Phdr[1] PT_LOAD: data segment (RW). Covers file [DATA_OFF, END_OFF).
    # filesz=8 (the sentinel qword), memsz=0x40 (trailing BSS).
    elf[PHDR1_OFF:PHDR1_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        PT_LOAD, 6,              # p_type, p_flags = R|W
        DATA_OFF,                # p_offset (file)
        SEG1_VADDR, SEG1_VADDR,  # p_vaddr, p_paddr
        8, 0x40,                 # p_filesz, p_memsz
        0x1000,                  # p_align
    )

    # Code
    code  = bytes.fromhex("48A1")                    # mov rax, moffs64
    code += struct.pack("<Q", BSS_READ_ADDR)
    code += bytes.fromhex("4889C7")                  # mov rdi, rax
    code += bytes.fromhex("B83C000000")              # mov eax, 60
    code += bytes.fromhex("0F05")                    # syscall
    assert len(code) == 2 + 8 + 3 + 5 + 2 == 20
    elf[CODE_OFF:CODE_OFF+len(code)] = code

    # Sentinel qword at the file-backed portion of segment 1.
    elf[DATA_OFF:DATA_OFF+8] = struct.pack("<Q", SENTINEL)

    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "multiSegment64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
