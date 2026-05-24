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

// AH/CH/DH/BH addressing: only valid when there is no REX prefix at all.
// With any REX (even REX without W/R/X/B set, i.e. 0x40 itself), indices
// 4..7 of the 8-bit ModR/M reg field name SPL/BPL/SIL/DIL — the low bytes
// of RSP/RBP/RSI/RDI — instead of the high bytes of A/C/D/B.
U8 CPU64::readReg8(U8 index, bool rexPresent) {
    if (!rexPresent && index >= 4 && index <= 7) {
        return reg[index - 4].h8;
    }
    return reg[index].u8;
}

void CPU64::writeReg8(U8 index, U8 value, bool rexPresent) {
    if (!rexPresent && index >= 4 && index <= 7) {
        reg[index - 4].setH8(value);
        return;
    }
    reg[index].setU8(value);
}

U32 CPU64::consumePrefixes(Prefixes& out) {
    U32 off = 0;
    // Loop until we hit a non-prefix byte. REX, if present, MUST be the last
    // prefix immediately before the opcode (Intel SDM Vol.2 §2.2.1) — we
    // still permit the loop to encounter it and stop on the next byte.
    while (true) {
        U8 b = fetchByte(rip + off);
        switch (b) {
            case 0x66: out.osize16 = true; off++; continue;
            case 0x67: out.asize32 = true; off++; continue;
            case 0x64: out.seg = 0x64; off++; continue; // FS
            case 0x65: out.seg = 0x65; off++; continue; // GS
            case 0x26: case 0x2E: case 0x36: case 0x3E:
                // ES/CS/SS/DS segment overrides — ignored in long mode
                // (effective base is 0). Branch hints repurposed from 2E/3E
                // are likewise non-architectural.
                off++; continue;
            case 0xF0:
                // LOCK — treat as no-op semantically; we're single-threaded
                // at the guest level for now.
                off++; continue;
            case 0xF2: out.rep = 0xF2; off++; continue;
            case 0xF3: out.rep = 0xF3; off++; continue;
            default:
                if ((b & 0xF0) == 0x40) {
                    out.rex = b;
                    off++;
                }
                return off;
        }
    }
}

CPU64::ModRM CPU64::decodeModRM(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes) {
    ModRM m;
    U8 modrm = fetchByte(modrmAddr);
    U8 mod = (modrm >> 6) & 0x3;
    U8 regF = (modrm >> 3) & 0x7;
    U8 rm  =  modrm        & 0x7;

    m.regField = (U8)(regF | ((p.rex & 0x04) ? 0x08 : 0));   // REX.R extends
    m.length = 1;

    if (mod == 0x3) {
        m.isReg = true;
        m.rmIndex = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));  // REX.B extends
        return m;
    }

    // RIP-relative: mod=00, rm=101 (no SIB).
    if (mod == 0x0 && rm == 0x5) {
        S32 disp32 = (S32)fetchDword(modrmAddr + 1);
        m.length = 5;
        // Effective address uses RIP of the *next* instruction = modrmAddr +
        // (length of modrm+disp) + trailingImmBytes.
        m.effAddr = modrmAddr + m.length + trailingImmBytes + (U64)(S64)disp32;
        m.isRipRel = true;
        return m;
    }

    // SIB byte follows when rm == 100 (in any mod != 11).
    U64 base = 0;
    bool haveBase = true;
    if (rm == 0x4) {
        U8 sib = fetchByte(modrmAddr + 1);
        m.length = 2;
        U8 scale = (sib >> 6) & 0x3;
        U8 idxF  = (sib >> 3) & 0x7;
        U8 baseF =  sib        & 0x7;

        U8 idxIndex = (U8)(idxF | ((p.rex & 0x02) ? 0x08 : 0));   // REX.X
        U8 baseIndex = (U8)(baseF | ((p.rex & 0x01) ? 0x08 : 0)); // REX.B

        // Index field 100 with REX.X=0 → "no index". With REX.X=1 it's R12.
        U64 idxVal = 0;
        if (!(idxF == 0x4 && !(p.rex & 0x02))) {
            idxVal = reg[idxIndex].u64 << scale;
        }

        // Base field 101 with mod=00 → disp32 only, no base. Else base reg.
        if (baseF == 0x5 && mod == 0x0) {
            haveBase = false;
            base = 0;
        } else {
            base = reg[baseIndex].u64;
        }

        m.effAddr = base + idxVal;
    } else {
        U8 baseIndex = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));
        m.effAddr = reg[baseIndex].u64;
    }

    // Displacement.
    if (mod == 0x1) {
        S8 disp8 = (S8)fetchByte(modrmAddr + m.length);
        m.effAddr += (U64)(S64)disp8;
        m.length += 1;
    } else if (mod == 0x2 || (mod == 0x0 && rm == 0x4 && !haveBase)) {
        S32 disp32 = (S32)fetchDword(modrmAddr + m.length);
        m.effAddr += (U64)(S64)disp32;
        m.length += 4;
    }

    if (p.asize32) {
        m.effAddr &= 0xFFFFFFFFULL;
    }

    return m;
}

