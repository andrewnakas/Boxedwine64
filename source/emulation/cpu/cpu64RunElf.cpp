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

    // Stack: one page below STACK_TOP. The embedded payload doesn't use
    // the stack, but glibc startup will; the runner sets it up either way.
    const U64 STACK_TOP = 0x800000;
    KMemory64 mem(nullptr);
    mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3);

    // PIE binaries (ET_DYN) need a load slide; for now pick a fixed one
    // that doesn't overlap the stack range. Static ET_EXEC uses reloc=0.
    U64 reloc = parsed.isPie ? 0x10000000ULL : 0ULL;
    if (!ElfLoader64::mapSegmentsFromBuffer(&mem, parsed, elf.data(), elf.size(), reloc, tag)) {
        printf("--x64-run-elf: map failed\n");
        return 3;
    }

    CPU64 cpu(&mem);
    cpu.rip = parsed.entry + reloc;
    cpu.reg[X64_RSP].setU64(STACK_TOP - 16);

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
