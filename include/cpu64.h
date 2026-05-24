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

    bool yield = false;
    U64  instructionCount = 0;

    void run();
    // Run up to maxInsn instructions or until yield / decode failure.
    // Returns the number actually executed. Used by the self-test harness
    // to avoid hanging the host on a buggy test program.
    U64 runBounded(U64 maxInsn);

    void push64(U64 value);
    U64  pop64();

private:
    U32 step();

    U8  fetchByte(U64 addr);
    U32 fetchDword(U64 addr);
    U64 fetchQword(U64 addr);

    // Decoded prefixes — populated by consumePrefixes() and consumed by the
    // opcode handler. Reset at the top of each step().
    struct Prefixes {
        U8   rex = 0;     // 0 if no REX, else 0x40..0x4F
        bool osize16 = false;   // 66h: operand size override
        bool asize32 = false;   // 67h: address size override (truncate to 32)
        U8   seg = 0;     // 0 none, else 0x64 (FS) or 0x65 (GS); other segs ignored in long mode
        U8   rep = 0;     // 0 none, 0xF2 REPNE, 0xF3 REP/REPE
    };

    // Effective operand of a ModR/M byte (excluding the "reg" field — that's
    // returned separately as regField). length is the count of bytes after
    // the opcode that the ModR/M+SIB+disp consume.
    struct ModRM {
        bool isReg = false;   // mod==11 → register direct
        U8   rmIndex = 0;     // 0..15 — REX.B extended
        U64  effAddr = 0;     // valid when !isReg (already finalized for RIP-rel)
        U8   length = 0;      // bytes consumed (modrm + optional sib + disp)
        U8   regField = 0;    // top-3-bit "reg" field, REX.R extended (always returned)
        bool isRipRel = false;// for diagnostic/future use
    };

    // Read prefixes starting at rip; advance an internal cursor. Returns
    // the byte index of the primary opcode, relative to rip.
    U32 consumePrefixes(Prefixes& out);

    // Decode the ModR/M byte at rip+offset using the prefixes. baseAfterImm
    // is what RIP would be at the end of the *full* instruction — needed for
    // RIP-relative effective address. Pass 0 if no further immediate; helpers
    // for common widths sit on top.
    ModRM decodeModRM(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes);

    // Read/write the operand using the effective ModR/M, in a given size
    // (1/2/4/8 bytes). 32-bit writes zero-extend per x86-64. 8-bit access
    // honours the REX-presence rule (with-REX uses SPL/BPL/SIL/DIL for
    // indices 4..7; without REX uses AH/CH/DH/BH).
    U64  loadRM(const ModRM& m, U32 size, bool rexPresent);
    void storeRM(const ModRM& m, U32 size, U64 value, bool rexPresent);

    // Whole-register 8-bit access (handles the AH/CH/DH/BH vs SPL/BPL/SIL/
    // DIL split). Used by both ModR/M memory ops (for reg field) and direct
    // register opcodes.
    U8   readReg8(U8 index, bool rexPresent);
    void writeReg8(U8 index, U8 value, bool rexPresent);

    // Execute one of the 8 ALU operations (0=ADD..7=CMP), update flags,
    // and store the result back to the chosen destination (r/m or reg-field
    // depending on destIsRM). CMP discards the result.
    void runAlu(U8 aluOp, U32 size, bool destIsRM, U64 lhs, U64 rhs,
                const ModRM& m, bool rexPresentLocal);

    // Evaluate a 4-bit condition code (Jcc/CMOVcc/SETcc share the same
    // encoding: 0=O 1=NO 2=B 3=AE 4=E 5=NE 6=BE 7=A 8=S 9=NS A=P B=NP
    // C=L D=GE E=LE F=G).
    bool evalCC(U8 cc) const;
};

#endif // BOXEDWINE_GUEST_X64
#endif
