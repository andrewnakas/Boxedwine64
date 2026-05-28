#!/usr/bin/env python3
"""
Emit an ET_EXEC with a PT_TLS segment containing a single qword
sentinel, then code that reads `fs:[-8]` (the canonical place where
glibc's __pthread_self() looks up the per-thread errno-ish slot).

Verifies the *end-to-end* path: parser sees PT_TLS, loader allocates
a TLS block, copies the image, sets the TCB self-pointer, and seeds
CPU64::fsbase. Without all of those, the FS-relative load reads
zeros and we get exit status 0; with them, we get 0xAB = 171.

ELF layout:
  EHDR_OFF   = 0x0000  (64 bytes)
  PHDR_OFF   = 0x0040  (2 phdrs × 56 = 112 bytes)
  TLS_IMAGE_OFF = 0x00B0  (8 bytes of "image": qword 0xAB)
  CODE_OFF   = 0x00C0
Vaddrs match offsets relative to LOAD_VADDR=0x400000.

The PT_TLS image is at vaddr 0x4000B0 in the LOAD'ed exe; the TLS
block (where the image gets copied at runtime) is at the loader's
chosen TLS_BLOCK_BASE; FS points at the TCB (= block_base + image_size).
Because image_size = 8 (filesz=8, align=8 → imageSize=8), the TCB is at
block_base+8, so fs:[-8] reads the start of the image (the first qword).

Code:
  mov rax, qword ptr fs:[-8]   ; 64 48 8B 04 25 F8 FF FF FF
  ; opcodes: 64=FS prefix, 48=REX.W, 8B=MOV r64,r/m64,
  ; ModRM=00_000_100 (reg=rax, rm=SIB), SIB=00_100_101 (scale=0,
  ; index=none, base=none → disp32 only), disp32=0xFFFFFFF8 (-8)
  mov rdi, rax                  ; 48 89 C7
  mov eax, 60                   ; B8 3C 00 00 00
  syscall                       ; 0F 05

Expect: exit status 0xAB = 171.
"""

import struct, sys, os

LOAD_VADDR    = 0x400000
EHDR_OFF      = 0x0000
PHDR_OFF      = 0x0040
TLS_IMAGE_OFF = 0x00B0
CODE_OFF      = 0x00C0
SENTINEL      = 0xAB

def build():
    # TLS image: a single qword sentinel.
    tls_image = struct.pack("<Q", SENTINEL)

    code  = bytes.fromhex("64488B0425F8FFFFFF")    # mov rax, fs:[-8]
    code += bytes.fromhex("4889C7")                # mov rdi, rax
    code += bytes.fromhex("B83C000000")            # mov eax, 60
    code += bytes.fromhex("0F05")                  # syscall

    file_size = CODE_OFF + len(code)
    elf = bytearray(file_size)

    # Ehdr — note 2 phdrs now (PT_LOAD + PT_TLS).
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2; ident[5] = 1; ident[6] = 1
    elf[EHDR_OFF:EHDR_OFF+16] = ident
    elf[EHDR_OFF+16:EHDR_OFF+64] = struct.pack(
        "<HHIQQQIHHHHHH",
        2, 0x3E, 1,
        LOAD_VADDR + CODE_OFF,
        PHDR_OFF, 0, 0,
        64, 56, 2, 0, 0, 0,    # phnum=2
    )

    # Phdr[0] PT_LOAD covering whole file RWX.
    elf[PHDR_OFF:PHDR_OFF+56] = struct.pack(
        "<IIQQQQQQ",
        1, 7, 0,
        LOAD_VADDR, LOAD_VADDR,
        file_size, file_size,
        0x1000,
    )

    # Phdr[1] PT_TLS — vaddr points at TLS image, filesz=8, memsz=8, align=8.
    PT_TLS = 7
    elf[PHDR_OFF+56:PHDR_OFF+112] = struct.pack(
        "<IIQQQQQQ",
        PT_TLS, 4, TLS_IMAGE_OFF,
        LOAD_VADDR + TLS_IMAGE_OFF, LOAD_VADDR + TLS_IMAGE_OFF,
        8, 8,
        8,
    )

    elf[TLS_IMAGE_OFF:TLS_IMAGE_OFF+8] = tls_image
    elf[CODE_OFF:CODE_OFF+len(code)] = code

    return bytes(elf)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tlsImage64.elf"
    blob = build()
    with open(out, "wb") as f:
        f.write(blob)
    os.chmod(out, 0o755)
    print(f"wrote {out} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
