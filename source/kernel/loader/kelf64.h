/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef __kelf64_H__
#define __kelf64_H__

#include "platformBoxedwine.h"
#include "kelf.h" // for k_EI_NIDENT and PACKED

// ELF64 struct definitions per the System V ABI x86-64 supplement.
// Pointer-bearing fields (Addr/Off) widen to 64 bits; field order and
// alignment differ from ELF32 in non-obvious ways — DO NOT just s/U32/U64/
// on the ELF32 structs.

#define k_Elf64_Addr   U64
#define k_Elf64_Half   U16
#define k_Elf64_Off    U64
#define k_Elf64_Sword  S32
#define k_Elf64_Word   U32
#define k_Elf64_Sxword S64
#define k_Elf64_Xword  U64

PACKED(
struct k_Elf64_Ehdr {
    unsigned char  e_ident[k_EI_NIDENT];
    k_Elf64_Half   e_type;
    k_Elf64_Half   e_machine;
    k_Elf64_Word   e_version;
    k_Elf64_Addr   e_entry;
    k_Elf64_Off    e_phoff;
    k_Elf64_Off    e_shoff;
    k_Elf64_Word   e_flags;
    k_Elf64_Half   e_ehsize;
    k_Elf64_Half   e_phentsize;
    k_Elf64_Half   e_phnum;
    k_Elf64_Half   e_shentsize;
    k_Elf64_Half   e_shnum;
    k_Elf64_Half   e_shstrndx;
}
);

PACKED(
struct k_Elf64_Shdr {
    k_Elf64_Word   sh_name;
    k_Elf64_Word   sh_type;
    k_Elf64_Xword  sh_flags;
    k_Elf64_Addr   sh_addr;
    k_Elf64_Off    sh_offset;
    k_Elf64_Xword  sh_size;
    k_Elf64_Word   sh_link;
    k_Elf64_Word   sh_info;
    k_Elf64_Xword  sh_addralign;
    k_Elf64_Xword  sh_entsize;
}
);

// Field order in Phdr is different from ELF32: p_flags moves before p_offset.
PACKED(
struct k_Elf64_Phdr {
    k_Elf64_Word   p_type;
    k_Elf64_Word   p_flags;
    k_Elf64_Off    p_offset;
    k_Elf64_Addr   p_vaddr;
    k_Elf64_Addr   p_paddr;
    k_Elf64_Xword  p_filesz;
    k_Elf64_Xword  p_memsz;
    k_Elf64_Xword  p_align;
}
);

// e_machine value for x86-64 (a.k.a. AMD64). i386 ELFs use EM_386 = 0x03.
#define k_EM_X86_64 0x3E
#define k_EM_386    0x03

// ELFCLASS values in e_ident[4].
#define k_ELFCLASS32 1
#define k_ELFCLASS64 2

#endif
