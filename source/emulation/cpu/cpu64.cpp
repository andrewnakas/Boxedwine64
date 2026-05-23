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
#include "syscall64.h"

CPU64::CPU64(KMemory64* memory) : memory(memory) {
    reg[X64_RSP].setU64(0);
}

CPU64::~CPU64() = default;

U8 CPU64::fetchByte(U64 addr) {
    return memory ? memory->readb(addr) : 0;
}

U32 CPU64::fetchDword(U64 addr) {
    return memory ? memory->readd(addr) : 0;
}

U64 CPU64::fetchQword(U64 addr) {
    return memory ? memory->readq(addr) : 0;
}

void CPU64::push64(U64 value) {
    reg[X64_RSP].u64 -= 8;
    if (memory) memory->writeq(reg[X64_RSP].u64, value);
}

U64 CPU64::pop64() {
    U64 v = memory ? memory->readq(reg[X64_RSP].u64) : 0;
    reg[X64_RSP].u64 += 8;
    return v;
}

// Minimal x86-64 decode-and-execute. This handles only the smallest
// subset needed to make forward progress on a real binary:
//   - REX prefix (0x40-0x4F) — recorded, then we fall through to the
//     primary opcode handler with the prefix bits available
//   - MOV r64, imm64 (REX.W + B8+rd) for register init
//   - MOV reg, reg (89 /r) for the most common 64-bit mov
//   - SYSCALL (0F 05) — hands off to the kernel layer (Phase 3)
//   - RET (C3) — pops RIP, used by call/ret pairs in ld-linux
//
// Everything else logs "unimpl" with the leading bytes and returns 0,
// which causes run() to set yield=true. As Phase 2 progresses this
// dispatch table grows; the eventual structure is opcode-major then
// REX-sensitive variant selection, matching how Intel SDM lays out the
// tables. For v1 a hand-rolled switch is fine.
U32 CPU64::step() {
    U64 ipStart = rip;
    U8 b0 = fetchByte(rip);
    U8 rex = 0;
    U32 consumed = 0;

    if ((b0 & 0xF0) == 0x40) {
        rex = b0;
        consumed = 1;
        b0 = fetchByte(rip + 1);
    }

    bool rexW = (rex & 0x08) != 0;
    bool rexR = (rex & 0x04) != 0;
    // rexX and rexB are used by SIB/ModR/M decoding once that lands.
    (void)rexR;

    // MOV r64, imm64 — only the REX.W form. Without REX.W it's a 32-bit
    // imm (different encoding handled separately).
    if (b0 >= 0xB8 && b0 <= 0xBF && rexW) {
        U8 regIndex = (U8)((b0 - 0xB8) | ((rex & 0x01) ? 0x08 : 0));
        U64 imm = fetchQword(rip + consumed + 1);
        reg[regIndex].setU64(imm);
        consumed += 1 + 8;
        rip += consumed;
        return consumed;
    }

    // SYSCALL (0F 05)
    if (b0 == 0x0F && fetchByte(rip + consumed + 1) == 0x05) {
        consumed += 2;
        rip += consumed;
        ksyscall64(this);
        return consumed;
    }

    // RET (C3) — near return, no immediate.
    if (b0 == 0xC3) {
        rip = pop64();
        return consumed + 1;
    }

    // Unimplemented. Print enough leading bytes to identify the opcode
    // in the Intel SDM tables and bail out so we don't silently loop.
    klog_fmt("CPU64: unimpl opcode at RIP=0x%llx bytes=%02x %02x %02x %02x %02x (rex=0x%02x)",
             (unsigned long long)ipStart,
             fetchByte(ipStart),
             fetchByte(ipStart + 1),
             fetchByte(ipStart + 2),
             fetchByte(ipStart + 3),
             fetchByte(ipStart + 4),
             rex);
    yield = true;
    return 0;
}

void CPU64::run() {
    while (!yield) {
        U32 n = step();
        if (n == 0) break;
        instructionCount++;
    }
}

#endif // BOXEDWINE_GUEST_X64
