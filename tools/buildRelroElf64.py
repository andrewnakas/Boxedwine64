#!/usr/bin/env python3
"""
Emit an ET_DYN (PIE) ELF that carries a PT_GNU_RELRO segment covering a
small region containing a function-pointer slot. The relocation writes
the callee into the slot, the loader applies RELRO post-relocation,
the indirect call jumps through the (now-RO) slot, and we exit with
the callee's return value.

The exit status itself doesn't prove RELRO was *honored* (our writes
don't fault on RO pages yet — that's a separate gap). What it proves
end-to-end is:
  - parseBuffer captured PT_GNU_RELRO
  - cpu64RunElf invoked KMemory64::mprotect on the right pages
  - the post-RELRO indirect call still works (reads succeed)
The "PT_GNU_RELRO 0x... -> RO" stdout line from the runner is the
visible witness — grep for it in the smoke harness.

Layout:
  EHDR_OFF      = 0x0000
  PHDR_OFF      = 0x0040   4 phdrs * 56 = 224 bytes
  DYN_OFF       = 0x0120
  RELA_OFF      = 0x0160
  GOT_OFF       = 0x1000   page-aligned so RELRO can cover it cleanly
  CODE_OFF      = 0x2000

GOT_OFF is on its own page so the RELRO region [0x1000, 0x2000) covers
exactly the GOT page and nothing else.
"""

import struct, sys, os

LOAD_VADDR    = 0x0
EHDR_OFF      = 0x0000
PHDR_OFF      = 0x0040
DYN_OFF       = 0x0120
RELA_OFF      = 0x0160
GOT_OFF       = 0x1000
CODE_OFF      = 0x2000

R_X86_64_RELATIVE = 8
DT_RELA, DT_RELASZ, DT_RELAENT, DT_NULL = 7, 8, 9, 0
PT_LOAD, PT_DYNAMIC = 1, 2
PT_GNU_RELRO = 0x6474E552

EXPECTED_EXIT = 0x5A  # 90

def build():
    # Main code at CODE_OFF.
    main_code = b""
    # lea rax, [rip + (GOT_OFF - rip_after)]
    lea_rip_after = CODE_OFF + 7
    disp = GOT_OFF - lea_rip_after
    main_code += bytes.fromhex("488D05") + struct.pack("<i", disp)
    main_code += bytes.fromhex("FF10")             # call qword ptr [rax]
    main_code += bytes.fromhex("4889C7")           # mov rdi, rax
    main_code += bytes.fromhex("B83C000000")       # mov eax, 60
    main_code += bytes.fromhex("0F05")             # syscall

    callee_off = CODE_OFF + len(main_code)
    callee  = bytes.fromhex("B85A000000")          # mov eax, 0x5A
    callee += bytes.fromhex("C3")                  # ret

    file_size = callee_off + len(callee)
    # Round up to a page so the second PT_LOAD ends cleanly.
    file_size = (file_size + 0xFFF) & ~0xFFF
    elf = bytearray(file_size)

    # R_X86_64_RELATIVE pointing GOT_OFF at callee.
    rela = struct.pack("<QQq", GOT_OFF, R_X86_64_RELATIVE, callee_off)
    elf[RELA_OFF:RELA_OFF+24] = rela
    elf[GOT_OFF:GOT_OFF+8] = struct.pack("<Q", 0)

    # Dynamic.
    dyn = b""
    dyn += struct.pack("<qQ", DT_RELA,    RELA_OFF)
    dyn += struct.pack("<qQ", DT_RELASZ,  24)
    dyn += struct.pack("<qQ", DT_RELAENT, 24)
    dyn += struct.pack("<qQ", DT_NULL,    0)
    elf[DYN_OFF:DYN_OFF+len(dyn)] = dyn

    # Ehdr.
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2; ident[5] = 1; ident[6] = 1
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    ET_DYN = 3
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        ET_DYN, 0x3E, 1,
        CODE_OFF,
        PHDR_OFF, 0, 0,
        64, 56, 4, 0, 0, 0,
    )

    # Phdr[0] PT_LOAD covering entire file.
    elf[PHDR_OFF:PHDR_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        PT_LOAD, 7, 0,
        0, 0,
        file_size, file_size,
        0x1000,
    )
    # Phdr[1] PT_DYNAMIC.
    elf[PHDR_OFF+56:PHDR_OFF+112] = struct.pack(
        "<IIQQQQQQ",
        PT_DYNAMIC, 6, DYN_OFF,
        DYN_OFF, DYN_OFF,
        len(dyn), len(dyn),
        8,
    )
    # Phdr[2] PT_GNU_RELRO covers GOT page [0x1000, 0x2000).
    elf[PHDR_OFF+112:PHDR_OFF+168] = struct.pack(
        "<IIQQQQQQ",
        PT_GNU_RELRO, 4, GOT_OFF,
        GOT_OFF, GOT_OFF,
        0x1000, 0x1000,
        1,
    )
    # Phdr[3] sentinel PT_LOAD (just a dup of phdr[0] to fill the slot —
    # not really needed; could be PT_NULL). Use PT_LOAD with size 0 for
    # safety, but most loaders just ignore an empty PT_LOAD. Easier: emit
    # 3 phdrs and update count.
    # Actually, simpler — overwrite e_phnum to 3 and zero phdr[3].
    elf[EHDR_OFF+56] = 3
    elf[PHDR_OFF+168:PHDR_OFF+224] = bytes(56)

    elf[CODE_OFF:CODE_OFF+len(main_code)] = main_code
    elf[callee_off:callee_off+len(callee)] = callee
    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "relro64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
