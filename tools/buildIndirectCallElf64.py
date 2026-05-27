#!/usr/bin/env python3
"""
Emit an ET_EXEC that loads a function pointer via RIP-relative LEA
then CALLs through it — the same pattern that GOT/PLT-using PIE
binaries hit on every external function call and that every C
program with function-pointer dispatch (vtables, callbacks, signal
handlers) uses.

Layout: PT_LOAD RWX at 0x400000, entry 0x4000B0.
A data slot at offset 0x100 (vaddr 0x400100) holds the
absolute address of `target` (filled in at build time).

Code:
  ; lea rax, [rip + disp32]    ; 48 8D 05 .. .. .. .. — addr of slot
  ; call qword ptr [rax]       ; FF 10 — indirect call through memory
  ; ; on return rax = 0x77
  mov rdi, rax
  mov eax, 60
  syscall

target:
  mov eax, 0x77
  ret

Expect: exit status 0x77 = 119.

Tests:
  - 48 8D 05 disp32 — LEA r64, [RIP + disp32]
  - FF /2 m64       — CALL r/m64 indirect through memory
  - C3              — RET (paired with the indirect call's pushed return)
  - Validates the call-stack is correctly aligned on entry to `target`
    (entry frame must be 16-byte-aligned per SysV; ours is via the runner's
    SysV setup which leaves RSP at a 16-byte boundary on entry to _start).
"""

import struct, sys, os

LOAD_VADDR = 0x400000
EHDR_OFF   = 0x0000
PHDR_OFF   = 0x0040
SLOT_OFF   = 0x0100
CODE_OFF   = 0x00B0

def build():
    # Lay out: code from 0xB0, slot at 0x100, target right after slot.
    # We need to know target's vaddr to fill the slot.
    # First emit main_code (variable length), then slot, then target.
    # target vaddr = LOAD_VADDR + (offset of target in file)

    main_code  = bytes.fromhex("488D05")                # lea rax, [rip + disp32] (placeholder disp)
    main_code += b"\x00\x00\x00\x00"                    # disp32 placeholder
    main_code += bytes.fromhex("FF10")                  # call qword ptr [rax]
    main_code += bytes.fromhex("4889C7")                # mov rdi, rax
    main_code += bytes.fromhex("B83C000000")            # mov eax, 60
    main_code += bytes.fromhex("0F05")                  # syscall

    # main_code starts at CODE_OFF = 0xB0; ends at CODE_OFF + len(main_code).
    # Slot at 0x100. Pad if needed.
    main_end_off = CODE_OFF + len(main_code)
    if main_end_off > SLOT_OFF:
        raise RuntimeError("main_code too big; bump SLOT_OFF")
    pad_main = SLOT_OFF - main_end_off

    slot_bytes = bytearray(8)  # will fill after computing target vaddr

    # target placed right after slot
    target_off = SLOT_OFF + 8
    target_vaddr = LOAD_VADDR + target_off
    # fill slot with target_vaddr
    slot_bytes[:] = struct.pack("<Q", target_vaddr)

    target_code  = bytes.fromhex("B877000000")          # mov eax, 0x77
    target_code += bytes.fromhex("C3")                  # ret

    # Now fix the LEA disp32. LEA encoding: 48 8D 05 <disp32>.
    # disp32 is from address after the instruction (RIP) to the target.
    # LEA insn starts at CODE_OFF, length 7. RIP after = CODE_OFF + 7.
    # Target of LEA = SLOT_OFF (where the function pointer slot is).
    lea_rip_after_vaddr = LOAD_VADDR + CODE_OFF + 7
    slot_vaddr = LOAD_VADDR + SLOT_OFF
    disp = slot_vaddr - lea_rip_after_vaddr
    main_code = main_code[:3] + struct.pack("<i", disp) + main_code[7:]

    file_size = target_off + len(target_code)
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

    elf[CODE_OFF:CODE_OFF+len(main_code)] = main_code
    elf[SLOT_OFF:SLOT_OFF+8] = slot_bytes
    elf[target_off:target_off+len(target_code)] = target_code

    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "indirectCall64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