U64 CPU64::loadRM(const ModRM& m, U32 size, bool rexPresent) {
    if (m.isReg) {
        switch (size) {
            case 1: return readReg8(m.rmIndex, rexPresent);
            case 2: return reg[m.rmIndex].u16;
            case 4: return reg[m.rmIndex].u32;
            case 8: return reg[m.rmIndex].u64;
        }
        return 0;
    }
    switch (size) {
        case 1: return memory->readb(m.effAddr);
        case 2: return memory->readw(m.effAddr);
        case 4: return memory->readd(m.effAddr);
        case 8: return memory->readq(m.effAddr);
    }
    return 0;
}

void CPU64::storeRM(const ModRM& m, U32 size, U64 value, bool rexPresent) {
    if (m.isReg) {
        switch (size) {
            case 1: writeReg8(m.rmIndex, (U8)value, rexPresent); return;
            case 2: reg[m.rmIndex].setU16((U16)value); return;
            case 4: reg[m.rmIndex].setU32((U32)value); return; // zero-extends
            case 8: reg[m.rmIndex].setU64(value); return;
        }
        return;
    }
    switch (size) {
        case 1: memory->writeb(m.effAddr, (U8)value); return;
        case 2: memory->writew(m.effAddr, (U16)value); return;
        case 4: memory->writed(m.effAddr, (U32)value); return;
        case 8: memory->writeq(m.effAddr, value); return;
    }
}

// -----------------------------------------------------------------------
// Flag computation helpers.
//
// Strategy: eager compute. v1 prioritises correctness and inspectability
// over the 32-bit path's lazy-flags machinery. Once the interpreter is
// running real programs we can revisit and port lazy flags.
//
// Flag definitions per Intel SDM Vol.1 §3.4.3.1:
//   CF — carry/borrow out of MSB (unsigned overflow)
//   PF — even parity of low byte of result
//   AF — carry/borrow out of bit 3 (nibble), for BCD ops
//   ZF — result is zero
//   SF — MSB of result
//   OF — signed overflow (carry-in to MSB != carry-out)
//
// Width is the byte width of the operands (1/2/4/8). Flag formulas are the
// same as 32-bit Boxedwine's eager paths, generalised to U64.
// -----------------------------------------------------------------------

static inline U64 maskFor(U32 width) {
    if (width >= 8) return 0xFFFFFFFFFFFFFFFFULL;
    return (1ULL << (width * 8)) - 1ULL;
}
static inline U64 signBitFor(U32 width) {
    return 1ULL << (width * 8 - 1);
}
static inline bool parityEven(U8 b) {
    b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
    return (b & 1) == 0;
}

// Sets ZF/SF/PF based on the masked result. Caller fills CF/OF/AF.
static void setSZP(U32& f, U64 result, U32 width) {
    U64 r = result & maskFor(width);
    f &= ~(X64_ZF | X64_SF | X64_PF);
    if (r == 0) f |= X64_ZF;
    if (r & signBitFor(width)) f |= X64_SF;
    if (parityEven((U8)r)) f |= X64_PF;
}

static void flagsAdd(U32& f, U64 a, U64 b, U64 r, U32 width) {
    U64 mask = maskFor(width);
    a &= mask; b &= mask; U64 rm = r & mask;
    f &= ~(X64_CF | X64_OF | X64_AF);
    // CF: did the unsigned sum overflow the width?
    if (rm < a) f |= X64_CF;
    // OF: signs of operands match and differ from result sign.
    U64 sb = signBitFor(width);
    if (((~(a ^ b)) & (a ^ rm)) & sb) f |= X64_OF;
    if (((a ^ b ^ rm) & 0x10) != 0) f |= X64_AF;
    setSZP(f, rm, width);
}

