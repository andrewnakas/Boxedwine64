#!/usr/bin/env python3
"""
Emit a PIE (ET_DYN) x86-64 ELF that exits with status read through a
RELATIVE-relocated GOT-style slot. Proves the full PIE + RELATIVE pipeline
end-to-end: loader applies the relocation, then code dereferences the
relocated slot at runtime and uses the value.

Layout:
    0x0000 Ehdr
    0x0040 Phdr[0] PT_LOAD  (R+W+X, covers whole file)
    0x0078 Phdr[1] PT_DYNAMIC
    0x00B0 dynamic[] (5 entries: RELA, RELASZ, RELAENT, NULL, NULL pad)
    0x00F0 RELA[0]   (R_X86_64_RELATIVE, r_offset=SLOT, r_addend=PAYLOAD)
    0x0108 SLOT      (8 bytes, gets overwritten with reloc+PAYLOAD by the
                     loader's RELATIVE pass)
    0x0110 CODE
    0x0130 end

Code (when entered, RIP = LOAD + 0x110):
    48 8B 3D F1 FF FF FF     mov rdi, [rip-0x0F]    ; rdi = *SLOT
    48 C1 EF 18              shr rdi, 24            ; isolate the PAYLOAD high byte
    B8 3C 00 00 00           mov eax, 60            ; SYS_exit
    0F 05                    syscall

PAYLOAD = 0x42 << 24. After RELATIVE relocation:
    *SLOT = reloc + 0x42000000
Reloc is page-aligned to 0x555555554000, so the top byte of reloc is 0x00
(48-bit canonical low half). After `shr 24`, the result is:
    ((0x555555554000 + 0x42000000) >> 24) & 0xFF
  = (0x555597554000 >> 24) & 0xFF
  = 0x97
So the exit status (low 8 bits of RDI seen by sys_exit) should be 0x97.

Choose this dance because it forces the relocation result to actually flow
through the CPU — a trivial "exit(0)" wouldn't notice if the loader skipped
RELATIVE entirely. Any non-zero specific exit status proves the loader
wrote the correct value and the CPU read it back through a real RIP-rel
load.

Usage:  python3 tools/buildPieRelocElf64.py [outfile]
"""

import struct, sys, os

LOAD_VADDR = 0x0
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
DYN_OFF    = 0x00B0
RELA_OFF   = 0x00F0
SLOT_OFF   = 0x0108
CODE_OFF   = 0x0110
END_OFF    = 0x0130

R_X86_64_RELATIVE = 8
DT_NULL    = 0
DT_RELA    = 7
DT_RELASZ  = 8
DT_RELAENT = 9
PT_LOAD    = 1
PT_DYNAMIC = 2

PAYLOAD_HIGH = 0x42
PAYLOAD = PAYLOAD_HIGH << 24  # 0x42000000

def build():
    elf = bytearray(END_OFF)

    # Ehdr
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2; ident[5] = 1; ident[6] = 1
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        3,                       # e_type ET_DYN
        0x3E,                    # e_machine EM_X86_64
        1,                       # e_version
        LOAD_VADDR + CODE_OFF,   # e_entry (unrelocated)
        PHDR_OFF, 0, 0,
        64, 56, 2, 0, 0, 0,
    )

    # Phdr[0] PT_LOAD covering the whole file (RWX)
    elf[PHDR_OFF:PHDR_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        PT_LOAD, 7, 0, LOAD_VADDR, LOAD_VADDR, END_OFF, END_OFF, 0x1000,
    )
    # Phdr[1] PT_DYNAMIC
    elf[PHDR_OFF+56:PHDR_OFF+112] = struct.pack(
        "<IIQQQQQQ",
        PT_DYNAMIC, 6, DYN_OFF, LOAD_VADDR + DYN_OFF, LOAD_VADDR + DYN_OFF,
        0x40, 0x40, 8,
    )

    # Dynamic array (5 entries, each 16 bytes = 0x50 bytes total, but we
    # only declared 0x40 = 4 entries because the last is DT_NULL implicit).
    # Use 4 entries: RELA, RELASZ, RELAENT, NULL.
    dyn = struct.pack("<qQ", DT_RELA,    LOAD_VADDR + RELA_OFF)
    dyn += struct.pack("<qQ", DT_RELASZ,  24)
    dyn += struct.pack("<qQ", DT_RELAENT, 24)
    dyn += struct.pack("<qQ", DT_NULL,    0)
    assert len(dyn) == 0x40
    elf[DYN_OFF:DYN_OFF+0x40] = dyn

    # RELA[0]: r_offset = SLOT vaddr, r_info = type=RELATIVE/sym=0, r_addend = PAYLOAD
    r_info = (0 << 32) | R_X86_64_RELATIVE
    elf[RELA_OFF:RELA_OFF+24] = struct.pack(
        "<QQq", LOAD_VADDR + SLOT_OFF, r_info, PAYLOAD,
    )

    # SLOT: 8 zero bytes (loader overwrites with reloc + PAYLOAD)
    # Already zeroed by bytearray init.

    # CODE
    # mov rdi, [rip-0x0F] : 48 8B 3D + disp32
    # CODE is at 0x110; the mov is 7 bytes so next-RIP = 0x117.
    # SLOT is at 0x108. disp32 = 0x108 - 0x117 = -0x0F.
    disp = SLOT_OFF - (CODE_OFF + 7)
    assert disp == -0x0F
    code = bytes.fromhex("488B3D") + struct.pack("<i", disp)
    code += bytes.fromhex("48C1EF18")  # shr rdi, 24
    code += bytes.fromhex("B83C000000")  # mov eax, 60
    code += bytes.fromhex("0F05")        # syscall
    assert len(code) == 7 + 4 + 5 + 2 == 18
    elf[CODE_OFF:CODE_OFF+len(code)] = code

    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "pieReloc64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
