#!/usr/bin/env python3
"""
Emit an ET_DYN (PIE) ELF that combines five Milestone A features in
one binary — the integration-shape closest to what a real glibc-
startup binary does in its first thousand instructions:

  1. PIE base relocation: ET_DYN, single PT_LOAD, R_X86_64_RELATIVE
     entry against a function-pointer table.
  2. PT_TLS: 16-byte tdata image with two sentinels.
  3. RIP-relative LEA to load the func-pointer table address.
  4. Indirect CALL through a relocated GOT-like slot.
  5. FS-relative TLS read to combine with the callee result.

The callee returns 0x40 = 64. We then read fs:[-16] (= TLS[0] = 0x07)
and fs:[-8] (= TLS[1] = 0x04), add them all: 64 + 7 + 4 = 75. Exit 75.

A single regression in any of: PIE base-add, R_X86_64_RELATIVE
application, PT_TLS install, FS prefix handling, RIP-relative LEA,
or indirect memory CALL will produce the wrong status. Together
this is the closest synthetic probe to the actual `_dl_start` shape.

Layout (offsets are file offsets; vaddrs = LOAD_VADDR + offset
in the source, but real vaddr at runtime = file_off + reloc_base):
  EHDR_OFF      = 0x0000  64 bytes
  PHDR_OFF      = 0x0040  3 phdrs * 56 = 168 bytes
  DYN_OFF       = 0x00F0  enough for 4-entry dynamic array (64 bytes)
  RELA_OFF      = 0x0130  one Rela = 24 bytes
  GOT_OFF       = 0x0148  one qword slot for the relocated func pointer
  TLS_IMAGE_OFF = 0x0150  16 bytes: qwords [0x07, 0x04]
  CODE_OFF      = 0x0160  main code + callee
"""

import struct, sys, os

LOAD_VADDR    = 0x0
EHDR_OFF      = 0x0000
PHDR_OFF      = 0x0040
DYN_OFF       = 0x00F0
RELA_OFF      = 0x0130
GOT_OFF       = 0x0148
TLS_IMAGE_OFF = 0x0150
CODE_OFF      = 0x0160

R_X86_64_RELATIVE = 8
DT_RELA, DT_RELASZ, DT_RELAENT, DT_NULL = 7, 8, 9, 0
PT_LOAD, PT_DYNAMIC, PT_TLS = 1, 2, 7

def build():
    # Callee placed AFTER main code. We'll compute its file offset after
    # assembling main code.
    main_code = b""
    # Read the slot's address via RIP-relative LEA:
    #   lea rax, [rip + (GOT_OFF - rip_after)]
    # The first instruction is at CODE_OFF; rip_after = CODE_OFF + 7.
    lea_rip_after = CODE_OFF + 7
    disp_to_got = GOT_OFF - lea_rip_after
    main_code += bytes.fromhex("488D05") + struct.pack("<i", disp_to_got)
    # call qword ptr [rax]   ; FF 10
    main_code += bytes.fromhex("FF10")
    # rax = 0x40 (returned by callee). Save in rbx.
    main_code += bytes.fromhex("4889C3")           # mov rbx, rax
    # rax = qword ptr fs:[-16]  ; 64 48 8B 04 25 F0 FF FF FF
    main_code += bytes.fromhex("64488B0425F0FFFFFF")
    main_code += bytes.fromhex("4801C3")           # add rbx, rax
    # rax = qword ptr fs:[-8]   ; 64 48 8B 04 25 F8 FF FF FF
    main_code += bytes.fromhex("64488B0425F8FFFFFF")
    main_code += bytes.fromhex("4801C3")           # add rbx, rax
    main_code += bytes.fromhex("4889DF")           # mov rdi, rbx
    main_code += bytes.fromhex("B83C000000")       # mov eax, 60
    main_code += bytes.fromhex("0F05")             # syscall

    callee_off = CODE_OFF + len(main_code)
    callee  = bytes.fromhex("B840000000")          # mov eax, 0x40
    callee += bytes.fromhex("C3")                  # ret

    file_size = callee_off + len(callee)
    elf = bytearray(file_size)

    # The R_X86_64_RELATIVE entry tells the loader: "at vaddr GOT_OFF (+ load
    # slide), write (callee_off + load_slide)". With load_slide = reloc_base
    # this becomes (callee_off + reloc), exactly the function pointer.
    # Per ELF spec for RELATIVE: r_offset is the target address (will be
    # added to slide), r_addend is the value (also added to slide).
    rela = struct.pack("<QQq", GOT_OFF, R_X86_64_RELATIVE, callee_off)
    elf[RELA_OFF:RELA_OFF+24] = rela

    # The GOT slot starts as zero; the relocation will write the real pointer
    # at load time. Leaving it zero in the file makes any bug visible (call
    # through 0 would obviously segv).
    elf[GOT_OFF:GOT_OFF+8] = struct.pack("<Q", 0)

    # TLS image: two qwords.
    elf[TLS_IMAGE_OFF:TLS_IMAGE_OFF+16] = struct.pack("<QQ", 0x07, 0x04)

    # Dynamic array
    dyn = b""
    dyn += struct.pack("<qQ", DT_RELA,    RELA_OFF)
    dyn += struct.pack("<qQ", DT_RELASZ,  24)
    dyn += struct.pack("<qQ", DT_RELAENT, 24)
    dyn += struct.pack("<qQ", DT_NULL,    0)
    elf[DYN_OFF:DYN_OFF+len(dyn)] = dyn

    # Ehdr — note ET_DYN, phnum=3.
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2; ident[5] = 1; ident[6] = 1
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    ET_DYN = 3
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        ET_DYN, 0x3E, 1,
        CODE_OFF,                  # entry — load slide will be added
        PHDR_OFF, 0, 0,
        64, 56, 3, 0, 0, 0,
    )

    # Phdr[0] PT_LOAD covering whole file RWX
    elf[PHDR_OFF:PHDR_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        PT_LOAD, 7, 0,
        0, 0,
        file_size, file_size,
        0x1000,
    )
    # Phdr[1] PT_DYNAMIC
    elf[PHDR_OFF+56:PHDR_OFF+112] = struct.pack(
        "<IIQQQQQQ",
        PT_DYNAMIC, 6, DYN_OFF,
        DYN_OFF, DYN_OFF,
        len(dyn), len(dyn),
        8,
    )
    # Phdr[2] PT_TLS
    elf[PHDR_OFF+112:PHDR_OFF+168] = struct.pack(
        "<IIQQQQQQ",
        PT_TLS, 4, TLS_IMAGE_OFF,
        TLS_IMAGE_OFF, TLS_IMAGE_OFF,
        16, 16,
        8,
    )

    elf[CODE_OFF:CODE_OFF+len(main_code)] = main_code
    elf[callee_off:callee_off+len(callee)] = callee

    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "integration64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