static void flagsSub(U32& f, U64 a, U64 b, U64 r, U32 width) {
    U64 mask = maskFor(width);
    a &= mask; b &= mask; U64 rm = r & mask;
    f &= ~(X64_CF | X64_OF | X64_AF);
    if (a < b) f |= X64_CF;
    U64 sb = signBitFor(width);
    if (((a ^ b) & (a ^ rm)) & sb) f |= X64_OF;
    if (((a ^ b ^ rm) & 0x10) != 0) f |= X64_AF;
    setSZP(f, rm, width);
}

// Logical ops always clear CF and OF, leave AF undefined (we clear it).
static void flagsLogic(U32& f, U64 r, U32 width) {
    f &= ~(X64_CF | X64_OF | X64_AF);
    setSZP(f, r, width);
}

void CPU64::runAlu(U8 aluOp, U32 size, bool destIsRM, U64 lhs, U64 rhs,
                   const ModRM& m, bool rexPresentLocal) {
    // aluOp: 0=ADD 1=OR 2=ADC 3=SBB 4=AND 5=SUB 6=XOR 7=CMP
    U64 result = 0;
    bool storeBack = (aluOp != 7); // CMP discards result
    switch (aluOp) {
        case 0: result = lhs + rhs;
                flagsAdd(rflags, lhs, rhs, result, size); break;
        case 1: result = lhs | rhs;
                flagsLogic(rflags, result, size); break;
        case 2: { U64 cIn = (rflags & X64_CF) ? 1 : 0;
                result = lhs + rhs + cIn;
                flagsAdd(rflags, lhs, rhs + cIn, result, size); break; }
        case 3: { U64 cIn = (rflags & X64_CF) ? 1 : 0;
                result = lhs - rhs - cIn;
                flagsSub(rflags, lhs, rhs + cIn, result, size); break; }
        case 4: result = lhs & rhs;
                flagsLogic(rflags, result, size); break;
        case 5: result = lhs - rhs;
                flagsSub(rflags, lhs, rhs, result, size); break;
        case 6: result = lhs ^ rhs;
                flagsLogic(rflags, result, size); break;
        case 7: result = lhs - rhs;
                flagsSub(rflags, lhs, rhs, result, size); break;
    }
    if (!storeBack) return;
    if (destIsRM) {
        storeRM(m, size, result, rexPresentLocal);
    } else {
        switch (size) {
            case 1: writeReg8(m.regField, (U8)result, rexPresentLocal); break;
            case 2: reg[m.regField].setU16((U16)result); break;
            case 4: reg[m.regField].setU32((U32)result); break;
            case 8: reg[m.regField].setU64(result); break;
        }
    }
}

