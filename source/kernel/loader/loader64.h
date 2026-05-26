/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __LOADER64_H__
#define __LOADER64_H__

#include "kelf64.h"
#include <vector>

class FsOpenNode;
class KThread;

// Parsed view of one PT_LOAD segment. All addresses are 64-bit guest
// virtual; offset/sizes are file offsets. Held only long enough for the
// mapper to act on.
struct Elf64LoadSegment {
    U64 vaddr;
    U64 memsz;
    U64 filesz;
    U64 offset;
    U32 flags; // p_flags: PF_R/PF_W/PF_X (1/2/4)
    U64 align;
};

// Captured PT_DYNAMIC location for later relocation processing. vaddr/memsz
// are the unrelocated values straight out of the phdr; the loader adds the
// load slide when reading the dynamic array from guest memory.
struct Elf64DynamicInfo {
    bool present = false;
    U64 vaddr = 0;
    U64 memsz = 0;
};

// Captured PT_TLS template info (used to size and copy per-thread TLS blocks).
struct Elf64TlsInfo {
    bool present = false;
    U64 vaddr = 0;
    U64 filesz = 0;
    U64 memsz = 0;
    U64 align = 0;
};

struct Elf64ParseResult {
    bool ok = false;
    U64 entry = 0;            // e_entry
    U64 phoff = 0;            // e_phoff (file offset of phdr table)
    U16 phentsize = 0;
    U16 phnum = 0;
    U64 baseAddrLow = 0;      // lowest p_vaddr across PT_LOAD
    U64 baseAddrHigh = 0;     // highest p_vaddr + p_memsz across PT_LOAD
    bool isPie = false;       // e_type == ET_DYN, needs relocation
    std::vector<Elf64LoadSegment> segments;
    BString interpreter;      // PT_INTERP contents (empty if none)
    Elf64DynamicInfo dynamic; // PT_DYNAMIC vaddr/memsz (empty if none)
    Elf64TlsInfo tls;         // PT_TLS template info (empty if none)
};

class ElfLoader64 {
public:
    // Pure-parse: reads the Ehdr + Phdrs and returns a structured view.
    // Does NOT touch guest memory or thread state. Safe to call without
    // any 64-bit CPU/memory subsystem present.
    static Elf64ParseResult parse(FsOpenNode* openNode);

#ifdef BOXEDWINE_GUEST_X64
    // Maps PT_LOAD segments into the thread's guest memory, sets RIP,
    // populates KProcess phdr/phentsize/phnum/loaderBaseAddress/brkEnd/entry.
    // Requires the thread's memory to be a 64-bit-capable KMemory.
    // Returns false if the binary cannot be loaded.
    //
    // NOT YET WIRED — depends on KMemory64 (Phase 1.5) and CPU64 (Phase 2).
    static bool loadProgram(KThread* thread, FsOpenNode* openNode, U64* rip);

    // Apply R_X86_64_RELATIVE relocations against the dynamic section at
    // dyn.vaddr+reloc. Public so self-tests can exercise the relocation
    // pass without an FS round-trip. Returns the number of RELATIVE
    // relocations applied; symbol-bound types (GLOB_DAT/JUMP_SLOT/64/COPY)
    // are skipped — those are Milestone A3's job.
    static U64 applyRelativeRelocations(class KMemory64* mem,
                                        const Elf64DynamicInfo& dyn,
                                        U64 reloc,
                                        const char* tag);
#endif
};

#endif
