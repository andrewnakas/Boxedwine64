/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __CPU64_H__
#define __CPU64_H__

#ifdef BOXEDWINE_GUEST_X64

#include "reg64.h"

class KThread;
class KMemory64;

// x86-64 register indices. These match the encoding in the ModR/M and REX
// bytes (REX.R/REX.X/REX.B extend the 3-bit field to 4 bits).
enum X64Reg : U8 {
    X64_RAX = 0, X64_RCX = 1, X64_RDX = 2, X64_RBX = 3,
    X64_RSP = 4, X64_RBP = 5, X64_RSI = 6, X64_RDI = 7,
    X64_R8  = 8, X64_R9  = 9, X64_R10 = 10, X64_R11 = 11,
    X64_R12 = 12, X64_R13 = 13, X64_R14 = 14, X64_R15 = 15,
    X64_REG_COUNT = 16
};

// Flag bits — same numeric layout as the 32-bit CPU. RFLAGS upper 32 bits
// are reserved (no documented user-visible bits live there on AMD64).
#define X64_CF  0x00000001
#define X64_PF  0x00000004
#define X64_AF  0x00000010
#define X64_ZF  0x00000040
#define X64_SF  0x00000080
#define X64_DF  0x00000400
#define X64_OF  0x00000800
#define X64_IF  0x00000200

// CPU64 is a parallel CPU for x86-64 guest binaries. v1 is interpreter
// only — no JIT. Lives alongside the existing 32-bit CPU; a process
// uses exactly one of the two depending on KProcess::is64Bit.
//
// Scope of v1:
//   - 16 GP regs (RAX..R15) with 64/32/16/8-bit aliasing
//   - RIP, RFLAGS, segment selectors (mostly inert under x86-64 long mode)
//   - 16 XMM registers (XMM0..XMM15) — declared, not yet operationally wired
//   - Interpreter loop that reads bytes from KMemory64 and dispatches
//
// Out of scope for this initial commit:
//   - Full instruction set (only a minimal subset wired here; the rest
//     stubs out to "unimplemented opcode" with a klog)
//   - Lazy flags (uses eager flag compute for simplicity until the
//     interpreter is correctness-verified end to end)
//   - AVX, FS/GS base via WRFSBASE/WRGSBASE (FS base is plumbed via
//     arch_prctl syscall in Phase 3)
class CPU64 {
public:
    explicit CPU64(KMemory64* memory);
    ~CPU64();

    Reg64 reg[X64_REG_COUNT] = {};
    U64   rip = 0;
    U32   rflags = 0x202; // IF | reserved bit 1
    U64   fsbase = 0;     // x86-64: set via arch_prctl(ARCH_SET_FS)
    U64   gsbase = 0;

    KThread*   thread = nullptr;
    KMemory64* memory = nullptr;

    // Interpreter entry. Runs until yield is true or an exception. yield
    // is the same control-flow signal used by the 32-bit CPU::run loop.
    bool yield = false;
    U64  instructionCount = 0;

    void run();

    // Stack helpers. RSP grows down; the x86-64 ABI requires 16-byte
    // alignment at function-call boundaries.
    void push64(U64 value);
    U64  pop64();

private:
    // Decode + execute one instruction at RIP. Returns the number of
    // bytes consumed, or 0 to signal "unimplemented / fatal".
    U32 step();

    // Helpers used by step(). Defined out-of-line in cpu64.cpp so this
    // header stays cheap to include.
    U8  fetchByte(U64 addr);
    U32 fetchDword(U64 addr);
    U64 fetchQword(U64 addr);
};

#endif // BOXEDWINE_GUEST_X64
#endif