// Minimal x86-64 decode-and-execute. Grows opcode by opcode. Anything not
// handled logs the leading bytes and yields so we surface gaps quickly
// instead of looping.
U32 CPU64::step() {
    U64 ipStart = rip;

    Prefixes p;
    U32 opOff = consumePrefixes(p);
    bool rexW = (p.rex & 0x08) != 0;
    bool rexPresent = (p.rex != 0);

    // Default operand size in long mode is 32; REX.W → 64; 66h → 16.
    U32 opSize = rexW ? 8u : (p.osize16 ? 2u : 4u);

    U8 op = fetchByte(rip + opOff);

    // ---- Single-byte opcodes ----

    // PUSH r64 (50+rd). Always 64-bit in long mode (operand size override
    // ignored on near push of GPR).
    if (op >= 0x50 && op <= 0x57) {
        U8 r = (U8)((op - 0x50) | ((p.rex & 0x01) ? 0x08 : 0));
        push64(reg[r].u64);
        rip += opOff + 1;
        return opOff + 1;
    }
    // POP r64 (58+rd).
    if (op >= 0x58 && op <= 0x5F) {
        U8 r = (U8)((op - 0x58) | ((p.rex & 0x01) ? 0x08 : 0));
        reg[r].setU64(pop64());
        rip += opOff + 1;
        return opOff + 1;
    }

    // MOV r64, imm64 (B8+rd with REX.W). Without REX.W this would be the
    // 32-bit imm form (also legal; handled in the !rexW branch below).
    if (op >= 0xB8 && op <= 0xBF) {
        U8 r = (U8)((op - 0xB8) | ((p.rex & 0x01) ? 0x08 : 0));
        if (rexW) {
            U64 imm = fetchQword(rip + opOff + 1);
            reg[r].setU64(imm);
            rip += opOff + 1 + 8;
            return opOff + 1 + 8;
        } else if (p.osize16) {
            U16 imm = (U16)(fetchByte(rip + opOff + 1) |
                            ((U16)fetchByte(rip + opOff + 2) << 8));
            reg[r].setU16(imm);
            rip += opOff + 1 + 2;
            return opOff + 1 + 2;
        } else {
            U32 imm = fetchDword(rip + opOff + 1);
            reg[r].setU32(imm);   // zero-extends to 64
            rip += opOff + 1 + 4;
            return opOff + 1 + 4;
        }
    }

    // MOV r/m, r  (89 /r). Operand size from prefix/REX.W.
    if (op == 0x89) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 src;
        switch (opSize) {
            case 1: src = readReg8(m.regField, rexPresent); break;
            case 2: src = reg[m.regField].u16; break;
            case 4: src = reg[m.regField].u32; break;
            default: src = reg[m.regField].u64; break;
        }
        storeRM(m, opSize, src, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r, r/m  (8B /r). Reverse direction; same width rules.
    if (op == 0x8B) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 val = loadRM(m, opSize, rexPresent);
        switch (opSize) {
            case 1: writeReg8(m.regField, (U8)val, rexPresent); break;
            case 2: reg[m.regField].setU16((U16)val); break;
            case 4: reg[m.regField].setU32((U32)val); break;
            case 8: reg[m.regField].setU64(val); break;
        }
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m8, r8 (88 /r) and MOV r8, r/m8 (8A /r).
    if (op == 0x88) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 src = readReg8(m.regField, rexPresent);
        storeRM(m, 1, src, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }
    if (op == 0x8A) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 val = (U8)loadRM(m, 1, rexPresent);
        writeReg8(m.regField, val, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m, imm  (C7 /0). imm is imm32 sign-extended for 64-bit, or
    // imm32 zero-extended for 32-bit, or imm16 for 16-bit. (We don't
    // support C6 /0 imm8 here yet — symmetric add if needed.)
    if (op == 0xC7) {
        ModRM m = decodeModRM(rip + opOff + 1, p, opSize == 2 ? 2 : 4);
        // /0 is the only defined sub-op for C7; any other reg field falls
        // through to the unhandled diagnostic at the bottom.
        if ((m.regField & 0x7) == 0) {
            if (opSize == 2) {
                U16 imm = (U16)(fetchByte(rip + opOff + 1 + m.length) |
                                ((U16)fetchByte(rip + opOff + 1 + m.length + 1) << 8));
                storeRM(m, 2, imm, rexPresent);
                U32 used = opOff + 1 + m.length + 2;
                rip += used;
                return used;
            }
            S32 imm32 = (S32)fetchDword(rip + opOff + 1 + m.length);
            U64 val = (opSize == 8) ? (U64)(S64)imm32 : (U64)(U32)imm32;
            storeRM(m, opSize, val, rexPresent);
            U32 used = opOff + 1 + m.length + 4;
            rip += used;
            return used;
        }
    }

    // LEA r, m (8D /r). Effective address only — no memory access. opSize
    // controls how much of the computed address is written.
    if (op == 0x8D) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        // LEA with reg src is undefined; falls through to unhandled.
        if (!m.isReg) {
            U64 addr = m.effAddr;
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)addr); break;
                case 4: reg[m.regField].setU32((U32)addr); break;
                case 8: reg[m.regField].setU64(addr); break;
            }
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
    }

    // NOP (90). Also 0F 1F /0 multi-byte NOP (handled below in 0F prefix).
    if (op == 0x90) {
        rip += opOff + 1;
        return opOff + 1;
    }

    // SYSCALL (0F 05).
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x05) {
        // Per AMD64 ABI, SYSCALL clobbers RCX with the return RIP and R11
        // with RFLAGS. Userspace relies on this even when the syscall
        // succeeds — set both before transferring to the kernel layer.
        U64 nextRip = rip + opOff + 2;
        reg[X64_RCX].setU64(nextRip);
        reg[X64_R11].setU64((U64)rflags);
        rip = nextRip;
        ksyscall64(this);
        return opOff + 2;
    }

    // Multi-byte NOP: 0F 1F /0 (any ModR/M form, any length).
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x1F) {
        ModRM m = decodeModRM(rip + opOff + 2, p, 0);
        if ((m.regField & 0x7) == 0) {
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
    }

    // RET (C3) — near return, no immediate.
    if (op == 0xC3) {
        rip = pop64();
        return opOff + 1;
    }

    // ---- ALU r/r and r/m forms ----
    //
    // Encodings for ADD/OR/ADC/SBB/AND/SUB/XOR/CMP follow a regular pattern:
    //   00 r/m8,  r8        01 r/m, r        02 r8, r/m8        03 r, r/m
    //   04 AL, ib           05 (E|R)AX, iz
    // and the same six-form block repeats every 8 bytes:
    //   ADD=00 OR=08 ADC=10 SBB=18 AND=20 SUB=28 XOR=30 CMP=38.
    // Bytes ending in /6 or /7 within each row are legacy segment/BCD
    // opcodes (PUSH ES/POP ES/DAA/AAS/etc) — all invalid in long mode,
    // and 0x0F is the two-byte escape, so we filter those out.
    // 00/01/08/09/...3B — regular 6-row block (8 alu ops × 4 encodings).
    // Bits: top 3 = alu op (op>>3 & 7), low 3: 0=r/m8 r8, 1=r/m r, 2=r8 r/m8, 3=r r/m.
    if (op <= 0x3D && ((op & 0x06) != 0x06)) {
        // Exclude segment-prefix bytes (already handled) and the imm-acc forms below.
        U8 aluOp = (op >> 3) & 0x7;
        U8 form  = op & 0x7;
        if (form <= 3) {
            U32 size = (form & 1) ? opSize : 1;
            bool destIsRM = (form < 2);  // forms 0,1: dest is r/m; forms 2,3: dest is reg
            ModRM m = decodeModRM(rip + opOff + 1, p, 0);
            U64 a, b;
            if (destIsRM) {
                a = loadRM(m, size, rexPresent);
                b = (size == 1) ? readReg8(m.regField, rexPresent)
                  : (size == 2) ? (U64)reg[m.regField].u16
                  : (size == 4) ? (U64)reg[m.regField].u32
                                : reg[m.regField].u64;
            } else {
                a = (size == 1) ? readReg8(m.regField, rexPresent)
                  : (size == 2) ? (U64)reg[m.regField].u16
                  : (size == 4) ? (U64)reg[m.regField].u32
                                : reg[m.regField].u64;
                b = loadRM(m, size, rexPresent);
            }
            runAlu(aluOp, size, destIsRM, a, b, m, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (form == 4 || form == 5) {
            // 04 ib / 05 iz — ALU AL/AX/EAX/RAX, imm. AL form is 1 byte, EAX/RAX
            // form takes imm32 (sign-extended for 64-bit).
            U32 size = (form == 4) ? 1 : opSize;
            U64 imm = 0;
            U32 immLen = 0;
            if (size == 1) {
                imm = fetchByte(rip + opOff + 1); immLen = 1;
            } else if (size == 2) {
                imm = (U16)(fetchByte(rip + opOff + 1) |
                            ((U16)fetchByte(rip + opOff + 2) << 8)); immLen = 2;
            } else if (size == 4) {
                imm = fetchDword(rip + opOff + 1); immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(rip + opOff + 1);
                imm = (U64)(S64)i32; immLen = 4;
            }
            U64 a = (size == 1) ? reg[X64_RAX].u8
                  : (size == 2) ? reg[X64_RAX].u16
                  : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
            ModRM fake;
            fake.isReg = true; fake.rmIndex = X64_RAX; fake.regField = 0;
            // Reuse runAlu by faking dest=RM=RAX.
            runAlu(aluOp, size, true, a, imm, fake, rexPresent);
            U32 used = opOff + 1 + immLen;
            rip += used;
            return used;
        }
    }

    // 80/81/83 immediate-group ALU. /digit selects the alu op.
    // 80: r/m8, imm8.  81: r/m, imm(opSize, imm32 sign-ext for 64).
    // 83: r/m, imm8 sign-extended to opSize.
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        U32 size = (op == 0x80) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p,
            (op == 0x81) ? (size == 2 ? 2 : 4) : 1);
        U8 aluOp = m.regField & 0x7;
        U64 imm = 0; U32 immLen = 0;
        U64 immAddr = rip + opOff + 1 + m.length;
        if (op == 0x80 || op == 0x83) {
            S8 i8 = (S8)fetchByte(immAddr);
            imm = (U64)(S64)i8; immLen = 1;
            if (size == 2) imm &= 0xFFFF;
            else if (size == 4) imm &= 0xFFFFFFFFULL;
        } else { // 0x81
            if (size == 2) {
                imm = (U16)(fetchByte(immAddr) | ((U16)fetchByte(immAddr + 1) << 8));
                immLen = 2;
            } else if (size == 4) {
                imm = fetchDword(immAddr); immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(immAddr);
                imm = (U64)(S64)i32; immLen = 4;
            }
        }
        U64 a = loadRM(m, size, rexPresent);
        runAlu(aluOp, size, true, a, imm, m, rexPresent);
        U32 used = opOff + 1 + m.length + immLen;
        rip += used;
        return used;
    }

    // TEST r/m, r (84/85). Computes AND, sets flags, discards result.
    if (op == 0x84 || op == 0x85) {
        U32 size = (op == 0x84) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 a = loadRM(m, size, rexPresent);
        U64 b = (size == 1) ? readReg8(m.regField, rexPresent)
              : (size == 2) ? reg[m.regField].u16
              : (size == 4) ? reg[m.regField].u32 : reg[m.regField].u64;
        flagsLogic(rflags, a & b, size);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // TEST AL/AX/EAX/RAX, imm  (A8 ib / A9 iz).
    if (op == 0xA8 || op == 0xA9) {
        U32 size = (op == 0xA8) ? 1 : opSize;
        U64 a = (size == 1) ? reg[X64_RAX].u8
              : (size == 2) ? reg[X64_RAX].u16
              : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
        U64 imm; U32 immLen;
        if (size == 1) { imm = fetchByte(rip + opOff + 1); immLen = 1; }
        else if (size == 2) {
            imm = (U16)(fetchByte(rip + opOff + 1) |
                        ((U16)fetchByte(rip + opOff + 2) << 8));
            immLen = 2;
        } else if (size == 4) {
            imm = fetchDword(rip + opOff + 1); immLen = 4;
        } else {
            S32 i32 = (S32)fetchDword(rip + opOff + 1);
            imm = (U64)(S64)i32; immLen = 4;
        }
        flagsLogic(rflags, a & imm, size);
        U32 used = opOff + 1 + immLen;
        rip += used;
        return used;
    }

    // F6/F7 — group 3: /0 TEST imm, /2 NOT, /3 NEG, /4 MUL, /5 IMUL, /6 DIV, /7 IDIV.
    // v1: only /0 TEST imm wired (needed by ld-linux); rest unimpl.
    if (op == 0xF6 || op == 0xF7) {
        U32 size = (op == 0xF6) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p,
            (op == 0xF7 && size != 1) ? (size == 2 ? 2 : 4) : 1);
        U8 sub = m.regField & 0x7;
        if (sub == 0) {
            U64 a = loadRM(m, size, rexPresent);
            U64 imm; U32 immLen;
            U64 immAddr = rip + opOff + 1 + m.length;
            if (size == 1) { imm = fetchByte(immAddr); immLen = 1; }
            else if (size == 2) {
                imm = (U16)(fetchByte(immAddr) | ((U16)fetchByte(immAddr + 1) << 8));
                immLen = 2;
            } else if (size == 4) {
                imm = fetchDword(immAddr); immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(immAddr);
                imm = (U64)(S64)i32; immLen = 4;
            }
            flagsLogic(rflags, a & imm, size);
            U32 used = opOff + 1 + m.length + immLen;
            rip += used;
            return used;
        }
        if (sub == 2) { // NOT — no flags affected
            U64 a = loadRM(m, size, rexPresent);
            storeRM(m, size, ~a, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 3) { // NEG — same as 0 - r/m, flags set per SUB.
            U64 a = loadRM(m, size, rexPresent);
            U64 r = (U64)0 - a;
            flagsSub(rflags, 0, a, r, size);
            // NEG sets CF = (operand != 0). flagsSub already does this when a != 0.
            storeRM(m, size, r, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        // /4 /5 /6 /7 not yet implemented
        goto unhandled;
    }

    // INC/DEC r/m via FF /0 and /1. (Single-byte 40-4F encodings are REX
    // in long mode and are already consumed by the prefix loop.)
    if (op == 0xFF) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 sub = m.regField & 0x7;
        if (sub == 0 || sub == 1) {
            U64 a = loadRM(m, opSize, rexPresent);
            U64 r = (sub == 0) ? a + 1 : a - 1;
            // INC/DEC don't affect CF; preserve it. Other flags per ADD/SUB.
            U32 savedCF = rflags & X64_CF;
            if (sub == 0) flagsAdd(rflags, a, 1, r, opSize);
            else          flagsSub(rflags, a, 1, r, opSize);
            rflags = (rflags & ~X64_CF) | savedCF;
            storeRM(m, opSize, r, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 2) { // CALL r/m (always 64-bit operand in long mode)
            U64 target = loadRM(m, 8, rexPresent);
            U32 used = opOff + 1 + m.length;
            U64 nextRip = rip + used;
            push64(nextRip);
            rip = target;
            return used;
        }
        if (sub == 4) { // JMP r/m (always 64-bit)
            U64 target = loadRM(m, 8, rexPresent);
            rip = target;
            return opOff + 1 + m.length; // bytes consumed regardless
        }
        if (sub == 6) { // PUSH r/m
            U64 v = loadRM(m, 8, rexPresent);
            push64(v);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        goto unhandled;
    }

    // 8F /0 — POP r/m (operand size always 64 in long mode for POP).
    if (op == 0x8F) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        if ((m.regField & 0x7) != 0) goto unhandled;
        U64 v = pop64();
        storeRM(m, 8, v, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // ---- Control flow ----

    // CALL rel32 (E8 cd). Pushes RIP-of-next-instruction; jumps to
    // RIP-of-next + sign_ext(disp32).
    if (op == 0xE8) {
        S32 disp = (S32)fetchDword(rip + opOff + 1);
        U32 used = opOff + 1 + 4;
        U64 nextRip = rip + used;
        push64(nextRip);
        rip = nextRip + (U64)(S64)disp;
        return used;
    }

    // JMP rel32 (E9 cd).
    if (op == 0xE9) {
        S32 disp = (S32)fetchDword(rip + opOff + 1);
        U32 used = opOff + 1 + 4;
        rip = rip + used + (U64)(S64)disp;
        return used;
    }

    // JMP rel8 (EB cb).
    if (op == 0xEB) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 1 + 1;
        rip = rip + used + (U64)(S64)disp;
        return used;
    }

    // Jcc rel8 (70-7F). Condition encoded in low 4 bits.
    if (op >= 0x70 && op <= 0x7F) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 1 + 1;
        bool take = false;
        U8 cond = op & 0xF;
        switch (cond) {
            case 0x0: take = (rflags & X64_OF) != 0; break;                    // JO
            case 0x1: take = (rflags & X64_OF) == 0; break;                    // JNO
            case 0x2: take = (rflags & X64_CF) != 0; break;                    // JB/JC/JNAE
            case 0x3: take = (rflags & X64_CF) == 0; break;                    // JAE/JNB/JNC
            case 0x4: take = (rflags & X64_ZF) != 0; break;                    // JE/JZ
            case 0x5: take = (rflags & X64_ZF) == 0; break;                    // JNE/JNZ
            case 0x6: take = (rflags & (X64_CF | X64_ZF)) != 0; break;          // JBE/JNA
            case 0x7: take = (rflags & (X64_CF | X64_ZF)) == 0; break;          // JA/JNBE
            case 0x8: take = (rflags & X64_SF) != 0; break;                    // JS
            case 0x9: take = (rflags & X64_SF) == 0; break;                    // JNS
            case 0xA: take = (rflags & X64_PF) != 0; break;                    // JP/JPE
            case 0xB: take = (rflags & X64_PF) == 0; break;                    // JNP/JPO
            case 0xC: take = ((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0); break; // JL
            case 0xD: take = ((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0); break; // JGE
            case 0xE: take = ((rflags & X64_ZF) != 0) ||
                             (((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0)); break; // JLE
            case 0xF: take = ((rflags & X64_ZF) == 0) &&
                             (((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0)); break; // JG
        }
        rip = take ? (rip + used + (U64)(S64)disp) : (rip + used);
        return used;
    }

    // Jcc rel32 (0F 80-8F). Same condition encoding.
    if (op == 0x0F) {
        U8 op2 = fetchByte(rip + opOff + 1);
        if (op2 >= 0x80 && op2 <= 0x8F) {
            S32 disp = (S32)fetchDword(rip + opOff + 2);
            U32 used = opOff + 2 + 4;
            bool take = false;
            U8 cond = op2 & 0xF;
            switch (cond) {
                case 0x0: take = (rflags & X64_OF) != 0; break;
                case 0x1: take = (rflags & X64_OF) == 0; break;
                case 0x2: take = (rflags & X64_CF) != 0; break;
                case 0x3: take = (rflags & X64_CF) == 0; break;
                case 0x4: take = (rflags & X64_ZF) != 0; break;
                case 0x5: take = (rflags & X64_ZF) == 0; break;
                case 0x6: take = (rflags & (X64_CF | X64_ZF)) != 0; break;
                case 0x7: take = (rflags & (X64_CF | X64_ZF)) == 0; break;
                case 0x8: take = (rflags & X64_SF) != 0; break;
                case 0x9: take = (rflags & X64_SF) == 0; break;
                case 0xA: take = (rflags & X64_PF) != 0; break;
                case 0xB: take = (rflags & X64_PF) == 0; break;
                case 0xC: take = ((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0); break;
                case 0xD: take = ((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0); break;
                case 0xE: take = ((rflags & X64_ZF) != 0) ||
                                 (((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0)); break;
                case 0xF: take = ((rflags & X64_ZF) == 0) &&
                                 (((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0)); break;
            }
            rip = take ? (rip + used + (U64)(S64)disp) : (rip + used);
            return used;
        }

        // 0F B6 /r — MOVZX r, r/m8.  0F B7 /r — MOVZX r, r/m16.
        if (op2 == 0xB6 || op2 == 0xB7) {
            U32 srcSize = (op2 == 0xB6) ? 1 : 2;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 v = loadRM(m, srcSize, rexPresent);
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)v); break;
                case 4: reg[m.regField].setU32((U32)v); break;
                case 8: reg[m.regField].setU64(v); break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BE /r — MOVSX r, r/m8.  0F BF /r — MOVSX r, r/m16.
        if (op2 == 0xBE || op2 == 0xBF) {
            U32 srcSize = (op2 == 0xBE) ? 1 : 2;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 raw = loadRM(m, srcSize, rexPresent);
            S64 v = (srcSize == 1) ? (S64)(S8)raw : (S64)(S16)raw;
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)v); break;
                case 4: reg[m.regField].setU32((U32)v); break;
                case 8: reg[m.regField].setU64((U64)v); break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
    }

    // 63 /r — MOVSXD r64, r/m32 (with REX.W). Without REX.W it acts like
    // MOV r32, r/m32 (Intel: deprecated form). We support both.
    if (op == 0x63) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 raw = loadRM(m, 4, rexPresent);
        if (rexW) {
            S64 v = (S64)(S32)raw;
            reg[m.regField].setU64((U64)v);
        } else {
            reg[m.regField].setU32((U32)raw);
        }
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // LEAVE (C9). Equivalent to: RSP = RBP; RBP = pop64().
    if (op == 0xC9) {
        reg[X64_RSP].setU64(reg[X64_RBP].u64);
        reg[X64_RBP].setU64(pop64());
        rip += opOff + 1;
        return opOff + 1;
    }

unhandled:
    // Unimplemented. Print enough leading bytes to identify the opcode
    // in the Intel SDM tables and bail out so we don't silently loop.
    klog_fmt("CPU64: unimpl opcode at RIP=0x%llx bytes=%02x %02x %02x %02x %02x %02x %02x (rex=0x%02x osz=%d asz=%d seg=%02x rep=%02x)",
             (unsigned long long)ipStart,
             fetchByte(ipStart),
             fetchByte(ipStart + 1),
             fetchByte(ipStart + 2),
             fetchByte(ipStart + 3),
             fetchByte(ipStart + 4),
             fetchByte(ipStart + 5),
             fetchByte(ipStart + 6),
             p.rex, (int)p.osize16, (int)p.asize32, p.seg, p.rep);
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
