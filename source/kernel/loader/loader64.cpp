/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#include "loader64.h"
#include "kelf64.h"
#ifdef BOXEDWINE_GUEST_X64
#include "kmemory64.h"
#include "cpu64.h"
#endif

// ELF p_type values reused from loader.cpp. Kept local here to avoid a
// cross-file include just for two constants.
#ifndef PT_LOAD
#define PT_LOAD 1
#endif
#ifndef PT_INTERP
#define PT_INTERP 3
#endif

// ET_EXEC=2, ET_DYN=3. PIE executables (and shared libs) are ET_DYN.
#define ET_EXEC 2
#define ET_DYN  3

// Maximum sensible PT_INTERP path. Real-world values are well under 256;
// the cap protects against a malformed/hostile binary.
#define INTERP_PATH_MAX 1024

Elf64ParseResult ElfLoader64::parse(FsOpenNode* openNode) {
    Elf64ParseResult result;
    if (!openNode) {
        return result;
    }

    struct k_Elf64_Ehdr ehdr = {};
    openNode->seek(0);
    if (openNode->readNative((U8*)&ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        klog("ElfLoader64::parse: short read on Ehdr");
        return result;
    }

    // Caller is expected to have already verified the ELF64 magic via
    // isElf64Ident, but recheck here so this function is safe in isolation.
    if (ehdr.e_ident[0] != 0x7F ||
        ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' ||
        ehdr.e_ident[3] != 'F' ||
        ehdr.e_ident[4] != k_ELFCLASS64) {
        klog("ElfLoader64::parse: not an ELF64 file");
        return result;
    }
    if (ehdr.e_machine != k_EM_X86_64) {
        klog_fmt("ElfLoader64::parse: unsupported e_machine 0x%x (only x86-64 is supported in v1)", ehdr.e_machine);
        return result;
    }
    if (ehdr.e_phentsize != sizeof(struct k_Elf64_Phdr)) {
        klog_fmt("ElfLoader64::parse: e_phentsize %u != sizeof(Elf64_Phdr) %u",
                 (U32)ehdr.e_phentsize, (U32)sizeof(struct k_Elf64_Phdr));
        return result;
    }
    if (ehdr.e_phnum == 0) {
        klog("ElfLoader64::parse: e_phnum == 0, nothing to load");
        return result;
    }

    result.entry = ehdr.e_entry;
    result.phoff = ehdr.e_phoff;
    result.phentsize = ehdr.e_phentsize;
    result.phnum = ehdr.e_phnum;
    result.isPie = (ehdr.e_type == ET_DYN);

    // First pass: find load extents and the interpreter.
    U64 lo = (U64)-1;
    U64 hi = 0;
    for (U16 i = 0; i < ehdr.e_phnum; i++) {
        struct k_Elf64_Phdr phdr = {};
        openNode->seek(ehdr.e_phoff + (U64)i * ehdr.e_phentsize);
        if (openNode->readNative((U8*)&phdr, sizeof(phdr)) != sizeof(phdr)) {
            klog_fmt("ElfLoader64::parse: short read on Phdr %u", (U32)i);
            return result;
        }
        if (phdr.p_type == PT_LOAD) {
            Elf64LoadSegment seg;
            seg.vaddr  = phdr.p_vaddr;
            seg.memsz  = phdr.p_memsz;
            seg.filesz = phdr.p_filesz;
            seg.offset = phdr.p_offset;
            seg.flags  = phdr.p_flags;
            seg.align  = phdr.p_align;
            result.segments.push_back(seg);
            if (phdr.p_vaddr < lo) lo = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > hi) hi = phdr.p_vaddr + phdr.p_memsz;
        } else if (phdr.p_type == PT_INTERP) {
            if (phdr.p_filesz == 0 || phdr.p_filesz > INTERP_PATH_MAX) {
                klog_fmt("ElfLoader64::parse: PT_INTERP filesz %llu out of range",
                         (unsigned long long)phdr.p_filesz);
                return result;
            }
            char interp[INTERP_PATH_MAX + 1] = { 0 };
            openNode->seek(phdr.p_offset);
            if (openNode->readNative((U8*)interp, (U32)phdr.p_filesz) != (U32)phdr.p_filesz) {
                klog("ElfLoader64::parse: short read on PT_INTERP");
                return result;
            }
            interp[phdr.p_filesz] = 0;
            result.interpreter = BString::copy(interp);
        }
    }

    if (result.segments.empty()) {
        klog("ElfLoader64::parse: no PT_LOAD segments");
        return result;
    }
    result.baseAddrLow = lo;
    result.baseAddrHigh = hi;
    result.ok = true;
    return result;
}

