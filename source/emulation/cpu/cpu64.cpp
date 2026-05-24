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
        if ((m.regField & 0x7) != 0) {
            // /0 is the only defined sub-op; anything else is undefined.
            goto unhandled;
        }
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

    // LEA r, m (8D /r). Effective address only — no memory access. opSize
    // controls how much of the computed address is written.
    if (op == 0x8D) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        if (m.isReg) goto unhandled; // LEA with reg src is undefined
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
