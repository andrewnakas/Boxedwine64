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
#include <string>
#include <unordered_map>

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

    // Same as parse(FsOpenNode*) but operates on a contiguous in-memory
    // buffer. Used by self-tests with synthesized ELF blobs (no FS
    // round-trip), and internally by parse() after slurping the file.
    static Elf64ParseResult parseBuffer(const U8* data, U64 length);

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

    // Apply symbol-bound relocations (R_X86_64_GLOB_DAT, JUMP_SLOT, 64) against
    // the dynamic section at dyn.vaddr+reloc. Symbol names are resolved against
    // a flat caller-supplied table — for the full DT_NEEDED recursion path this
    // would be the merged symbol table across all loaded DSOs; for self-tests
    // the caller injects a small synthetic table.
    //
    // Result counts:
    //   resolved   = number of relocations whose symbol was found and written
    //   unresolved = symbol not in the table (caller decides whether to fail
    //                or defer to ld-linux)
    // Returns resolved + unresolved; RELATIVE/NONE entries are silently
    // ignored (they're handled by applyRelativeRelocations).
    //
    // Walks BOTH the DT_RELA table AND the DT_JMPREL (PLT) table — on x86-64
    // both are RELA-shaped and both can contain symbol-bound entries.
    //
    // Public so self-tests can exercise resolution without an FS round-trip.
    static U64 applySymbolRelocations(class KMemory64* mem,
                                      const Elf64DynamicInfo& dyn,
                                      U64 reloc,
                                      const std::unordered_map<std::string, U64>& symbols,
                                      const char* tag,
                                      U64* outResolved = nullptr,
                                      U64* outUnresolved = nullptr);

    // Extract DT_NEEDED entries from PT_DYNAMIC and resolve each one to a
    // shared-object name via DT_STRTAB. Returns library names in their
    // original PT_DYNAMIC order (which matters for symbol-resolution
    // ordering once DT_NEEDED recursion is wired). Returns an empty vector
    // if there's no PT_DYNAMIC, no DT_STRTAB, or no DT_NEEDED entries.
    //
    // Pure parser — does NOT open files, does NOT recurse. The file-loading
    // and recursive parse pass (real Milestone A3) consumes this list and
    // is gated on the rootfs containing libc.so.6.
    //
    // Public so self-tests can exercise the parse path against synthetic
    // dynamic-array bytes without an FS round-trip.
    static std::vector<std::string> extractNeededLibraries(class KMemory64* mem,
                                                          const Elf64DynamicInfo& dyn,
                                                          U64 reloc);

    // Map every PT_LOAD segment from a contiguous in-memory ELF buffer
    // into guest memory at the given relocation base. Mirrors the file-
    // backed loader path but takes its segment bytes from a buffer.
    //
    // Used by self-tests that synthesize ELF blobs in memory; the file-
    // backed loader path uses an internal helper with the same shape.
    //
    // Returns true on success.
    static bool mapSegmentsFromBuffer(class KMemory64* mem,
                                      const Elf64ParseResult& r,
                                      const U8* buffer,
                                      U64 bufferLength,
                                      U64 reloc,
                                      const char* tag);

    // Allocate and populate a static TLS block from the PT_TLS template.
    // Layout (glibc x86-64): [tls image at negative offsets] [TCB] ← FS.
    // TCB head's first qword is the self-pointer ($fs:0 = $fs).
    //
    //   imageBase = guest address from which the TLS template bytes
    //               (filesz bytes) should be read. For the main exe this
    //               is r.tls.vaddr + reloc.
    //   blockBase = guest address where the new TLS block is mapped. Must
    //               be page-aligned and pre-mapped by the caller (or
    //               picked from a known free region).
    //   Returns the FS base (= TCB address) on success, or 0 on failure.
    //
    // Public so self-tests can exercise template-copy without an FS
    // round-trip. Does NOT touch CPU64::fsbase — caller is responsible
    // for that (typically via arch_prctl(ARCH_SET_FS)).
    static U64 setupStaticTls(class KMemory64* mem,
                              const Elf64TlsInfo& tls,
                              U64 imageBase,
                              U64 blockBase);
#endif
};

#endif
