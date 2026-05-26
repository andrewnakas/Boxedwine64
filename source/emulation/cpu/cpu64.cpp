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

#include <cmath>
#include <cstring>

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

    // FS/GS segment overrides add the per-thread segment base. In long mode
    // CS/DS/ES/SS bases are always 0 and any prefix for them is a no-op,
    // but FS and GS keep their MSR-controlled bases — that's how glibc and
    // ld-linux access TLS (mov rax, fs:[0x28] stack canary, etc.).
    if (p.seg == 0x64) m.effAddr += fsbase;
    else if (p.seg == 0x65) m.effAddr += gsbase;

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

bool CPU64::evalCC(U8 cc) const {
    switch (cc & 0xF) {
        case 0x0: return (rflags & X64_OF) != 0;
        case 0x1: return (rflags & X64_OF) == 0;
        case 0x2: return (rflags & X64_CF) != 0;
        case 0x3: return (rflags & X64_CF) == 0;
        case 0x4: return (rflags & X64_ZF) != 0;
        case 0x5: return (rflags & X64_ZF) == 0;
        case 0x6: return (rflags & (X64_CF | X64_ZF)) != 0;
        case 0x7: return (rflags & (X64_CF | X64_ZF)) == 0;
        case 0x8: return (rflags & X64_SF) != 0;
        case 0x9: return (rflags & X64_SF) == 0;
        case 0xA: return (rflags & X64_PF) != 0;
        case 0xB: return (rflags & X64_PF) == 0;
        case 0xC: return ((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0);
        case 0xD: return ((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0);
        case 0xE: return ((rflags & X64_ZF) != 0) ||
                         (((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0));
        case 0xF: return ((rflags & X64_ZF) == 0) &&
                         (((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0));
    }
    return false;
}

// Compute SHL/SHR/SAR/ROL/ROR/RCL/RCR result + flags. count is already masked.
// Returns the new value; flag effects are applied to rflags. For rotates the
// SZP flags are not touched (per Intel SDM); only CF (and OF for count==1).
static U64 doShift(U32& rflags, U8 sub, U64 v, U8 count, U32 width) {
    if (count == 0) return v;
    U64 mask = maskFor(width);
    v &= mask;
    U64 result = v;
    U64 sb = signBitFor(width);
    U32 wbits = width * 8;
    bool cf = (rflags & X64_CF) != 0;
    bool isRotate = (sub <= 3);
    switch (sub) {
        case 0: { // ROL
            U8 c = count % wbits;
            if (c == 0) {
                result = v;
                // CF still set from low bit of result per Intel
                cf = (v & 1) != 0;
            } else {
                result = ((v << c) | (v >> (wbits - c))) & mask;
                cf = (result & 1) != 0;
            }
            break;
        }
        case 1: { // ROR
            U8 c = count % wbits;
            if (c == 0) {
                result = v;
                cf = (v & sb) != 0;
            } else {
                result = ((v >> c) | (v << (wbits - c))) & mask;
                cf = (result & sb) != 0;
            }
            break;
        }
        case 2: { // RCL — rotate through carry, (wbits+1)-bit rotation
            U8 c = count % (wbits + 1);
            for (U8 i = 0; i < c; i++) {
                bool newCf = (result & sb) != 0;
                result = ((result << 1) | (cf ? 1 : 0)) & mask;
                cf = newCf;
            }
            break;
        }
        case 3: { // RCR
            U8 c = count % (wbits + 1);
            for (U8 i = 0; i < c; i++) {
                bool newCf = (result & 1) != 0;
                result = ((result >> 1) | (cf ? sb : 0)) & mask;
                cf = newCf;
            }
            break;
        }
        case 4: // SHL/SAL
        case 6: // alias
            cf = (v >> (wbits - count)) & 1;
            result = (v << count) & mask;
            break;
        case 5: // SHR
            cf = (v >> (count - 1)) & 1;
            result = v >> count;
            break;
        case 7: // SAR — arithmetic, replicate sign bit
            {
                S64 sv = (width == 8) ? (S64)v
                       : (width == 4) ? (S64)(S32)v
                       : (width == 2) ? (S64)(S16)v
                                      : (S64)(S8)v;
                cf = (v >> (count - 1)) & 1;
                result = (U64)(sv >> count) & mask;
            }
            break;
        default:
            return v;
    }
    rflags &= ~(X64_CF | X64_OF);
    if (cf) rflags |= X64_CF;
    if (count == 1) {
        bool of = false;
        if (sub == 4 || sub == 6) of = ((result & sb) != 0) != cf;
        else if (sub == 5) of = (v & sb) != 0;
        else if (sub == 0 || sub == 2) of = ((result & sb) != 0) != cf;   // ROL/RCL: MSB(result) XOR CF
        else if (sub == 1 || sub == 3) of = ((result & sb) != 0) != (((result << 1) & sb) != 0); // ROR/RCR: MSB XOR (MSB-1)
        if (of) rflags |= X64_OF;
    }
    if (!isRotate) setSZP(rflags, result, width);
    return result;
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

    // MOV r8, imm8 (B0+rb). REX.B extends the destination; absence of REX
    // means AH/BH/CH/DH for indices 4-7, REX present means SPL/BPL/SIL/DIL.
    if (op >= 0xB0 && op <= 0xB7) {
        U8 r = (U8)((op - 0xB0) | ((p.rex & 0x01) ? 0x08 : 0));
        U8 imm = fetchByte(rip + opOff + 1);
        writeReg8(r, imm, rexPresent);
        rip += opOff + 2;
        return opOff + 2;
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

    // NOP (90). 0x90 without REX prefix is also XCHG RAX,RAX which is a NOP.
    // With REX.B=1 it becomes XCHG R8,RAX — we fall through to the XCHG block
    // below in that case. opcode 0x90 with REX.B=0 is the plain NOP.
    if (op == 0x90 && !(p.rex & 0x01)) {
        rip += opOff + 1;
        return opOff + 1;
    }

    // XCHG r, RAX (90+rd). XCHG R8-R15 with RAX when REX.B=1. The plain 0x90
    // case is handled above as NOP. opSize from prefix/REX.W.
    if (op >= 0x90 && op <= 0x97) {
        U8 ri = (U8)((op - 0x90) | ((p.rex & 0x01) ? 0x08 : 0));
        if (ri == X64_RAX) {
            rip += opOff + 1;
            return opOff + 1;
        }
        U64 a, b;
        if (opSize == 2) { a = reg[X64_RAX].u16; b = reg[ri].u16; reg[X64_RAX].setU16((U16)b); reg[ri].setU16((U16)a); }
        else if (opSize == 4) { a = reg[X64_RAX].u32; b = reg[ri].u32; reg[X64_RAX].setU64((U32)b); reg[ri].setU64((U32)a); }
        else { a = reg[X64_RAX].u64; b = reg[ri].u64; reg[X64_RAX].setU64(b); reg[ri].setU64(a); }
        rip += opOff + 1;
        return opOff + 1;
    }

    // XCHG r/m, r (86 r/m8, 87 r/m). Atomic when used with LOCK; we're
    // single-threaded so plain swap is correct.
    if (op == 0x86 || op == 0x87) {
        U32 size = (op == 0x86) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 a = loadRM(m, size, rexPresent);
        U64 b = (size == 1) ? readReg8(m.regField, rexPresent)
              : (size == 2) ? (U64)reg[m.regField].u16
              : (size == 4) ? (U64)reg[m.regField].u32
                            : reg[m.regField].u64;
        storeRM(m, size, b, rexPresent);
        if (size == 1) writeReg8(m.regField, (U8)a, rexPresent);
        else if (size == 2) reg[m.regField].setU16((U16)a);
        else if (size == 4) reg[m.regField].setU64((U32)a);
        else reg[m.regField].setU64(a);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // CDQ/CQO (99). Sign-extend EAX/RAX into EDX/RDX. 16-bit form (with 66
    // prefix) is CWD: sign-extend AX into DX.
    if (op == 0x99) {
        if (opSize == 2) {
            reg[X64_RDX].setU16((reg[X64_RAX].u16 & 0x8000) ? 0xFFFFu : 0u);
        } else if (opSize == 4) {
            reg[X64_RDX].setU64((reg[X64_RAX].u32 & 0x80000000u) ? 0xFFFFFFFFu : 0u);
        } else {
            reg[X64_RDX].setU64((reg[X64_RAX].u64 & 0x8000000000000000ULL) ? ~(U64)0 : 0ULL);
        }
        rip += opOff + 1;
        return opOff + 1;
    }

    // CBW/CWDE/CDQE (98). Sign-extend AL→AX (16), AX→EAX (32), EAX→RAX (64).
    if (op == 0x98) {
        if (opSize == 2) reg[X64_RAX].setU16((U16)(S16)(S8)reg[X64_RAX].u8);
        else if (opSize == 4) reg[X64_RAX].setU64((U64)(U32)(S32)(S16)reg[X64_RAX].u16);
        else reg[X64_RAX].setU64((U64)(S64)(S32)reg[X64_RAX].u32);
        rip += opOff + 1;
        return opOff + 1;
    }

    // PUSH imm8 (6A) / PUSH imm32 sign-ext (68). Always 64-bit push in long mode.
    if (op == 0x6A || op == 0x68) {
        S64 imm = (op == 0x6A)
            ? (S64)(S8)fetchByte(rip + opOff + 1)
            : (S64)(S32)fetchDword(rip + opOff + 1);
        U32 immLen = (op == 0x6A) ? 1 : 4;
        U64 sp = reg[X64_RSP].u64 - 8;
        memory->writeq(sp, (U64)imm);
        reg[X64_RSP].setU64(sp);
        U32 used = opOff + 1 + immLen;
        rip += used;
        return used;
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
        if (sub == 4 || sub == 5) {
            // MUL (/4 unsigned) and IMUL (/5 signed) one-operand form.
            //   byte:  AX        = AL  * r/m8
            //   word:  DX:AX     = AX  * r/m16
            //   dword: EDX:EAX   = EAX * r/m32   (RDX/RAX upper bits zeroed)
            //   qword: RDX:RAX   = RAX * r/m64
            // CF=OF set when the high half is non-zero (MUL) or doesn't match
            // sign-extension of the low half (IMUL). SF/ZF/AF/PF undefined.
            U64 a = loadRM(m, size, rexPresent);
            bool isSigned = (sub == 5);
            U64 lo = 0, hi = 0;
            bool overflow = false;
            if (size == 1) {
                if (isSigned) {
                    S16 prod = (S16)(S8)reg[X64_RAX].u8 * (S16)(S8)a;
                    reg[X64_RAX].setU16((U16)prod);
                    overflow = ((S8)(prod & 0xFF) != prod);
                } else {
                    U16 prod = (U16)reg[X64_RAX].u8 * (U16)(U8)a;
                    reg[X64_RAX].setU16(prod);
                    overflow = (prod >> 8) != 0;
                }
            } else if (size == 2) {
                if (isSigned) {
                    S32 prod = (S32)(S16)reg[X64_RAX].u16 * (S32)(S16)(U16)a;
                    lo = (U16)prod;
                    hi = (U16)(prod >> 16);
                    overflow = ((S16)lo != prod);
                } else {
                    U32 prod = (U32)reg[X64_RAX].u16 * (U32)(U16)a;
                    lo = (U16)prod;
                    hi = (U16)(prod >> 16);
                    overflow = hi != 0;
                }
                reg[X64_RAX].setU16((U16)lo);
                reg[X64_RDX].setU16((U16)hi);
            } else if (size == 4) {
                if (isSigned) {
                    S64 prod = (S64)(S32)reg[X64_RAX].u32 * (S64)(S32)(U32)a;
                    lo = (U32)prod;
                    hi = (U32)((U64)prod >> 32);
                    overflow = ((S32)lo != prod);
                } else {
                    U64 prod = (U64)reg[X64_RAX].u32 * (U64)(U32)a;
                    lo = (U32)prod;
                    hi = (U32)(prod >> 32);
                    overflow = hi != 0;
                }
                // 32-bit dest writes zero-extend the full 64-bit reg.
                reg[X64_RAX].setU64(lo);
                reg[X64_RDX].setU64(hi);
            } else {
                __uint128_t prod;
                if (isSigned) {
                    __int128 p = (__int128)(S64)reg[X64_RAX].u64 * (__int128)(S64)a;
                    prod = (__uint128_t)p;
                    lo = (U64)prod;
                    hi = (U64)(prod >> 64);
                    overflow = ((S64)lo != p);
                } else {
                    prod = (__uint128_t)reg[X64_RAX].u64 * (__uint128_t)a;
                    lo = (U64)prod;
                    hi = (U64)(prod >> 64);
                    overflow = hi != 0;
                }
                reg[X64_RAX].setU64(lo);
                reg[X64_RDX].setU64(hi);
            }
            rflags &= ~(X64_CF | X64_OF);
            if (overflow) rflags |= (X64_CF | X64_OF);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 6 || sub == 7) {
            // DIV (/6 unsigned) and IDIV (/7 signed) one-operand form.
            //   byte:  AL  = AX        / r/m8 ;  AH = remainder
            //   word:  AX  = DX:AX     / r/m16; DX = remainder
            //   dword: EAX = EDX:EAX   / r/m32; EDX = remainder
            //   qword: RAX = RDX:RAX   / r/m64; RDX = remainder
            // Divide by zero or quotient overflow → #DE. We currently just
            // skip the write and continue; full #DE delivery is a TODO.
            U64 a = loadRM(m, size, rexPresent);
            if (a == 0) {
                klog_fmt("CPU64: DIV by zero at RIP=0x%llx (TODO: deliver #DE)",
                         (unsigned long long)rip);
                yield = true;
                return 0;
            }
            bool isSigned = (sub == 7);
            if (size == 1) {
                U16 num = reg[X64_RAX].u16;
                if (isSigned) {
                    S16 sn = (S16)num;
                    S8 d = (S8)a;
                    S16 q = sn / d;
                    S16 rem = sn % d;
                    reg[X64_RAX].setU8((U8)(S8)q);
                    reg[X64_RAX].setH8((U8)(S8)rem);
                } else {
                    U16 q = num / (U8)a;
                    U16 rem = num % (U8)a;
                    reg[X64_RAX].setU8((U8)q);
                    reg[X64_RAX].setH8((U8)rem);
                }
            } else if (size == 2) {
                U32 num = ((U32)reg[X64_RDX].u16 << 16) | reg[X64_RAX].u16;
                if (isSigned) {
                    S32 sn = (S32)num;
                    S16 d = (S16)a;
                    reg[X64_RAX].setU16((U16)(S16)(sn / d));
                    reg[X64_RDX].setU16((U16)(S16)(sn % d));
                } else {
                    reg[X64_RAX].setU16((U16)(num / (U16)a));
                    reg[X64_RDX].setU16((U16)(num % (U16)a));
                }
            } else if (size == 4) {
                U64 num = ((U64)reg[X64_RDX].u32 << 32) | reg[X64_RAX].u32;
                if (isSigned) {
                    S64 sn = (S64)num;
                    S32 d = (S32)a;
                    reg[X64_RAX].setU64((U64)(U32)(sn / d));
                    reg[X64_RDX].setU64((U64)(U32)(sn % d));
                } else {
                    reg[X64_RAX].setU64((U64)(U32)(num / (U32)a));
                    reg[X64_RDX].setU64((U64)(U32)(num % (U32)a));
                }
            } else {
                // 128/64 -> 64 quotient. Use __int128.
                __uint128_t num = ((__uint128_t)reg[X64_RDX].u64 << 64) | reg[X64_RAX].u64;
                if (isSigned) {
                    __int128 sn = (__int128)num;
                    S64 d = (S64)a;
                    reg[X64_RAX].setU64((U64)(sn / d));
                    reg[X64_RDX].setU64((U64)(sn % d));
                } else {
                    reg[X64_RAX].setU64((U64)(num / (__uint128_t)a));
                    reg[X64_RDX].setU64((U64)(num % (__uint128_t)a));
                }
            }
            // DIV/IDIV leave flags undefined per Intel SDM.
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
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
        rip = evalCC(op & 0xF) ? (rip + used + (U64)(S64)disp) : (rip + used);
        return used;
    }

    // Jcc rel32 (0F 80-8F). Same condition encoding.
    if (op == 0x0F) {
        U8 op2 = fetchByte(rip + opOff + 1);
        if (op2 >= 0x80 && op2 <= 0x8F) {
            S32 disp = (S32)fetchDword(rip + opOff + 2);
            U32 used = opOff + 2 + 4;
            rip = evalCC(op2 & 0xF) ? (rip + used + (U64)(S64)disp) : (rip + used);
            return used;
        }

        // 0F 40-4F — CMOVcc r, r/m. Same condition encoding as Jcc.
        if (op2 >= 0x40 && op2 <= 0x4F) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent);
            if (evalCC(op2 & 0xF)) {
                switch (opSize) {
                    case 2: reg[m.regField].setU16((U16)src); break;
                    case 4: reg[m.regField].setU32((U32)src); break;
                    case 8: reg[m.regField].setU64(src); break;
                }
            } else if (opSize == 4) {
                // Important x86-64 quirk: even when CMOVcc is NOT taken, the
                // 32-bit operand-size form still zero-extends the destination
                // (because the destination is the 32-bit name of the reg,
                // and *any* write to a 32-bit name zero-extends). So we
                // must write back the existing low 32 bits.
                reg[m.regField].setU32(reg[m.regField].u32);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F 90-9F — SETcc r/m8.
        if (op2 >= 0x90 && op2 <= 0x9F) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 val = evalCC(op2 & 0xF) ? 1 : 0;
            storeRM(m, 1, val, rexPresent);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F AF /r — IMUL r, r/m (two-operand). Signed multiply.
        if (op2 == 0xAF) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 a = (opSize == 2) ? reg[m.regField].u16
                  : (opSize == 4) ? reg[m.regField].u32 : reg[m.regField].u64;
            U64 b = loadRM(m, opSize, rexPresent);
            // Sign-extend operands to do a proper signed multiply.
            S64 sa, sb;
            if (opSize == 2) { sa = (S64)(S16)a; sb = (S64)(S16)b; }
            else if (opSize == 4) { sa = (S64)(S32)a; sb = (S64)(S32)b; }
            else { sa = (S64)a; sb = (S64)b; }
            S64 r = sa * sb;
            // CF/OF set if signed result doesn't fit in opSize.
            bool overflow = false;
            if (opSize == 2) overflow = (r != (S64)(S16)r);
            else if (opSize == 4) overflow = (r != (S64)(S32)r);
            else { __int128 r128 = (__int128)sa * (__int128)sb; overflow = (r128 != (__int128)r); }
            rflags &= ~(X64_CF | X64_OF);
            if (overflow) rflags |= X64_CF | X64_OF;
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)r); break;
                case 4: reg[m.regField].setU32((U32)r); break;
                case 8: reg[m.regField].setU64((U64)r); break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BC /r — BSF (bit scan forward). ZF=1 if src==0, else dest=index.
        // 0F BD /r — BSR (bit scan reverse).
        if (op2 == 0xBC || op2 == 0xBD) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            if (src == 0) {
                rflags |= X64_ZF;
                // Destination value is architecturally undefined; leave as-is.
            } else {
                rflags &= ~X64_ZF;
                U32 idx = 0;
                if (op2 == 0xBC) { while (((src >> idx) & 1) == 0) idx++; }
                else { idx = opSize * 8 - 1; while (((src >> idx) & 1) == 0) idx--; }
                switch (opSize) {
                    case 2: reg[m.regField].setU16((U16)idx); break;
                    case 4: reg[m.regField].setU32(idx); break;
                    case 8: reg[m.regField].setU64(idx); break;
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
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

        // 0F C8+rd — BSWAP r32 / r64. Reverses byte order. 16-bit form is
        // architecturally undefined; we treat it as zero-extend of low byte
        // swap which matches what most CPUs do (and glibc never emits the 16
        // form).
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            U8 ri = (U8)((op2 - 0xC8) | ((p.rex & 0x01) ? 0x08 : 0));
            if (opSize == 8) {
                U64 v = reg[ri].u64;
                v = __builtin_bswap64(v);
                reg[ri].setU64(v);
            } else {
                U32 v = reg[ri].u32;
                v = __builtin_bswap32(v);
                reg[ri].setU64(v);
            }
            rip += opOff + 2;
            return opOff + 2;
        }

        // 0F B0 /r — CMPXCHG r/m8, r8.  0F B1 /r — CMPXCHG r/m, r.
        // If AL/AX/EAX/RAX == r/m, store r into r/m and set ZF=1. Otherwise
        // load r/m into AL/AX/EAX/RAX and clear ZF. Flags follow the implicit
        // SUB of acc vs r/m.
        if (op2 == 0xB0 || op2 == 0xB1) {
            U32 size = (op2 == 0xB0) ? 1 : opSize;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 dest = loadRM(m, size, rexPresent);
            U64 acc = (size == 1) ? reg[X64_RAX].u8
                    : (size == 2) ? (U64)reg[X64_RAX].u16
                    : (size == 4) ? (U64)reg[X64_RAX].u32
                                  : reg[X64_RAX].u64;
            U64 cmpRes = acc - dest;
            flagsSub(rflags, acc, dest, cmpRes, size);
            if (acc == dest) {
                U64 src = (size == 1) ? readReg8(m.regField, rexPresent)
                        : (size == 2) ? (U64)reg[m.regField].u16
                        : (size == 4) ? (U64)reg[m.regField].u32
                                      : reg[m.regField].u64;
                storeRM(m, size, src, rexPresent);
            } else {
                if (size == 1) reg[X64_RAX].setU8((U8)dest);
                else if (size == 2) reg[X64_RAX].setU16((U16)dest);
                else if (size == 4) reg[X64_RAX].setU64((U32)dest);
                else reg[X64_RAX].setU64(dest);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F A3 /r — BT  r/m, r       (test bit, no modify)
        // 0F AB /r — BTS r/m, r       (set bit, return old in CF)
        // 0F B3 /r — BTR r/m, r       (reset bit)
        // 0F BB /r — BTC r/m, r       (complement bit)
        if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 v = loadRM(m, opSize, rexPresent);
            U64 idx = (opSize == 8) ? reg[m.regField].u64 :
                      (opSize == 4) ? (U64)reg[m.regField].u32 :
                                      (U64)reg[m.regField].u16;
            U64 bit = idx & (opSize * 8 - 1);
            U64 mask = 1ULL << bit;
            bool old = (v & mask) != 0;
            rflags &= ~X64_CF;
            if (old) rflags |= X64_CF;
            if (op2 != 0xA3) {
                U64 nv = v;
                if (op2 == 0xAB) nv |= mask;
                else if (op2 == 0xB3) nv &= ~mask;
                else nv ^= mask; // BTC
                storeRM(m, opSize, nv, rexPresent);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BA /r — group with imm8: /4 BT  /5 BTS  /6 BTR  /7 BTC
        if (op2 == 0xBA) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 sub = m.regField & 0x7;
            if (sub >= 4 && sub <= 7) {
                U64 v = loadRM(m, opSize, rexPresent);
                U8 imm = fetchByte(rip + opOff + 2 + m.length);
                U64 bit = imm & (opSize * 8 - 1);
                U64 mask = 1ULL << bit;
                bool old = (v & mask) != 0;
                rflags &= ~X64_CF;
                if (old) rflags |= X64_CF;
                if (sub != 4) {
                    U64 nv = v;
                    if (sub == 5) nv |= mask;
                    else if (sub == 6) nv &= ~mask;
                    else nv ^= mask;
                    storeRM(m, opSize, nv, rexPresent);
                }
                U32 used = opOff + 2 + m.length + 1;
                rip += used;
                return used;
            }
        }

        // 0F A4 /r ib — SHLD r/m, r, imm8 (double-precision shift left)
        // 0F A5 /r    — SHLD r/m, r, CL
        // 0F AC /r ib — SHRD r/m, r, imm8
        // 0F AD /r    — SHRD r/m, r, CL
        if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {
            bool isLeft = (op2 == 0xA4 || op2 == 0xA5);
            bool hasImm = (op2 == 0xA4 || op2 == 0xAC);
            ModRM m = decodeModRM(rip + opOff + 2, p, hasImm ? 1 : 0);
            U64 dest = loadRM(m, opSize, rexPresent);
            U64 src = (opSize == 8) ? reg[m.regField].u64 :
                      (opSize == 4) ? (U64)reg[m.regField].u32 :
                                      (U64)reg[m.regField].u16;
            U8 count;
            U32 immLen = 0;
            if (hasImm) {
                count = fetchByte(rip + opOff + 2 + m.length);
                immLen = 1;
            } else {
                count = reg[X64_RCX].u8;
            }
            count &= (opSize == 8) ? 0x3F : 0x1F;
            U64 mask = maskFor(opSize);
            dest &= mask; src &= mask;
            if (count != 0) {
                U64 result;
                bool cf;
                if (isLeft) {
                    cf = (dest >> (opSize * 8 - count)) & 1;
                    result = ((dest << count) | (src >> (opSize * 8 - count))) & mask;
                } else {
                    cf = (dest >> (count - 1)) & 1;
                    result = ((dest >> count) | (src << (opSize * 8 - count))) & mask;
                }
                rflags &= ~(X64_CF | X64_OF);
                if (cf) rflags |= X64_CF;
                setSZP(rflags, result, opSize);
                storeRM(m, opSize, result, rexPresent);
            }
            U32 used = opOff + 2 + m.length + immLen;
            rip += used;
            return used;
        }

        // F3 0F B8 — POPCNT r, r/m  (REP prefix selects POPCNT vs BSF)
        // F3 0F BC — TZCNT (vs 0F BC BSF)
        // F3 0F BD — LZCNT (vs 0F BD BSR)
        if ((op2 == 0xB8 && p.rep == 0xF3) ||
            (op2 == 0xBC && p.rep == 0xF3) ||
            (op2 == 0xBD && p.rep == 0xF3)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            U64 result;
            if (op2 == 0xB8) { // POPCNT
                result = __builtin_popcountll(src);
            } else if (op2 == 0xBC) { // TZCNT
                result = src ? __builtin_ctzll(src) : opSize * 8;
            } else { // LZCNT
                U32 bits = opSize * 8;
                result = src ? (__builtin_clzll(src) - (64 - bits)) : bits;
            }
            if (opSize == 8) reg[m.regField].setU64(result);
            else if (opSize == 4) reg[m.regField].setU64((U32)result);
            else reg[m.regField].setU16((U16)result);
            rflags &= ~(X64_ZF | X64_CF | X64_OF | X64_SF | X64_PF | X64_AF);
            if (op2 == 0xB8) { if (result == 0) rflags |= X64_ZF; }
            else if (op2 == 0xBC) { if (src == 0) rflags |= X64_CF; if ((result & maskFor(opSize)) == 0) rflags |= X64_ZF; }
            else { if (src == 0) rflags |= X64_CF; if ((result & maskFor(opSize)) == 0) rflags |= X64_ZF; }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BC — BSF r, r/m  (bit scan forward); 0F BD — BSR (reverse).
        if (op2 == 0xBC || op2 == 0xBD) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            rflags &= ~X64_ZF;
            if (src == 0) {
                rflags |= X64_ZF;
                // dest undefined; leave unchanged
            } else {
                U64 idx = (op2 == 0xBC) ? __builtin_ctzll(src)
                                        : (63 - __builtin_clzll(src));
                if (opSize == 8) reg[m.regField].setU64(idx);
                else if (opSize == 4) reg[m.regField].setU64((U32)idx);
                else reg[m.regField].setU16((U16)idx);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F 31 — RDTSC. Returns guest "cycles" in EDX:EAX. We just shove the
        // host microsecond counter in; precision doesn't matter for early boot.
        if (op2 == 0x31) {
            U64 tsc = KSystem::getSystemTimeAsMicroSeconds();
            reg[X64_RAX].setU64((U32)(tsc & 0xFFFFFFFF));
            reg[X64_RDX].setU64((U32)(tsc >> 32));
            rip += opOff + 2;
            return opOff + 2;
        }

        // 0F C0 /r — XADD r/m8, r8.  0F C1 /r — XADD r/m, r.
        // tmp = r/m + r;  r = r/m;  r/m = tmp. Flags follow the ADD.
        if (op2 == 0xC0 || op2 == 0xC1) {
            U32 size = (op2 == 0xC0) ? 1 : opSize;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 d = loadRM(m, size, rexPresent);
            U64 s = (size == 1) ? readReg8(m.regField, rexPresent)
                  : (size == 2) ? (U64)reg[m.regField].u16
                  : (size == 4) ? (U64)reg[m.regField].u32
                                : reg[m.regField].u64;
            U64 sum = d + s;
            flagsAdd(rflags, d, s, sum, size);
            // src reg <- old r/m
            if (size == 1) writeReg8(m.regField, (U8)d, rexPresent);
            else if (size == 2) reg[m.regField].setU16((U16)d);
            else if (size == 4) reg[m.regField].setU64((U32)d);
            else reg[m.regField].setU64(d);
            storeRM(m, size, sum, rexPresent);
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

    // Shift group — D0/D1/D2/D3/C0/C1 with /digit selecting the op:
    //   /4 SHL  /5 SHR  /6 alias SHL  /7 SAR
    //   /0 ROL  /1 ROR  /2 RCL  /3 RCR
    // D0/C0 = byte form; D1/C1 = opSize form. D0/D1 shift by 1; D2/D3 shift
    // by CL; C0/C1 shift by imm8.
    if (op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3 ||
        op == 0xC0 || op == 0xC1) {
        U32 size = (op == 0xD0 || op == 0xD2 || op == 0xC0) ? 1 : opSize;
        bool hasImm = (op == 0xC0 || op == 0xC1);
        ModRM m = decodeModRM(rip + opOff + 1, p, hasImm ? 1 : 0);
        U8 sub = m.regField & 0x7;
        if (sub <= 7) {
            U8 count;
            U32 immLen = 0;
            if (op == 0xD0 || op == 0xD1) {
                count = 1;
            } else if (op == 0xD2 || op == 0xD3) {
                count = reg[X64_RCX].u8;
            } else {
                count = fetchByte(rip + opOff + 1 + m.length);
                immLen = 1;
            }
            // Mask count: 5 bits for 8/16/32, 6 bits for 64.
            count &= (size == 8) ? 0x3F : 0x1F;
            U64 v = loadRM(m, size, rexPresent);
            U64 r = doShift(rflags, sub, v, count, size);
            if (count != 0) storeRM(m, size, r, rexPresent);
            else if (size == 4 && m.isReg) {
                // Zero-extend the 32-bit destination even on count==0.
                reg[m.rmIndex].setU32((U32)v);
            }
            U32 used = opOff + 1 + m.length + immLen;
            rip += used;
            return used;
        }
    }

    // IMUL r, r/m, imm (69 iz / 6B ib). Three-operand signed multiply.
    if (op == 0x69 || op == 0x6B) {
        ModRM m = decodeModRM(rip + opOff + 1, p,
            (op == 0x69) ? (opSize == 2 ? 2 : 4) : 1);
        U64 a = loadRM(m, opSize, rexPresent);
        S64 imm;
        U32 immLen;
        U64 immAddr = rip + opOff + 1 + m.length;
        if (op == 0x6B) {
            imm = (S64)(S8)fetchByte(immAddr); immLen = 1;
        } else if (opSize == 2) {
            imm = (S64)(S16)(fetchByte(immAddr) |
                             ((U16)fetchByte(immAddr + 1) << 8));
            immLen = 2;
        } else {
            imm = (S64)(S32)fetchDword(immAddr); immLen = 4;
        }
        S64 sa = (opSize == 2) ? (S64)(S16)a
               : (opSize == 4) ? (S64)(S32)a : (S64)a;
        S64 r = sa * imm;
        bool overflow = false;
        if (opSize == 2) overflow = (r != (S64)(S16)r);
        else if (opSize == 4) overflow = (r != (S64)(S32)r);
        else { __int128 r128 = (__int128)sa * (__int128)imm; overflow = (r128 != (__int128)r); }
        rflags &= ~(X64_CF | X64_OF);
        if (overflow) rflags |= X64_CF | X64_OF;
        switch (opSize) {
            case 2: reg[m.regField].setU16((U16)r); break;
            case 4: reg[m.regField].setU32((U32)r); break;
            case 8: reg[m.regField].setU64((U64)r); break;
        }
        U32 used = opOff + 1 + m.length + immLen;
        rip += used;
        return used;
    }

    // String ops — MOVS and STOS. Address size in long mode defaults to 64
    // (RSI/RDI/RCX); with 0x67 prefix it's ESI/EDI/ECX. Direction flag DF
    // controls increment vs decrement. With REP (F3), repeat until RCX==0.
    //
    // MOVSB/MOVSW/MOVSD/MOVSQ — A4 (byte), A5 (opSize).
    // STOSB/STOSW/STOSD/STOSQ — AA (byte), AB (opSize). Source is RAX.
    if (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB) {
        U32 size = (op == 0xA4 || op == 0xAA) ? 1 : opSize;
        bool isStos = (op == 0xAA || op == 0xAB);
        S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        U64 count = (p.rep != 0) ? reg[X64_RCX].u64 : 1;
        if (p.asize32) count &= 0xFFFFFFFFULL;
        while (count--) {
            U64 src = reg[X64_RSI].u64;
            U64 dst = reg[X64_RDI].u64;
            if (p.asize32) { src &= 0xFFFFFFFFULL; dst &= 0xFFFFFFFFULL; }
            U64 val;
            if (isStos) {
                val = (size == 1) ? reg[X64_RAX].u8
                    : (size == 2) ? reg[X64_RAX].u16
                    : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
            } else {
                switch (size) {
                    case 1: val = memory->readb(src); break;
                    case 2: val = memory->readw(src); break;
                    case 4: val = memory->readd(src); break;
                    default: val = memory->readq(src); break;
                }
            }
            switch (size) {
                case 1: memory->writeb(dst, (U8)val); break;
                case 2: memory->writew(dst, (U16)val); break;
                case 4: memory->writed(dst, (U32)val); break;
                case 8: memory->writeq(dst, val); break;
            }
            if (!isStos) reg[X64_RSI].setU64(reg[X64_RSI].u64 + (U64)step);
            reg[X64_RDI].setU64(reg[X64_RDI].u64 + (U64)step);
        }
        if (p.rep != 0) reg[X64_RCX].setU64(0);
        rip += opOff + 1;
        return opOff + 1;
    }

    // CMPS / SCAS — string compare. CMPS compares [RSI] vs [RDI]; SCAS
    // compares AL/AX/EAX/RAX vs [RDI]. Both update flags as a CMP and step
    // RSI/RDI by ±size. REPE (F3) repeats while ZF=1; REPNE (F2) while ZF=0;
    // both also break when RCX reaches 0.
    //   CMPSB/CMPSW/CMPSD/CMPSQ — A6 / A7
    //   SCASB/SCASW/SCASD/SCASQ — AE / AF
    if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
        U32 size = (op == 0xA6 || op == 0xAE) ? 1 : opSize;
        bool isScas = (op == 0xAE || op == 0xAF);
        S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        U64 count = (p.rep != 0) ? reg[X64_RCX].u64 : 1;
        if (p.asize32) count &= 0xFFFFFFFFULL;
        bool stopOnZF = (p.rep == 0xF3); // REPE
        bool stopOnNZ = (p.rep == 0xF2); // REPNE
        U64 lhs = 0, rhs = 0;
        while (count > 0) {
            count--;
            if (isScas) {
                lhs = (size == 1) ? reg[X64_RAX].u8
                    : (size == 2) ? reg[X64_RAX].u16
                    : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
            } else {
                U64 src = reg[X64_RSI].u64;
                if (p.asize32) src &= 0xFFFFFFFFULL;
                switch (size) {
                    case 1: lhs = memory->readb(src); break;
                    case 2: lhs = memory->readw(src); break;
                    case 4: lhs = memory->readd(src); break;
                    default: lhs = memory->readq(src); break;
                }
            }
            U64 dst = reg[X64_RDI].u64;
            if (p.asize32) dst &= 0xFFFFFFFFULL;
            switch (size) {
                case 1: rhs = memory->readb(dst); break;
                case 2: rhs = memory->readw(dst); break;
                case 4: rhs = memory->readd(dst); break;
                default: rhs = memory->readq(dst); break;
            }
            U64 diff = lhs - rhs;
            flagsSub(rflags, lhs, rhs, diff, size);
            if (!isScas) reg[X64_RSI].setU64(reg[X64_RSI].u64 + (U64)step);
            reg[X64_RDI].setU64(reg[X64_RDI].u64 + (U64)step);
            if (p.rep != 0) {
                bool zfNow = (rflags & X64_ZF) != 0;
                if (stopOnZF && !zfNow) break;
                if (stopOnNZ && zfNow) break;
            }
        }
        if (p.rep != 0) reg[X64_RCX].setU64(count);
        rip += opOff + 1;
        return opOff + 1;
    }

    // LEAVE (C9). Equivalent to: RSP = RBP; RBP = pop64().
    if (op == 0xC9) {
        reg[X64_RSP].setU64(reg[X64_RBP].u64);
        reg[X64_RBP].setU64(pop64());
        rip += opOff + 1;
        return opOff + 1;
    }

    // PUSHFQ (9C) / POPFQ (9D). PUSHFQ pushes the low 32 bits of rflags
    // extended to 64; POPFQ pops 64 bits but only the low 32 carry the
    // user-visible flags. We mirror that: writeable mask covers the
    // arithmetic and direction flags + IF.
    if (op == 0x9C) {
        push64((U64)rflags);
        rip += opOff + 1;
        return opOff + 1;
    }
    if (op == 0x9D) {
        U64 v = pop64();
        const U32 writable = 0x00254FD5u; // CF PF AF ZF SF TF IF DF OF + others
        rflags = (rflags & ~writable) | ((U32)v & writable);
        rip += opOff + 1;
        return opOff + 1;
    }

    // CLD (FC) / STD (FD). Direction flag for string ops.
    if (op == 0xFC) { rflags &= ~X64_DF; rip += opOff + 1; return opOff + 1; }
    if (op == 0xFD) { rflags |=  X64_DF; rip += opOff + 1; return opOff + 1; }

    // CMC (F5) / CLC (F8) / STC (F9). Carry-flag toggle/clear/set.
    if (op == 0xF5) { rflags ^= X64_CF; rip += opOff + 1; return opOff + 1; }
    if (op == 0xF8) { rflags &= ~X64_CF; rip += opOff + 1; return opOff + 1; }
    if (op == 0xF9) { rflags |=  X64_CF; rip += opOff + 1; return opOff + 1; }

    // CPUID (0F A2). Return a conservative feature set: SSE2 only, no SSE3+,
    // no AVX. glibc IFUNC dispatchers consult ECX feature bits and select the
    // simpler implementations when AVX/SSSE3 are clear, which keeps us off
    // unimplemented vector opcodes during memcpy/strcmp.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0xA2) {
        U32 leaf = reg[X64_RAX].u32;
        U32 sub  = reg[X64_RCX].u32;
        U32 a = 0, b = 0, c = 0, d = 0;
        switch (leaf) {
            case 0x0:
                a = 0x1; // max leaf
                // "Genu" "ineI" "ntel"
                b = 0x756E6547; d = 0x49656E69; c = 0x6C65746E;
                break;
            case 0x1: {
                a = (6 << 8) | (15 << 4) | 3; // family 6 stepping 3
                b = 0;
                // ECX feature bits: SSE3=0, SSSE3=0, SSE4.1=0, SSE4.2=0,
                // POPCNT=0 (bit23), XSAVE=0, OSXSAVE=0, AVX=0.
                c = 0;
                // EDX bits: FPU=0 PDE=1 TSC=4 MSR=5 PAE=6 MCE=7 CX8=8
                // APIC=9 SEP=11 MTRR=12 PGE=13 MCA=14 CMOV=15 PAT=16
                // PSE36=17 CLFSH=19 MMX=23 FXSR=24 SSE=25 SSE2=26.
                d = (1u<<0)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
                break;
            }
            case 0x80000000:
                a = 0x80000001;
                break;
            case 0x80000001:
                // EDX bit 29 = LM (long mode), bit 11 = SYSCALL, bit 20 = NX.
                d = (1u<<29)|(1u<<11)|(1u<<20);
                break;
            default:
                break;
        }
        (void)sub;
        reg[X64_RAX].setU64(a);
        reg[X64_RBX].setU64(b);
        reg[X64_RCX].setU64(c);
        reg[X64_RDX].setU64(d);
        rip += opOff + 2;
        return opOff + 2;
    }

    // ---- SSE2 (minimum-viable subset for ld.so + glibc) ----
    //
    // Encodings: 0F xx with optional 0x66 (packed-integer) or 0xF3/0xF2
    // (scalar/string) prefix. We handle the moves and PXOR — that covers
    // SSE register zeroing and 16-byte stack-aligned loads/stores used by
    // ld-linux during relocation.
    if (op == 0x0F) {
        U8 op2 = fetchByte(rip + opOff + 1);
        bool osize66 = p.osize16; // for SSE this means "use packed-integer form"
        // MOVAPS/MOVAPD xmm, xmm/m128       — 0F 28 (load) / 0F 29 (store)
        // MOVUPS/MOVUPD                     — 0F 10/11
        // MOVDQA xmm, xmm/m128              — 66 0F 6F (load) / 66 0F 7F (store)
        // MOVDQU                            — F3 0F 6F / F3 0F 7F
        // All do a 16-byte aligned-or-unaligned memcpy. We treat them
        // identically — no #GP on misalignment.
        //
        // Carve out F2/F3 prefixed variants of 0x10/0x11 — those are the
        // scalar SSE2 MOVSD/MOVSS forms, handled in the scalar-FP block
        // further down, not as 16-byte moves.
        bool isScalarPrefixed10or11 =
            (op2 == 0x10 || op2 == 0x11) && (p.rep == 0xF2 || p.rep == 0xF3);
        bool isMove128 =
            ((op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29) && !isScalarPrefixed10or11) ||
            ((op2 == 0x6F || op2 == 0x7F) && (osize66 || p.rep == 0xF3));
        if (isMove128) {
            bool isStore = (op2 == 0x11 || op2 == 0x29 || op2 == 0x7F);
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (isStore) {
                // dst = r/m, src = xmm[regField]
                if (m.isReg) {
                    xmm[m.rmIndex] = xmm[m.regField];
                } else {
                    memory->writeq(m.effAddr,     xmm[m.regField].lo);
                    memory->writeq(m.effAddr + 8, xmm[m.regField].hi);
                }
            } else {
                // dst = xmm[regField], src = r/m
                if (m.isReg) {
                    xmm[m.regField] = xmm[m.rmIndex];
                } else {
                    xmm[m.regField].lo = memory->readq(m.effAddr);
                    xmm[m.regField].hi = memory->readq(m.effAddr + 8);
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PXOR xmm, xmm/m128 — 66 0F EF /r. Used everywhere for register
        // zeroing (faster than MOV 0).
        if (op2 == 0xEF && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            xmm[m.regField].lo ^= srcLo;
            xmm[m.regField].hi ^= srcHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVD/MOVQ — scalar move between GPR and XMM.
        //   66 0F 6E /r  MOVD xmm, r/m32     (or MOVQ xmm, r/m64 with REX.W)
        //   66 0F 7E /r  MOVD r/m32, xmm     (or MOVQ with REX.W)
        //   F3 0F 7E /r  MOVQ xmm, xmm/m64   (load low qword, zero high)
        if ((op2 == 0x6E || op2 == 0x7E) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            bool isStore = (op2 == 0x7E);
            bool wide = rexW;
            U32 width = wide ? 8 : 4;
            if (isStore) {
                U64 v = wide ? xmm[m.regField].lo : (xmm[m.regField].lo & 0xFFFFFFFFULL);
                storeRM(m, width, v, rexPresent);
            } else {
                U64 v = loadRM(m, width, rexPresent);
                if (!wide) v &= 0xFFFFFFFFULL;
                xmm[m.regField].lo = v;
                xmm[m.regField].hi = 0;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x7E && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 lo;
            if (m.isReg) {
                lo = xmm[m.rmIndex].lo;
            } else {
                lo = memory->readq(m.effAddr);
            }
            xmm[m.regField].lo = lo;
            xmm[m.regField].hi = 0;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PCMPEQB xmm, xmm/m128 — 66 0F 74 /r. Compare 16 bytes; each byte
        // of dst becomes 0xFF if equal, 0x00 if not. glibc's strlen / strchr
        // / memchr loop on this.
        if ((op2 == 0x74 || op2 == 0x75 || op2 == 0x76) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dstLo = xmm[m.regField].lo;
            U64 dstHi = xmm[m.regField].hi;
            // op2 == 0x74 → bytes, 0x75 → words, 0x76 → dwords.
            U32 elemBits = (op2 == 0x74) ? 8 : (op2 == 0x75) ? 16 : 32;
            U32 elemCount = 128 / elemBits;
            U64 mask = (elemBits == 64) ? ~0ULL : ((1ULL << elemBits) - 1);
            U64 outLo = 0, outHi = 0;
            for (U32 i = 0; i < elemCount; i++) {
                U64 a, b;
                U64 bitOff;
                if (i * elemBits < 64) {
                    bitOff = i * elemBits;
                    a = (dstLo >> bitOff) & mask;
                    b = (srcLo >> bitOff) & mask;
                } else {
                    bitOff = i * elemBits - 64;
                    a = (dstHi >> bitOff) & mask;
                    b = (srcHi >> bitOff) & mask;
                }
                U64 r = (a == b) ? mask : 0;
                if (i * elemBits < 64) outLo |= r << (i * elemBits);
                else                   outHi |= r << (i * elemBits - 64);
            }
            xmm[m.regField].lo = outLo;
            xmm[m.regField].hi = outHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMOVMSKB r32, xmm — 66 0F D7 /r. Extract the high bit of each of
        // the 16 bytes into the low 16 bits of r32. Paired with PCMPEQB to
        // turn the 16-byte compare into a 16-bit "any equal?" mask.
        if (op2 == 0xD7 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 lo = xmm[m.rmIndex].lo;
            U64 hi = xmm[m.rmIndex].hi;
            U32 mask = 0;
            for (U32 i = 0; i < 8; i++) {
                if (lo & (1ULL << (i * 8 + 7))) mask |= (1u << i);
                if (hi & (1ULL << (i * 8 + 7))) mask |= (1u << (i + 8));
            }
            reg[m.regField].setU32(mask);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // POR / PAND / PANDN — 66 0F EB / DB / DF /r. Same shape as PXOR.
        if ((op2 == 0xEB || op2 == 0xDB || op2 == 0xDF) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            if (op2 == 0xEB) {
                xmm[m.regField].lo |= srcLo;
                xmm[m.regField].hi |= srcHi;
            } else if (op2 == 0xDB) {
                xmm[m.regField].lo &= srcLo;
                xmm[m.regField].hi &= srcHi;
            } else { // PANDN: dst = (~dst) & src
                xmm[m.regField].lo = (~xmm[m.regField].lo) & srcLo;
                xmm[m.regField].hi = (~xmm[m.regField].hi) & srcHi;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PSHUFD xmm, xmm/m128, imm8 — 66 0F 70 /r ib.
        // imm8 picks four 2-bit indices selecting which source dword goes to
        // each destination dword position. Common forms: imm=0 (broadcast
        // low dword), imm=0xFF (broadcast high dword).
        if (op2 == 0x70 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U32 dwords[4] = {
                (U32)srcLo, (U32)(srcLo >> 32),
                (U32)srcHi, (U32)(srcHi >> 32)
            };
            U32 out[4];
            for (int i = 0; i < 4; i++) out[i] = dwords[(imm >> (i * 2)) & 0x3];
            xmm[m.regField].lo = ((U64)out[1] << 32) | out[0];
            xmm[m.regField].hi = ((U64)out[3] << 32) | out[2];
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // Per-lane shift with imm8 — 66 0F 71/72/73 /sub ib.
        //   71 → word lanes, 72 → dword lanes, 73 → qword lanes.
        //   sub == 6 → shift left logical  (PSLLW/PSLLD/PSLLQ)
        //   sub == 2 → shift right logical (PSRLW/PSRLD/PSRLQ)
        //   sub == 4 → shift right arith   (PSRAW/PSRAD; not defined for qwords)
        //   sub == 7 with 0x73 → PSLLDQ (byte-granular), handled below
        //   sub == 3 with 0x73 → PSRLDQ
        if ((op2 == 0x71 || op2 == 0x72 || (op2 == 0x73)) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 sub = m.regField & 0x7;
            // Defer byte-granular forms (73 /7 and 73 /3) to the PSLLDQ block.
            if (op2 == 0x73 && (sub == 7 || sub == 3)) {
                // fall through to the existing PSLLDQ handler below
            } else if (sub == 6 || sub == 2 || sub == 4) {
                U8 imm = fetchByte(rip + opOff + 2 + m.length);
                U32 elemBits = (op2 == 0x71) ? 16 : (op2 == 0x72) ? 32 : 64;
                U32 elemCount = 128 / elemBits;
                U64 lo = xmm[m.rmIndex].lo;
                U64 hi = xmm[m.rmIndex].hi;
                U64 oLo = 0, oHi = 0;
                U64 mask = (elemBits == 64) ? ~0ULL : ((1ULL << elemBits) - 1);
                bool saturate = (imm >= elemBits);
                for (U32 i = 0; i < elemCount; i++) {
                    U32 bitPos = i * elemBits;
                    U64 v;
                    if (bitPos < 64) v = (lo >> bitPos) & mask;
                    else             v = (hi >> (bitPos - 64)) & mask;
                    U64 r;
                    if (saturate) {
                        if (sub == 4) {
                            U64 signMask = 1ULL << (elemBits - 1);
                            r = (v & signMask) ? mask : 0;
                        } else {
                            r = 0;
                        }
                    } else if (sub == 6) {
                        r = (v << imm) & mask;
                    } else if (sub == 2) {
                        r = v >> imm;
                    } else { // SRA
                        U64 signMask = 1ULL << (elemBits - 1);
                        if (v & signMask) {
                            U64 fillBits = (~0ULL) << (elemBits - imm);
                            r = ((v >> imm) | fillBits) & mask;
                        } else {
                            r = (v >> imm) & mask;
                        }
                    }
                    if (bitPos < 64) oLo |= r << bitPos;
                    else             oHi |= r << (bitPos - 64);
                }
                xmm[m.rmIndex].lo = oLo;
                xmm[m.rmIndex].hi = oHi;
                U32 used = opOff + 2 + m.length + 1;
                rip += used;
                return used;
            }
        }
        // PSLLDQ/PSRLDQ xmm, imm8 — 66 0F 73 /7 ib (left) or /3 ib (right).
        // Byte-granularity logical shift of the entire 16-byte register.
        // Used by glibc memcpy/memset to mask the tail when a copy isn't
        // 16-byte aligned.
        if (op2 == 0x73 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 sub = m.regField & 0x7;
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            if (sub == 7 || sub == 3) {
                U64 lo = xmm[m.rmIndex].lo;
                U64 hi = xmm[m.rmIndex].hi;
                U32 shift = imm > 16 ? 16 : imm;
                if (shift == 0) {
                    // no-op
                } else if (sub == 7) {
                    // shift left by `shift` bytes
                    if (shift >= 8) {
                        hi = lo << ((shift - 8) * 8);
                        lo = 0;
                    } else {
                        U32 bits = shift * 8;
                        hi = (hi << bits) | (lo >> (64 - bits));
                        lo = lo << bits;
                    }
                } else { // PSRLDQ
                    if (shift >= 8) {
                        lo = hi >> ((shift - 8) * 8);
                        hi = 0;
                    } else {
                        U32 bits = shift * 8;
                        lo = (lo >> bits) | (hi << (64 - bits));
                        hi = hi >> bits;
                    }
                }
                xmm[m.rmIndex].lo = lo;
                xmm[m.rmIndex].hi = hi;
                U32 used = opOff + 2 + m.length + 1;
                rip += used;
                return used;
            }
        }
        // PSUBB/PSUBW/PSUBD/PSUBQ — 66 0F F8/F9/FA/FB /r. Same shape as PADD.
        if ((op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA || op2 == 0xFB) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0xF8) { // PSUBB
                for (int i = 0; i < 8; i++) {
                    U8 a = (dLo >> (i*8)) & 0xFF; U8 b = (srcLo >> (i*8)) & 0xFF;
                    oLo |= (U64)((U8)(a - b)) << (i*8);
                    U8 c = (dHi >> (i*8)) & 0xFF; U8 d = (srcHi >> (i*8)) & 0xFF;
                    oHi |= (U64)((U8)(c - d)) << (i*8);
                }
            } else if (op2 == 0xF9) { // PSUBW
                for (int i = 0; i < 4; i++) {
                    U16 a = (dLo >> (i*16)) & 0xFFFF; U16 b = (srcLo >> (i*16)) & 0xFFFF;
                    oLo |= (U64)((U16)(a - b)) << (i*16);
                    U16 c = (dHi >> (i*16)) & 0xFFFF; U16 d = (srcHi >> (i*16)) & 0xFFFF;
                    oHi |= (U64)((U16)(c - d)) << (i*16);
                }
            } else if (op2 == 0xFA) { // PSUBD
                for (int i = 0; i < 2; i++) {
                    U32 a = (dLo >> (i*32)) & 0xFFFFFFFFu; U32 b = (srcLo >> (i*32)) & 0xFFFFFFFFu;
                    oLo |= (U64)((U32)(a - b)) << (i*32);
                    U32 c = (dHi >> (i*32)) & 0xFFFFFFFFu; U32 d = (srcHi >> (i*32)) & 0xFFFFFFFFu;
                    oHi |= (U64)((U32)(c - d)) << (i*32);
                }
            } else { // PSUBQ
                oLo = dLo - srcLo;
                oHi = dHi - srcHi;
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PUNPCKLBW/W/D/Q xmm, xmm/m128 — 66 0F 60/61/62/6C /r.
        // Interleave low-half lanes of dst and src.
        //   60 = bytes:  out[0]=d[0] out[1]=s[0] out[2]=d[1] out[3]=s[1] ...
        //   61 = words, 62 = dwords, 6C = qwords.
        // PUNPCKH (high half) at 0F 68/69/6A/6D, same interleave on the
        // upper 8 bytes of each input.
        if ((op2 == 0x60 || op2 == 0x61 || op2 == 0x62 || op2 == 0x6C ||
             op2 == 0x68 || op2 == 0x69 || op2 == 0x6A || op2 == 0x6D) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            // For low forms we read the low 8 bytes of each; for high forms,
            // the high 8 bytes. Result occupies all 16 bytes.
            bool isHigh = (op2 >= 0x68);
            U64 dSrc = isHigh ? dHi : dLo;
            U64 sSrc = isHigh ? srcHi : srcLo;
            U64 oLo = 0, oHi = 0;
            U8 sub = isHigh ? (op2 - 0x68) : (op2 - 0x60);
            // sub: 0=byte 1=word 2=dword 4=qword (6C/6D); coerce 4 to dword index 3.
            if (sub == 0) { // byte interleave: 16 bytes out, 8 from each input
                for (int i = 0; i < 8; i++) {
                    U64 db = (dSrc >> (i*8)) & 0xFF;
                    U64 sb = (sSrc >> (i*8)) & 0xFF;
                    U32 pos = i * 16;
                    if (pos < 64) oLo |= db << pos;
                    else          oHi |= db << (pos - 64);
                    pos += 8;
                    if (pos < 64) oLo |= sb << pos;
                    else          oHi |= sb << (pos - 64);
                }
            } else if (sub == 1) { // word interleave
                for (int i = 0; i < 4; i++) {
                    U64 dw = (dSrc >> (i*16)) & 0xFFFF;
                    U64 sw = (sSrc >> (i*16)) & 0xFFFF;
                    U32 pos = i * 32;
                    if (pos < 64) oLo |= dw << pos;
                    else          oHi |= dw << (pos - 64);
                    pos += 16;
                    if (pos < 64) oLo |= sw << pos;
                    else          oHi |= sw << (pos - 64);
                }
            } else if (sub == 2) { // dword interleave
                oLo = (U32)dSrc | ((U64)(U32)sSrc << 32);
                oHi = (U32)(dSrc >> 32) | ((U64)(U32)(sSrc >> 32) << 32);
            } else { // qword interleave (sub==4)
                oLo = dSrc;
                oHi = sSrc;
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PCMPGTB/W/D xmm, xmm/m128 — 66 0F 64/65/66 /r. Signed greater-than:
        // each lane of dst becomes all-1s if dst > src, else all-0s. Paired
        // with PCMPEQB in glibc's classified-character scans.
        if ((op2 == 0x64 || op2 == 0x65 || op2 == 0x66) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0x64) { // PCMPGTB (signed bytes)
                for (int i = 0; i < 8; i++) {
                    S8 a = (S8)((dLo >> (i*8)) & 0xFF);
                    S8 b = (S8)((srcLo >> (i*8)) & 0xFF);
                    if (a > b) oLo |= 0xFFULL << (i*8);
                    S8 c = (S8)((dHi >> (i*8)) & 0xFF);
                    S8 d = (S8)((srcHi >> (i*8)) & 0xFF);
                    if (c > d) oHi |= 0xFFULL << (i*8);
                }
            } else if (op2 == 0x65) { // PCMPGTW
                for (int i = 0; i < 4; i++) {
                    S16 a = (S16)((dLo >> (i*16)) & 0xFFFF);
                    S16 b = (S16)((srcLo >> (i*16)) & 0xFFFF);
                    if (a > b) oLo |= 0xFFFFULL << (i*16);
                    S16 c = (S16)((dHi >> (i*16)) & 0xFFFF);
                    S16 d = (S16)((srcHi >> (i*16)) & 0xFFFF);
                    if (c > d) oHi |= 0xFFFFULL << (i*16);
                }
            } else { // PCMPGTD
                for (int i = 0; i < 2; i++) {
                    S32 a = (S32)((dLo >> (i*32)) & 0xFFFFFFFFu);
                    S32 b = (S32)((srcLo >> (i*32)) & 0xFFFFFFFFu);
                    if (a > b) oLo |= 0xFFFFFFFFULL << (i*32);
                    S32 c = (S32)((dHi >> (i*32)) & 0xFFFFFFFFu);
                    S32 d = (S32)((srcHi >> (i*32)) & 0xFFFFFFFFu);
                    if (c > d) oHi |= 0xFFFFFFFFULL << (i*32);
                }
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PADDB/PADDW/PADDD/PADDQ — 66 0F FC/FD/FE/D4 /r. Used by SSE
        // crypto-style mixing in glibc's hash routines.
        if ((op2 == 0xFC || op2 == 0xFD || op2 == 0xFE || op2 == 0xD4) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0xFC) { // PADDB
                for (int i = 0; i < 8; i++) {
                    U8 a = (dLo >> (i*8)) & 0xFF; U8 b = (srcLo >> (i*8)) & 0xFF;
                    oLo |= (U64)((U8)(a + b)) << (i*8);
                    U8 c = (dHi >> (i*8)) & 0xFF; U8 d = (srcHi >> (i*8)) & 0xFF;
                    oHi |= (U64)((U8)(c + d)) << (i*8);
                }
            } else if (op2 == 0xFD) { // PADDW
                for (int i = 0; i < 4; i++) {
                    U16 a = (dLo >> (i*16)) & 0xFFFF; U16 b = (srcLo >> (i*16)) & 0xFFFF;
                    oLo |= (U64)((U16)(a + b)) << (i*16);
                    U16 c = (dHi >> (i*16)) & 0xFFFF; U16 d = (srcHi >> (i*16)) & 0xFFFF;
                    oHi |= (U64)((U16)(c + d)) << (i*16);
                }
            } else if (op2 == 0xFE) { // PADDD
                for (int i = 0; i < 2; i++) {
                    U32 a = (dLo >> (i*32)) & 0xFFFFFFFFu; U32 b = (srcLo >> (i*32)) & 0xFFFFFFFFu;
                    oLo |= (U64)((U32)(a + b)) << (i*32);
                    U32 c = (dHi >> (i*32)) & 0xFFFFFFFFu; U32 d = (srcHi >> (i*32)) & 0xFFFFFFFFu;
                    oHi |= (U64)((U32)(c + d)) << (i*32);
                }
            } else { // PADDQ
                oLo = dLo + srcLo;
                oHi = dHi + srcHi;
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVQ xmm/m64, xmm — 66 0F D6 /r. Store low qword.
        if (op2 == 0xD6 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) {
                xmm[m.rmIndex].lo = xmm[m.regField].lo;
                xmm[m.rmIndex].hi = 0;
            } else {
                memory->writeq(m.effAddr, xmm[m.regField].lo);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMINUB/PMAXUB — 66 0F DA/DE /r. Unsigned byte min/max across 16
        // lanes. Used by glibc's strlen/memchr fast paths.
        if ((op2 == 0xDA || op2 == 0xDE) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 8; i++) {
                U8 a = (dLo  >> (i*8)) & 0xFF; U8 b = (srcLo >> (i*8)) & 0xFF;
                U8 c = (dHi  >> (i*8)) & 0xFF; U8 d = (srcHi >> (i*8)) & 0xFF;
                U8 lo8 = (op2 == 0xDA) ? (a < b ? a : b) : (a > b ? a : b);
                U8 hi8 = (op2 == 0xDA) ? (c < d ? c : d) : (c > d ? c : d);
                oLo |= (U64)lo8 << (i*8);
                oHi |= (U64)hi8 << (i*8);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMINSW/PMAXSW — 66 0F EA/EE /r. Signed word min/max across 8 lanes.
        if ((op2 == 0xEA || op2 == 0xEE) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 4; i++) {
                S16 a = (S16)((dLo  >> (i*16)) & 0xFFFF);
                S16 b = (S16)((srcLo >> (i*16)) & 0xFFFF);
                S16 c = (S16)((dHi  >> (i*16)) & 0xFFFF);
                S16 d = (S16)((srcHi >> (i*16)) & 0xFFFF);
                S16 lo16 = (op2 == 0xEA) ? (a < b ? a : b) : (a > b ? a : b);
                S16 hi16 = (op2 == 0xEA) ? (c < d ? c : d) : (c > d ? c : d);
                oLo |= ((U64)(U16)lo16) << (i*16);
                oHi |= ((U64)(U16)hi16) << (i*16);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PAVGB — 66 0F E0 /r. Unsigned byte average with rounding:
        // dst[i] = (dst[i] + src[i] + 1) >> 1.
        if (op2 == 0xE0 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 8; i++) {
                U16 a = (dLo  >> (i*8)) & 0xFF; U16 b = (srcLo >> (i*8)) & 0xFF;
                U16 c = (dHi  >> (i*8)) & 0xFF; U16 d = (srcHi >> (i*8)) & 0xFF;
                oLo |= (U64)((U8)((a + b + 1) >> 1)) << (i*8);
                oHi |= (U64)((U8)((c + d + 1) >> 1)) << (i*8);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PSADBW — 66 0F F6 /r. Sum-of-absolute-differences over each 8-byte
        // half. Used by glibc memcmp/strncmp. Output: low qword = sum for low
        // half, high qword = sum for high half.
        if (op2 == 0xF6 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 sumLo = 0, sumHi = 0;
            for (int i = 0; i < 8; i++) {
                int a = (dLo  >> (i*8)) & 0xFF;
                int b = (srcLo >> (i*8)) & 0xFF;
                int c = (dHi  >> (i*8)) & 0xFF;
                int d = (srcHi >> (i*8)) & 0xFF;
                sumLo += (U64)(a > b ? a - b : b - a);
                sumHi += (U64)(c > d ? c - d : d - c);
            }
            xmm[m.regField].lo = sumLo;
            xmm[m.regField].hi = sumHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMULLW — 66 0F D5 /r. Per-word signed multiply, store low 16 of
        // each 32-bit result. PMULHW — 66 0F E5 /r. Same but store high 16.
        if ((op2 == 0xD5 || op2 == 0xE5) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            bool high = (op2 == 0xE5);
            for (int i = 0; i < 4; i++) {
                S16 a = (S16)((dLo  >> (i*16)) & 0xFFFF);
                S16 b = (S16)((srcLo >> (i*16)) & 0xFFFF);
                S16 c = (S16)((dHi  >> (i*16)) & 0xFFFF);
                S16 d = (S16)((srcHi >> (i*16)) & 0xFFFF);
                S32 p1 = (S32)a * (S32)b;
                S32 p2 = (S32)c * (S32)d;
                U16 outLo = high ? (U16)((p1 >> 16) & 0xFFFF) : (U16)(p1 & 0xFFFF);
                U16 outHi = high ? (U16)((p2 >> 16) & 0xFFFF) : (U16)(p2 & 0xFFFF);
                oLo |= ((U64)outLo) << (i*16);
                oHi |= ((U64)outHi) << (i*16);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMULUDQ — 66 0F F4 /r. Multiply unsigned dwords in lanes 0 and 2,
        // producing two 64-bit results.
        if (op2 == 0xF4 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 a = dLo & 0xFFFFFFFFULL;
            U64 b = srcLo & 0xFFFFFFFFULL;
            U64 c = dHi & 0xFFFFFFFFULL;
            U64 d = srcHi & 0xFFFFFFFFULL;
            xmm[m.regField].lo = a * b;
            xmm[m.regField].hi = c * d;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PACKSSWB / PACKUSWB / PACKSSDW — 66 0F 63 / 67 / 6B /r.
        // Saturate two source halves down to a smaller element size and
        // interleave dst-first, src-second.
        if ((op2 == 0x63 || op2 == 0x67 || op2 == 0x6B) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0x6B) {
                // PACKSSDW: 4 signed dwords → 4 signed words per half.
                // dst halves produce 4 words → 8 words total. Lanes 0..3 from
                // dst (lo+hi dwords of dLo/dHi), lanes 4..7 from src.
                auto satS16 = [](S32 v) -> U16 {
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    return (U16)(v & 0xFFFF);
                };
                U16 w[8];
                w[0] = satS16((S32)(dLo & 0xFFFFFFFFu));
                w[1] = satS16((S32)((dLo >> 32) & 0xFFFFFFFFu));
                w[2] = satS16((S32)(dHi & 0xFFFFFFFFu));
                w[3] = satS16((S32)((dHi >> 32) & 0xFFFFFFFFu));
                w[4] = satS16((S32)(srcLo & 0xFFFFFFFFu));
                w[5] = satS16((S32)((srcLo >> 32) & 0xFFFFFFFFu));
                w[6] = satS16((S32)(srcHi & 0xFFFFFFFFu));
                w[7] = satS16((S32)((srcHi >> 32) & 0xFFFFFFFFu));
                for (int i = 0; i < 4; i++) oLo |= ((U64)w[i]) << (i*16);
                for (int i = 0; i < 4; i++) oHi |= ((U64)w[i+4]) << (i*16);
            } else {
                // PACKSSWB / PACKUSWB: 8 signed words from each input →
                // 8 bytes per side. Total 16 bytes interleaved dst-then-src.
                bool sign = (op2 == 0x63);
                auto satS8 = [](S16 v) -> U8 {
                    if (v > 127) v = 127;
                    if (v < -128) v = -128;
                    return (U8)(v & 0xFF);
                };
                auto satU8 = [](S16 v) -> U8 {
                    if (v > 255) v = 255;
                    if (v < 0) v = 0;
                    return (U8)(v & 0xFF);
                };
                U8 b[16];
                for (int i = 0; i < 4; i++) {
                    S16 a = (S16)((dLo  >> (i*16)) & 0xFFFF);
                    S16 c = (S16)((dHi  >> (i*16)) & 0xFFFF);
                    b[i]   = sign ? satS8(a) : satU8(a);
                    b[i+4] = sign ? satS8(c) : satU8(c);
                }
                for (int i = 0; i < 4; i++) {
                    S16 a = (S16)((srcLo >> (i*16)) & 0xFFFF);
                    S16 c = (S16)((srcHi >> (i*16)) & 0xFFFF);
                    b[i+8]  = sign ? satS8(a) : satU8(a);
                    b[i+12] = sign ? satS8(c) : satU8(c);
                }
                for (int i = 0; i < 8; i++) oLo |= ((U64)b[i]) << (i*8);
                for (int i = 0; i < 8; i++) oHi |= ((U64)b[i+8]) << (i*8);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVMSKPS — 0F 50 /r. Extract sign bits of the 4 single-precision
        // floats in xmm[rmIndex] into bits 0..3 of the general-purpose
        // r32/r64 destination, zero the rest. The reg-only form is the
        // only legal encoding. We don't model FP types — just take bit 31
        // of each 32-bit lane. MOVMSKPD — 66 0F 50 /r. Same idea but 2
        // double-precision lanes (bits 63 of each 64-bit half).
        // SHUFPS — 0F C6 /r ib (no 66). Per-imm8 4-dword shuffle: bits
        // [1:0]/[3:2] pick lanes from dst, [5:4]/[7:6] pick from src.
        // Output lanes 0,1 ← dst, 2,3 ← src.
        // SHUFPD — 66 0F C6 /r ib. 2-qword shuffle: bit 0 picks dst lane,
        // bit 1 picks src lane.
        if (op2 == 0xC6) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (osize66) {
                // SHUFPD
                U64 dHalves[2] = { dLo, dHi };
                U64 sHalves[2] = { srcLo, srcHi };
                oLo = dHalves[(imm >> 0) & 1];
                oHi = sHalves[(imm >> 1) & 1];
            } else {
                // SHUFPS — break into 4 dwords.
                U32 d[4] = { (U32)dLo, (U32)(dLo >> 32), (U32)dHi, (U32)(dHi >> 32) };
                U32 s[4] = { (U32)srcLo, (U32)(srcLo >> 32), (U32)srcHi, (U32)(srcHi >> 32) };
                U32 o[4];
                o[0] = d[(imm >> 0) & 3];
                o[1] = d[(imm >> 2) & 3];
                o[2] = s[(imm >> 4) & 3];
                o[3] = s[(imm >> 6) & 3];
                oLo = (U64)o[0] | ((U64)o[1] << 32);
                oHi = (U64)o[2] | ((U64)o[3] << 32);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // PEXTRW — 66 0F C5 /r ib. Extract one word from xmm[rmIndex] (imm8
        // & 7 selects the lane) into r32/r64.
        if (op2 == 0xC5 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            if (m.isReg) {
                int lane = imm & 7;
                U64 src = (lane < 4) ? xmm[m.rmIndex].lo : xmm[m.rmIndex].hi;
                U64 w = (src >> ((lane & 3) * 16)) & 0xFFFF;
                reg[m.regField].setU64(w);
            }
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // PINSRW — 66 0F C4 /r ib. Insert low 16 of r32 (or m16) into xmm[reg]
        // at lane (imm8 & 7).
        if (op2 == 0xC4 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U16 v;
            if (m.isReg) v = (U16)reg[m.rmIndex].u32;
            else         v = memory->readw(m.effAddr);
            int lane = imm & 7;
            U64* dst = (lane < 4) ? &xmm[m.regField].lo : &xmm[m.regField].hi;
            int sub = lane & 3;
            U64 mask = ~((U64)0xFFFF << (sub * 16));
            *dst = (*dst & mask) | ((U64)v << (sub * 16));
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // MOVNTDQ — 66 0F E7 /r. Non-temporal store of xmm to m128. We
        // ignore the cache hint and treat as a plain 16-byte store.
        if (op2 == 0xE7 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                memory->writeq(m.effAddr,     xmm[m.regField].lo);
                memory->writeq(m.effAddr + 8, xmm[m.regField].hi);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVLHPS — 0F 16 /r (reg form). xmm[reg].hi := xmm[rm].lo.
        // MOVHLPS — 0F 12 /r (reg form). xmm[reg].lo := xmm[rm].hi.
        // Memory forms (MOVLPS/MOVHPS) are also encoded with 0F 12/16 but
        // with a non-reg ModR/M — keep those simple too.
        if ((op2 == 0x12 || op2 == 0x16) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) {
                if (op2 == 0x16) {
                    xmm[m.regField].hi = xmm[m.rmIndex].lo; // MOVLHPS
                } else {
                    xmm[m.regField].lo = xmm[m.rmIndex].hi; // MOVHLPS
                }
            } else {
                // Memory form: MOVLPS loads/stores low qword, MOVHPS high.
                if (op2 == 0x12) {
                    xmm[m.regField].lo = memory->readq(m.effAddr);
                } else {
                    xmm[m.regField].hi = memory->readq(m.effAddr);
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // 0F 13 /r — MOVLPS m64, xmm. 0F 17 /r — MOVHPS m64, xmm.
        if ((op2 == 0x13 || op2 == 0x17) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                U64 v = (op2 == 0x13) ? xmm[m.regField].lo : xmm[m.regField].hi;
                memory->writeq(m.effAddr, v);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x50) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) {
                U64 lo = xmm[m.rmIndex].lo;
                U64 hi = xmm[m.rmIndex].hi;
                U32 out = 0;
                if (osize66) {
                    // MOVMSKPD — 2 doubles, take bit 63 of each.
                    if (lo & (1ULL << 63)) out |= 1;
                    if (hi & (1ULL << 63)) out |= 2;
                } else {
                    // MOVMSKPS — 4 floats, take bit 31 of each 32-bit lane.
                    if ((lo >> 31) & 1) out |= 1;
                    if ((lo >> 63) & 1) out |= 2;
                    if ((hi >> 31) & 1) out |= 4;
                    if ((hi >> 63) & 1) out |= 8;
                }
                reg[m.regField].setU64((U64)out);
                U32 used = opOff + 2 + m.length;
                rip += used;
                return used;
            }
        }
        // PMULHUW — 66 0F E4 /r. Unsigned word multiply, store high 16 of
        // each product. Glibc's hash mixing uses this.
        if (op2 == 0xE4 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 4; i++) {
                U16 a = (dLo  >> (i*16)) & 0xFFFF;
                U16 b = (srcLo >> (i*16)) & 0xFFFF;
                U16 c = (dHi  >> (i*16)) & 0xFFFF;
                U16 d = (srcHi >> (i*16)) & 0xFFFF;
                U32 p1 = (U32)a * (U32)b;
                U32 p2 = (U32)c * (U32)d;
                oLo |= ((U64)((p1 >> 16) & 0xFFFF)) << (i*16);
                oHi |= ((U64)((p2 >> 16) & 0xFFFF)) << (i*16);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // Variable per-lane shifts — 66 0F D1/D2/D3 (right logical W/D/Q),
        // E1/E2 (right arithmetic W/D), F1/F2/F3 (left W/D/Q). Shift count
        // comes from the LOW QWORD of the source xmm; if > element width,
        // the result is zero (logical) or all-sign (arithmetic).
        bool isVarShift = osize66 &&
            (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
             op2 == 0xE1 || op2 == 0xE2 ||
             op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3);
        if (isVarShift) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo;
            if (m.isReg) srcLo = xmm[m.rmIndex].lo;
            else         srcLo = memory->readq(m.effAddr);
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            // Element size and direction by opcode.
            int  esize  = (op2 == 0xD1 || op2 == 0xE1 || op2 == 0xF1) ? 16 :
                          (op2 == 0xD2 || op2 == 0xE2 || op2 == 0xF2) ? 32 : 64;
            bool isLeft = (op2 >= 0xF1);
            bool isArith = (op2 == 0xE1 || op2 == 0xE2);
            U64 cnt = srcLo;
            // Saturate count: too-large shifts zero everything for logical,
            // or replicate sign for arithmetic.
            if (cnt >= (U64)esize) {
                if (!isArith) {
                    xmm[m.regField].lo = 0;
                    xmm[m.regField].hi = 0;
                } else {
                    // Per lane, fill with the sign bit replicated.
                    auto fillSign = [&](U64 v) -> U64 {
                        U64 out = 0;
                        for (int i = 0; i < 128 / esize / 2; i++) {
                            U64 lane = (v >> (i*esize)) & ((esize == 64) ? ~0ULL : ((1ULL << esize) - 1));
                            U64 signMask = (lane >> (esize - 1)) & 1 ? ((esize == 64) ? ~0ULL : ((1ULL << esize) - 1)) : 0;
                            out |= signMask << (i*esize);
                        }
                        return out;
                    };
                    xmm[m.regField].lo = fillSign(dLo);
                    xmm[m.regField].hi = fillSign(dHi);
                }
                U32 used = opOff + 2 + m.length;
                rip += used;
                return used;
            }
            int n = 64 / esize;
            U64 mask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
            for (int i = 0; i < n; i++) {
                U64 a = (dLo >> (i*esize)) & mask;
                U64 b = (dHi >> (i*esize)) & mask;
                U64 ra, rb;
                if (isLeft) {
                    ra = (a << cnt) & mask;
                    rb = (b << cnt) & mask;
                } else if (isArith) {
                    S64 sa = (esize == 16) ? (S64)(S16)a :
                             (esize == 32) ? (S64)(S32)a : (S64)a;
                    S64 sb = (esize == 16) ? (S64)(S16)b :
                             (esize == 32) ? (S64)(S32)b : (S64)b;
                    ra = (U64)(sa >> cnt) & mask;
                    rb = (U64)(sb >> cnt) & mask;
                } else {
                    ra = (a >> cnt) & mask;
                    rb = (b >> cnt) & mask;
                }
                oLo |= ra << (i*esize);
                oHi |= rb << (i*esize);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PREFETCH* — 0F 18 /reg. Treated as a no-op (hint only).
        if (op2 == 0x18) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // NOP (multi-byte) — 0F 1F /reg with optional operand. Used by glibc
        // for code alignment; ModR/M absorbs disp/SIB bytes.
        if (op2 == 0x1F) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE2 scalar double-precision FP ----
        //
        // Scalar SSE2 ops touch only the low 64 bits of the XMM register
        // (one double); the high 64 bits are left untouched for reg/reg
        // forms and zeroed for memory loads via MOVSD (per the Intel SDM
        // wording, but glibc's hot paths only care about the low qword,
        // so we leave .hi alone for moves between registers too).
        //
        // We use type-punning via memcpy on a U64<->double pair to stay
        // strict-aliasing-clean.
        auto u64ToDouble = [](U64 bits) -> double {
            double d; std::memcpy(&d, &bits, sizeof(d)); return d;
        };
        auto doubleToU64 = [](double d) -> U64 {
            U64 bits; std::memcpy(&bits, &d, sizeof(bits)); return bits;
        };

        // MOVSD xmm, xmm/m64   F2 0F 10 /r   (load low qword; zero high if mem)
        // MOVSD xmm/m64, xmm   F2 0F 11 /r   (store low qword)
        if ((op2 == 0x10 || op2 == 0x11) && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            bool isStore = (op2 == 0x11);
            if (isStore) {
                U64 v = xmm[m.regField].lo;
                if (m.isReg) xmm[m.rmIndex].lo = v;
                else         memory->writeq(m.effAddr, v);
            } else {
                U64 v;
                if (m.isReg) {
                    v = xmm[m.rmIndex].lo;
                    xmm[m.regField].lo = v; // .hi unchanged for reg/reg
                } else {
                    v = memory->readq(m.effAddr);
                    xmm[m.regField].lo = v;
                    xmm[m.regField].hi = 0; // memory form zeros high
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Scalar FP arithmetic — all share the F2 0F prefix and a ModR/M
        // operand that is either xmm or m64.
        //   ADDSD  F2 0F 58
        //   MULSD  F2 0F 59
        //   SUBSD  F2 0F 5C
        //   DIVSD  F2 0F 5E
        //   SQRTSD F2 0F 51
        //   MINSD  F2 0F 5D
        //   MAXSD  F2 0F 5F
        if (p.rep == 0xF2 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double a = u64ToDouble(xmm[m.regField].lo);
            double b = u64ToDouble(srcBits);
            double r;
            switch (op2) {
                case 0x58: r = a + b; break;
                case 0x59: r = a * b; break;
                case 0x5C: r = a - b; break;
                case 0x5E: r = a / b; break;
                case 0x51: r = std::sqrt(b); break; // SQRTSD reads src, ignores dst
                case 0x5D: r = (a < b) ? a : b; break;
                case 0x5F: r = (a > b) ? a : b; break;
                default:   r = a; break;
            }
            xmm[m.regField].lo = doubleToU64(r);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSI2SD xmm, r/m32   F2 0F 2A /r       (REX.W → r/m64)
        // Convert int to double; result in low 64 of dst, high unchanged.
        if (op2 == 0x2A && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcSize = rexW ? 8 : 4;
            U64 srcRaw = loadRM(m, srcSize, rexPresent);
            double d = rexW
                ? (double)(S64)srcRaw
                : (double)(S32)(U32)srcRaw;
            xmm[m.regField].lo = doubleToU64(d);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSD2SI r32, xmm/m64   F2 0F 2D /r     (REX.W → r64)
        // Convert double to signed int with current rounding (truncate-or-
        // round-to-nearest). For Milestone C we use C cast (truncate).
        if (op2 == 0x2D && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double d = u64ToDouble(srcBits);
            if (rexW) reg[m.regField].setU64((U64)(S64)d);
            else      reg[m.regField].setU32((U32)(S32)d);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UCOMISD xmm, xmm/m64   66 0F 2E /r
        // COMISD  xmm, xmm/m64   66 0F 2F /r
        // Compare two doubles, set EFLAGS ZF/PF/CF per result. UCOMISD and
        // COMISD only differ in whether QNaN raises #IA — we treat them
        // identically (no FP exceptions modelled).
        if ((op2 == 0x2E || op2 == 0x2F) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double a = u64ToDouble(xmm[m.regField].lo);
            double b = u64ToDouble(srcBits);
            // Per Intel SDM: unordered → ZF=PF=CF=1, greater → all 0,
            // less → CF=1, equal → ZF=1. Also clears OF/SF/AF.
            U64 newFlags = rflags & ~(X64_ZF | X64_PF | X64_CF |
                                      X64_OF | X64_SF | X64_AF);
            if (std::isnan(a) || std::isnan(b)) {
                newFlags |= X64_ZF | X64_PF | X64_CF;
            } else if (a > b) {
                // all three remain 0
            } else if (a < b) {
                newFlags |= X64_CF;
            } else {
                newFlags |= X64_ZF;
            }
            rflags = newFlags;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
    }

    // FXSAVE / FXRSTOR (0F AE /0 and /1). v1: no-op — we don't model x87/SSE
    // state save/restore yet. Programs that rely on FXRSTOR to *initialise*
    // SSE state (uncommon outside of context-switch code) will see stale
    // XMM regs, but ld-linux startup itself does not depend on it.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0xAE) {
        ModRM m = decodeModRM(rip + opOff + 2, p, 0);
        U32 used = opOff + 2 + m.length;
        rip += used;
        return used;
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

U64 CPU64::runBounded(U64 maxInsn) {
    U64 ran = 0;
    while (!yield && ran < maxInsn) {
        U32 n = step();
        if (n == 0) break;
        instructionCount++;
        ran++;
    }
    return ran;
}

#endif // BOXEDWINE_GUEST_X64