#ifdef BOXEDWINE_GUEST_X64

// PIE relocation base for ET_DYN binaries when the loader hasn't picked
// one. Sits well below the x86-64 user-space cap (0x7FFFFFFFFFFF) and far
// above anything ELF32 ever touches, so the address is recognisable in
// logs.
#define X64_PIE_BASE 0x400000000ULL

#ifndef K_PROT_READ
#define K_PROT_READ  1
#define K_PROT_WRITE 2
#define K_PROT_EXEC  4
#endif

// p_flags bits: PF_X=1, PF_W=2, PF_R=4 (note: opposite of K_PROT_* ordering).
static U32 phdrFlagsToProt(U32 pFlags) {
    U32 prot = 0;
    if (pFlags & 0x4) prot |= K_PROT_READ;
    if (pFlags & 0x2) prot |= K_PROT_WRITE;
    if (pFlags & 0x1) prot |= K_PROT_EXEC;
    return prot;
}

bool ElfLoader64::loadProgram(KThread* thread, FsOpenNode* openNode, U64* rip) {
    Elf64ParseResult r = parse(openNode);
    if (!r.ok) {
        return false;
    }
    klog_fmt("loadProgram64: entry=0x%llx phoff=0x%llx phnum=%u segments=%u interp=%s pie=%d",
             (unsigned long long)r.entry,
             (unsigned long long)r.phoff,
             (U32)r.phnum,
             (U32)r.segments.size(),
             r.interpreter.length() ? r.interpreter.c_str() : "(none)",
             r.isPie ? 1 : 0);
    klog_fmt("loadProgram64: load range [0x%llx, 0x%llx) span=%llu bytes",
             (unsigned long long)r.baseAddrLow,
             (unsigned long long)r.baseAddrHigh,
             (unsigned long long)(r.baseAddrHigh - r.baseAddrLow));

    if (!thread || !thread->process) {
        klog("loadProgram64: null thread/process");
        return false;
    }
    KProcess* process = thread->process.get();
    if (!process->memory64) {
        process->memory64 = new KMemory64(process);
    }
    KMemory64* mem = process->memory64;

    U64 reloc = r.isPie ? X64_PIE_BASE : 0;

    for (const Elf64LoadSegment& seg : r.segments) {
        U64 vaddr = seg.vaddr + reloc;
        U64 alignedAddr = vaddr & ~K64_PAGE_MASK;
        U64 trailing = vaddr - alignedAddr;
        U64 mapLen = (seg.memsz + trailing + K64_PAGE_SIZE - 1) & ~K64_PAGE_MASK;
        U32 prot = phdrFlagsToProt(seg.flags);
        U64 mapped = mem->mmapAnonymousFixed(alignedAddr, mapLen, prot);
        if (mapped != alignedAddr) {
            klog_fmt("loadProgram64: mmap failed for segment at 0x%llx (got 0x%llx)",
                     (unsigned long long)alignedAddr, (unsigned long long)mapped);
            return false;
        }
        if (seg.filesz > 0) {
            // Copy file bytes through a host buffer (KMemory64 is not host-
            // backed in v1, so we can't pass an open file directly to it).
            std::vector<U8> buf((size_t)seg.filesz);
            openNode->seek((U64)seg.offset);
            U32 read = openNode->readNative(buf.data(), (U32)seg.filesz);
            if (read != seg.filesz) {
                klog_fmt("loadProgram64: short read on segment (got %u of %llu)",
                         read, (unsigned long long)seg.filesz);
                return false;
            }
            mem->memcpyToGuest(vaddr, buf.data(), seg.filesz);
        }
        klog_fmt("loadProgram64:   mapped seg vaddr=0x%llx len=0x%llx prot=0x%x filesz=0x%llx",
                 (unsigned long long)alignedAddr,
                 (unsigned long long)mapLen,
                 prot,
                 (unsigned long long)seg.filesz);
    }

    *rip = r.entry + reloc;
    klog_fmt("loadProgram64: RIP set to 0x%llx (pages mapped: %llu)",
             (unsigned long long)*rip,
             (unsigned long long)mem->mappedPageCount());

    // Populate process bookkeeping. ld-linux reads phdr via AT_PHDR; brk
    // syscall reads brkEnd64.
    process->entry64 = r.entry + reloc;
    process->phdr64 = r.baseAddrLow + reloc + r.phoff;
    process->phnum64 = r.phnum;
    process->phentsize64 = r.phentsize;
    process->brkEnd64 = (r.baseAddrHigh + reloc + K64_PAGE_SIZE - 1) & ~K64_PAGE_MASK;
    process->is64Bit = true;

    // -------------------------------------------------------------------
    // Build the initial user-mode stack.
    //
    // System V x86-64 init stack layout, top-down:
    //   <strings>           argv + envp string pool
    //   <padding to 16B>
    //   auxv [terminated by AT_NULL=0]
    //   envp [terminated by NULL]
    //   argv [terminated by NULL]
    //   argc (8 bytes)      ← RSP points here on entry
    //
    // The ABI requires (RSP & 0xF) == 0 at the *call* to _start, which
    // means RSP must be 16-byte aligned when _start begins. _start expects
    // argc at [RSP], argv at [RSP+8], etc.
    // -------------------------------------------------------------------
    const U64 STACK_TOP   = 0x7FFFFFFFE000ULL; // well below 0x7FFFFFFFFFFF
    const U64 STACK_SIZE  = 8ULL * 1024 * 1024;
    const U64 STACK_BASE  = STACK_TOP - STACK_SIZE;
    U64 mapped = mem->mmapAnonymousFixed(STACK_BASE, STACK_SIZE,
                                         K_PROT_READ | K_PROT_WRITE);
    if (mapped != STACK_BASE) {
        klog_fmt("loadProgram64: stack mmap failed (got 0x%llx)", (unsigned long long)mapped);
        return false;
    }

    // Build a minimal argv/envp: just the exe name. The real path will
    // come from KProcess::startProcess once we wire that side up; for now
    // a single-element argv lets us validate _start/_dl_start.
    const char* exeName = process->name.length() ? process->name.c_str() : "boxedwine64.exe";
    std::vector<BString> argv;
    argv.push_back(BString::copy(exeName));
    std::vector<BString> envp;
    envp.push_back(BString::copy("PATH=/bin:/usr/bin"));

    // First write all the strings near the top of stack, recording each
    // string's guest address.
    U64 sp = STACK_TOP - 16;
    std::vector<U64> argvPtrs;
    std::vector<U64> envpPtrs;
    for (auto it = envp.rbegin(); it != envp.rend(); ++it) {
        U64 len = it->length() + 1;
        sp -= len;
        mem->memcpyToGuest(sp, it->c_str(), len);
        envpPtrs.insert(envpPtrs.begin(), sp);
    }
    for (auto it = argv.rbegin(); it != argv.rend(); ++it) {
        U64 len = it->length() + 1;
        sp -= len;
        mem->memcpyToGuest(sp, it->c_str(), len);
        argvPtrs.insert(argvPtrs.begin(), sp);
    }

    // 16-byte random pool for AT_RANDOM (glibc TLS canary). Zero-filled is
    // fine for early bringup — deterministic and harmless.
    sp -= 16;
    U64 randomAddr = sp;
    mem->memsetGuest(randomAddr, 0, 16);

    // Align sp down to 16B. The vector below pushes pairs of qwords, so
    // we need (sp - totalSize) to land on a 16-byte boundary.
    auto pushQ = [&](U64 v) {
        sp -= 8;
        mem->writeq(sp, v);
    };

    // Compute auxv. Minimal viable set for glibc / ld-linux:
    enum {
        AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5,
        AT_PAGESZ = 6, AT_BASE = 7, AT_ENTRY = 9, AT_UID = 11,
        AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_RANDOM = 25,
        AT_HWCAP = 16, AT_CLKTCK = 17, AT_PLATFORM = 15, AT_SECURE = 23,
    };
    struct Aux { U64 k, v; };
    std::vector<Aux> aux = {
        { AT_PHDR,    process->phdr64 },
        { AT_PHENT,   process->phentsize64 },
        { AT_PHNUM,   process->phnum64 },
        { AT_PAGESZ,  K64_PAGE_SIZE },
        { AT_BASE,    0 },              // no separate interpreter base in v1
        { AT_ENTRY,   process->entry64 },
        { AT_UID,     process->userId },
        { AT_EUID,    process->effectiveUserId },
        { AT_GID,     process->groupId },
        { AT_EGID,    process->effectiveGroupId },
        { AT_RANDOM,  randomAddr },
        { AT_HWCAP,   0 },
        { AT_CLKTCK,  100 },
        { AT_SECURE,  0 },
        { AT_NULL,    0 },
    };

    // Total qwords that go onto the stack:
    //   argc(1) + argvPtrs + NULL + envpPtrs + NULL + 2*aux
    U64 totalQ = 1 + argvPtrs.size() + 1 + envpPtrs.size() + 1 + 2 * aux.size();
    // After pushing totalQ*8 bytes from sp, the new sp must be 16-aligned.
    U64 afterSize = totalQ * 8;
    U64 targetSp = (sp - afterSize) & ~0xFULL;
    sp = targetSp + afterSize;

    // Push from top of frame down to argc — meaning we walk in reverse.
    for (auto it = aux.rbegin(); it != aux.rend(); ++it) {
        pushQ(it->v);
        pushQ(it->k);
    }
    pushQ(0); // envp terminator
    for (auto it = envpPtrs.rbegin(); it != envpPtrs.rend(); ++it) pushQ(*it);
    pushQ(0); // argv terminator
    for (auto it = argvPtrs.rbegin(); it != argvPtrs.rend(); ++it) pushQ(*it);
    pushQ((U64)argv.size()); // argc

    // sp now points at argc; this is what RSP should be at _start.
    klog_fmt("loadProgram64: stack built, RSP=0x%llx argc=%u envc=%u auxc=%u",
             (unsigned long long)sp, (U32)argv.size(), (U32)envp.size(), (U32)aux.size());

    // Create the CPU64 and seed RIP/RSP. The thread's eip field stays at 0
    // because ELF64 processes don't use the 32-bit CPU — the schedule path
    // (when we add it) must branch on KProcess::is64Bit to pick which
    // run-loop to invoke.
    if (!process->cpu64) {
        process->cpu64 = new CPU64(mem);
    }
    process->cpu64->thread = thread;
    process->cpu64->rip = process->entry64;
    process->cpu64->reg[X64_RSP].setU64(sp);
    // System V calls _start with RDX = pointer to a function to register
    // with atexit, or 0 if none. Linux passes 0.
    process->cpu64->reg[X64_RDX].setU64(0);

    return true;
}
#endif
