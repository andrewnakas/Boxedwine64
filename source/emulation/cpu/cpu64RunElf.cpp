/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "cpu64.h"
#include "kmemory64.h"
#include "../../kernel/loader/loader64.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// `--x64-run-elf [path]` — load an x86-64 ELF buffer into a fresh
// KMemory64 + CPU64 and run it to completion (or the instruction cap).
//
// Two sources:
//   - With a path: read the file from host disk.
//   - Without a path: use an embedded hand-built static ELF that prints
//     "hello, world\n" via raw write(1) + exit(0) syscalls. No libc, no
//     dynamic linker, no relocations — exercises the smallest possible
//     end-to-end path: parseBuffer → mapSegmentsFromBuffer → CPU64 →
//     sys_write64 (tees to host stdout) → sys_exit.
//
// The intent of the embedded payload is to prove the full pipeline works
// from a real ELF blob without needing a cross-compiler installed. Once a
// Docker / x86_64-linux-gnu-gcc path produces a real static-PIE glibc
// binary, drop it on disk and run `--x64-run-elf path/to/it`; the runner
// itself doesn't change.

namespace {

// Layout for the embedded hello-world ELF.
//   0x0000 Ehdr (64 bytes)
//   0x0040 Phdr (56 bytes, single PT_LOAD)
//   0x0078 Code (33 bytes)
//   0x0099 "hello, world\n" (13 bytes)
//   0x00A6 end
//
// LOAD vaddr = 0x400000, p_filesz = p_memsz = 0xA6, flags = PF_R|PF_W|PF_X.
// Entry = LOAD + CODE_OFF.
//
// Code (33 bytes):
//   48 8D 35 1A 00 00 00     lea  rsi, [rip+0x1A]   ; rsi = msg
//   BF 01 00 00 00           mov  edi, 1            ; fd = stdout
//   BA 0D 00 00 00           mov  edx, 13           ; len
//   B8 01 00 00 00           mov  eax, 1            ; SYS_write
//   0F 05                    syscall
//   B8 3C 00 00 00           mov  eax, 60           ; SYS_exit
//   31 FF                    xor  edi, edi          ; status = 0
//   0F 05                    syscall
//
// disp32 for lea: msg lives at CODE_OFF+33; the lea's next-RIP is
// CODE_OFF+7. So disp = 33 - 7 = 26 = 0x1A. (Vaddr offset is identical
// to file offset because both Ehdr+Phdr+code+msg are inside the single
// PT_LOAD that starts at p_offset=0, p_vaddr=LOAD_VADDR.)

std::vector<U8> buildEmbeddedHelloElf() {
    const U64 LOAD_VADDR = 0x400000;
    const U64 EHDR_OFF   = 0x0000;
    const U64 PHDR_OFF   = 0x0040;
    const U64 CODE_OFF   = 0x0078;
    const U64 MSG_OFF    = 0x0099;
    const U64 END_OFF    = 0x00A6;

    std::vector<U8> elf(END_OFF, 0);

    // Ehdr.
    k_Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7F;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = k_ELFCLASS64;
    eh.e_ident[5] = 1; // ELFDATA2LSB
    eh.e_ident[6] = 1; // EV_CURRENT
    eh.e_type     = 2; // ET_EXEC
    eh.e_machine  = k_EM_X86_64;
    eh.e_version  = 1;
    eh.e_entry    = LOAD_VADDR + CODE_OFF;
    eh.e_phoff    = PHDR_OFF;
    eh.e_ehsize   = sizeof(k_Elf64_Ehdr);
    eh.e_phentsize = sizeof(k_Elf64_Phdr);
    eh.e_phnum    = 1;
    memcpy(elf.data() + EHDR_OFF, &eh, sizeof(eh));

    // Phdr: PT_LOAD covers the whole file. RWX so writes (if any) work
    // and so we don't have to manage a separate writable data segment
    // for this throwaway payload.
    k_Elf64_Phdr ph{};
    ph.p_type   = k_PT_LOAD;
    ph.p_flags  = 7; // PF_R | PF_W | PF_X
    ph.p_offset = 0;
    ph.p_vaddr  = LOAD_VADDR;
    ph.p_paddr  = LOAD_VADDR;
    ph.p_filesz = END_OFF;
    ph.p_memsz  = END_OFF;
    ph.p_align  = 0x1000;
    memcpy(elf.data() + PHDR_OFF, &ph, sizeof(ph));

    // Code.
    U8* c = elf.data() + CODE_OFF;
    // lea rsi, [rip+0x1A]
    c[0] = 0x48; c[1] = 0x8D; c[2] = 0x35;
    c[3] = 0x1A; c[4] = 0x00; c[5] = 0x00; c[6] = 0x00;
    // mov edi, 1
    c[7]  = 0xBF; c[8]  = 0x01; c[9]  = 0x00; c[10] = 0x00; c[11] = 0x00;
    // mov edx, 13
    c[12] = 0xBA; c[13] = 0x0D; c[14] = 0x00; c[15] = 0x00; c[16] = 0x00;
    // mov eax, 1 (SYS_write)
    c[17] = 0xB8; c[18] = 0x01; c[19] = 0x00; c[20] = 0x00; c[21] = 0x00;
    // syscall
    c[22] = 0x0F; c[23] = 0x05;
    // mov eax, 60 (SYS_exit)
    c[24] = 0xB8; c[25] = 0x3C; c[26] = 0x00; c[27] = 0x00; c[28] = 0x00;
    // xor edi, edi
    c[29] = 0x31; c[30] = 0xFF;
    // syscall
    c[31] = 0x0F; c[32] = 0x05;

    // Message.
    const char* msg = "hello, world\n";
    memcpy(elf.data() + MSG_OFF, msg, 13);

    return elf;
}

bool readFileAll(const char* path, std::vector<U8>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    out.resize((size_t)sz);
    size_t got = std::fread(out.data(), 1, (size_t)sz, f);
    std::fclose(f);
    return got == (size_t)sz;
}

// Build a SysV x86-64 init stack on top of `mem`. Mirrors the layout in
// loader64.cpp's loadProgram but operates on raw KMemory64 (no KProcess),
// so the runner can hand off to glibc-style _start with a valid argc /
// argv / envp / auxv frame. Returns the resulting RSP value or 0 on failure.
//
// Layout (top-down):
//   strings (argv[0] + env)
//   AT_RANDOM pool (16 bytes)
//   <pad to 16B>
//   auxv[]   (AT_NULL terminated)
//   envp[]   (NULL terminated)
//   argv[]   (NULL terminated)
//   argc                              ← returned RSP
U64 buildSysVStack(KMemory64* mem,
                   U64 stackTop,
                   const Elf64ParseResult& r,
                   U64 reloc,
                   U64 interpBase,
                   const char* exeName) {
    enum {
        AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5,
        AT_PAGESZ = 6, AT_BASE = 7, AT_ENTRY = 9, AT_UID = 11,
        AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_RANDOM = 25,
        AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23,
    };

    std::vector<const char*> argv = { exeName };
    std::vector<const char*> envp = { "PATH=/bin:/usr/bin" };

    U64 sp = stackTop - 16;
    std::vector<U64> argvPtrs, envpPtrs;

    for (auto it = envp.rbegin(); it != envp.rend(); ++it) {
        U64 len = (U64)std::strlen(*it) + 1;
        sp -= len;
        mem->memcpyToGuest(sp, *it, len);
        envpPtrs.insert(envpPtrs.begin(), sp);
    }
    for (auto it = argv.rbegin(); it != argv.rend(); ++it) {
        U64 len = (U64)std::strlen(*it) + 1;
        sp -= len;
        mem->memcpyToGuest(sp, *it, len);
        argvPtrs.insert(argvPtrs.begin(), sp);
    }

    sp -= 16;
    U64 randomAddr = sp;
    for (int i = 0; i < 16; i++) mem->writeb(randomAddr + i, 0);

    struct Aux { U64 k, v; };
    U64 phdrAddr = r.baseAddrLow + reloc + r.phoff;
    std::vector<Aux> aux = {
        { AT_PHDR,   phdrAddr },
        { AT_PHENT,  r.phentsize },
        { AT_PHNUM,  r.phnum },
        { AT_PAGESZ, 4096 },
        { AT_BASE,   interpBase },
        { AT_ENTRY,  r.entry + reloc },
        { AT_UID,    0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_RANDOM, randomAddr },
        { AT_HWCAP,  0 }, { AT_CLKTCK, 100 }, { AT_SECURE, 0 },
        { AT_NULL,   0 },
    };

    U64 totalQ = 1 + argvPtrs.size() + 1 + envpPtrs.size() + 1 + 2 * aux.size();
    U64 afterSize = totalQ * 8;
    U64 targetSp = (sp - afterSize) & ~0xFULL;
    sp = targetSp + afterSize;

    auto pushQ = [&](U64 v) { sp -= 8; mem->writeq(sp, v); };

    for (auto it = aux.rbegin(); it != aux.rend(); ++it) {
        pushQ(it->v);
        pushQ(it->k);
    }
    pushQ(0);
    for (auto it = envpPtrs.rbegin(); it != envpPtrs.rend(); ++it) pushQ(*it);
    pushQ(0);
    for (auto it = argvPtrs.rbegin(); it != argvPtrs.rend(); ++it) pushQ(*it);
    pushQ((U64)argv.size());

    return sp;
}

} // namespace

extern "C" int runX64RunElf(const char* path) {
    std::vector<U8> elf;
    const char* tag;
    if (path) {
        if (!readFileAll(path, elf)) {
            printf("--x64-run-elf: cannot read '%s'\n", path);
            return 1;
        }
        tag = path;
        printf("\n=== CPU64 run ELF: %s (%zu bytes) ===\n", path, elf.size());
    } else {
        elf = buildEmbeddedHelloElf();
        tag = "embedded-hello";
        printf("\n=== CPU64 run ELF: embedded hello-world (%zu bytes) ===\n", elf.size());
    }

    Elf64ParseResult parsed = ElfLoader64::parseBuffer(elf.data(), elf.size());
    if (!parsed.ok) {
        printf("--x64-run-elf: parse failed\n");
        return 2;
    }
    printf("--x64-run-elf: entry=0x%llx phnum=%u segments=%zu isPie=%d\n",
           (unsigned long long)parsed.entry,
           (unsigned)parsed.phnum,
           parsed.segments.size(),
           parsed.isPie);

    // High stack mirroring the real loader (0x7FFFFFFFE000 minus 8 MiB).
    // Sized to safely host a SysV argc/argv/envp/auxv frame plus glibc
    // startup, while staying well above PIE reloc bases.
    const U64 STACK_TOP  = 0x7FFFFFFFE000ULL;
    const U64 STACK_SIZE = 8ULL * 1024 * 1024;
    const U64 STACK_BASE = STACK_TOP - STACK_SIZE;
    KMemory64 mem(nullptr);
    if (mem.mmapAnonymousFixed(STACK_BASE, STACK_SIZE, 3) != STACK_BASE) {
        printf("--x64-run-elf: stack mmap failed\n");
        return 3;
    }

    // PIE binaries (ET_DYN) need a load slide; pick a mid-range base well
    // away from the stack. Static ET_EXEC uses reloc=0.
    U64 reloc = parsed.isPie ? 0x555555554000ULL : 0ULL;
    if (!ElfLoader64::mapSegmentsFromBuffer(&mem, parsed, elf.data(), elf.size(), reloc, tag)) {
        printf("--x64-run-elf: map failed\n");
        return 3;
    }

    // Apply R_X86_64_RELATIVE relocations against the exe's PT_DYNAMIC.
    // Symbol-bound relocations are deferred to ld-linux (when we add
    // DT_NEEDED file-loading recursion).
    if (parsed.dynamic.present) {
        U64 applied = ElfLoader64::applyRelativeRelocations(&mem, parsed.dynamic, reloc, tag);
        printf("--x64-run-elf: applied %llu RELATIVE relocations\n",
               (unsigned long long)applied);
    }

    // Build the SysV initial stack so glibc-style _start sees the right
    // frame. AT_BASE is 0 here — interpreter loading isn't wired in the
    // runner yet, so dynamic binaries that need ld-linux won't get past
    // _dl_start. That's fine for static binaries and informative for
    // dynamic ones (the unimpl-tracer will report exactly where they die).
    U64 sp = buildSysVStack(&mem, STACK_TOP, parsed, reloc, /*interpBase=*/0,
                            path ? path : "/boxedwine/embedded");

    // Install initial-thread TLS block when the binary has a PT_TLS phdr.
    // Mirrors loadProgram64 so on-disk probes that exercise FS-relative
    // accesses see the same setup a real-loader-launched program would.
    U64 fsBaseToSet = 0;
    if (parsed.tls.present && parsed.tls.memsz) {
        const U64 TLS_BLOCK_BASE = 0x7FFFF7800000ULL;
        const U64 TLS_BLOCK_SIZE = 0x100000ULL;
        U64 mapped = mem.mmapAnonymousFixed(TLS_BLOCK_BASE, TLS_BLOCK_SIZE, 3);
        if (mapped == TLS_BLOCK_BASE) {
            U64 imageBase = parsed.tls.vaddr + reloc;
            fsBaseToSet = ElfLoader64::setupStaticTls(
                &mem, parsed.tls, imageBase, TLS_BLOCK_BASE);
            printf("--x64-run-elf: PT_TLS installed at 0x%llx, fsBase=0x%llx\n",
                   (unsigned long long)TLS_BLOCK_BASE,
                   (unsigned long long)fsBaseToSet);
        } else {
            printf("--x64-run-elf: PT_TLS block mmap failed (got 0x%llx)\n",
                   (unsigned long long)mapped);
        }
    }

    CPU64 cpu(&mem);
    cpu.rip = parsed.entry + reloc;
    cpu.reg[X64_RSP].setU64(sp);
    // SysV ABI: RDX at entry holds atexit-callback pointer or 0.
    cpu.reg[X64_RDX].setU64(0);
    if (fsBaseToSet) cpu.fsbase = fsBaseToSet;

    // Bounded run. 4M instructions is way more than the embedded payload
    // needs (it's ~7 instructions) but plenty of headroom for a real
    // glibc-startup binary to reveal the first unimplemented opcode via
    // the unimpl-tracer at cpu64.cpp:3654.
    const U64 INSN_LIMIT = 4ULL * 1024 * 1024;
    cpu.runBounded(INSN_LIMIT);

    printf("--x64-run-elf: stopped after %llu instructions (yield=%d RIP=0x%llx RAX=0x%llx)\n",
           (unsigned long long)cpu.instructionCount,
           cpu.yield,
           (unsigned long long)cpu.rip,
           (unsigned long long)cpu.reg[X64_RAX].u64);

    if (cpu.yield && cpu.instructionCount < INSN_LIMIT) {
        printf("--x64-run-elf: exit reached cleanly\n");
        return 0;
    }
    if (cpu.instructionCount >= INSN_LIMIT) {
        printf("--x64-run-elf: instruction cap hit — binary did not exit\n");
        return 4;
    }
    printf("--x64-run-elf: stopped without exit (likely decode fail; see tracer)\n");
    return 5;
}

#endif // BOXEDWINE_GUEST_X64
