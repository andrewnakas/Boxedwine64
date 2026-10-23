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
#include "ksignal.h"   // K_SIGFPE
#include "common/splitLock.h"

// Defined here until the CPU32 split-lock path (Winbox common_lock.cpp) is ported.
std::mutex g_splitLockMutex;

#include <cmath>
#include <cstring>
#include <mutex>
#include <chrono>

// Sharded locks for CPU64 atomic read-modify-write memory ops (XCHG with
// memory, CMPXCHG, XADD, LOCK-prefixed ALU). In the multi-threaded build each
// guest thread runs on its own host thread over a shared KMemory64, so glibc's
// pthread mutexes/condvars — which rely on these instructions being atomic —
// would race without this.
//
// Aligned operands use sharded recursive mutexes, so unrelated locked words
// can proceed concurrently. Unaligned operands use g_splitLockMutex and plain
// guest-memory reads/writes: WebAssembly atomic instructions trap on a
// misaligned host address. Recursive locks also let a guest exception/signal
// path re-enter an aligned operation without self-deadlocking.
//
// CPU64_ATOMIC_LOCKS must be a power of two so the mask is a single AND.
#define CPU64_ATOMIC_LOCKS 256
static std::recursive_mutex g_cpu64AtomicLocks[CPU64_ATOMIC_LOCKS];

// Pick the lock bank for a guest effective address. Shift by the page shift so
// same-page accesses (the common contended case is a single lock word) share a
// bank while distinct pages spread across banks; the low page bits carry no
// useful entropy for sharding since a lock word sits at one offset.
static inline std::recursive_mutex& cpu64AtomicLockFor(U64 effAddr) {
    return g_cpu64AtomicLocks[(effAddr >> K64_PAGE_SHIFT) & (CPU64_ATOMIC_LOCKS - 1)];
}

class CPU64AtomicLock {
public:
    CPU64AtomicLock(KMemory64* memory, U64 effAddr, U32 width, std::defer_lock_t)
        : memory(memory), effAddr(effAddr), width(width),
          alignedLock(cpu64AtomicLockFor(effAddr), std::defer_lock),
          splitLock(g_splitLockMutex, std::defer_lock) {}

    void lock() {
        U8* hostAddress = memory->getRamPtr(effAddr, width);
        if (!hostAddress || (reinterpret_cast<uintptr_t>(hostAddress) & (width - 1))) {
            splitLock.lock();
        } else {
            alignedLock.lock();
        }
    }

private:
    KMemory64* memory;
    U64 effAddr;
    U32 width;
    std::unique_lock<std::recursive_mutex> alignedLock;
    std::unique_lock<std::mutex> splitLock;
};

// FPU status-word C0/C2/C3 bit positions (see Intel SDM Vol 1 §8.1.3).
// We can't reuse the macros in fpu.cpp because they're file-local there.
static inline void cpu64_fpu_set_c012_3(FPU& f, int c3, int c2, int c0) {
    f.sw &= ~(0x4000 | 0x0400 | 0x0100);
    if (c3) f.sw |= 0x4000;
    if (c2) f.sw |= 0x0400;
    if (c0) f.sw |= 0x0100;
}

// FCOM/FUCOM compare result: C3/C2/C0 = 111 unordered, 100 equal,
// 001 ST(0)<src, 000 ST(0)>src.
static inline void cpu64_fpu_compare(FPU& f, double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        cpu64_fpu_set_c012_3(f, 1, 1, 1);
    } else if (a == b) {
        cpu64_fpu_set_c012_3(f, 1, 0, 0);
    } else if (a < b) {
        cpu64_fpu_set_c012_3(f, 0, 0, 1);
    } else {
        cpu64_fpu_set_c012_3(f, 0, 0, 0);
    }
}

static inline double cpu64RoundByMxcsr(double value, U32 mxcsr) {
    switch ((mxcsr >> 13) & 3) {
        case 1: return std::floor(value);
        case 2: return std::ceil(value);
        case 3: return std::trunc(value);
        default: {
            // Round-to-nearest, ties-to-even without depending on the host's
            // process-wide floating-point rounding mode.
            double lower = std::floor(value);
            double fraction = value - lower;
            if (fraction < 0.5) return lower;
            if (fraction > 0.5) return lower + 1.0;
            return std::fmod(std::fabs(lower), 2.0) == 0.0 ? lower : lower + 1.0;
        }
    }
}

static inline U64 cpu64ConvertFpToInt(double value, bool is64, bool truncate,
                                      U32 mxcsr) {
    if (!std::isfinite(value)) return is64 ? 0x8000000000000000ULL : 0x80000000U;
    double rounded = truncate ? std::trunc(value) : cpu64RoundByMxcsr(value, mxcsr);
    double minValue = is64 ? -0x1p63 : -0x1p31;
    double maxValue = is64 ?  0x1p63 :  0x1p31;
    if (rounded < minValue || rounded >= maxValue)
        return is64 ? 0x8000000000000000ULL : 0x80000000U;
    S64 result = (S64)rounded;
    return is64 ? (U64)result : (U32)(S32)result;
}

static inline U32 cpu64StringElement(U64 lo, U64 hi, U32 index, U32 bits) {
    U32 bit = index * bits;
    U64 lane = bit < 64 ? lo >> bit : hi >> (bit - 64);
    return (U32)(lane & (bits == 8 ? 0xFFU : 0xFFFFU));
}

static inline bool cpu64StringElementCompare(U32 a, U32 b, U32 bits,
                                              bool isSigned, U32 operation) {
    if (isSigned) {
        S32 sa = bits == 8 ? (S8)a : (S16)a;
        S32 sb = bits == 8 ? (S8)b : (S16)b;
        if (operation == 0) return sa == sb;
        if (operation == 1) return sb >= sa;
        return sb <= sa;
    }
    if (operation == 0) return a == b;
    if (operation == 1) return b >= a;
    return b <= a;
}

struct CPU64PackedStringResult {
    U32 mask;
    U32 length1;
    U32 length2;
};

static inline U32 cpu64StringLength(S64 length, U32 limit) {
    if (length >= (S64)limit || length <= -(S64)limit) return limit;
    return (U32)(length < 0 ? -length : length);
}

static inline CPU64PackedStringResult cpu64ComparePackedStrings(
    U64 firstLo, U64 firstHi, U64 secondLo, U64 secondHi,
    U8 control, bool explicitLength, S64 inputLength1, S64 inputLength2) {
    const U32 bits = (control & 1) ? 16 : 8;
    const U32 count = (control & 1) ? 8 : 16;
    const bool isSigned = (control & 2) != 0;
    const U32 aggregation = (control >> 2) & 3;
    U32 length1 = count, length2 = count;
    if (explicitLength) {
        length1 = cpu64StringLength(inputLength1, count);
        length2 = cpu64StringLength(inputLength2, count);
    } else {
        for (U32 i = 0; i < count; i++) {
            if (cpu64StringElement(firstLo, firstHi, i, bits) == 0) {
                length1 = i;
                break;
            }
        }
        for (U32 i = 0; i < count; i++) {
            if (cpu64StringElement(secondLo, secondHi, i, bits) == 0) {
                length2 = i;
                break;
            }
        }
    }

    U32 result = 0;
    if (aggregation == 0) { // equal any: each second-string element vs all first
        for (U32 j = 0; j < count; j++) {
            bool found = false;
            if (j < length2) {
                for (U32 i = 0; i < count; i++) {
                    if (i < length1 && cpu64StringElementCompare(
                            cpu64StringElement(firstLo, firstHi, i, bits),
                            cpu64StringElement(secondLo, secondHi, j, bits),
                            bits, isSigned, 0)) {
                        found = true;
                        break;
                    }
                }
            }
            if (found) result |= 1U << j;
        }
    } else if (aggregation == 1) { // ranges: even/odd first-string bounds
        for (U32 j = 0; j < count; j++) {
            if (j >= length2) continue;
            U32 value = cpu64StringElement(secondLo, secondHi, j, bits);
            for (U32 i = 0; i < count; i += 2) {
                if (i + 1 >= length1) continue;
                U32 lower = cpu64StringElement(firstLo, firstHi, i, bits);
                U32 upper = cpu64StringElement(firstLo, firstHi, i + 1, bits);
                if (cpu64StringElementCompare(lower, value, bits, isSigned, 1) &&
                    cpu64StringElementCompare(upper, value, bits, isSigned, 2)) {
                    result |= 1U << j;
                    break;
                }
            }
        }
    } else if (aggregation == 2) { // equal each; invalid pairs follow SDM table
        for (U32 i = 0; i < count; i++) {
            bool valid1 = i < length1, valid2 = i < length2;
            bool equal = valid1 == valid2 &&
                (!valid1 || cpu64StringElementCompare(
                    cpu64StringElement(firstLo, firstHi, i, bits),
                    cpu64StringElement(secondLo, secondHi, i, bits), bits,
                    isSigned, 0));
            if (equal) result |= 1U << i;
        }
    } else { // equal ordered: first string searched within the second string
        for (U32 start = 0; start < count; start++) {
            bool matched = true;
            for (U32 i = 0; i < count - start; i++) {
                bool valid1 = i < length1;
                bool valid2 = start + i < length2;
                if (!valid1) continue; // invalid first-string lanes are wildcards
                if (!valid2 || !cpu64StringElementCompare(
                        cpu64StringElement(firstLo, firstHi, i, bits),
                        cpu64StringElement(secondLo, secondHi, start + i, bits),
                        bits, isSigned, 0)) {
                    matched = false;
                    break;
                }
            }
            if (matched) result |= 1U << start;
        }
    }

    const U32 resultMask = (1U << count) - 1;
    U32 polarity = (control >> 4) & 3;
    if (polarity == 1) {
        result = (~result) & resultMask;
    } else if (polarity == 3) {
        for (U32 i = 0; i < count; i++) {
            if (i < length2) result ^= 1U << i;
        }
    }
    return {result, length1, length2};
}

static inline U32 cpu64Crc32c(U32 crc, U64 value, U32 sourceBytes) {
    constexpr U32 polynomial = 0x82F63B78U; // reflected CRC-32C / Castagnoli
    for (U32 byte = 0; byte < sourceBytes; byte++) {
        crc ^= (U8)(value >> (byte * 8));
        for (U32 bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
    }
    return crc;
}


CPU64::CPU64(KMemory64* memory) : memory(memory) {
    reg[X64_RSP].setU64(0);
    mxcsr = 0x1F80;
    fpu.FINIT();
}

CPU64::~CPU64() = default;

void CPU64::cloneRegistersFrom(const CPU64* from) {
    for (int i = 0; i < X64_REG_COUNT; i++) {
        reg[i] = from->reg[i];
    }
    rip = from->rip;
    rflags = from->rflags;
    fsbase = from->fsbase;
    gsbase = from->gsbase;
    mxcsr = from->mxcsr;
    for (int i = 0; i < 16; i++) {
        xmm[i] = from->xmm[i];
    }
    fpu = from->fpu;
    // Signal state is process-wide in glibc's model; copy the parent's so the
    // new thread observes the same handler/mask registrations (delivery across
    // threads is a later milestone).
    for (int i = 0; i < 65; i++) {
        sigActions[i] = from->sigActions[i];
    }
    sigMask = from->sigMask;
    sigAltStack = from->sigAltStack;
}

// Hot enough to appear as its own frame in native profiles (i.e. the compiler
// chose not to inline it into step()'s huge body) — force it: the fast path is
// a compare + two array indexes and call overhead dominates it.
__attribute__((always_inline)) inline U8 CPU64::fetchByte(U64 addr) {
    if (!memory) return 0;
    // Fast path: same code page as the last fetch -> read straight from the
    // cached backing buffer, no lock, no map lookup. Code locality makes this
    // hit on essentially every byte within an instruction and across a loop
    // body, eliminating the per-byte pagesMutex + unordered_map cost that
    // dominated interpreter throughput.
    U64 pageNum = addr >> K64_PAGE_SHIFT;
    if (pageNum == fetchCachePage) {
        return fetchCacheData[addr & K64_PAGE_MASK];
    }
    // Miss: resolve the page once under the lock and cache its buffer. An
    // uncommitted/absent page returns nullptr -> fall back to readb (which
    // zero-fills) and leave the cache empty so we don't memoize a hole.
    U8* data = memory->getCommittedPagePtr(pageNum);
    if (data) {
        fetchCachePage = pageNum;
        fetchCacheData = data;
        return data[addr & K64_PAGE_MASK];
    }
    return memory->readb(addr);
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

// Same story as fetchByte: ~14% of interpreter time as a standalone frame,
// mostly call overhead + spilling `out` — inline it into step().
__attribute__((always_inline)) inline U32 CPU64::consumePrefixes(Prefixes& out) {
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
                // LOCK — record it so the RMW handler serializes the op across
                // host threads (the multi-threaded build runs guest threads on
                // real host threads sharing one KMemory64).
                out.lock = true; off++; continue;
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

__attribute__((always_inline)) inline CPU64::ModRM CPU64::decodeModRM(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes) {
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

__attribute__((always_inline)) inline U64 CPU64::loadRM(const ModRM& m, U32 size, bool rexPresent) {
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

__attribute__((always_inline)) inline void CPU64::storeRM(const ModRM& m, U32 size, U64 value, bool rexPresent) {
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


// ADC: flags for a + b + cIn. Uses unsigned __int128 for the full sum
// so CF/AF/OF are derived from the true operation, not a wrapped operand.
static void flagsAdc(U32& f, U64 a, U64 b, U64 cIn, U64 r, U32 width) {
    U64 mask = maskFor(width);
    a &= mask; b &= mask;
    U64 rm = r & mask;
    U32 wbits = width * 8;
    f &= ~(X64_CF | X64_OF | X64_AF);
    unsigned __int128 full = (unsigned __int128)a + b + (cIn & 1);
    // CF: unsigned carry out of the top bit.
    if ((full >> wbits) != 0) f |= X64_CF;
    // OF: signed overflow. Sign-extend a, b to 128 bits and check the
    // true signed sum against the signed range.
    U64 sb = signBitFor(width);
    __int128 sa = (a & sb) ? (__int128)a | (~(__int128)mask) : (__int128)a;
    __int128 sbb = (b & sb) ? (__int128)b | (~(__int128)mask) : (__int128)b;
    __int128 sfull = sa + sbb + (cIn & 1);
    __int128 smin = -(__int128)sb;
    __int128 smax = (__int128)(sb - 1);
    if (sfull < smin || sfull > smax) f |= X64_OF;
    // AF: carry from bit 3 to bit 4.
    if (((a & 0xF) + (b & 0xF) + (cIn & 1)) > 0xF) f |= X64_AF;
    setSZP(f, rm, width);
}

// SBB: flags for a - b - cIn. Borrow and signed overflow from 128-bit.
static void flagsSbb(U32& f, U64 a, U64 b, U64 cIn, U64 r, U32 width) {
    U64 mask = maskFor(width);
    a &= mask; b &= mask;
    U64 rm = r & mask;
    f &= ~(X64_CF | X64_OF | X64_AF);
    unsigned __int128 subtrahend = (unsigned __int128)b + (cIn & 1);
    // CF (borrow): 1 if a < b + cIn (unsigned).
    if ((unsigned __int128)a < subtrahend) f |= X64_CF;
    // OF: signed overflow on a - b - cIn.
    U64 sb = signBitFor(width);
    __int128 sa = (a & sb) ? (__int128)a | (~(__int128)mask) : (__int128)a;
    __int128 sbb = (b & sb) ? (__int128)b | (~(__int128)mask) : (__int128)b;
    __int128 sfull = sa - sbb - (cIn & 1);
    __int128 smin = -(__int128)sb;
    __int128 smax = (__int128)(sb - 1);
    if (sfull < smin || sfull > smax) f |= X64_OF;
    // AF: borrow from bit 4 (a_low4 < b_low4 + cIn).
    if ((a & 0xF) < ((b & 0xF) + (cIn & 1))) f |= X64_AF;
    setSZP(f, rm, width);
}

// Logical ops always clear CF and OF, leave AF undefined (we clear it).
static void flagsLogic(U32& f, U64 r, U32 width) {
    f &= ~(X64_CF | X64_OF | X64_AF);
    setSZP(f, r, width);
}

__attribute__((always_inline)) inline void CPU64::runAlu(U8 aluOp, U32 size, bool destIsRM, U64 lhs, U64 rhs,
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
                flagsAdc(rflags, lhs, rhs, cIn, result, size); break; }
        case 3: { U64 cIn = (rflags & X64_CF) ? 1 : 0;
                result = lhs - rhs - cIn;
                flagsSbb(rflags, lhs, rhs, cIn, result, size); break; }
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
__attribute__((always_inline)) inline static U64 doShift(U32& rflags, U8 sub, U64 v, U8 count, U32 width) {
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
// BW64_OPPROF: opcode-frequency profiler (env-gated, zero cost when off). Counts
// each executed (op, op2) pair so we can see which opcodes dominate a real
// wineserver/notepad boot and order the step() dispatch by measured frequency.
// op2 is the second byte for the 0F two-byte map, else 0. Dumped at process exit.
static bool g_opProf = false;
static bool g_opProfInit = false;
static U64 g_opProfCount[256][256] = {};   // [op][op2]
static U64 g_opProf1[256] = {};            // single-byte totals

void cpu64DumpOpProfile() {
    if (!g_opProf) return;
    // Top single-byte opcodes.
    struct E { U32 op, op2; U64 n; };
    std::vector<E> v;
    for (U32 a = 0; a < 256; a++) {
        if (g_opProf1[a]) v.push_back({a, 0, g_opProf1[a]});
        for (U32 b = 0; b < 256; b++)
            if (g_opProfCount[a][b]) v.push_back({a, b, g_opProfCount[a][b]});
    }
    std::sort(v.begin(), v.end(), [](const E& x, const E& y){ return x.n > y.n; });
    klog("BW64_OPPROF: top opcodes by execution count (op[/op2] = count):");
    U64 total = 0; for (auto& e : v) total += e.n;
    for (size_t i = 0; i < v.size() && i < 40; i++) {
        if (v[i].op == 0x0F)
            klog_fmt("  0F %02x = %llu (%.1f%%)", v[i].op2,
                     (unsigned long long)v[i].n, total ? 100.0*v[i].n/total : 0.0);
        else
            klog_fmt("  %02x    = %llu (%.1f%%)", v[i].op,
                     (unsigned long long)v[i].n, total ? 100.0*v[i].n/total : 0.0);
    }
    klog_fmt("BW64_OPPROF: %llu instructions profiled", (unsigned long long)total);
}

// ---------------------------------------------------------------------------
// BW64_REFWATCH: refcount double-release canary for bug #2 (wineserver teardown
// heap corruption). The assert that aborts wineserver is
//     server/object.c:443  assert( obj->refcount );   // release_object()
// i.e. release_object() is called on a struct object whose refcount is ALREADY
// 0 — a double-release / use-after-free. (refcount is the first member, a 4-byte
// unsigned int at offset 0; ops* is at +8. Verified from the stripped wineserver64
// disassembly: release_object @ file-vaddr 0x30170 begins
//   53               push %rbx                ; function entry (we trap HERE)
//   8b 07            mov  (%rdi),%eax        ; eax = obj->refcount
//   85 c0            test %eax,%eax
//   74 xx            je   <assert_fail path>  ; refcount==0 -> abort.)
//
// WSBT (stack scan at the abort WRITE) couldn't name the culprit because the
// binary is stripped + frame-pointer-omitted. This catches the bug ONE
// instruction EARLIER — at release_object's entry, while %rdi still holds the
// dying object and [%rsp] still holds the CALLER's return address — which is the
// exact wineserver call site that released a dead object (the fix lead).
//
// Self-validating: rather than trust a load base, we confirm the 4 prologue
// bytes (8b 07 85 c0) at the probed address; default address is the fixed PIE
// base (0x400000000) + 0x30170, overridable via BW64_REFWATCH=0xADDR. Gated;
// one rip compare per instruction only when the env is set.
static bool g_refWatchInit = false;
static U64  g_refObjAddr = 0;          // absolute guest vaddr of release_object
static bool g_refValidated = false;    // prologue bytes confirmed at g_refObjAddr
// Small ring of recent release_object calls so that, at the fatal double-release,
// we can also show the object's PRIOR (legitimate, final) release + its caller.
struct RefRelRec { U64 obj; U64 caller; U32 refBefore; U32 pid; };
static RefRelRec g_refRing[64];
static U32       g_refRingNext = 0;
static std::mutex g_refRingMutex;

static void refWatchInit() {
    g_refWatchInit = true;
    const char* e = std::getenv("BW64_REFWATCH");
    if (!e) return;
    if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X'))
        g_refObjAddr = std::strtoull(e, nullptr, 0);
    else
        g_refObjAddr = 0x400000000ULL + 0x30170ULL; // PIE base + release_object
}

U32 CPU64::step() {
    U64 ipStart = rip;
    if (ipStart != recoverLastRip) {
        recoverLastRip = ipStart;
        recoverFaultsAtRip = 0;
    }

    // BW64_REFWATCH probe: catch wineserver's double-release at release_object
    // entry, one instruction before the assert. See the big comment above.
    if (!g_refWatchInit) refWatchInit();
    if (g_refObjAddr && rip == g_refObjAddr) {
        // Confirm we're really at release_object (prologue 8b 07 85 c0). Done
        // once; if the bytes don't match, the base guess is wrong — disarm so we
        // don't spam on a coincidental rip match.
        if (!g_refValidated) {
            // release_object entry: 53 (push %rbx) 8b 07 (mov (%rdi),%eax)
            // 85 c0 (test %eax,%eax). We trap the very first byte (entry), where
            // %rdi still holds the object and [%rsp] is the caller's return addr.
            U8 b0 = fetchByte(rip), b1 = fetchByte(rip + 1),
               b2 = fetchByte(rip + 2), b3 = fetchByte(rip + 3);
            if (b0 == 0x53 && b1 == 0x8b && b2 == 0x07 && b3 == 0x85) {
                g_refValidated = true;
                klog_fmt("REFWATCH: armed — release_object confirmed at 0x%llx (pid=%u %s)",
                         (unsigned long long)rip,
                         (unsigned)(thread && thread->process ? thread->process->id : 0),
                         (thread && thread->process) ? thread->process->name.c_str() : "?");
            } else {
                klog_fmt("REFWATCH: prologue mismatch at 0x%llx (got %02x %02x %02x %02x) — disarming; set BW64_REFWATCH=0x<release_object addr>",
                         (unsigned long long)rip, b0, b1, b2, b3);
                g_refObjAddr = 0;
            }
        }
        if (g_refObjAddr) {
            U64 obj    = reg[X64_RDI].u64;
            U32 refNow = memory ? memory->readd(obj) : 0;
            U64 caller = (memory && reg[X64_RSP].u64) ? memory->readq(reg[X64_RSP].u64) : 0;
            U32 pid    = (thread && thread->process) ? thread->process->id : 0;
            {
                std::lock_guard<std::mutex> lk(g_refRingMutex);
                if (refNow == 0) {
                    // THE BUG: releasing an object whose refcount is already 0.
                    U64 ops = memory ? memory->readq(obj + 8) : 0;
                    klog_fmt("REFWATCH: *** DOUBLE-RELEASE *** pid=%u obj=0x%llx refcount=0 "
                             "ops=0x%llx caller=0x%llx (caller-PIE=0x%llx)",
                             (unsigned)pid, (unsigned long long)obj,
                             (unsigned long long)ops, (unsigned long long)caller,
                             (unsigned long long)(caller >= 0x400000000ULL ? caller - 0x400000000ULL : caller));
                    // First 32 bytes of the dead object for ops/struct identification.
                    char hx[128] = {0};
                    for (int i = 0; i < 32; i++)
                        snprintf(hx + i * 3, 4, "%02x ", memory ? memory->readb(obj + i) : 0);
                    klog_fmt("REFWATCH:   obj[0..32]= %s", hx);
                    // Replay any earlier release of THIS object from the ring — the
                    // legitimate final release + its caller, to pair against this
                    // buggy one. (Newest-first.)
                    for (U32 k = 0; k < 64; k++) {
                        U32 idx = (g_refRingNext + 64 - 1 - k) % 64;
                        if (g_refRing[idx].obj == obj) {
                            klog_fmt("REFWATCH:   prior release of obj=0x%llx: caller=0x%llx (PIE=0x%llx) refBefore=%u pid=%u",
                                     (unsigned long long)obj,
                                     (unsigned long long)g_refRing[idx].caller,
                                     (unsigned long long)(g_refRing[idx].caller >= 0x400000000ULL ? g_refRing[idx].caller - 0x400000000ULL : g_refRing[idx].caller),
                                     g_refRing[idx].refBefore, g_refRing[idx].pid);
                        }
                    }
                    // Dump the wineserver write ring filtered to this object's
                    // body (refcount + list linkage) — shows every write to the
                    // dead object's next/prev/refcount + the issuing RIP, so we
                    // can tell a stray/duplicate LINK write (emulator-induced)
                    // from a genuine wine double-link. (BW64_MEMRING must be set.)
                    kmemory64DumpMemRing(obj);
                }
                // Record every release (including this fatal one) into the ring.
                g_refRing[g_refRingNext % 64] = { obj, caller, refNow, pid };
                g_refRingNext++;
            }
        }
    }

    Prefixes p;
    U32 opOff = consumePrefixes(p);
    bool rexW = (p.rex & 0x08) != 0;
    bool rexPresent = (p.rex != 0);

    // Default operand size in long mode is 32; REX.W → 64; 66h → 16.
    U32 opSize = rexW ? 8u : (p.osize16 ? 2u : 4u);

    U8 op = fetchByte(rip + opOff);

    // Deliver a synchronous, instruction-addressed fault. The test harness
    // has no attached thread or signal handlers, so an unhandled fault yields
    // at the faulting RIP; a guest handler instead receives the saved context.
    auto faultAtInstruction = [&](U32 sig, U32 trapNo) -> U32 {
        if (trapNo == 13 && std::getenv("BW64_UNIMPLDUMP")) {
            klog_fmt("CPU64: #GP at RIP=0x%llx bytes=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     (unsigned long long)ipStart,
                     fetchByte(ipStart), fetchByte(ipStart + 1), fetchByte(ipStart + 2),
                     fetchByte(ipStart + 3), fetchByte(ipStart + 4), fetchByte(ipStart + 5),
                     fetchByte(ipStart + 6), fetchByte(ipStart + 7), fetchByte(ipStart + 8),
                     fetchByte(ipStart + 9), fetchByte(ipStart + 10), fetchByte(ipStart + 11),
                     fetchByte(ipStart + 12), fetchByte(ipStart + 13), fetchByte(ipStart + 14));
        }
        if (raiseSyncFault(sig, trapNo, 0, ipStart)) return 0;
        if (thread && thread->process) thread->process->signalProcess(sig);
        yield = true;
        return 0;
    };


    if (!g_opProfInit) {
        g_opProfInit = true;
        g_opProf = std::getenv("BW64_OPPROF") != nullptr;
        if (g_opProf) std::atexit(cpu64DumpOpProfile);
    }
    if (g_opProf) {
        if (op == 0x0F) g_opProfCount[0x0F][fetchByte(rip + opOff + 1)]++;
        else g_opProf1[op]++;
    }

    // Instructions unavailable in this user-mode CPU model fault at their
    // original RIP. INT3 retains its debugger/breakpoint yield behavior below.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x0B) { // UD2: #UD
        if (bw64RecoverEnabled()) bw64RecoverNoteSigillFallback();
        return faultAtInstruction(K_SIGILL, 6);
    }
    // Browser recovery treats an absent I/O port as returning zero. Support
    // the single non-REP INSD form used by the Q3 startup probe; strict mode
    // still raises the architectural user-mode #GP below.
    if (op == 0x6D && p.rep == 0 && bw64RecoverEnabled()) {
        const U32 size = p.osize16 ? 2 : 4;
        const U64 dst = p.asize32 ? (U64)reg[X64_RDI].u32 : reg[X64_RDI].u64;
        if (size == 2) memory->writew(dst, 0);
        else memory->writed(dst, 0);
        const S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        if (p.asize32) reg[X64_RDI].setU32(reg[X64_RDI].u32 + (U32)step);
        else reg[X64_RDI].setU64(reg[X64_RDI].u64 + (U64)step);
        U32 used = opOff + 1;
        rip += used;
        return used;
    }
    if (op == 0xCD || op == 0xF1 || op == 0xF4 || op == 0xFA || op == 0xFB ||
        (op >= 0x6C && op <= 0x6F) || (op >= 0xE4 && op <= 0xE7) ||
        (op >= 0xEC && op <= 0xEF) || op == 0x9A || op == 0xEA ||
        op == 0xCA || op == 0xCB) {
        return faultAtInstruction(K_SIGSEGV, 13); // #GP in user mode
    }
    if (op == 0xFF) {
        U8 modrmByte = fetchByte(rip + opOff + 1);
        U8 group = (modrmByte >> 3) & 7;
        if (group == 3 || group == 5) // far CALL/JMP r/m
            return faultAtInstruction(K_SIGSEGV, 13);
    }

    // ---- Dispatch jump table ----
    // Generated by tools/gen_dispatch64.py from the if-cascade below: every
    // opcode value jumps to the FIRST block whose condition could possibly
    // match it (conditions with non-op terms still evaluate at the target and
    // fall through to later blocks exactly as before — only the provably
    // non-matching compares above the target are skipped). A native profile
    // had 34% of interpreter time in step(); the linear compare chain was a
    // large slice of that. Ops no block can match go straight to `unhandled`.
    // REGENERATE (do not hand-edit) after adding/reordering opcode blocks:
    //   python3 tools/gen_dispatch64.py
    switch (op) {
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: 
        goto dsp_0;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: 
        goto dsp_1;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7: 
        goto dsp_2;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: 
        goto dsp_3;
        case 0x89: goto dsp_4;
        case 0x8b: goto dsp_5;
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x08: case 0x09: 
        case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x10: case 0x11: case 0x12: case 0x13: 
        case 0x14: case 0x15: case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: 
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x28: case 0x29: 
        case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x30: case 0x31: case 0x32: case 0x33: 
        case 0x34: case 0x35: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: 
        goto dsp_6;
        case 0x80: case 0x81: case 0x83: goto dsp_7;
        case 0x84: case 0x85: goto dsp_8;
        case 0x88: goto dsp_9;
        case 0x8a: goto dsp_10;
        case 0x8c: case 0x8e: goto dsp_11;
        case 0xc6: goto dsp_12;
        case 0xc7: goto dsp_13;
        case 0x8d: goto dsp_14;
        case 0x90: goto dsp_15;
        case 0x9b: goto dsp_16;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: goto dsp_17;
        case 0x86: case 0x87: goto dsp_18;
        case 0x99: goto dsp_19;
        case 0x98: goto dsp_20;
        case 0xd7: goto dsp_21;
        case 0x68: case 0x6a: goto dsp_22;
        case 0x0f: goto dsp_23;
        case 0xc3: goto dsp_26;
        case 0xc2: goto dsp_27;
        case 0xcf: goto dsp_28;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: goto dsp_29;
        case 0xa8: case 0xa9: goto dsp_30;
        case 0xf6: case 0xf7: goto dsp_31;
        case 0xfe: goto dsp_32;
        case 0xff: goto dsp_33;
        case 0x8f: goto dsp_34;
        case 0xe8: goto dsp_35;
        case 0xe9: goto dsp_36;
        case 0xeb: goto dsp_37;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: 
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: 
        goto dsp_38;
        case 0x63: goto dsp_40;
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: goto dsp_41;
        case 0x69: case 0x6b: goto dsp_42;
        case 0xa4: case 0xa5: case 0xaa: case 0xab: goto dsp_43;
        case 0xa6: case 0xa7: case 0xae: case 0xaf: goto dsp_44;
        case 0xc9: goto dsp_45;
        case 0x9c: goto dsp_46;
        case 0x9d: goto dsp_47;
        case 0x9e: goto dsp_48;
        case 0x9f: goto dsp_49;
        case 0xcc: goto dsp_50;
        case 0xfc: goto dsp_51;
        case 0xfd: goto dsp_52;
        case 0xf5: goto dsp_53;
        case 0xf8: goto dsp_54;
        case 0xf9: goto dsp_55;
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: 
        goto dsp_59;
        case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17: case 0x1e: case 0x1f: case 0x26: 
        case 0x27: case 0x2e: case 0x2f: case 0x36: case 0x37: case 0x3e: case 0x3f: case 0x40: 
        case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: 
        case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x60: 
        case 0x61: case 0x62: case 0x64: case 0x65: case 0x66: case 0x67: case 0x6c: case 0x6d: 
        case 0x6e: case 0x6f: case 0x82: case 0x9a: case 0xc4: case 0xc5: 
        case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xd4: case 0xd5: case 0xd6: 
        case 0xe3: goto dsp_60;
        case 0xe0: case 0xe1: case 0xe2: goto dsp_61;
        case 0xac: case 0xad: goto dsp_62;
        case 0xc8: goto dsp_63;
        case 0xe4: case 0xe5: case 0xe6: case 0xe7: 
        case 0xea: case 0xec: case 0xed: case 0xee: case 0xef: case 0xf0: case 0xf1: case 0xf2: 
        case 0xf3: case 0xf4: case 0xfa: case 0xfb: goto unhandled;
    }

    // JRCXZ rel8 (E3 cb). Jump short if RCX is zero. The 0x67
    // address-size prefix selects the 32-bit ECX test (JECXZ);
    // 0x66 operand-size prefix has no effect. Follows the dsp_37
    // (JMP rel8) rip/used pattern.
dsp_60:
    if (op == 0xE3) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 2;
        U64 count = p.asize32 ? (U64)reg[X64_RCX].u32 : reg[X64_RCX].u64;
        rip = (count == 0) ? (rip + used + (U64)(S64)disp) : (rip + used);
        return used;
    }
    goto unhandled;

    // LOOP/LOOPE/LOOPNE rel8 (E2/E1/E0 cb). Decrement the count register
    // (RCX, or ECX with the 0x67 address-size prefix), then jump short if
    // the decremented count is non-zero and, for E1/E0, the ZF condition
    // holds: E2 jumps when count != 0; E1 when count != 0 && ZF==1;
    // E0 when count != 0 && ZF==0. The decrement does not modify flags.
    // The 0x66 operand-size prefix has no effect. Follows the dsp_60
    // (JRCXZ) rip/used pattern.
dsp_61:
    if (op >= 0xE0 && op <= 0xE2) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 2;
        U64 count;
        if (p.asize32) {
            U32 c = reg[X64_RCX].u32 - 1;
            reg[X64_RCX].setU32(c);   // 32-bit write zero-extends, per x86-64
            count = c;
        } else {
            count = reg[X64_RCX].u64 - 1;
            reg[X64_RCX].setU64(count);
        }
        bool zf = (rflags & X64_ZF) != 0;
        bool take = (count != 0) &&
                    (op == 0xE2 || (op == 0xE1 ? zf : !zf));
        rip = take ? (rip + used + (U64)(S64)disp) : (rip + used);
        return used;
    }
    goto unhandled;

    // LODS -- string load. LODSB (AC): AL = [RSI]; LODSW/LODSD/LODSQ (AD):
    // AX/EAX/RAX = [RSI]. The index is then adjusted by +/- the operand
    // size according to DF (CLD increments, STD decrements). Address size
    // defaults to 64-bit (RSI); the 0x67 prefix selects 32-bit ESI -- the
    // effective address is masked to 32 bits and the ESI update
    // zero-extends into RSI, per x86-64 semantics (cf. dsp_61 LOOP asize32
    // handling). Follows the dsp_43 (MOVS) memory/DF conventions. REP
    // prefixes have no effect on LODS (single iteration).
dsp_62:
    if (op == 0xAC || op == 0xAD) {
        U32 size = (op == 0xAC) ? 1 : opSize;
        S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        U64 addr = reg[X64_RSI].u64;
        if (p.asize32) addr &= 0xFFFFFFFFULL;
        switch (size) {
            case 1:
                writeReg8(X64_RAX, memory->readb(addr), rexPresent);
                break;
            case 2:
                reg[X64_RAX].setU16(memory->readw(addr));
                break;
            case 4:
                reg[X64_RAX].setU32(memory->readd(addr));
                break;
            default:
                reg[X64_RAX].setU64(memory->readq(addr));
                break;
        }
        if (p.asize32) {
            reg[X64_RSI].setU32(reg[X64_RSI].u32 + (U32)step);
        } else {
            reg[X64_RSI].setU64(reg[X64_RSI].u64 + (U64)step);
        }
        U32 used = opOff + 1;
        rip += used;
        return used;
    }
    goto unhandled;

    // ENTER imm16, imm8 (C8 iw ib). Create a stack frame for a procedure.
    // AllocSize (imm16) bytes are reserved on the stack; NestingLevel
    // (imm8, only the low 5 bits are used) controls how many display
    // entries are pushed for nested lexical scopes (Borland/Delphi style).
    //   64-bit (default): push RBP; FrameTemp = RSP;
    //     for i in 1..Level-1: push [RBP - i*8]; if Level > 0: push FrameTemp;
    //     RBP = FrameTemp; RSP -= AllocSize.
    //   16-bit (0x66 prefix): same shape with 2-byte pushes on BP/SP.
    // The 0x67 address-size prefix has no effect. Follows the dsp_45
    // (LEAVE) register conventions and dsp_60 rip/used pattern.
dsp_63:
    if (op == 0xC8) {
        U16 allocSize = (U16)(fetchByte(rip + opOff + 1) |
                              ((U16)fetchByte(rip + opOff + 2) << 8));
        U8 level = (U8)(fetchByte(rip + opOff + 3) & 0x1F);
        U32 used = opOff + 4;
        if (p.osize16) {
            // 16-bit operand size: 2-byte pushes, BP/SP with 16-bit wrap.
            U16 bp = reg[X64_RBP].u16;
            reg[X64_RSP].setU16(reg[X64_RSP].u16 - 2);
            memory->writew(reg[X64_RSP].u64, bp);
            U16 frameTemp = reg[X64_RSP].u16;
            for (U8 i = 1; i < level; i++) {
                reg[X64_RSP].setU16(reg[X64_RSP].u16 - 2);
                memory->writew(reg[X64_RSP].u64,
                               memory->readw((U64)(bp - (U16)(i * 2))));
            }
            if (level > 0) {
                reg[X64_RSP].setU16(reg[X64_RSP].u16 - 2);
                memory->writew(reg[X64_RSP].u64, frameTemp);
            }
            reg[X64_RBP].setU16(frameTemp);
            reg[X64_RSP].setU16(reg[X64_RSP].u16 - allocSize);
        } else {
            // 64-bit: 8-byte pushes on RBP/RSP.
            push64(reg[X64_RBP].u64);
            U64 frameTemp = reg[X64_RSP].u64;
            for (U8 i = 1; i < level; i++) {
                push64(memory->readq(reg[X64_RBP].u64 - (U64)i * 8));
            }
            if (level > 0) {
                push64(frameTemp);
            }
            reg[X64_RBP].setU64(frameTemp);
            reg[X64_RSP].setU64(reg[X64_RSP].u64 - allocSize);
        }
        rip += used;
        return used;
    }
    goto unhandled;

    // ---- Single-byte opcodes ----

    // PUSH r64 (50+rd). Always 64-bit in long mode (operand size override
    // ignored on near push of GPR).
dsp_0:
    if (op >= 0x50 && op <= 0x57) {
        U8 r = (U8)((op - 0x50) | ((p.rex & 0x01) ? 0x08 : 0));
        push64(reg[r].u64);
        rip += opOff + 1;
        return opOff + 1;
    }
    // POP r64 (58+rd).
dsp_1:
    if (op >= 0x58 && op <= 0x5F) {
        U8 r = (U8)((op - 0x58) | ((p.rex & 0x01) ? 0x08 : 0));
        reg[r].setU64(pop64());
        rip += opOff + 1;
        return opOff + 1;
    }

    // MOV r8, imm8 (B0+rb). REX.B extends the destination; absence of REX
    // means AH/BH/CH/DH for indices 4-7, REX present means SPL/BPL/SIL/DIL.
dsp_2:
    if (op >= 0xB0 && op <= 0xB7) {
        U8 r = (U8)((op - 0xB0) | ((p.rex & 0x01) ? 0x08 : 0));
        U8 imm = fetchByte(rip + opOff + 1);
        writeReg8(r, imm, rexPresent);
        rip += opOff + 2;
        return opOff + 2;
    }

    // MOV r64, imm64 (B8+rd with REX.W). Without REX.W this would be the
    // 32-bit imm form (also legal; handled in the !rexW branch below).
dsp_3:
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
dsp_4:
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
dsp_5:
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

    // ---- Hot-opcode fast dispatch (Milestone H) ----
    // These blocks were profiled (BW64_OPPROF) as the most-executed opcodes in a
    // real wine64 boot — the ALU r/r-and-r/m group, the 80/81/83 imm-ALU group,
    // and 84/85 TEST together are ~30% of all instructions, on top of the 89/8B
    // MOVs above (~27%). They were physically below ~30 other single-byte checks;
    // hoisting them here (they are disjoint from every check above — verified: no
    // earlier branch matches 00-3D / 80 / 81 / 83 / 84 / 85) cuts the average
    // dispatch scan from ~30 to ~5 for >55% of executions. Bodies are unchanged.

    // ALU r/r and r/m forms. 00/01/02/03 + 04/05 imm-acc, repeated per 8 bytes
    // for ADD/OR/ADC/SBB/AND/SUB/XOR/CMP. /6 and /7 rows are legacy long-mode-
    // invalid opcodes (filtered), and 0F is the two-byte escape.
dsp_6:
    if (op <= 0x3D && ((op & 0x06) != 0x06)) {
        U8 aluOp = (op >> 3) & 0x7;
        U8 form  = op & 0x7;
        if (form <= 3) {
            U32 size = (form & 1) ? opSize : 1;
            bool destIsRM = (form < 2);  // forms 0,1: dest is r/m; forms 2,3: dest is reg
            ModRM m = decodeModRM(rip + opOff + 1, p, 0);
            // `lock add`/`lock or`/`lock and`/... on a memory dest must be an
            // atomic RMW across host threads (glibc futex/lock words use these).
            CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
            if (p.lock && destIsRM && !m.isReg) atomicLock.lock();
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
            runAlu(aluOp, size, true, a, imm, fake, rexPresent);
            U32 used = opOff + 1 + immLen;
            rip += used;
            return used;
        }
    }

    // 80/81/83 immediate-group ALU. /digit selects the alu op.
dsp_7:
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
        CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
        if (p.lock && !m.isReg) atomicLock.lock();
        U64 a = loadRM(m, size, rexPresent);
        runAlu(aluOp, size, true, a, imm, m, rexPresent);
        U32 used = opOff + 1 + m.length + immLen;
        rip += used;
        return used;
    }

    // TEST r/m, r (84/85). Computes AND, sets flags, discards result.
dsp_8:
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

    // MOV r/m8, r8 (88 /r) and MOV r8, r/m8 (8A /r).
dsp_9:
    if (op == 0x88) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 src = readReg8(m.regField, rexPresent);
        storeRM(m, 1, src, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }
dsp_10:
    if (op == 0x8A) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 val = (U8)loadRM(m, 1, rexPresent);
        writeReg8(m.regField, val, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m16, Sreg (8C /r) and MOV Sreg, r/m16 (8E /r). In 64-bit long mode
    // the segment registers are vestigial: CS/SS/DS/ES carry the flat user-mode
    // selectors and FS/GS bases are set via arch_prctl (the selector value is
    // 0). The ModRM `reg` field selects the segment: 0=ES 1=CS 2=SS 3=DS 4=FS
    // 5=GS. wine's PE loader reads %cs (8C /1) to confirm it's running 64-bit,
    // so we must return the canonical Linux user selectors. Stores are 16-bit
    // (a register destination is zero-extended to the operand size per the SDM,
    // but the common encoding here targets memory or a 16-bit slot).
dsp_11:
    if (op == 0x8C || op == 0x8E) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        // Linux x86-64 user-mode selectors. FS/GS selectors are 0 (base via MSR).
        static const U16 kSegSel[6] = { 0x2b, 0x33, 0x2b, 0x2b, 0x00, 0x00 };
        U8 segIdx = m.regField & 0x7;
        if (op == 0x8C) {
            U16 sel = (segIdx < 6) ? kSegSel[segIdx] : 0;
            // Destination width: for a memory operand it's 16-bit; for a register
            // operand the selector is zero-extended to the operand size.
            if (m.isReg) {
                storeRM(m, opSize == 8 ? 8 : (opSize == 4 ? 4 : 2), (U64)sel, rexPresent);
            } else {
                storeRM(m, 2, (U64)sel, rexPresent);
            }
        } else {
            // MOV Sreg, r/m16 — loading a segment selector. In long mode this is
            // effectively a no-op for CS/SS/DS/ES (the descriptor cache is flat);
            // we read (and discard) the operand to advance correctly. Loading
            // FS/GS selector doesn't change the base (that's arch_prctl's job).
            (void)loadRM(m, 2, rexPresent);
        }
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m8, imm8 (C6 /0). Discovered via real musl-static hello: the
    // x86_64-linux-musl startup sets a static byte flag (often
    // __environ_locked or a TLS-init guard) with this exact encoding.
dsp_12:
    if (op == 0xC6) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 1);
        if ((m.regField & 0x7) == 0) {
            U8 imm = fetchByte(rip + opOff + 1 + m.length);
            storeRM(m, 1, imm, rexPresent);
            U32 used = opOff + 1 + m.length + 1;
            rip += used;
            return used;
        }
    }

    // MOV r/m, imm  (C7 /0). imm is imm32 sign-extended for 64-bit, or
    // imm32 zero-extended for 32-bit, or imm16 for 16-bit.
dsp_13:
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
dsp_14:
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
dsp_15:
    if (op == 0x90 && !(p.rex & 0x01)) {
        rip += opOff + 1;
        return opOff + 1;
    }

    // FWAIT / WAIT (9B). On hardware, waits for pending x87 exceptions to
    // be serviced. Our soft FPU has no asynchronous exceptions, so this
    // is a NOP. Discovered around x87 epilogue sequences emitted by
    // gcc/clang when a function returns long double or stores via FSTP m80.
dsp_16:
    if (op == 0x9B) {
        rip += opOff + 1;
        return opOff + 1;
    }

    // XCHG r, RAX (90+rd). XCHG R8-R15 with RAX when REX.B=1. The plain 0x90
    // case is handled above as NOP. opSize from prefix/REX.W.
dsp_17:
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
dsp_18:
    if (op == 0x86 || op == 0x87) {
        U32 size = (op == 0x86) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        // XCHG with a memory operand is implicitly atomic on x86 (LOCK is
        // assumed); serialize it across host threads so glibc's spinlock
        // acquire (xchg [lock], eax) is correct under real threading.
        CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
        if (!m.isReg) atomicLock.lock();
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
dsp_19:
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
dsp_20:
    if (op == 0x98) {
        if (opSize == 2) reg[X64_RAX].setU16((U16)(S16)(S8)reg[X64_RAX].u8);
        else if (opSize == 4) reg[X64_RAX].setU64((U64)(U32)(S32)(S16)reg[X64_RAX].u16);
        else reg[X64_RAX].setU64((U64)(S64)(S32)reg[X64_RAX].u32);
        rip += opOff + 1;
        return opOff + 1;
    }

    // XLAT/XLATB (D7): AL = [RBX + AL] (zero-extended AL as a byte index). In
    // long mode the base is the full 64-bit RBX; no segment base is applied
    // (CS/DS/ES/SS are flat). An address-size override would use EBX, but
    // FreeType's table-driven code uses the default 64-bit form.
dsp_21:
    if (op == 0xD7) {
        U64 base = p.asize32 ? (U64)reg[X64_RBX].u32 : reg[X64_RBX].u64;
        U64 addr = base + (U64)reg[X64_RAX].u8;
        reg[X64_RAX].setU8(memory->readb(addr));
        rip += opOff + 1;
        return opOff + 1;
    }

    // PUSH imm8 (6A) / PUSH imm32 sign-ext (68). Always 64-bit push in long mode.
dsp_22:
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
dsp_23:
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x05) {
        // Per AMD64 ABI, SYSCALL clobbers RCX with the return RIP and R11
        // with RFLAGS. Userspace relies on this even when the syscall
        // succeeds — set both before transferring to the kernel layer.
        U64 syscallStartRip = rip;
        U64 nextRip = rip + opOff + 2;
        // Wine NT-syscall redirect. Wine's PE-side NtXxx stubs choose between a
        // raw `syscall` (0F 05) and an indirect `call [KUSER_SHARED_DATA+0x1000]`
        // to __wine_syscall_dispatcher, based on the SystemCall flag at
        // KUSER_SHARED_DATA+0x308 (0x7ffe0308). On a real host wine sets that
        // flag when it has wired the raw syscall to its dispatcher (SUD/seccomp);
        // we don't, so the flag reads 0 and the stub falls into 0F 05 with a
        // *Windows NT* ordinal in RAX (e.g. NtQueryVirtualMemory) — which is NOT
        // a Linux syscall and would be mis-dispatched, then trip wine's
        // __fastfail (UD2). Wine has already published its dispatcher pointer at
        // [0x7ffe1000] (verified: a unix ntdll.so address). So when a SYSCALL
        // comes from inside the PE ntdll image and that pointer is set, emulate
        // exactly what `call [0x7ffe1000]` would have done: push the return
        // address (the instruction after the 0F 05) and transfer to the
        // dispatcher. Wine then reads the NT number from RAX and invokes the
        // correct unix-side implementation entirely in userspace. The dispatcher
        // itself, and the unix ntdll, still issue genuine Linux syscalls (from
        // low unix addresses) which fall through to ksyscall64 normally.
        // Only PE-image syscalls (ntdll.dll ImageBase 0x170000000) are NT
        // stubs; gate the shared-data read on the cheap RIP range so the hot
        // Linux-syscall path (glibc/unix ntdll, low addresses) is untouched.
        //
        // Wine's NtXxx stub is:
        //     mov r10, rcx
        //     mov eax, <ntno>
        //     test byte ptr [0x7ffe0308], 1     ; KUSER_SHARED_DATA SystemCall
        //     jne  .indirect                    ; flag set -> call dispatcher
        //     syscall                           ; flag clear -> raw syscall  (HERE)
        //     ret
        //   .indirect:
        //     call [0x7ffe1000]                 ; -> __wine_syscall_dispatcher
        //     ret
        // Wine publishes the dispatcher pointer at [0x7ffe1000] but, on a host
        // where it didn't wire the raw `syscall` to its dispatcher, leaves the
        // SystemCall flag at 0 — so PE NT stubs reach the raw `syscall`, whose
        // RAX holds a *Windows NT* ordinal, not a Linux number. Rather than jump
        // into the dispatcher with a synthesized frame (its prolog pops the
        // return off the stack and reads gs:[0x328] — a fragile ABI to fake), we
        // flip wine's own decision: SET the SystemCall flag so every NtXxx stub
        // uses its native `call [0x7ffe1000]` path (correct call ABI, correct
        // return), and steer THIS in-flight stub onto that path by resuming at
        // the `jne` target. The flag write makes all subsequent stubs branch
        // there on their own without re-entering this handler.
        if (syscallStartRip >= 0x170000000ULL && syscallStartRip < 0x170400000ULL) {
            U64 ntDispatch = memory->readq(0x7ffe1000ULL);
            if (ntDispatch) {
                // Persist the choice for all future stubs.
                memory->writeb(0x7ffe0308ULL, memory->readb(0x7ffe0308ULL) | 1);
                // Resume on the indirect path. The stub layout is fixed:
                //   syscall(0F 05) @ R, ret @ R+1, .indirect (jmp/call) @ R+3.
                // The `jne` (2 bytes before the syscall) targets R+3; jump there
                // so wine's own `call [0x7ffe1000]` executes with its real ABI.
                rip = syscallStartRip + 3;
                if (getenv("BW64_NTDUMP")) {
                    static int once = 0;
                    if (once++ < 6)
                        klog_fmt("NT-redirect RAX=%llx stub=0x%llx -> indirect 0x%llx disp=0x%llx",
                                 (unsigned long long)reg[X64_RAX].u64,
                                 (unsigned long long)syscallStartRip,
                                 (unsigned long long)rip,
                                 (unsigned long long)ntDispatch);
                }
                return opOff + 2;
            }
        }
        reg[X64_RCX].setU64(nextRip);
        reg[X64_R11].setU64((U64)rflags);
        rip = nextRip;
        syscallRip = syscallStartRip;
        ksyscall64(this);
        // Single-threaded cooperative park: a futex WAIT that had to block set
        // reExecuteSyscall and parked this thread. Rewind RIP to the SYSCALL so
        // it re-runs when the thread is rescheduled (RAX was not written). yield
        // has already been requested by the park so the run loop returns now.
        if (reExecuteSyscall) {
            reExecuteSyscall = false;
            rip = syscallStartRip;
        }
        return opOff + 2;
    }

    // ENDBR64 (F3 0F 1E FA) / ENDBR32 (F3 0F 1E FB) — CET indirect-branch
    // landing pads. We don't model shadow stacks / IBT, so they're NOPs.
    // glibc 2.36 emits ENDBR64 at the top of nearly every function, so this
    // is hit constantly the moment real glibc code runs. Also accept the
    // RDSSPQ/INCSSP-adjacent NOP form F3 0F 1E /r generically (modrm reg/reg)
    // as a NOP since none of the CET state is observable here.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x1E && p.rep == 0xF3) {
        U8 modrm = fetchByte(rip + opOff + 2);
        // ENDBR64=FA, ENDBR32=FB, plus the general reg/reg NOP-equivalent form.
        if (modrm == 0xFA || modrm == 0xFB || (modrm & 0xC0) == 0xC0) {
            U32 used = opOff + 3;
            rip += used;
            return used;
        }
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
dsp_26:
    if (op == 0xC3) {
        rip = pop64();
        return opOff + 1;
    }

    // RET imm16 (C2 iw) — near return, then pop imm16 bytes of parameters.
    // Win64 callees that forward to a stdcall-style helper (and tiny loader
    // stubs like BALDI.exe's entry tail) emit `ret 0`. RSP is a plain GPR
    // here, so the add is exact.
dsp_27:
    if (op == 0xC2) {
        U16 imm = (U16)(fetchByte(rip + opOff + 1) |
                        ((U16)fetchByte(rip + opOff + 2) << 8));
        rip = pop64();
        reg[X64_RSP].u64 += imm;
        return opOff + 3;
    }

    // IRETQ (REX.W CF == 48 CF), and the non-promoted IRET (CF) / IRETD.
    // Wine's PE-side ntdll uses iretq to return from its user-mode exception
    // dispatcher (NtContinue / the KiUserExceptionDispatcher tail). In long
    // mode the interrupt stack frame is, from the top of stack downward:
    //   RIP, CS, RFLAGS, RSP, SS  (five entries).
    // Each entry is pushed as a qword for IRETQ (REX.W); for a 32-bit IRETD
    // (no REX.W) the entries are dwords. We don't model segmentation (CS/SS
    // are a flat-model approximation — see the CSGSFS handling in syscall64),
    // so we pop and discard CS/SS. RFLAGS is masked exactly like POPFQ (9D):
    // only the user-visible arithmetic/direction/IF bits are writable; the
    // reserved bits keep their fixed values. This is effectively a privileged
    // return-from-frame, structurally the same as restoreSignalFrame.
dsp_28:
    if (op == 0xCF) {
        const U32 writable = 0x00254FD5u; // CF PF AF ZF SF TF IF DF OF (matches POPFQ)
        if (rexW) {
            U64 newRip   = pop64();
            (void)         pop64();        // CS (ignored — flat model)
            U64 newFlags = pop64();
            U64 newRsp   = pop64();
            (void)         pop64();        // SS (ignored — flat model)
            rip = newRip;
            rflags = (rflags & ~writable) | ((U32)newFlags & writable);
            reg[X64_RSP].setU64(newRsp);
        } else {
            // 32-bit IRETD: dword entries off the (still 64-bit) stack.
            U64 sp = reg[X64_RSP].u64;
            U32 newEip   = memory->readd(sp);      sp += 4;
            (void)         memory->readd(sp);      sp += 4; // CS
            U32 newFlags = memory->readd(sp);      sp += 4;
            U32 newEsp   = memory->readd(sp);      sp += 4;
            (void)         memory->readd(sp);      sp += 4; // SS
            rip = (U64)newEip;
            rflags = (rflags & ~writable) | (newFlags & writable);
            reg[X64_RSP].setU64((U64)newEsp);
        }
        return opOff + 1;
    }

    // ---- ALU r/r-and-r/m (00-3D), 80/81/83 imm-ALU, and 84/85 TEST were
    // hoisted to the hot-opcode fast dispatch right after the 89/8B MOVs
    // (Milestone H) — they were profiled as ~30% of executions and no longer
    // pay the ~30-deep single-byte scan to reach here. See that block above.

    // TEST AL/AX/EAX/RAX, imm  (A8 ib / A9 iz).
    // A0/A1/A2/A3 — MOV AL/AX/EAX/RAX ↔ moffs (64-bit absolute address).
    // In long mode the displacement is always 8 bytes regardless of address-size
    // prefix. REX.W on A1/A3 makes the operand 64-bit; otherwise normal opSize
    // rules apply. AL/A0/A2 are byte-size; AX/EAX/RAX A1/A3 follow opSize.
    //
    // gcc emits these for `mov rax, [absolute_addr]` from PIC C code with
    // -fno-pic compiled against fixed-address globals, and for any static
    // ET_EXEC that addresses a known absolute symbol. Surfaced by
    // tools/buildMultiSegmentElf64.py via the unimpl-tracer.
dsp_29:
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        U32 size = (op == 0xA0 || op == 0xA2) ? 1 : opSize;
        // Fetch the 8-byte absolute address that follows the opcode.
        U64 addr = (U64)fetchByte(rip + opOff + 1)
                 | ((U64)fetchByte(rip + opOff + 2) << 8)
                 | ((U64)fetchByte(rip + opOff + 3) << 16)
                 | ((U64)fetchByte(rip + opOff + 4) << 24)
                 | ((U64)fetchByte(rip + opOff + 5) << 32)
                 | ((U64)fetchByte(rip + opOff + 6) << 40)
                 | ((U64)fetchByte(rip + opOff + 7) << 48)
                 | ((U64)fetchByte(rip + opOff + 8) << 56);
        if (op == 0xA0 || op == 0xA1) {
            // load from [moffs] into AL/AX/EAX/RAX
            switch (size) {
                case 1: reg[X64_RAX].setU8(memory->readb(addr)); break;
                case 2: reg[X64_RAX].setU16(memory->readw(addr)); break;
                case 4: reg[X64_RAX].setU32(memory->readd(addr)); break;
                case 8: reg[X64_RAX].setU64(memory->readq(addr)); break;
            }
        } else {
            // store AL/AX/EAX/RAX into [moffs]
            switch (size) {
                case 1: memory->writeb(addr, reg[X64_RAX].u8); break;
                case 2: memory->writew(addr, reg[X64_RAX].u16); break;
                case 4: memory->writed(addr, reg[X64_RAX].u32); break;
                case 8: memory->writeq(addr, reg[X64_RAX].u64); break;
            }
        }
        U32 used = opOff + 1 + 8;
        rip += used;
        return used;
    }

dsp_30:
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
dsp_31:
    if (op == 0xF6 || op == 0xF7) {
        U32 size = (op == 0xF6) ? 1 : opSize;
        // Only /0 TEST carries an immediate. Inspect its sub-opcode before
        // decoding so RIP-relative operands use the end of the complete
        // instruction; the other group-3 forms have no trailing immediate.
        U8 groupSub = (fetchByte(rip + opOff + 1) >> 3) & 0x7;
        U32 immBytes = groupSub == 0
            ? (size == 1 ? 1 : size == 2 ? 2 : 4)
            : 0;
        ModRM m = decodeModRM(rip + opOff + 1, p, immBytes);
        U8 sub = m.regField & 0x7;
        if (sub == 0) {
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
            U64 a = loadRM(m, size, rexPresent);
            flagsLogic(rflags, a & imm, size);
            U32 used = opOff + 1 + m.length + immLen;
            rip += used;
            return used;
        }
        if (sub == 2) { // NOT — no flags affected
            CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
            if (p.lock && !m.isReg) atomicLock.lock();
            U64 a = loadRM(m, size, rexPresent);
            storeRM(m, size, ~a, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 3) { // NEG — same as 0 - r/m, flags set per SUB.
            CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
            if (p.lock && !m.isReg) atomicLock.lock();
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
                // #DE: divide error. Deliver SIGFPE/FPE_INTDIV at this RIP so
                // the guest's exception handling runs exactly as on real
                // hardware (wine maps it to EXCEPTION_INT_DIVIDE_BY_ZERO and
                // its vectored/SEH handlers decide what to do). rip still
                // points at the DIV, which is what the handler's ucontext and
                // si_addr must capture.
                if (getenv("BW64_DIVZERO")) {
                    U64 ea = m.isReg ? 0 : m.effAddr;
                    // For the libX11 _XrmInternalStringToQuark rehash divide,
                    // the divisor global sits at ea; the table mask is ea+0x10
                    // and the table pointer at ea-0x3240. Dump them to see
                    // whether the table is live while the divisor reads 0.
                    U64 mask = (!m.isReg) ? memory->readq(ea + 0x10) : 0;
                    U64 tptr = (!m.isReg) ? memory->readq(ea - 0x3240) : 0;
                    klog_fmt("CPU64: #DE divide-by-zero RIP=0x%llx size=%u sub=%u "
                             "RAX=0x%llx RDX=0x%llx RCX=0x%llx RBX=0x%llx "
                             "ea=0x%llx [ea]=0x%llx mask@+0x10=0x%llx tptr@-0x3240=0x%llx",
                             (unsigned long long)rip, size, sub,
                             (unsigned long long)reg[X64_RAX].u64,
                             (unsigned long long)reg[X64_RDX].u64,
                             (unsigned long long)reg[X64_RCX].u64,
                             (unsigned long long)reg[X64_RBX].u64,
                             (unsigned long long)ea,
                             (unsigned long long)(m.isReg ? 0 : memory->readq(ea)),
                             (unsigned long long)mask, (unsigned long long)tptr);
                }
                if (this->raiseSyncFault(K_SIGFPE, /*trapNo #DE*/0,
                                         K_FPE_INTDIV, rip)) {
                    return 0; // rip now at the handler
                }
                // No SIGFPE handler installed — terminate like the kernel's
                // default action for an uncaught #DE instead of spinning on a
                // re-executed faulting DIV.
                klog_fmt("CPU64: unhandled #DE at RIP=0x%llx (no SIGFPE handler) "
                         "— terminating thread", (unsigned long long)rip);
                if (thread) thread->process->signalProcess(K_SIGFPE);
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

    // FE — group 4: INC r/m8 (/0) and DEC r/m8 (/1). Byte-only sibling of the
    // FF group; all other /digit values are invalid encodings. wineserver's
    // startup hits this (INC byte ptr [rip+disp32] on a refcount/flag byte).
dsp_32:
    if (op == 0xFE) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 sub = m.regField & 0x7;
        if (sub == 0 || sub == 1) {
            CPU64AtomicLock atomicLock(memory, m.effAddr, 1, std::defer_lock);
            if (p.lock && !m.isReg) atomicLock.lock();
            U64 a = loadRM(m, 1, rexPresent);
            U64 r = (sub == 0) ? a + 1 : a - 1;
            // INC/DEC preserve CF; other flags per ADD/SUB of 1.
            U32 savedCF = rflags & X64_CF;
            if (sub == 0) flagsAdd(rflags, a, 1, r, 1);
            else          flagsSub(rflags, a, 1, r, 1);
            rflags = (rflags & ~X64_CF) | savedCF;
            storeRM(m, 1, r, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        goto unhandled;
    }

    // INC/DEC r/m via FF /0 and /1. (Single-byte 40-4F encodings are REX
    // in long mode and are already consumed by the prefix loop.)
dsp_33:
    if (op == 0xFF) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 sub = m.regField & 0x7;
        if (sub == 0 || sub == 1) {
            CPU64AtomicLock atomicLock(memory, m.effAddr, opSize, std::defer_lock);
            if (p.lock && !m.isReg) atomicLock.lock();
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
dsp_34:
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
dsp_35:
    if (op == 0xE8) {
        S32 disp = (S32)fetchDword(rip + opOff + 1);
        U32 used = opOff + 1 + 4;
        U64 nextRip = rip + used;
        push64(nextRip);
        rip = nextRip + (U64)(S64)disp;
        return used;
    }

    // JMP rel32 (E9 cd).
dsp_36:
    if (op == 0xE9) {
        S32 disp = (S32)fetchDword(rip + opOff + 1);
        U32 used = opOff + 1 + 4;
        rip = rip + used + (U64)(S64)disp;
        return used;
    }

    // JMP rel8 (EB cb).
dsp_37:
    if (op == 0xEB) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 1 + 1;
        rip = rip + used + (U64)(S64)disp;
        return used;
    }

    // Jcc rel8 (70-7F). Condition encoded in low 4 bits.
dsp_38:
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
            // Note: 64-bit signed multiply can overflow S64 (UB), so do the
            // 64-bit path in __int128 and truncate; 16/32-bit products always
            // fit in S64.
            S64 r;
            bool overflow = false;
            if (opSize == 2) { r = sa * sb; overflow = (r != (S64)(S16)r); }
            else if (opSize == 4) { r = sa * sb; overflow = (r != (S64)(S32)r); }
            else {
                __int128 r128 = (__int128)sa * (__int128)sb;
                r = (S64)r128;
                overflow = (r128 != (__int128)r);
            }
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
            // CMPXCHG is glibc's mutex fast path; make the read-compare-write
            // atomic across host threads when the operand is in memory.
            CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
            if (!m.isReg) atomicLock.lock();
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

        // 0F C7 /1 — CMPXCHG8B m64 (REX.W=0) / CMPXCHG16B m128 (REX.W=1).
        // Compares EDX:EAX (8B) or RDX:RAX (16B) with the memory operand:
        // equal -> ZF=1 and ECX:EBX / RCX:RBX stored to memory, else ZF=0
        // and the memory value loaded into the accumulator pair. Only ZF is
        // modified; other arithmetic flags are preserved. Register destination is #UD on hardware;
        // fall through to the unhandled diagnostic in that case. Mono's GC
        // and Unity's Baselib atomics emit the 16-byte form.
        if (op2 == 0xC7) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg && (m.regField & 0x7) == 1) {
                CPU64AtomicLock atomicLock(memory, m.effAddr, (p.rex & 0x08) ? 16 : 8, std::defer_lock);
                atomicLock.lock();
                if (!(p.rex & 0x08)) {
                    U64 acc = ((U64)reg[X64_RDX].u32 << 32) | reg[X64_RAX].u32;
                    U64 mem = ((U64)memory->readd(m.effAddr + 4) << 32) | memory->readd(m.effAddr);
                    rflags = (rflags & ~X64_ZF) | (acc == mem ? X64_ZF : 0);
                    if (acc == mem) {
                        memory->writed(m.effAddr, reg[X64_RBX].u32);
                        memory->writed(m.effAddr + 4, reg[X64_RCX].u32);
                    } else {
                        reg[X64_RAX].setU64((U32)mem);
                        reg[X64_RDX].setU64((U32)(mem >> 32));
                    }
                } else {
                    U64 accLo = reg[X64_RAX].u64, accHi = reg[X64_RDX].u64;
                    U64 memLo = memory->readq(m.effAddr);
                    U64 memHi = memory->readq(m.effAddr + 8);
                    bool eq = (accLo == memLo && accHi == memHi);
                    rflags = (rflags & ~X64_ZF) | (eq ? X64_ZF : 0);
                    if (eq) {
                        memory->writeq(m.effAddr, reg[X64_RBX].u64);
                        memory->writeq(m.effAddr + 8, reg[X64_RCX].u64);
                    } else {
                        reg[X64_RAX].setU64(memLo);
                        reg[X64_RDX].setU64(memHi);
                    }
                }
                U32 used = opOff + 2 + m.length;
                rip += used;
                return used;
            }
        }
        // Packed single-precision reciprocal ops: RSQRTPS 0F 52 / RCPPS 0F 53
        // (no prefix) and scalar RSQRTSS/RCPSS (F3 prefix, low lane only,
        // upper lanes pass through). Hardware returns ~12-bit approximations;
        // exact host results are a valid refinement (games follow with a
        // Newton-Raphson MULPS, as Unity's math does). Inf/NaN flow through
        // IEEE rules naturally.
        if ((!p.osize16 && p.rep == 0 && (op2 == 0x52 || op2 == 0x53)) ||
            (p.rep == 0xF3 && (op2 == 0x52 || op2 == 0x53))) {
            bool scalar = (p.rep == 0xF3);
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = scalar ? memory->readd(m.effAddr) : memory->readq(m.effAddr);
                srcHi = scalar ? 0 : memory->readq(m.effAddr + 8);
            }
            auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
            auto fu32 = [](float f){ U32 b; std::memcpy(&b,&f,4); return b; };
            U64 outLo, outHi;
            if (scalar) {
                float b0 = u32f((U32)(srcLo & 0xFFFFFFFFULL));
                float r0 = (op2 == 0x52) ? (1.0f / std::sqrt(b0)) : (1.0f / b0);
                outLo = (fu32(r0) & 0xFFFFFFFFULL) | (xmm[m.regField].lo & 0xFFFFFFFF00000000ULL);
                outHi = xmm[m.regField].hi;
            } else {
                float b[4];
                b[0] = u32f((U32)(srcLo & 0xFFFFFFFFULL));
                b[1] = u32f((U32)(srcLo >> 32));
                b[2] = u32f((U32)(srcHi & 0xFFFFFFFFULL));
                b[3] = u32f((U32)(srcHi >> 32));
                U32 r[4];
                for (int i = 0; i < 4; i++) {
                    float v = (op2 == 0x52) ? (1.0f / std::sqrt(b[i])) : (1.0f / b[i]);
                    r[i] = fu32(v);
                }
                outLo = (U64)r[0] | ((U64)r[1] << 32);
                outHi = (U64)r[2] | ((U64)r[3] << 32);
            }
            xmm[m.regField].lo = outLo;
            xmm[m.regField].hi = outHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // FP conversions between scalar/packed singles, doubles and integers.
        //   F3 0F 5A CVTSS2SD   F2 0F 5A CVTSD2SS   0F 5A CVTPS2PD
        //   66 0F 5A CVTPD2PS  F3 0F 2A CVTSI2SS   F2 0F 2A CVTSI2SD
        //   F3 0F 2D/2C CVT(T)SS2SI   F2 0F 2D/2C CVT(T)SD2SI
        // Non-truncating float-to-integer conversions use MXCSR.RC.
        // Out-of-range and NaN inputs yield the integer indefinite value,
        // per the SDM. Unity's
        // startup (rsqrtps -> cvtss2sd -> movsd) and Mono's time/GC paths hit
        // these immediately.
        if ((p.rep == 0xF3 && (op2 == 0x5A || op2 == 0x2A || op2 == 0x2D || op2 == 0x2C)) ||
            (p.rep == 0xF2 && (op2 == 0x5A || op2 == 0x2A || op2 == 0x2D || op2 == 0x2C)) ||
            (p.rep == 0 && op2 == 0x5A)) {
            bool toInt = (op2 == 0x2D || op2 == 0x2C);
            bool trunc = (op2 == 0x2C);
            bool dbl = (p.rep == 0xF2) || (p.osize16 && op2 == 0x5A);
            bool i64 = (p.rex & 0x08) != 0;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
            auto fu32 = [](float f){ U32 b; std::memcpy(&b,&f,4); return b; };
            auto u64d = [](U64 b){ double d; std::memcpy(&d,&b,8); return d; };
            auto du64 = [](double d){ U64 b; std::memcpy(&b,&d,8); return b; };
            if (!toInt && op2 == 0x5A && p.rep == 0 && !p.osize16) {
                // CVTPS2PD: two packed singles -> two doubles.
                U64 sLo = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
                float b0 = u32f((U32)(sLo & 0xFFFFFFFFULL));
                float b1 = u32f((U32)(sLo >> 32));
                xmm[m.regField].lo = du64((double)b0);
                xmm[m.regField].hi = du64((double)b1);
            } else if (!toInt && op2 == 0x5A && p.rep == 0 && p.osize16) {
                // CVTPD2PS: two packed doubles -> two singles, upper zeroed.
                U64 sLo = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
                U64 sHi = m.isReg ? xmm[m.rmIndex].hi : memory->readq(m.effAddr + 8);
                float r0 = (float)u64d(sLo), r1 = (float)u64d(sHi);
                xmm[m.regField].lo = (U64)fu32(r0) | ((U64)fu32(r1) << 32);
                xmm[m.regField].hi = 0;
            } else if (!toInt && op2 == 0x5A && p.rep == 0xF3) {
                // CVTSS2SD: scalar single -> scalar double, upper preserved.
                U32 bits = m.isReg ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                                   : memory->readd(m.effAddr);
                xmm[m.regField].lo = du64((double)u32f(bits));
            } else if (!toInt && op2 == 0x5A && p.rep == 0xF2) {
                // CVTSD2SS: scalar double -> scalar single, upper preserved.
                U64 bits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
                float r = (float)u64d(bits);
                xmm[m.regField].lo = (xmm[m.regField].lo & 0xFFFFFFFF00000000ULL) | fu32(r);
            } else if (!toInt && op2 == 0x2A) {
                // CVTSI2SS / CVTSI2SD: integer -> scalar float/double.
                S64 iv = i64 ? (S64)(m.isReg ? reg[m.rmIndex].u64 : memory->readq(m.effAddr))
                             : (S64)(S32)(m.isReg ? reg[m.rmIndex].u32 : memory->readd(m.effAddr));
                if (dbl) {
                    xmm[m.regField].lo = du64((double)iv);
                } else {
                    float r = (float)iv;
                    xmm[m.regField].lo = (xmm[m.regField].lo & 0xFFFFFFFF00000000ULL) | fu32(r);
                }
            } else if (toInt) {
                // CVT(T)SS2SI / CVT(T)SD2SI: scalar float/double -> GP reg.
                double v;
                if (dbl) {
                    v = u64d(m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr));
                } else {
                    U32 bits = m.isReg ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                                       : memory->readd(m.effAddr);
                    v = (double)u32f(bits);
                }
                U64 converted = cpu64ConvertFpToInt(v, i64, trunc, mxcsr);
                if (i64) reg[m.regField].setU64(converted);
                else     reg[m.regField].setU32((U32)converted);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // 0F AB /r — BTS r/m, r       (set bit, return old in CF)
        // 0F B3 /r — BTR r/m, r       (reset bit)
        // 0F BB /r — BTC r/m, r       (complement bit)
        if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 rawIndex = (opSize == 8) ? reg[m.regField].u64 :
                           (opSize == 4) ? (U64)reg[m.regField].u32 :
                                           (U64)reg[m.regField].u16;
            ModRM bitOperand = m;
            U64 bit;
            if (m.isReg) {
                bit = rawIndex & (opSize * 8 - 1);
            } else {
                S64 index = opSize == 8 ? (S64)rawIndex :
                            opSize == 4 ? (S64)(S32)(U32)rawIndex :
                                          (S64)(S16)(U16)rawIndex;
                S64 bitsPerOperand = (S64)opSize * 8;
                S64 operandIndex = index / bitsPerOperand;
                S64 bitIndex = index % bitsPerOperand;
                if (bitIndex < 0) {
                    operandIndex--;
                    bitIndex += bitsPerOperand;
                }
                bitOperand.effAddr += (U64)(operandIndex * (S64)opSize);
                bit = (U64)bitIndex;
            }
            CPU64AtomicLock atomicLock(memory, bitOperand.effAddr, opSize, std::defer_lock);
            if (p.lock && op2 != 0xA3 && !m.isReg) atomicLock.lock();
            U64 v = loadRM(bitOperand, opSize, rexPresent);
            U64 mask = 1ULL << bit;
            bool old = (v & mask) != 0;
            rflags &= ~X64_CF;
            if (old) rflags |= X64_CF;
            if (op2 != 0xA3) {
                U64 nv = v;
                if (op2 == 0xAB) nv |= mask;
                else if (op2 == 0xB3) nv &= ~mask;
                else nv ^= mask; // BTC
                storeRM(bitOperand, opSize, nv, rexPresent);
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
                CPU64AtomicLock atomicLock(memory, m.effAddr, opSize, std::defer_lock);
                if (p.lock && sub != 4 && !m.isReg) atomicLock.lock();
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

        // F3 0F B8 — POPCNT r, r/m.
        if (op2 == 0xB8 && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            U64 result = __builtin_popcountll(src);
            if (opSize == 8) reg[m.regField].setU64(result);
            else if (opSize == 4) reg[m.regField].setU64((U32)result);
            else reg[m.regField].setU16((U16)result);
            rflags &= ~(X64_ZF | X64_CF | X64_OF | X64_SF | X64_PF | X64_AF);
            if (result == 0) rflags |= X64_ZF;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BC/BD remain BSF/BSR when F3 is present because ABM is not
        // advertised; on pre-ABM CPUs the prefix is ignored.
        // 0F BC — BSF r, r/m (bit scan forward); 0F BD — BSR (reverse).
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
            // XADD (LOCK XADD) backs glibc's atomic counters; serialize the
            // memory RMW across host threads.
            CPU64AtomicLock atomicLock(memory, m.effAddr, size, std::defer_lock);
            if (!m.isReg) atomicLock.lock();
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
dsp_40:
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
dsp_41:
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
dsp_42:
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
        // Compute before truncating: overflowing S64 multiplication is undefined
        // and lets native optimizers discard the guest's CF/OF overflow test.
        __int128 r128 = (__int128)sa * (__int128)imm;
        S64 r = (S64)r128;
        bool overflow = false;
        if (opSize == 2) overflow = (r != (S64)(S16)r);
        else if (opSize == 4) overflow = (r != (S64)(S32)r);
        else overflow = (r128 != (__int128)r);
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
dsp_43:
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
dsp_44:
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
dsp_45:
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
dsp_46:
    if (op == 0x9C) {
        push64((U64)rflags);
        rip += opOff + 1;
        return opOff + 1;
    }
dsp_47:
    if (op == 0x9D) {
        U64 v = pop64();
        const U32 writable = 0x00254FD5u; // CF PF AF ZF SF TF IF DF OF + others
        rflags = (rflags & ~writable) | ((U32)v & writable);
        rip += opOff + 1;
        return opOff + 1;
    }

    // SAHF (9E). Load the low 8 bits of rflags from AH. Only SF/ZF/AF/PF/CF
    // are user-visible in those bits per AMD64; bit 1 reads as 1.
dsp_48:
    if (op == 0x9E) {
        U8 ah = (U8)((reg[X64_RAX].u64 >> 8) & 0xFF);
        const U32 mask = X64_SF | X64_ZF | X64_AF | X64_PF | X64_CF;
        rflags = (rflags & ~mask) | ((U32)ah & mask);
        rip += opOff + 1;
        return opOff + 1;
    }

    // LAHF (9F). Store the low 8 bits of rflags (SF/ZF/0/AF/0/PF/1/CF) into
    // AH. Bit 1 is always 1, bits 3/5 are always 0.
dsp_49:
    if (op == 0x9F) {
        U8 v = (U8)(rflags & (X64_SF | X64_ZF | X64_AF | X64_PF | X64_CF));
        v |= 0x02; // reserved bit 1 reads as 1
        U64 rax = reg[X64_RAX].u64;
        rax = (rax & ~0xFF00ULL) | ((U64)v << 8);
        reg[X64_RAX].setU64(rax);
        rip += opOff + 1;
        return opOff + 1;
    }

    // INT3 (CC). Software-breakpoint. In user mode this raises SIGTRAP;
    // without real signal delivery, klog the trap and yield so the guest
    // exits cleanly instead of looping or corrupting host state. assert()
    // failure paths and debugger-injected breakpoints emit this.
dsp_50:
    if (op == 0xCC) {
        klog_fmt("CPU64: INT3 at RIP=0x%llx — yielding (no SIGTRAP delivery yet)",
                 (unsigned long long)rip);
        yield = true;
        rip += opOff + 1;
        return opOff + 1;
    }

    // CLD (FC) / STD (FD). Direction flag for string ops.
dsp_51:
    if (op == 0xFC) { rflags &= ~X64_DF; rip += opOff + 1; return opOff + 1; }
dsp_52:
    if (op == 0xFD) { rflags |=  X64_DF; rip += opOff + 1; return opOff + 1; }

    // CMC (F5) / CLC (F8) / STC (F9). Carry-flag toggle/clear/set.
dsp_53:
    if (op == 0xF5) { rflags ^= X64_CF; rip += opOff + 1; return opOff + 1; }
dsp_54:
    if (op == 0xF8) { rflags &= ~X64_CF; rip += opOff + 1; return opOff + 1; }
dsp_55:
    if (op == 0xF9) { rflags |=  X64_CF; rip += opOff + 1; return opOff + 1; }

    // CPUID (0F A2). Report the implemented baseline through SSE4.2/POPCNT;
    // AVX, BMI, and other unsupported extensions stay clear.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0xA2) {
        U32 leaf = reg[X64_RAX].u32;
        U32 sub  = reg[X64_RCX].u32;
        U32 a = 0, b = 0, c = 0, d = 0;
        switch (leaf) {
            case 0x0:
                a = 0x7; // max basic leaf
                // "Genu" "ineI" "ntel"
                b = 0x756E6547; d = 0x49656E69; c = 0x6C65746E;
                break;
            case 0x1: {
                a = (6 << 8) | (15 << 4) | 3; // family 6 stepping 3
                // EBX: eight 8-byte CLFLUSH units; one logical processor.
                b = (8u << 8) | (1u << 16);
                // ECX: SSE3, SSSE3, CX16, SSE4.1, SSE4.2, POPCNT.
                c = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) |
                    (1u << 20) | (1u << 23);
                // EDX bits: FPU=0 PDE=1 TSC=4 MSR=5 PAE=6 MCE=7 CX8=8
                // APIC=9 SEP=11 MTRR=12 PGE=13 MCA=14 CMOV=15 PAT=16
                // PSE36=17 CLFSH=19 MMX=23 FXSR=24 SSE=25 SSE2=26.
                d = (1u<<0)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<15)|(1u<<19)|
                    (1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
                break;
            }
            case 0x7:
                // Subleaf 0 and all later subleaves advertise no extensions.
                break;
            case 0x80000000:
                a = 0x80000001;
                break;
            case 0x80000001:
                // ECX: LAHF/SAHF in long mode and PREFETCHW.
                c = (1u << 0) | (1u << 8);
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
        if (op2 == 0x77 && !p.osize16 && p.rep == 0) {
            // EMMS: end MMX state and mark all x87 stack registers empty.
            fpu.SetTag(0xFFFFFFFF);
            fpu.isMMXInUse = false;
            U32 used = opOff + 2;
            rip += used;
            return used;
        }
        // Legacy MMX forms use the low 64-bit significands of the x87 stack
        // registers. startMMX() resets TOP to zero and marks all eight tags
        // valid; writing through getMMX() keeps FXSAVE/FNSAVE on the same state.
        if (p.rep == 0 && !osize66 &&
            (op2 == 0x6E || op2 == 0x6F || op2 == 0x7E || op2 == 0x7F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 used = opOff + 2 + m.length;
            bool wide = (p.rex & 8) != 0;
            if (op2 == 0x6E) { // MOVD/MOVQ r/m -> mm
                U64 value = loadRM(m, wide ? 8 : 4, rexPresent);
                fpu.startMMX();
                fpu.getMMX(m.regField & 7)->q = wide ? value : (U32)value;
            } else if (op2 == 0x7E) { // MOVD/MOVQ mm -> r/m
                fpu.startMMX();
                U64 value = fpu.getMMX(m.regField & 7)->q;
                if (m.isReg) {
                    if (wide) reg[m.rmIndex].setU64(value);
                    else reg[m.rmIndex].setU32((U32)value);
                } else {
                    storeRM(m, wide ? 8 : 4, wide ? value : (U32)value, rexPresent);
                }
            } else if (op2 == 0x6F) { // MOVQ r/m64 -> mm
                U64 value = m.isReg ? fpu.getMMX(m.rmIndex & 7)->q
                                    : memory->readq(m.effAddr);
                fpu.startMMX();
                fpu.getMMX(m.regField & 7)->q = value;
            } else { // MOVQ mm -> r/m64
                fpu.startMMX();
                U64 value = fpu.getMMX(m.regField & 7)->q;
                if (m.isReg) fpu.getMMX(m.rmIndex & 7)->q = value;
                else memory->writeq(m.effAddr, value);
            }
            rip += used;
            return used;
        }
        if ((p.rep == 0 && (op2 == 0x2A || op2 == 0x2C || op2 == 0x2D)) ||
            ((p.rep == 0xF3 || p.rep == 0xF2) && op2 == 0xD6)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 used = opOff + 2 + m.length;
            auto u32f = [](U32 b) { float f; std::memcpy(&f, &b, 4); return f; };
            auto fu32 = [](float f) { U32 b; std::memcpy(&b, &f, 4); return b; };
            auto u64d = [](U64 b) { double d; std::memcpy(&d, &b, 8); return d; };
            auto du64 = [](double d) { U64 b; std::memcpy(&b, &d, 8); return b; };
            if (op2 == 0x2A) { // CVTPI2PS/CVTPI2PD mm/m64 -> xmm
                U64 source = m.isReg ? fpu.getMMX(m.rmIndex & 7)->q : memory->readq(m.effAddr);
                fpu.startMMX();
                if (osize66) {
                    xmm[m.regField].lo = du64((double)(S32)(U32)source);
                    xmm[m.regField].hi = du64((double)(S32)(U32)(source >> 32));
                } else {
                    xmm[m.regField].lo = (U64)fu32((float)(S32)(U32)source) |
                                         ((U64)fu32((float)(S32)(U32)(source >> 32)) << 32);
                }
            } else if (op2 == 0x2C || op2 == 0x2D) { // CVTT/CVTPS2PI or CVTT/CVTPD2PI
                U64 src0, src1 = 0;
                if (osize66) {
                    if (m.isReg) { src0 = xmm[m.rmIndex].lo; src1 = xmm[m.rmIndex].hi; }
                    else { src0 = memory->readq(m.effAddr); src1 = memory->readq(m.effAddr + 8); }
                } else {
                    src0 = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
                }
                double a = osize66 ? u64d(src0) : (double)u32f((U32)src0);
                double b = osize66 ? u64d(src1) : (double)u32f((U32)(src0 >> 32));
                U32 low = (U32)cpu64ConvertFpToInt(a, false, op2 == 0x2C, mxcsr);
                U32 high = (U32)cpu64ConvertFpToInt(b, false, op2 == 0x2C, mxcsr);
                fpu.startMMX();
                fpu.getMMX(m.regField & 7)->q = (U64)low | ((U64)high << 32);
            } else if (p.rep == 0xF3) { // MOVQ2DQ mm -> xmm
                if (!m.isReg) goto unhandled;
                fpu.startMMX();
                xmm[m.regField].lo = fpu.getMMX(m.rmIndex & 7)->q;
                xmm[m.regField].hi = 0;
            } else { // MOVDQ2Q xmm -> mm
                if (!m.isReg) goto unhandled;
                U64 value = xmm[m.rmIndex].lo;
                fpu.startMMX();
                fpu.getMMX(m.regField & 7)->q = value;
            }
            rip += used;
            return used;
        }
        if (p.rep == 0 && !osize66 && op2 == 0x70) { // PSHUFW mm,mm/m64,imm8
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 src = m.isReg ? fpu.getMMX(m.rmIndex & 7)->q : memory->readq(m.effAddr);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U64 result = 0;
            for (int i = 0; i < 4; i++) {
                U16 word = (U16)(src >> (((imm >> (i * 2)) & 3) * 16));
                result |= (U64)word << (i * 16);
            }
            fpu.startMMX();
            fpu.getMMX(m.regField & 7)->q = result;
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        if (p.rep == 0 && !osize66 && op2 == 0xC4) { // PINSRW r/m16,imm8 -> mm
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U16 value = (U16)loadRM(m, 2, rexPresent);
            fpu.startMMX();
            MMX_reg* dst = fpu.getMMX(m.regField & 7);
            U32 shift = (imm & 3) * 16;
            dst->q = (dst->q & ~(0xFFFFULL << shift)) | ((U64)value << shift);
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        if (p.rep == 0 && !osize66 && op2 == 0xC5) { // PEXTRW mm,imm8 -> r32
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            if (!m.isReg) goto unhandled;
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            fpu.startMMX();
            U16 value = (U16)(fpu.getMMX(m.rmIndex & 7)->q >> ((imm & 3) * 16));
            reg[m.regField].setU32(value);
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        if (p.rep == 0 && !osize66 && op2 == 0xD7) { // PMOVMSKB r32,mm
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) goto unhandled;
            fpu.startMMX();
            U64 src = fpu.getMMX(m.rmIndex & 7)->q;
            U32 mask = 0;
            for (int i = 0; i < 8; i++) mask |= (U32)((src >> (i * 8 + 7)) & 1) << i;
            reg[m.regField].setU32(mask);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (p.rep == 0 && !osize66 && op2 == 0xE7) { // MOVNTQ m64,mm
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) goto unhandled;
            fpu.startMMX();
            memory->writeq(m.effAddr, fpu.getMMX(m.regField & 7)->q);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (p.rep == 0 && !osize66 && op2 == 0xF7) { // MASKMOVQ mm,mm -> [RDI]
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) goto unhandled;
            fpu.startMMX();
            U64 data = fpu.getMMX(m.regField & 7)->q;
            U64 mask = fpu.getMMX(m.rmIndex & 7)->q;
            U64 addr = reg[X64_RDI].u64;
            for (U32 i = 0; i < 8; i++) {
                U8 maskByte = (U8)(mask >> (i * 8));
                if (maskByte & 0x80) memory->writeb(addr + i, (U8)(data >> (i * 8)));
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        if (p.rep == 0 && !osize66 && (op2 == 0x71 || op2 == 0x72 || op2 == 0x73)) {
            // MMX immediate shifts: /2 logical right, /4 arithmetic right,
            // /6 logical left. The immediate follows any ModRM addressing bytes.
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            if (!m.isReg || (m.regField != 2 && m.regField != 4 && m.regField != 6) ||
                (op2 == 0x73 && m.regField != 2 && m.regField != 6)) goto unhandled;
            U8 count = fetchByte(rip + opOff + 2 + m.length);
            U32 width = op2 == 0x71 ? 16 : op2 == 0x72 ? 32 : 64;
            U64 mask = width == 64 ? ~0ULL : ((1ULL << width) - 1);
            U64 value = fpu.getMMX(m.rmIndex & 7)->q;
            U64 shifted = 0;
            for (U32 lane = 0; lane < 64 / width; lane++) {
                U64 element = (value >> (lane * width)) & mask;
                U64 out = 0;
                if (m.regField == 2) out = count >= width ? 0 : element >> count;
                else if (m.regField == 6) out = count >= width ? 0 : (element << count) & mask;
                else {
                    S64 signedValue = width == 16 ? (S16)element : (S32)element;
                    out = count >= width ? (signedValue < 0 ? mask : 0)
                                         : ((U64)(signedValue >> count) & mask);
                }
                shifted |= out << (lane * width);
            }
            value = shifted;
            fpu.startMMX();
            fpu.getMMX(m.rmIndex & 7)->q = value;
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }

        // The MMX packed integer arithmetic opcodes parallel the 128-bit
        // SSE2 handlers below but operate on one 64-bit lane group.
        bool mmxPacked = (op2 >= 0x60 && op2 <= 0x6B) ||
                         (op2 >= 0x74 && op2 <= 0x76) ||
                         (op2 >= 0xD1 && op2 <= 0xD5) ||
                         (op2 >= 0xD8 && op2 <= 0xDF) ||
                         (op2 >= 0xE0 && op2 <= 0xE5) ||
                         (op2 >= 0xE8 && op2 <= 0xEF) ||
                         (op2 >= 0xF1 && op2 <= 0xF6) ||
                         (op2 >= 0xF8 && op2 <= 0xFE) ||
                         op2 == 0xFC || op2 == 0xFD;
        if (p.rep == 0 && !osize66 && mmxPacked) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 d = fpu.getMMX(m.regField & 7)->q;
            U64 s = m.isReg ? fpu.getMMX(m.rmIndex & 7)->q : memory->readq(m.effAddr);
            fpu.startMMX();
            int bits = 0;
            if (op2 == 0x60 || op2 == 0x64 || op2 == 0x74 || op2 == 0xD8 || op2 == 0xDA ||
                op2 == 0xDC || op2 == 0xDE || op2 == 0xE0 || op2 == 0xE8 || op2 == 0xEC ||
                op2 == 0xF8 || op2 == 0xFC) bits = 8;
            else if (op2 == 0x61 || op2 == 0x63 || op2 == 0x65 || op2 == 0x67 || op2 == 0x68 ||
                     op2 == 0x69 || op2 == 0x6B || op2 == 0x75 || op2 == 0xD1 || op2 == 0xD5 ||
                     op2 == 0xD9 || op2 == 0xDD || op2 == 0xE3 || op2 == 0xE4 || op2 == 0xE5 ||
                     op2 == 0xE9 || op2 == 0xEA || op2 == 0xED || op2 == 0xEE || op2 == 0xF1 ||
                     op2 == 0xF5 || op2 == 0xFD || op2 == 0xE1 || op2 == 0xE9 ||
                     op2 == 0xED || op2 == 0xF9) bits = 16;
            else if (op2 == 0x62 || op2 == 0x66 || op2 == 0x6A || op2 == 0x76 || op2 == 0xD2 ||
                     op2 == 0xE2 || op2 == 0xF2 || op2 == 0xFE || op2 == 0xFA) bits = 32;
            else if (op2 == 0xD3 || op2 == 0xD4 || op2 == 0xF3 || op2 == 0xFB) bits = 64;
            auto laneMask = [](int width) -> U64 { return width == 64 ? ~0ULL : ((1ULL << width) - 1); };
            auto signedLane = [](U64 value, int width) -> S64 {
                if (width == 8) return (S8)value;
                if (width == 16) return (S16)value;
                if (width == 32) return (S32)value;
                return (S64)value;
            };
            auto mapLanes = [&](int width, auto fn) -> U64 {
                U64 out = 0, mask = laneMask(width);
                for (int i = 0; i < 64 / width; i++) {
                    U64 a = (d >> (i * width)) & mask;
                    U64 b = (s >> (i * width)) & mask;
                    out |= (fn(a, b) & mask) << (i * width);
                }
                return out;
            };
            U64 result = d;
            if (op2 >= 0x60 && op2 <= 0x62) {
                int width = op2 == 0x60 ? 8 : op2 == 0x61 ? 16 : 32;
                int lanes = 64 / width, half = lanes / 2;
                result = 0;
                for (int i = 0; i < half; i++) {
                    U64 a = (d >> (i * width)) & laneMask(width);
                    U64 b = (s >> (i * width)) & laneMask(width);
                    result &= ~(laneMask(width) << ((i * 2) * width));
                    result |= a << ((i * 2) * width);
                    result |= b << ((i * 2 + 1) * width);
                }
            } else if (op2 >= 0x68 && op2 <= 0x6A) {
                int width = op2 == 0x68 ? 8 : op2 == 0x69 ? 16 : 32;
                int half = 32 / width;
                result = 0;
                for (int i = 0; i < half; i++) {
                    U64 a = (d >> ((i + half) * width)) & laneMask(width);
                    U64 b = (s >> ((i + half) * width)) & laneMask(width);
                    result |= a << ((i * 2) * width);
                    result |= b << ((i * 2 + 1) * width);
                }
            } else if (op2 == 0x63 || op2 == 0x67 || op2 == 0x6B) { // PACK*
                result = 0;
                int srcWidth = op2 == 0x6B ? 32 : 16;
                int dstWidth = op2 == 0x6B ? 16 : 8;
                U64 dstMask = laneMask(dstWidth);
                int srcLanes = srcWidth == 32 ? 2 : 4;
                for (int i = 0; i < srcLanes; i++) {
                    S64 v = signedLane((d >> (i * srcWidth)) & laneMask(srcWidth), srcWidth);
                    if (op2 == 0x67) v = std::max<S64>(0, std::min<S64>(255, v));
                    else v = std::max<S64>(-(1LL << (dstWidth - 1)),
                                            std::min<S64>((1LL << (dstWidth - 1)) - 1, v));
                    result |= ((U64)v & dstMask) << (i * dstWidth);
                }
                for (int i = 0; i < srcLanes; i++) {
                    S64 v = signedLane((s >> (i * srcWidth)) & laneMask(srcWidth), srcWidth);
                    if (op2 == 0x67) v = std::max<S64>(0, std::min<S64>(255, v));
                    else v = std::max<S64>(-(1LL << (dstWidth - 1)),
                                            std::min<S64>((1LL << (dstWidth - 1)) - 1, v));
                    result |= ((U64)v & dstMask) << ((i + (dstWidth == 16 ? 2 : 4)) * dstWidth);
                }
            } else if (op2 == 0x64 || op2 == 0x65 || op2 == 0x66) { // PCMPGT*
                result = mapLanes(bits, [&](U64 a, U64 b) -> U64 {
                    return signedLane(a, bits) > signedLane(b, bits) ? laneMask(bits) : 0;
                });
            } else if (op2 == 0x74 || op2 == 0x75 || op2 == 0x76) { // PCMPEQ*
                result = mapLanes(bits, [&](U64 a, U64 b) -> U64 { return a == b ? laneMask(bits) : 0; });
            } else if (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
                       op2 == 0xE1 || op2 == 0xE2 || op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3) {
                U64 count = s;
                bool left = op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3;
                bool arithmetic = op2 == 0xE1 || op2 == 0xE2;
                result = mapLanes(bits, [&](U64 a, U64) -> U64 {
                    if (count >= (U64)bits) return arithmetic && signedLane(a, bits) < 0 ? laneMask(bits) : 0;
                    if (left) return (a << count) & laneMask(bits);
                    if (arithmetic) return (U64)(signedLane(a, bits) >> count) & laneMask(bits);
                    return a >> count;
                });
            } else if (op2 == 0xD4 || op2 == 0xDB || op2 == 0xDF || op2 == 0xEB || op2 == 0xEF || op2 == 0xFB) {
                if (op2 == 0xD4) result = d + s; // PADDQ
                else if (op2 == 0xFB) result = d - s; // PSUBQ
                else if (op2 == 0xDB) result = d & s;
                else if (op2 == 0xDF) result = ~d & s;
                else if (op2 == 0xEB) result = d | s;
                else result = d ^ s;
            } else if (op2 == 0xD5) { // PMULLW
                result = mapLanes(16, [](U64 a, U64 b) -> U64 { return a * b; });
            } else if (op2 == 0xF4) { // PMULUDQ
                result = (U64)(U32)d * (U64)(U32)s;
            } else if (op2 == 0xF5) { // PMADDWD
                S64 a0 = (S16)d, a1 = (S16)(d >> 16);
                S64 a2 = (S16)(d >> 32), a3 = (S16)(d >> 48);
                S64 b0 = (S16)s, b1 = (S16)(s >> 16);
                S64 b2 = (S16)(s >> 32), b3 = (S16)(s >> 48);
                result = (U32)(a0*b0 + a1*b1) |
                         ((U64)(U32)(a2*b2 + a3*b3) << 32);
            } else if (op2 == 0xE4 || op2 == 0xE5) { // PMULHUW/HW
                result = mapLanes(16, [&](U64 a, U64 b) -> U64 {
                    S64 product = op2 == 0xE4 ? (S64)(U16)a * (U16)b : (S16)a * (S16)b;
                    return (U64)product >> 16;
                });
            } else if (op2 == 0xF6) { // PSADBW
                U32 sum = 0;
                for (int i = 0; i < 8; i++) {
                    int a = (U8)(d >> (i * 8)), b = (U8)(s >> (i * 8));
                    sum += (U32)(a > b ? a - b : b - a);
                }
                result = sum;
            } else if (op2 == 0xE0 || op2 == 0xE3) { // PAVG*
                result = mapLanes(bits, [](U64 a, U64 b) -> U64 { return (a + b + 1) >> 1; });
            } else if (op2 == 0xDA || op2 == 0xDE || op2 == 0xEA || op2 == 0xEE) { // PMIN/MAX*
                bool isWord = op2 == 0xEA || op2 == 0xEE;
                bool isMax = op2 == 0xDE || op2 == 0xEE;
                result = mapLanes(isWord ? 16 : 8, [&](U64 a, U64 b) -> U64 {
                    if (isWord) {
                        S64 x = (S16)a, y = (S16)b;
                        return isMax ? (x > y ? a : b) : (x < y ? a : b);
                    }
                    return isMax ? (a > b ? a : b) : (a < b ? a : b);
                });
            } else if (op2 == 0xD8 || op2 == 0xD9 || op2 == 0xDC || op2 == 0xDD ||
                       op2 == 0xE8 || op2 == 0xE9 || op2 == 0xEC || op2 == 0xED) {
                bool word = op2 == 0xD9 || op2 == 0xDD || op2 == 0xE9 || op2 == 0xED;
                bool subtract = op2 == 0xD8 || op2 == 0xD9 || op2 == 0xE8 || op2 == 0xE9;
                bool signedSat = op2 >= 0xE8;
                int width = word ? 16 : 8;
                result = mapLanes(width, [&](U64 a, U64 b) -> U64 {
                    if (signedSat) {
                        S64 x = signedLane(a, width), y = signedLane(b, width);
                        S64 lo = -(1LL << (width - 1)), hi = (1LL << (width - 1)) - 1;
                        S64 value = subtract ? x - y : x + y;
                        return (U64)std::max(lo, std::min(hi, value));
                    }
                    U64 hi = laneMask(width);
                    if (subtract) return a > b ? a - b : 0;
                    U64 value = a + b;
                    return value > hi ? hi : value;
                });
            } else if (op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA || op2 == 0xFC || op2 == 0xFD || op2 == 0xFE) {
                bool subtract = op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA;
                result = mapLanes(bits, [&](U64 a, U64 b) -> U64 { return subtract ? a - b : a + b; });
            }
            fpu.getMMX(m.regField & 7)->q = result;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x2B && p.rep == 0) {
            // MOVNTPS / MOVNTPD are memory-only non-temporal 128-bit stores.
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) goto unhandled;
            memory->writeq(m.effAddr, xmm[m.regField].lo);
            memory->writeq(m.effAddr + 8, xmm[m.regField].hi);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0xF7 && osize66 && p.rep == 0) {
            // MASKMOVDQU xmm1,xmm2: store selected data bytes to [RDI].
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) goto unhandled;
            U64 data[2] = {xmm[m.regField].lo, xmm[m.regField].hi};
            U64 mask[2] = {xmm[m.rmIndex].lo, xmm[m.rmIndex].hi};
            U64 addr = reg[X64_RDI].u64;
            for (U32 i = 0; i < 16; i++) {
                U8 maskByte = (U8)(mask[i / 8] >> ((i % 8) * 8));
                if (maskByte & 0x80)
                    memory->writeb(addr + i, (U8)(data[i / 8] >> ((i % 8) * 8)));
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
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
        // XORPS xmm, xmm/m128 — 0F 57 /r (no prefix). Float-typed bitwise
        // XOR; bit pattern identical to PXOR. Discovered in musl printf
        // init — used to zero XMM0 before a varargs scalar-FP write.
        // XORPD xmm, xmm/m128 — 66 0F 57 /r. Same bit op, double-typed.
        // Packed bitwise-logical float ops — all operate on the full 128 bits;
        // PS vs PD differ only in the 0x66 prefix, irrelevant to a bit op:
        //   0F 54 ANDPS/ANDPD    dst &=  src
        //   0F 55 ANDNPS/ANDNPD  dst = ~dst & src
        //   0F 56 ORPS/ORPD      dst |=  src
        //   0F 57 XORPS/XORPD    dst ^=  src   ;  66 0F EF PXOR (same op)
        // glibc __printf_fp uses ANDPD to mask the sign bit (fabs) for %f.
        if (op2 == 0x54 || op2 == 0x55 || op2 == 0x56 || op2 == 0x57 ||
            (op2 == 0xEF && osize66)) {
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
            switch (op2) {
                case 0x54: xmm[m.regField].lo = dLo & srcLo;  xmm[m.regField].hi = dHi & srcHi;  break;
                case 0x55: xmm[m.regField].lo = ~dLo & srcLo; xmm[m.regField].hi = ~dHi & srcHi; break;
                case 0x56: xmm[m.regField].lo = dLo | srcLo;  xmm[m.regField].hi = dHi | srcHi;  break;
                default:   xmm[m.regField].lo = dLo ^ srcLo;  xmm[m.regField].hi = dHi ^ srcHi;  break;
            }
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
        // PSHUFLW xmm, xmm/m128, imm8 — F2 0F 70 /r ib. Shuffles the four
        // low words (lo qword) per imm8; copies hi qword through unchanged.
        // PSHUFHW xmm, xmm/m128, imm8 — F3 0F 70 /r ib. Same but for hi.
        // Discovered in musl strchr/strlen path — the byte-search uses
        // PSHUFLW to broadcast a search byte across the low half of an XMM
        // register before a 16-byte PCMPEQB.
        if (op2 == 0x70 && (p.rep == 0xF2 || p.rep == 0xF3)) {
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
            if (p.rep == 0xF2) {
                // PSHUFLW: shuffle low 4 words from srcLo by imm; hi passes through.
                U16 w[4] = {
                    (U16)(srcLo & 0xFFFF),
                    (U16)((srcLo >> 16) & 0xFFFF),
                    (U16)((srcLo >> 32) & 0xFFFF),
                    (U16)((srcLo >> 48) & 0xFFFF)
                };
                U16 out[4];
                for (int i = 0; i < 4; i++) out[i] = w[(imm >> (i * 2)) & 0x3];
                xmm[m.regField].lo = (U64)out[0] | ((U64)out[1] << 16)
                                   | ((U64)out[2] << 32) | ((U64)out[3] << 48);
                xmm[m.regField].hi = srcHi;
            } else {
                // PSHUFHW: shuffle high 4 words from srcHi by imm; lo passes through.
                U16 w[4] = {
                    (U16)(srcHi & 0xFFFF),
                    (U16)((srcHi >> 16) & 0xFFFF),
                    (U16)((srcHi >> 32) & 0xFFFF),
                    (U16)((srcHi >> 48) & 0xFFFF)
                };
                U16 out[4];
                for (int i = 0; i < 4; i++) out[i] = w[(imm >> (i * 2)) & 0x3];
                xmm[m.regField].lo = srcLo;
                xmm[m.regField].hi = (U64)out[0] | ((U64)out[1] << 16)
                                   | ((U64)out[2] << 32) | ((U64)out[3] << 48);
            }
            U32 used = opOff + 2 + m.length + 1;
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
            // Opcode map (PUNPCK): LOW forms take the low halves of each
            // operand, HIGH forms the high halves.
            //   LOW : 60=LBW 61=LWD 62=LDQ 6C=LQDQ
            //   HIGH: 68=HBW 69=HWD 6A=HDQ 6D=HQDQ
            // NOTE 0x6C (PUNPCKLQDQ) is numerically >= 0x68 but is a LOW
            // form — classifying by ">= 0x68" wrongly buckets it as HIGH and
            // reads the (often-zero) high halves, which silently corrupted
            // glibc's malloc bin self-pointers. Classify explicitly.
            bool isHigh = (op2 == 0x68 || op2 == 0x69 || op2 == 0x6A || op2 == 0x6D);
            U64 dSrc = isHigh ? dHi : dLo;
            U64 sSrc = isHigh ? srcHi : srcLo;
            U64 oLo = 0, oHi = 0;
            // Map opcode -> element-width index: 0=byte 1=word 2=dword 4=qword.
            U8 sub;
            switch (op2) {
                case 0x60: case 0x68: sub = 0; break; // byte
                case 0x61: case 0x69: sub = 1; break; // word
                case 0x62: case 0x6A: sub = 2; break; // dword
                default:              sub = 4; break; // 6C/6D qword
            }
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
        // Saturated packed byte/word add/sub — 66 0F D8/D9/DC/DD/E8/E9/EC/ED.
        // Gecko's xul.dll uses PSUBUSB while laying out its first document.
        if ((op2 == 0xD8 || op2 == 0xD9 || op2 == 0xDC || op2 == 0xDD ||
             op2 == 0xE8 || op2 == 0xE9 || op2 == 0xEC || op2 == 0xED) && osize66) {
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
            bool word = (op2 == 0xD9 || op2 == 0xDD || op2 == 0xE9 || op2 == 0xED);
            bool sub = (op2 == 0xD8 || op2 == 0xD9 || op2 == 0xE8 || op2 == 0xE9);
            bool sgn = (op2 == 0xE8 || op2 == 0xE9 || op2 == 0xEC || op2 == 0xED);
            U64 oLo = 0, oHi = 0;
            int lanes = word ? 4 : 8;
            int width = word ? 16 : 8;
            for (int i = 0; i < lanes; i++) {
                U32 ua = (U32)((dLo >> (i*width)) & (word ? 0xFFFF : 0xFF));
                U32 ub = (U32)((srcLo >> (i*width)) & (word ? 0xFFFF : 0xFF));
                U32 uc = (U32)((dHi >> (i*width)) & (word ? 0xFFFF : 0xFF));
                U32 ud = (U32)((srcHi >> (i*width)) & (word ? 0xFFFF : 0xFF));
                U32 rLo, rHi;
                if (sgn) {
                    S32 a = word ? (S32)(S16)ua : (S32)(S8)ua;
                    S32 b = word ? (S32)(S16)ub : (S32)(S8)ub;
                    S32 c = word ? (S32)(S16)uc : (S32)(S8)uc;
                    S32 d = word ? (S32)(S16)ud : (S32)(S8)ud;
                    S32 maxV = word ? 32767 : 127;
                    S32 minV = word ? -32768 : -128;
                    S32 vLo = sub ? a - b : a + b;
                    S32 vHi = sub ? c - d : c + d;
                    if (vLo < minV) vLo = minV;
                    if (vLo > maxV) vLo = maxV;
                    if (vHi < minV) vHi = minV;
                    if (vHi > maxV) vHi = maxV;
                    rLo = (U32)(vLo & (word ? 0xFFFF : 0xFF));
                    rHi = (U32)(vHi & (word ? 0xFFFF : 0xFF));
                } else if (sub) {
                    rLo = ua > ub ? ua - ub : 0;
                    rHi = uc > ud ? uc - ud : 0;
                } else {
                    U32 vLo = ua + ub, vHi = uc + ud;
                    U32 cap = word ? 0xFFFF : 0xFF;
                    rLo = vLo > cap ? cap : vLo;
                    rHi = vHi > cap ? cap : vHi;
                }
                oLo |= (U64)rLo << (i*width);
                oHi |= (U64)rHi << (i*width);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
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
        // PMADDWD — 66 0F F5 /r. Multiply signed word pairs and add each
        // adjacent pair into a wrapping signed dword result.
        if (op2 == 0xF5 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dst[2] = {xmm[m.regField].lo, xmm[m.regField].hi};
            U64 src[2] = {srcLo, srcHi};
            U32 result[4];
            for (int pair = 0; pair < 4; pair++) {
                int half = pair / 2;
                int lane = (pair % 2) * 2;
                S16 a = (S16)(dst[half] >> (lane * 16));
                S16 b = (S16)(dst[half] >> ((lane + 1) * 16));
                S16 c = (S16)(src[half] >> (lane * 16));
                S16 d = (S16)(src[half] >> ((lane + 1) * 16));
                S64 sum = (S64)a * c + (S64)b * d;
                result[pair] = (U32)sum;
            }
            xmm[m.regField].lo = (U64)result[0] | ((U64)result[1] << 32);
            xmm[m.regField].hi = (U64)result[2] | ((U64)result[3] << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PAVGW — 66 0F E3 /r. Unsigned word average rounded toward +infinity.
        if (op2 == 0xE3 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dst[2] = {xmm[m.regField].lo, xmm[m.regField].hi};
            U64 src[2] = {srcLo, srcHi};
            for (int half = 0; half < 2; half++) {
                U64 result = 0;
                for (int lane = 0; lane < 4; lane++) {
                    U32 a = (dst[half] >> (lane * 16)) & 0xFFFF;
                    U32 b = (src[half] >> (lane * 16)) & 0xFFFF;
                    result |= (U64)((a + b + 1) >> 1) << (lane * 16);
                }
                if (half == 0) xmm[m.regField].lo = result;
                else xmm[m.regField].hi = result;
            }
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
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
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
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
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
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
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
        // 0F 01 — group containing XGETBV / RDTSCP / SWAPGS / etc.
        // The third byte distinguishes the form. We implement the two
        // that user-space glibc actually issues:
        //   0F 01 D0   XGETBV   — read XSAVE feature mask into EDX:EAX
        //   0F 01 F9   RDTSCP   — read TSC and processor ID
        if (op2 == 0x01) {
            U8 op3 = fetchByte(rip + opOff + 2);
            if (op3 == 0xC8 || op3 == 0xC9) {
                // MONITOR/MWAIT are user-mode hints with no modeled monitor
                // state; MWAIT follows PAUSE's harmless no-op behavior here.
                U32 used = opOff + 3;
                rip += used;
                return used;
            }
            if (op3 == 0xD0) {
                // XGETBV: glibc calls it via __get_cpu_features to decide
                // which XMM state is enabled. We advertise legacy XSAVE
                // state only: bit0=x87, bit1=SSE → ECX selector 0 returns
                // EDX:EAX = 0:0x3.
                if (reg[X64_RCX].u32 == 0) {
                    reg[X64_RAX].setU64(0x3);
                    reg[X64_RDX].setU64(0);
                } else {
                    // Unknown XCR — return zero (legacy behaviour).
                    reg[X64_RAX].setU64(0);
                    reg[X64_RDX].setU64(0);
                }
                U32 used = opOff + 3;
                rip += used;
                return used;
            }
            if (op3 == 0xF9) {
                // RDTSCP: same as RDTSC plus aux ID in ECX. Reuse the
                // TSC counter our RDTSC path uses (instructionCount-based
                // monotonic synthetic clock); ECX = 0 (CPU 0).
                U64 tsc = (U64)instructionCount;
                reg[X64_RAX].setU64(tsc & 0xFFFFFFFFULL);
                reg[X64_RDX].setU64((tsc >> 32) & 0xFFFFFFFFULL);
                reg[X64_RCX].setU64(0);
                U32 used = opOff + 3;
                rip += used;
                return used;
            }
            // Fall through to default panic for unimplemented 0F 01 .. forms.
        }

        // PREFETCH* (0F 18) and AMD PREFETCH/PREFETCHW (0F 0D).
        // Cache hints only: decode the full address but do not read memory.
        // Unity's player emits PREFETCHW [rbx+0x14] during startup.
        if (op2 == 0x18 || op2 == 0x0D) {
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

        // ---- SSSE3 three-byte opcodes ----
        //
        // 66 0F 38 00 /r  — PSHUFB  xmm1, xmm2/m128
        //   For each byte i of the destination: if shuffle[i] high bit set,
        //   write 0; else write src[shuffle[i] & 0x0F]. Used by glibc's
        //   SSSE3-optimised strlen/strcmp/memcpy.
        //
        // 66 0F 3A 0F /r ib — PALIGNR xmm1, xmm2/m128, imm8
        //   Concatenate dest:src into a 32-byte temp, then right-shift it
        //   by imm8 bytes, take the low 16 bytes. Used by some glibc
        //   memcpy variants and Wine's CRT.
        if ((op2 == 0x38 || op2 == 0x3A) && p.rep == 0 && !osize66) {
            U8 op3 = fetchByte(rip + opOff + 2);
            bool palignr = op2 == 0x3A && op3 == 0x0F;
            bool packed = op2 == 0x38 &&
                (op3 == 0x00 || (op3 >= 0x01 && op3 <= 0x0B) ||
                 (op3 >= 0x1C && op3 <= 0x1E));
            if (palignr || packed) {
                ModRM m = decodeModRM(rip + opOff + 3, p, palignr ? 1 : 0);
                U64 d = fpu.getMMX(m.regField & 7)->q;
                U64 s = m.isReg ? fpu.getMMX(m.rmIndex & 7)->q : memory->readq(m.effAddr);
                U64 result = 0;
                if (palignr) {
                    U8 imm = fetchByte(rip + opOff + 3 + m.length);
                    if (imm < 16) {
                        for (U32 i = 0; i < 8; i++) {
                            U32 idx = imm + i;
                            U8 value = idx < 8 ? (U8)(s >> (idx * 8))
                                     : idx < 16 ? (U8)(d >> ((idx - 8) * 8)) : 0;
                            result |= (U64)value << (i * 8);
                        }
                    }
                } else {
                    U8 db[8], sb[8], out[8] = {};
                    for (U32 i = 0; i < 8; i++) {
                        db[i] = (U8)(d >> (i * 8));
                        sb[i] = (U8)(s >> (i * 8));
                    }
                    auto sat16 = [](S64 x) -> U16 {
                        if (x < -32768) x = -32768;
                        if (x > 32767) x = 32767;
                        return (U16)x;
                    };
                    if (op3 == 0x00) { // PSHUFB
                        for (U32 i = 0; i < 8; i++)
                            out[i] = (sb[i] & 0x80) ? 0 : db[sb[i] & 7];
                    } else if (op3 == 0x1C) { // PABSB
                        for (U32 i = 0; i < 8; i++) {
                            S32 x = (S8)sb[i]; out[i] = (U8)(x < 0 ? -x : x);
                        }
                    } else if (op3 == 0x1D) { // PABSW
                        for (U32 i = 0; i < 4; i++) {
                            S32 x = (S16)(U16)(s >> (i * 16));
                            U16 v = (U16)(x < 0 ? -x : x);
                            result |= (U64)v << (i * 16);
                        }
                    } else if (op3 == 0x1E) { // PABSD
                        for (U32 i = 0; i < 2; i++) {
                            S64 x = (S32)(U32)(s >> (i * 32));
                            U32 v = (U32)(x < 0 ? -x : x);
                            result |= (U64)v << (i * 32);
                        }
                    } else if (op3 == 0x01 || op3 == 0x03 || op3 == 0x05 || op3 == 0x07) {
                        U16 r[4] = {};
                        for (U32 pair = 0; pair < 2; pair++) {
                            S64 da = (S16)(U16)(d >> (pair * 32));
                            S64 dbb = (S16)(U16)(d >> (pair * 32 + 16));
                            S64 sa = (S16)(U16)(s >> (pair * 32));
                            S64 sbb = (S16)(U16)(s >> (pair * 32 + 16));
                            if (op3 == 0x01) {
                                r[pair] = (U16)((U16)da + (U16)dbb);
                                r[pair + 2] = (U16)((U16)sa + (U16)sbb);
                            } else if (op3 == 0x03) {
                                r[pair] = sat16(da + dbb); r[pair + 2] = sat16(sa + sbb);
                            } else if (op3 == 0x05) {
                                r[pair] = (U16)((U16)da - (U16)dbb);
                                r[pair + 2] = (U16)((U16)sa - (U16)sbb);
                            } else {
                                r[pair] = sat16(da - dbb); r[pair + 2] = sat16(sa - sbb);
                            }
                        }
                        for (U32 i = 0; i < 4; i++) result |= (U64)r[i] << (i * 16);
                    } else if (op3 == 0x02 || op3 == 0x06) {
                        U32 d0 = (U32)d, d1 = (U32)(d >> 32);
                        U32 s0 = (U32)s, s1 = (U32)(s >> 32);
                        U32 lo = op3 == 0x02 ? d0 + d1 : d0 - d1;
                        U32 hi = op3 == 0x02 ? s0 + s1 : s0 - s1;
                        result = (U64)lo | ((U64)hi << 32);
                    } else if (op3 == 0x04) { // PMADDUBSW
                        for (U32 i = 0; i < 4; i++) {
                            S64 sum = (S64)db[i * 2] * (S8)sb[i * 2] +
                                      (S64)db[i * 2 + 1] * (S8)sb[i * 2 + 1];
                            result |= (U64)sat16(sum) << (i * 16);
                        }
                    } else if (op3 == 0x08) { // PSIGNB
                        for (U32 i = 0; i < 8; i++) {
                            S64 x = (S8)db[i], sign = (S8)sb[i];
                            out[i] = (U8)(sign < 0 ? -x : sign == 0 ? 0 : x);
                        }
                    } else if (op3 == 0x09 || op3 == 0x0B) { // PSIGNW / PMULHRSW
                        for (U32 i = 0; i < 4; i++) {
                            S64 a = (S16)(U16)(d >> (i * 16));
                            S64 b = (S16)(U16)(s >> (i * 16));
                            U16 value;
                            if (op3 == 0x09) value = (U16)(b < 0 ? -a : b == 0 ? 0 : a);
                            else value = (U16)(((a * b + 0x4000) >> 15));
                            result |= (U64)value << (i * 16);
                        }
                    } else if (op3 == 0x0A) { // PSIGND
                        for (U32 i = 0; i < 2; i++) {
                            S64 a = (S32)(U32)(d >> (i * 32));
                            S64 b = (S32)(U32)(s >> (i * 32));
                            result |= (U64)(U32)(b < 0 ? -a : b == 0 ? 0 : a) << (i * 32);
                        }
                    }
                    if (op3 == 0x00 || op3 == 0x08 || op3 == 0x1C) {
                        for (U32 i = 0; i < 8; i++) result |= (U64)out[i] << (i * 8);
                    }
                }
                fpu.startMMX();
                fpu.getMMX(m.regField & 7)->q = result;
                U32 used = opOff + 3 + m.length + (palignr ? 1 : 0);
                rip += used;
                return used;
            }
        }

        if (p.rep == 0xF2 && op2 == 0x38) {
            U8 op3 = fetchByte(rip + opOff + 2);
            if (op3 == 0xF0 || op3 == 0xF1) { // CRC32 r32/r64, r/m8/16/32/64
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U32 sourceBytes = op3 == 0xF0 ? 1 : rexW ? 8 : osize66 ? 2 : 4;
                U64 source = loadRM(m, sourceBytes, rexPresent);
                U32 crc = cpu64Crc32c(reg[m.regField].u32, source, sourceBytes);
                if (rexW) reg[m.regField].setU64(crc);
                else      reg[m.regField].setU32(crc);
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
        }

        if (osize66 && p.rep == 0 && op2 == 0x38 &&
            fetchByte(rip + opOff + 2) == 0x37) { // PCMPGTQ
            ModRM m = decodeModRM(rip + opOff + 3, p, 0);
            U64 sourceLo, sourceHi;
            if (m.isReg) {
                sourceLo = xmm[m.rmIndex].lo;
                sourceHi = xmm[m.rmIndex].hi;
            } else {
                sourceLo = memory->readq(m.effAddr);
                sourceHi = memory->readq(m.effAddr + 8);
            }
            U64 destLo = xmm[m.regField].lo;
            U64 destHi = xmm[m.regField].hi;
            xmm[m.regField].lo = (S64)destLo > (S64)sourceLo ? ~0ULL : 0;
            xmm[m.regField].hi = (S64)destHi > (S64)sourceHi ? ~0ULL : 0;
            U32 used = opOff + 3 + m.length;
            rip += used;
            return used;
        }

        if (osize66 && p.rep == 0 && op2 == 0x3A) {
            U8 op3 = fetchByte(rip + opOff + 2);
            if (op3 >= 0x60 && op3 <= 0x63) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 1);
                U8 control = fetchByte(rip + opOff + 3 + m.length);
                U64 firstLo = xmm[m.regField].lo, firstHi = xmm[m.regField].hi;
                U64 secondLo, secondHi;
                if (m.isReg) {
                    secondLo = xmm[m.rmIndex].lo;
                    secondHi = xmm[m.rmIndex].hi;
                } else {
                    secondLo = memory->readq(m.effAddr);
                    secondHi = memory->readq(m.effAddr + 8);
                }
                bool explicitLength = op3 == 0x60 || op3 == 0x61;
                S64 length1 = rexW ? (S64)reg[X64_RAX].u64
                                  : (S64)(S32)reg[X64_RAX].u32;
                S64 length2 = rexW ? (S64)reg[X64_RDX].u64
                                  : (S64)(S32)reg[X64_RDX].u32;
                CPU64PackedStringResult result = cpu64ComparePackedStrings(
                    firstLo, firstHi, secondLo, secondHi, control,
                    explicitLength, length1, length2);
                U32 count = (control & 1) ? 8 : 16;
                U32 flags = (result.mask ? X64_CF : 0) |
                            (result.length2 < count ? X64_ZF : 0) |
                            (result.length1 < count ? X64_SF : 0) |
                            ((result.mask & 1) ? X64_OF : 0);
                rflags &= ~(X64_CF | X64_ZF | X64_SF | X64_OF | X64_AF | X64_PF);
                rflags |= flags;

                if (op3 == 0x60 || op3 == 0x62) { // PCMPESTRM / PCMPISTRM
                    U64 outLo = 0, outHi = 0;
                    if (control & 0x40) {
                        U32 bits = (control & 1) ? 16 : 8;
                        U32 lanes = 128 / bits;
                        for (U32 i = 0; i < lanes; i++) {
                            U32 value = (result.mask & (1U << i))
                                ? (bits == 8 ? 0xFFU : 0xFFFFU) : 0;
                            if (bits == 8) {
                                if (i < 8) outLo |= (U64)value << (i * 8);
                                else       outHi |= (U64)value << ((i - 8) * 8);
                            } else {
                                if (i < 4) outLo |= (U64)value << (i * 16);
                                else       outHi |= (U64)value << ((i - 4) * 16);
                            }
                        }
                    } else {
                        outLo = result.mask;
                    }
                    xmm[0].lo = outLo;
                    xmm[0].hi = outHi;
                } else { // PCMPESTRI / PCMPISTRI
                    S32 index = (S32)count;
                    if (control & 0x40) {
                        for (S32 i = (S32)count - 1; i >= 0; i--) {
                            if (result.mask & (1U << i)) { index = i; break; }
                        }
                    } else {
                        for (U32 i = 0; i < count; i++) {
                            if (result.mask & (1U << i)) { index = (S32)i; break; }
                        }
                    }
                    reg[X64_RCX].setU32((U32)index);
                }
                U32 used = opOff + 3 + m.length + 1;
                rip += used;
                return used;
            }
        }

        if ((op2 == 0x38 || op2 == 0x3A) && osize66) {
            U8 op3 = fetchByte(rip + opOff + 2);
            if (op2 == 0x38 && (op3 == 0x03 || op3 == 0x04 ||
                                op3 == 0x05 || op3 == 0x06 ||
                                op3 == 0x07 || op3 == 0x0B)) {
                // Remaining SSSE3 horizontal, mixed multiply, and rounded
                // high-word multiply operations.
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                U8 d[16], s[16], out[16] = {};
                for (U32 i = 0; i < 8; i++) {
                    d[i] = (U8)(xmm[m.regField].lo >> (i * 8));
                    d[i + 8] = (U8)(xmm[m.regField].hi >> (i * 8));
                    s[i] = (U8)(sLo >> (i * 8));
                    s[i + 8] = (U8)(sHi >> (i * 8));
                }
                auto sat16 = [](S64 x) -> U16 {
                    if (x < -32768) x = -32768;
                    if (x > 32767) x = 32767;
                    return (U16)x;
                };
                if (op3 == 0x03 || op3 == 0x05 || op3 == 0x07) {
                    for (U32 side = 0; side < 2; side++) {
                        const U8* in = side == 0 ? d : s;
                        for (U32 pair = 0; pair < 4; pair++) {
                            S64 a = (S16)(U16)(in[pair * 4] | ((U16)in[pair * 4 + 1] << 8));
                            S64 b = (S16)(U16)(in[pair * 4 + 2] | ((U16)in[pair * 4 + 3] << 8));
                            S64 value = op3 == 0x03 ? a + b : a - b;
                            U16 packed = (op3 == 0x03 || op3 == 0x07)
                                ? sat16(value) : (U16)value;
                            U32 lane = side * 4 + pair;
                            out[lane * 2] = (U8)packed;
                            out[lane * 2 + 1] = (U8)(packed >> 8);
                        }
                    }
                } else if (op3 == 0x06) { // PHSUBD
                    for (U32 side = 0; side < 2; side++) {
                        const U8* in = side == 0 ? d : s;
                        for (U32 pair = 0; pair < 2; pair++) {
                            U32 a = (U32)in[pair * 8] |
                                    ((U32)in[pair * 8 + 1] << 8) |
                                    ((U32)in[pair * 8 + 2] << 16) |
                                    ((U32)in[pair * 8 + 3] << 24);
                            U32 b = (U32)in[pair * 8 + 4] |
                                    ((U32)in[pair * 8 + 5] << 8) |
                                    ((U32)in[pair * 8 + 6] << 16) |
                                    ((U32)in[pair * 8 + 7] << 24);
                            U32 value = a - b;
                            U32 lane = side * 2 + pair;
                            for (U32 byte = 0; byte < 4; byte++)
                                out[lane * 4 + byte] = (U8)(value >> (byte * 8));
                        }
                    }
                } else if (op3 == 0x04) { // PMADDUBSW
                    for (U32 i = 0; i < 8; i++) {
                        S64 value = (S64)d[i * 2] * (S8)s[i * 2] +
                                    (S64)d[i * 2 + 1] * (S8)s[i * 2 + 1];
                        U16 packed = sat16(value);
                        out[i * 2] = (U8)packed;
                        out[i * 2 + 1] = (U8)(packed >> 8);
                    }
                } else { // PMULHRSW
                    for (U32 i = 0; i < 8; i++) {
                        S64 a = (S16)(U16)(d[i * 2] | ((U16)d[i * 2 + 1] << 8));
                        S64 b = (S16)(U16)(s[i * 2] | ((U16)s[i * 2 + 1] << 8));
                        U16 packed = (U16)((a * b + 0x4000) >> 15);
                        out[i * 2] = (U8)packed;
                        out[i * 2 + 1] = (U8)(packed >> 8);
                    }
                }
                U64 nLo = 0, nHi = 0;
                for (U32 i = 0; i < 8; i++) {
                    nLo |= (U64)out[i] << (i * 8);
                    nHi |= (U64)out[i + 8] << (i * 8);
                }
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x38 && op3 == 0x00) {
                // PSHUFB
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 ctrlLo, ctrlHi;
                if (m.isReg) {
                    ctrlLo = xmm[m.rmIndex].lo;
                    ctrlHi = xmm[m.rmIndex].hi;
                } else {
                    ctrlLo = memory->readq(m.effAddr);
                    ctrlHi = memory->readq(m.effAddr + 8);
                }
                U8 src[16];
                for (int i = 0; i < 8; i++) src[i]     = (U8)(xmm[m.regField].lo >> (i*8));
                for (int i = 0; i < 8; i++) src[i+8]   = (U8)(xmm[m.regField].hi >> (i*8));
                U8 ctrl[16];
                for (int i = 0; i < 8; i++) ctrl[i]    = (U8)(ctrlLo >> (i*8));
                for (int i = 0; i < 8; i++) ctrl[i+8]  = (U8)(ctrlHi >> (i*8));
                U8 dst[16];
                for (int i = 0; i < 16; i++) {
                    dst[i] = (ctrl[i] & 0x80) ? 0 : src[ctrl[i] & 0x0F];
                }
                U64 nLo = 0, nHi = 0;
                for (int i = 0; i < 8; i++) nLo |= ((U64)dst[i])   << (i*8);
                for (int i = 0; i < 8; i++) nHi |= ((U64)dst[i+8]) << (i*8);
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x38 && op3 == 0x29) {
                // PCMPEQQ xmm1, xmm2/m128 — SSE4.1
                // Per qword: if equal, all-ones (0xFFFFFFFFFFFFFFFF); else zero.
                // Used by glibc's SSE4.1 strstr / __strcasestr_sse42.
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                xmm[m.regField].lo = (xmm[m.regField].lo == sLo) ? 0xFFFFFFFFFFFFFFFFULL : 0;
                xmm[m.regField].hi = (xmm[m.regField].hi == sHi) ? 0xFFFFFFFFFFFFFFFFULL : 0;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x38 && op3 == 0x17) {
                // PTEST xmm1, xmm2/m128 — SSE4.1
                // ZF = ((dst & src) == 0) ? 1 : 0
                // CF = ((~dst & src) == 0) ? 1 : 0    (AND NOT test)
                // OF/SF/AF/PF cleared. Used everywhere by glibc's SSE4.2
                // string ops for fast "all zero?" branches.
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                U64 dLo = xmm[m.regField].lo;
                U64 dHi = xmm[m.regField].hi;
                bool zf = ((dLo & sLo) == 0) && ((dHi & sHi) == 0);
                bool cf = ((~dLo & sLo) == 0) && ((~dHi & sHi) == 0);
                rflags &= ~(X64_ZF | X64_CF | X64_OF | X64_SF | X64_AF | X64_PF);
                if (zf) rflags |= X64_ZF;
                if (cf) rflags |= X64_CF;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x3A && op3 == 0x0F) {
                // PALIGNR
                ModRM m = decodeModRM(rip + opOff + 3, p, 1);
                U64 srcLo, srcHi;
                if (m.isReg) {
                    srcLo = xmm[m.rmIndex].lo;
                    srcHi = xmm[m.rmIndex].hi;
                } else {
                    srcLo = memory->readq(m.effAddr);
                    srcHi = memory->readq(m.effAddr + 8);
                }
                U8 imm = fetchByte(rip + opOff + 3 + m.length);
                // Concatenate dest:src — dest is high 16, src is low 16.
                U8 cat[32];
                for (int i = 0; i < 8; i++) cat[i]     = (U8)(srcLo >> (i*8));
                for (int i = 0; i < 8; i++) cat[i+8]   = (U8)(srcHi >> (i*8));
                for (int i = 0; i < 8; i++) cat[i+16]  = (U8)(xmm[m.regField].lo >> (i*8));
                for (int i = 0; i < 8; i++) cat[i+24]  = (U8)(xmm[m.regField].hi >> (i*8));
                U8 dst[16] = {0};
                if (imm < 32) {
                    for (int i = 0; i < 16; i++) {
                        int idx = i + imm;
                        dst[i] = (idx < 32) ? cat[idx] : 0;
                    }
                }
                U64 nLo = 0, nHi = 0;
                for (int i = 0; i < 8; i++) nLo |= ((U64)dst[i])   << (i*8);
                for (int i = 0; i < 8; i++) nHi |= ((U64)dst[i+8]) << (i*8);
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length + 1;
                rip += used;
                return used;
            }
            // ---- SSSE3 packed integer ops sharing 66 0F 38 /r ----
            //
            // PABSB/W/D (1C/1D/1E): packed absolute value of signed bytes/
            // words/dwords. PHADDW/D (01/02): horizontal add of adjacent
            // element pairs across dest then src. PSIGNB/W/D (08/09/0A):
            // apply the sign of each src element to the corresponding dest
            // element (negate if src<0, zero if src==0, keep if src>0).
            // All are reg/reg or reg/mem, no immediate. clang emits these
            // for vectorised abs()/reductions and libc++ ranges code.
            if (op2 == 0x38 && (op3 == 0x1C || op3 == 0x1D || op3 == 0x1E ||
                                op3 == 0x01 || op3 == 0x02 ||
                                op3 == 0x08 || op3 == 0x09 || op3 == 0x0A)) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                U64 dLo = xmm[m.regField].lo;
                U64 dHi = xmm[m.regField].hi;

                // Unpack dest and src into element arrays at the right width.
                auto unpackB = [](U64 lo, U64 hi, S8 out[16]) {
                    for (int i = 0; i < 8; i++) out[i]   = (S8)(U8)(lo >> (i*8));
                    for (int i = 0; i < 8; i++) out[i+8] = (S8)(U8)(hi >> (i*8));
                };
                auto unpackW = [](U64 lo, U64 hi, S16 out[8]) {
                    for (int i = 0; i < 4; i++) out[i]   = (S16)(U16)(lo >> (i*16));
                    for (int i = 0; i < 4; i++) out[i+4] = (S16)(U16)(hi >> (i*16));
                };
                auto unpackD = [](U64 lo, U64 hi, S32 out[4]) {
                    out[0] = (S32)(U32)lo; out[1] = (S32)(U32)(lo >> 32);
                    out[2] = (S32)(U32)hi; out[3] = (S32)(U32)(hi >> 32);
                };
                auto packB = [](S8 in[16], U64& lo, U64& hi) {
                    lo = hi = 0;
                    for (int i = 0; i < 8; i++) lo |= ((U64)(U8)in[i])   << (i*8);
                    for (int i = 0; i < 8; i++) hi |= ((U64)(U8)in[i+8]) << (i*8);
                };
                auto packW = [](S16 in[8], U64& lo, U64& hi) {
                    lo = hi = 0;
                    for (int i = 0; i < 4; i++) lo |= ((U64)(U16)in[i])   << (i*16);
                    for (int i = 0; i < 4; i++) hi |= ((U64)(U16)in[i+4]) << (i*16);
                };
                auto packD = [](S32 in[4], U64& lo, U64& hi) {
                    lo = ((U64)(U32)in[0]) | (((U64)(U32)in[1]) << 32);
                    hi = ((U64)(U32)in[2]) | (((U64)(U32)in[3]) << 32);
                };

                U64 nLo = 0, nHi = 0;
                if (op3 == 0x1C) {            // PABSB
                    S8 d[16]; unpackB(sLo, sHi, d);
                    S8 r[16];
                    for (int i = 0; i < 16; i++) r[i] = (S8)(d[i] < 0 ? -d[i] : d[i]);
                    packB(r, nLo, nHi);
                } else if (op3 == 0x1D) {     // PABSW
                    S16 d[8]; unpackW(sLo, sHi, d);
                    S16 r[8];
                    for (int i = 0; i < 8; i++) r[i] = (S16)(d[i] < 0 ? -d[i] : d[i]);
                    packW(r, nLo, nHi);
                } else if (op3 == 0x1E) {     // PABSD
                    S32 d[4]; unpackD(sLo, sHi, d);
                    S32 r[4];
                    for (int i = 0; i < 4; i++) {
                        S64 x = d[i]; r[i] = (S32)(x < 0 ? -x : x);
                    }
                    packD(r, nLo, nHi);
                } else if (op3 == 0x01) {     // PHADDW
                    S16 d[8], s[8]; unpackW(dLo, dHi, d); unpackW(sLo, sHi, s);
                    S16 r[8];
                    r[0]=(S16)(d[0]+d[1]); r[1]=(S16)(d[2]+d[3]);
                    r[2]=(S16)(d[4]+d[5]); r[3]=(S16)(d[6]+d[7]);
                    r[4]=(S16)(s[0]+s[1]); r[5]=(S16)(s[2]+s[3]);
                    r[6]=(S16)(s[4]+s[5]); r[7]=(S16)(s[6]+s[7]);
                    packW(r, nLo, nHi);
                } else if (op3 == 0x02) {     // PHADDD
                    S32 d[4], s[4]; unpackD(dLo, dHi, d); unpackD(sLo, sHi, s);
                    U32 r[4];
                    r[0]=(U32)d[0]+(U32)d[1]; r[1]=(U32)d[2]+(U32)d[3];
                    r[2]=(U32)s[0]+(U32)s[1]; r[3]=(U32)s[2]+(U32)s[3];
                    nLo = (U64)r[0] | ((U64)r[1] << 32);
                    nHi = (U64)r[2] | ((U64)r[3] << 32);
                } else if (op3 == 0x08) {     // PSIGNB
                    S8 d[16], s[16]; unpackB(dLo, dHi, d); unpackB(sLo, sHi, s);
                    S8 r[16];
                    for (int i = 0; i < 16; i++)
                        r[i] = (S8)(s[i] < 0 ? -d[i] : (s[i] == 0 ? 0 : d[i]));
                    packB(r, nLo, nHi);
                } else if (op3 == 0x09) {     // PSIGNW
                    S16 d[8], s[8]; unpackW(dLo, dHi, d); unpackW(sLo, sHi, s);
                    S16 r[8];
                    for (int i = 0; i < 8; i++)
                        r[i] = (S16)(s[i] < 0 ? -d[i] : (s[i] == 0 ? 0 : d[i]));
                    packW(r, nLo, nHi);
                } else {                       // PSIGND (0x0A)
                    S32 d[4], s[4]; unpackD(dLo, dHi, d); unpackD(sLo, sHi, s);
                    S32 r[4];
                    for (int i = 0; i < 4; i++) {
                        S64 x = d[i];
                        r[i] = (S32)(s[i] < 0 ? -x : (s[i] == 0 ? 0 : x));
                    }
                    packD(r, nLo, nHi);
                }
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            // Other 0F 38 / 0F 3A forms — fall through to default panic.
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

        // Packed double-precision FP arithmetic — 66 0F prefix selects the
        // packed-double form. Operates on two doubles in parallel (low qword
        // and high qword of the XMM regs). Discovered via musl libm
        // (sqrt/sin/cos use packed double pairs internally for table-based
        // polynomial evaluation).
        //   ADDPD  66 0F 58
        //   MULPD  66 0F 59
        //   SUBPD  66 0F 5C
        //   DIVPD  66 0F 5E
        //   SQRTPD 66 0F 51
        //   MINPD  66 0F 5D
        //   MAXPD  66 0F 5F
        if (osize66 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            double aLo = u64ToDouble(xmm[m.regField].lo);
            double aHi = u64ToDouble(xmm[m.regField].hi);
            double bLo = u64ToDouble(srcLo);
            double bHi = u64ToDouble(srcHi);
            double rLo, rHi;
            switch (op2) {
                case 0x58: rLo = aLo + bLo; rHi = aHi + bHi; break;
                case 0x59: rLo = aLo * bLo; rHi = aHi * bHi; break;
                case 0x5C: rLo = aLo - bLo; rHi = aHi - bHi; break;
                case 0x5E: rLo = aLo / bLo; rHi = aHi / bHi; break;
                case 0x51: rLo = std::sqrt(bLo); rHi = std::sqrt(bHi); break;
                case 0x5D: rLo = (aLo < bLo) ? aLo : bLo;
                           rHi = (aHi < bHi) ? aHi : bHi; break;
                case 0x5F: rLo = (aLo > bLo) ? aLo : bLo;
                           rHi = (aHi > bHi) ? aHi : bHi; break;
                default:   rLo = aLo; rHi = aHi; break;
            }
            xmm[m.regField].lo = doubleToU64(rLo);
            xmm[m.regField].hi = doubleToU64(rHi);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Packed single-precision FP arithmetic — no prefix selects PS form.
        //   ADDPS  0F 58, MULPS 0F 59, SUBPS 0F 5C, DIVPS 0F 5E,
        //   SQRTPS 0F 51, MINPS 0F 5D, MAXPS 0F 5F
        // Operates on 4 floats packed in the XMM register.
        if (!osize66 && p.rep == 0 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
            auto fu32 = [](float f){ U32 b; std::memcpy(&b,&f,4); return b; };
            float a[4], b[4], r[4];
            a[0] = u32f((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            a[1] = u32f((U32)(xmm[m.regField].lo >> 32));
            a[2] = u32f((U32)(xmm[m.regField].hi & 0xFFFFFFFFULL));
            a[3] = u32f((U32)(xmm[m.regField].hi >> 32));
            b[0] = u32f((U32)(srcLo & 0xFFFFFFFFULL));
            b[1] = u32f((U32)(srcLo >> 32));
            b[2] = u32f((U32)(srcHi & 0xFFFFFFFFULL));
            b[3] = u32f((U32)(srcHi >> 32));
            for (int i = 0; i < 4; i++) {
                switch (op2) {
                    case 0x58: r[i] = a[i] + b[i]; break;
                    case 0x59: r[i] = a[i] * b[i]; break;
                    case 0x5C: r[i] = a[i] - b[i]; break;
                    case 0x5E: r[i] = a[i] / b[i]; break;
                    case 0x51: {
                        U32 bits = fu32(b[i]);
                        // Negative (sign bit set) and not -0.0 (0x80000000):
                        // x86 returns the indefinite QNaN 0xFFC00000.
                        if ((bits & 0x80000000U) != 0 && bits != 0x80000000U)
                            r[i] = u32f(0xFFC00000U);
                        else
                            r[i] = std::sqrt(b[i]);
                        break;
                    }
                    case 0x5D: r[i] = (a[i] < b[i]) ? a[i] : b[i]; break;
                    case 0x5F: r[i] = (a[i] > b[i]) ? a[i] : b[i]; break;
                    default:   r[i] = a[i]; break;
                }
            }
            xmm[m.regField].lo = (U64)fu32(r[0]) | ((U64)fu32(r[1]) << 32);
            xmm[m.regField].hi = (U64)fu32(r[2]) | ((U64)fu32(r[3]) << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE3 horizontal FP: HADDPS/HSUBPS (F2 0F 7C/7D),
        //      HADDPD/HSUBPD (66 0F 7C/7D) ----
        //
        // HADD pairs adjacent lanes: for HADDPS the result is
        //   { d0+d1, d2+d3, s0+s1, s2+s3 }  (singles)
        // HADDPD:
        //   { d0+d1, s0+s1 }                (doubles)
        // HSUB is the same with subtraction (lane0-lane1, etc). clang emits
        // these for std::accumulate / reduction loops over float arrays.
        if ((op2 == 0x7C || op2 == 0x7D) &&
            (p.rep == 0xF2 || osize66)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) { srcLo = xmm[m.rmIndex].lo; srcHi = xmm[m.rmIndex].hi; }
            else { srcLo = memory->readq(m.effAddr); srcHi = memory->readq(m.effAddr + 8); }
            bool isSub = (op2 == 0x7D);
            if (osize66) {
                // packed double
                double d0 = u64ToDouble(xmm[m.regField].lo);
                double d1 = u64ToDouble(xmm[m.regField].hi);
                double s0 = u64ToDouble(srcLo);
                double s1 = u64ToDouble(srcHi);
                double r0 = isSub ? (d0 - d1) : (d0 + d1);
                double r1 = isSub ? (s0 - s1) : (s0 + s1);
                xmm[m.regField].lo = doubleToU64(r0);
                xmm[m.regField].hi = doubleToU64(r1);
            } else {
                // packed single (F2 prefix)
                auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
                auto fu32 = [](float f){ U32 b; std::memcpy(&b,&f,4); return b; };
                float d[4], s[4];
                d[0]=u32f((U32)xmm[m.regField].lo); d[1]=u32f((U32)(xmm[m.regField].lo>>32));
                d[2]=u32f((U32)xmm[m.regField].hi); d[3]=u32f((U32)(xmm[m.regField].hi>>32));
                s[0]=u32f((U32)srcLo); s[1]=u32f((U32)(srcLo>>32));
                s[2]=u32f((U32)srcHi); s[3]=u32f((U32)(srcHi>>32));
                float r[4];
                r[0]=isSub?(d[0]-d[1]):(d[0]+d[1]);
                r[1]=isSub?(d[2]-d[3]):(d[2]+d[3]);
                r[2]=isSub?(s[0]-s[1]):(s[0]+s[1]);
                r[3]=isSub?(s[2]-s[3]):(s[2]+s[3]);
                xmm[m.regField].lo = (U64)fu32(r[0]) | ((U64)fu32(r[1]) << 32);
                xmm[m.regField].hi = (U64)fu32(r[2]) | ((U64)fu32(r[3]) << 32);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // SSE3 ADDSUBPS/ADDSUBPD alternate subtract and add by element index.
        // F2 0F D0 performs PS (subtract even lanes, add odd); 66 0F D0 does
        // the same over the two packed doubles.
        if (op2 == 0xD0 && (p.rep == 0xF2 || osize66)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) { srcLo = xmm[m.rmIndex].lo; srcHi = xmm[m.rmIndex].hi; }
            else { srcLo = memory->readq(m.effAddr); srcHi = memory->readq(m.effAddr + 8); }
            if (osize66) {
                double d0 = u64ToDouble(xmm[m.regField].lo);
                double d1 = u64ToDouble(xmm[m.regField].hi);
                double s0 = u64ToDouble(srcLo);
                double s1 = u64ToDouble(srcHi);
                xmm[m.regField].lo = doubleToU64(d0 - s0);
                xmm[m.regField].hi = doubleToU64(d1 + s1);
            } else {
                auto u32f = [](U32 b) { float f; std::memcpy(&f, &b, 4); return f; };
                auto fu32 = [](float f) { U32 b; std::memcpy(&b, &f, 4); return b; };
                U32 dst[4] = {(U32)xmm[m.regField].lo, (U32)(xmm[m.regField].lo >> 32),
                              (U32)xmm[m.regField].hi, (U32)(xmm[m.regField].hi >> 32)};
                U32 src[4] = {(U32)srcLo, (U32)(srcLo >> 32), (U32)srcHi, (U32)(srcHi >> 32)};
                U32 out[4];
                for (int lane = 0; lane < 4; lane++) {
                    float a = u32f(dst[lane]), b = u32f(src[lane]);
                    out[lane] = fu32((lane & 1) ? a + b : a - b);
                }
                xmm[m.regField].lo = (U64)out[0] | ((U64)out[1] << 32);
                xmm[m.regField].hi = (U64)out[2] | ((U64)out[3] << 32);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        if (p.rep == 0xF2 && op2 == 0xF0) { // LDDQU xmm,m128
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) goto unhandled;
            xmm[m.regField].lo = memory->readq(m.effAddr);
            xmm[m.regField].hi = memory->readq(m.effAddr + 8);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE3 MOVSHDUP (F3 0F 16) / MOVSLDUP (F3 0F 12) ----
        //   MOVSHDUP {a0,a1,a2,a3} -> {a1,a1,a3,a3} (duplicate odd lanes)
        //   MOVSLDUP {a0,a1,a2,a3} -> {a0,a0,a2,a2} (duplicate even lanes)
        // Used in complex-arithmetic and broadcast patterns. Note 0F 12 with
        // F3 is MOVSLDUP, distinct from MOVLPS (no prefix) / MOVDDUP (F2).
        if (p.rep == 0xF3 && (op2 == 0x16 || op2 == 0x12)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            U32 e0 = (U32)sLo, e1 = (U32)(sLo >> 32);
            U32 e2 = (U32)sHi, e3 = (U32)(sHi >> 32);
            U32 r0, r1, r2, r3;
            if (op2 == 0x16) { r0 = e1; r1 = e1; r2 = e3; r3 = e3; }   // SHDUP
            else             { r0 = e0; r1 = e0; r2 = e2; r3 = e2; }   // SLDUP
            xmm[m.regField].lo = (U64)r0 | ((U64)r1 << 32);
            xmm[m.regField].hi = (U64)r2 | ((U64)r3 << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE4.1 packed dword min/max/mul: 66 0F 38 38/39/3D/3C/40 ----
        //   PMINSD 39, PMAXSD 3D, PMINSB 38, PMAXSB 3C, PMULLD 40
        // (the byte forms 38/3C round out the signed min/max family). All
        // are 66 0F 38 /r with a standard ModRM. clang emits PMULLD for
        // int-vector multiply and PMIN/PMAXSD for std::min/max over int[].
        if (osize66 && op2 == 0x38) {
            U8 op3b = fetchByte(rip + opOff + 2);
            if (op3b == 0x39 || op3b == 0x3D || op3b == 0x40 ||
                op3b == 0x38 || op3b == 0x3C) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
                else { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
                U64 dLo = xmm[m.regField].lo, dHi = xmm[m.regField].hi;
                U64 nLo, nHi;
                if (op3b == 0x38 || op3b == 0x3C) {
                    // signed byte min(38)/max(3C)
                    auto doByte = [&](U64 dq, U64 sq) -> U64 {
                        U64 out = 0;
                        for (int i = 0; i < 8; i++) {
                            S8 dv = (S8)(U8)(dq >> (i*8));
                            S8 sv = (S8)(U8)(sq >> (i*8));
                            S8 rv = (op3b == 0x38) ? (dv < sv ? dv : sv)
                                                   : (dv > sv ? dv : sv);
                            out |= ((U64)(U8)rv) << (i*8);
                        }
                        return out;
                    };
                    nLo = doByte(dLo, sLo); nHi = doByte(dHi, sHi);
                } else {
                    // dword min(39)/max(3D)/mullo(40)
                    auto doDword = [&](U64 dq, U64 sq) -> U64 {
                        S32 d0 = (S32)(U32)dq, d1 = (S32)(U32)(dq >> 32);
                        S32 s0 = (S32)(U32)sq, s1 = (S32)(U32)(sq >> 32);
                        S32 r0, r1;
                        if (op3b == 0x39) { r0 = d0 < s0 ? d0 : s0; r1 = d1 < s1 ? d1 : s1; }
                        else if (op3b == 0x3D) { r0 = d0 > s0 ? d0 : s0; r1 = d1 > s1 ? d1 : s1; }
                        else { r0 = (S32)((U32)d0 * (U32)s0); r1 = (S32)((U32)d1 * (U32)s1); }
                        return ((U64)(U32)r0) | (((U64)(U32)r1) << 32);
                    };
                    nLo = doDword(dLo, sLo); nHi = doDword(dHi, sHi);
                }
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
        }

        // ---- Remaining SSE4.1 0F38 packed integer operations ----
        if (osize66 && p.rep == 0 && op2 == 0x38) {
            U8 op3b = fetchByte(rip + opOff + 2);
            const bool supported = op3b == 0x10 || op3b == 0x14 || op3b == 0x15 ||
                (op3b >= 0x20 && op3b <= 0x25) || op3b == 0x28 ||
                op3b == 0x2A || op3b == 0x2B ||
                (op3b >= 0x30 && op3b <= 0x35) || op3b == 0x3A ||
                op3b == 0x3B || op3b == 0x3E || op3b == 0x3F || op3b == 0x41;
            if (supported) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U32 sourceBytes = 16;
                if (op3b >= 0x20 && op3b <= 0x25) {
                    const U8 sizes[] = {8,4,2,8,4,8}; sourceBytes=sizes[op3b-0x20];
                } else if (op3b >= 0x30 && op3b <= 0x35) {
                    const U8 sizes[] = {8,4,2,8,4,8}; sourceBytes=sizes[op3b-0x30];
                }
                U64 sLo = 0, sHi = 0;
                if (m.isReg) { sLo=xmm[m.rmIndex].lo; sHi=xmm[m.rmIndex].hi; }
                else if (sourceBytes==16) { sLo=memory->readq(m.effAddr); sHi=memory->readq(m.effAddr+8); }
                else if (sourceBytes==8) sLo=memory->readq(m.effAddr);
                else if (sourceBytes==4) sLo=memory->readd(m.effAddr);
                else if (sourceBytes==2) sLo=memory->readw(m.effAddr);
                U64 dLo = xmm[m.regField].lo, dHi = xmm[m.regField].hi;
                U64 nLo = 0, nHi = 0;
                auto byteAt = [](U64 lo, U64 hi, U32 index) -> U8 {
                    return index < 8 ? (U8)(lo >> (index * 8))
                                     : (U8)(hi >> ((index - 8) * 8));
                };
                if (op3b == 0x10 || op3b == 0x14 || op3b == 0x15) {
                    // PBLENDVB / BLENDVPS / BLENDVPD select from SRC when
                    // the corresponding implicit XMM0 sign bit is set.
                    U64 maskLo = xmm[0].lo, maskHi = xmm[0].hi;
                    U8 out[16];
                    if (op3b == 0x10) {
                        for (U32 i = 0; i < 16; i++)
                            out[i] = (byteAt(maskLo,maskHi,i) & 0x80)
                                ? byteAt(sLo,sHi,i) : byteAt(dLo,dHi,i);
                    } else if (op3b == 0x14) {
                        for (U32 lane = 0; lane < 4; lane++) {
                            U32 dv = (U32)(lane < 2 ? dLo >> (lane*32) : dHi >> ((lane-2)*32));
                            U32 sv = (U32)(lane < 2 ? sLo >> (lane*32) : sHi >> ((lane-2)*32));
                            U32 mv = (U32)(lane < 2 ? maskLo >> (lane*32) : maskHi >> ((lane-2)*32));
                            U32 v = (mv & 0x80000000U) ? sv : dv;
                            for (U32 b = 0; b < 4; b++) out[lane*4+b] = (U8)(v >> (b*8));
                        }
                    } else {
                        for (U32 lane = 0; lane < 2; lane++) {
                            U64 dv = lane ? dHi : dLo, sv = lane ? sHi : sLo;
                            U64 mv = lane ? maskHi : maskLo;
                            U64 v = (mv >> 63) ? sv : dv;
                            for (U32 b = 0; b < 8; b++) out[lane*8+b] = (U8)(v >> (b*8));
                        }
                    }
                    for (U32 i = 0; i < 8; i++) {
                        nLo |= (U64)out[i] << (i*8);
                        nHi |= (U64)out[i+8] << (i*8);
                    }
                } else if (op3b >= 0x20 && op3b <= 0x25) {
                    // PMOVSX{BW,BD,BQ,WD,WQ,DQ}
                    U32 srcBits, dstBits;
                    switch (op3b) {
                        case 0x20: srcBits=8; dstBits=16; break;
                        case 0x21: srcBits=8; dstBits=32; break;
                        case 0x22: srcBits=8; dstBits=64; break;
                        case 0x23: srcBits=16; dstBits=32; break;
                        case 0x24: srcBits=16; dstBits=64; break;
                        default:   srcBits=32; dstBits=64; break;
                    }
                    U32 lanes = 128 / dstBits;
                    U64 src = sLo;
                    U64 outs[2] = {};
                    for (U32 i = 0; i < lanes; i++) {
                        U64 raw = (src >> (i * srcBits)) & ((1ULL << srcBits) - 1);
                        S64 value = srcBits == 8 ? (S8)raw : srcBits == 16 ? (S16)raw : (S32)raw;
                        U32 bit = i * dstBits;
                        U64 encoded = (U64)value;
                        if (dstBits < 64) encoded &= (1ULL << dstBits) - 1;
                        outs[bit / 64] |= encoded << (bit & 63);
                    }
                    nLo=outs[0]; nHi=outs[1];
                } else if (op3b >= 0x30 && op3b <= 0x35) {
                    // PMOVZX{BW,BD,BQ,WD,WQ,DQ}
                    U32 srcBits, dstBits;
                    switch (op3b) {
                        case 0x30: srcBits=8; dstBits=16; break;
                        case 0x31: srcBits=8; dstBits=32; break;
                        case 0x32: srcBits=8; dstBits=64; break;
                        case 0x33: srcBits=16; dstBits=32; break;
                        case 0x34: srcBits=16; dstBits=64; break;
                        default:   srcBits=32; dstBits=64; break;
                    }
                    U32 lanes = 128 / dstBits;
                    U64 src = sLo;
                    U64 outs[2] = {};
                    for (U32 i = 0; i < lanes; i++) {
                        U64 value = (src >> (i * srcBits)) & ((1ULL << srcBits) - 1);
                        U32 bit = i * dstBits;
                        outs[bit / 64] |= value << (bit & 63);
                    }
                    nLo=outs[0]; nHi=outs[1];
                } else if (op3b == 0x28) { // PMULDQ: signed dword lanes 0 and 2
                    S64 p0 = (S64)(S32)(U32)dLo * (S32)(U32)sLo;
                    S64 p1 = (S64)(S32)(U32)(dHi) * (S32)(U32)(sHi);
                    nLo=(U64)p0; nHi=(U64)p1;
                } else if (op3b == 0x2A) { // MOVNTDQA m128
                    if (m.isReg) goto unhandled;
                    nLo=sLo; nHi=sHi;
                } else if (op3b == 0x2B) { // PACKUSDW
                    U16 words[8];
                    for (U32 i=0;i<8;i++) {
                        U64 bits = i < 2 ? dLo >> (i*32) :
                                   i < 4 ? dHi >> ((i-2)*32) :
                                   i < 6 ? sLo >> ((i-4)*32) : sHi >> ((i-6)*32);
                        S64 value=(S32)(U32)bits;
                        words[i]=(U16)(value < 0 ? 0 : value > 65535 ? 65535 : value);
                    }
                    for (U32 i=0;i<4;i++) nLo|=(U64)words[i]<<(i*16);
                    for (U32 i=0;i<4;i++) nHi|=(U64)words[i+4]<<(i*16);
                } else if (op3b == 0x3A || op3b == 0x3B || op3b == 0x3E || op3b == 0x3F) {
                    // Unsigned word/dword min/max.
                    U32 bits = (op3b == 0x3A || op3b == 0x3E) ? 16 : 32;
                    U32 lanes = 128 / bits;
                    for (U32 i=0;i<lanes;i++) {
                        U64 mask = bits == 16 ? 0xFFFFULL : 0xFFFFFFFFULL;
                        U64 dv = (i < 64/bits ? dLo : dHi) >> ((i%(64/bits))*bits) & mask;
                        U64 sv = (i < 64/bits ? sLo : sHi) >> ((i%(64/bits))*bits) & mask;
                        bool isMin = op3b == 0x3A || op3b == 0x3B;
                        U64 value = isMin ? (dv < sv ? dv : sv) : (dv > sv ? dv : sv);
                        U32 bit=i*bits;
                        if (bit < 64) nLo |= value << bit; else nHi |= value << (bit-64);
                    }
                } else { // PHMINPOSUW
                    U16 minValue=0xFFFF; U16 minIndex=0;
                    for (U32 i=0;i<8;i++) {
                        U16 value=(U16)((i<4 ? sLo>>(i*16) : sHi>>((i-4)*16)) & 0xFFFF);
                        if (value < minValue) { minValue=value; minIndex=(U16)i; }
                    }
                    nLo=(U64)minValue | ((U64)minIndex<<16);
                }
                xmm[m.regField].lo=nLo; xmm[m.regField].hi=nHi;
                U32 used=opOff+3+m.length; rip+=used; return used;
            }
        }

        // ---- SSE4.1 0F3A immediate operations ----
        if (osize66 && p.rep == 0 && op2 == 0x3A) {
            U8 op3b=fetchByte(rip+opOff+2);
            const bool supported = (op3b>=0x08 && op3b<=0x0E) ||
                (op3b>=0x14 && op3b<=0x17) || (op3b>=0x20 && op3b<=0x22) ||
                op3b==0x40 || op3b==0x41 || op3b==0x42;
            if (supported) {
                ModRM m=decodeModRM(rip+opOff+3,p,1);
                U8 imm=fetchByte(rip+opOff+3+m.length);
                U32 sourceBytes=16;
                if(op3b==0x0A || op3b==0x21)sourceBytes=4;
                else if(op3b==0x0B)sourceBytes=8;
                else if(op3b==0x20)sourceBytes=1;
                else if(op3b==0x22)sourceBytes=rexW?8:4;
                else if(op3b>=0x14 && op3b<=0x17)sourceBytes=0;
                U64 sLo=0,sHi=0;
                if(m.isReg){sLo=xmm[m.rmIndex].lo;sHi=xmm[m.rmIndex].hi;}
                else if(sourceBytes==16){sLo=memory->readq(m.effAddr);sHi=memory->readq(m.effAddr+8);}
                else if(sourceBytes==8)sLo=memory->readq(m.effAddr);
                else if(sourceBytes==4)sLo=memory->readd(m.effAddr);
                else if(sourceBytes==1)sLo=memory->readb(m.effAddr);
                U64 dLo=xmm[m.regField].lo,dHi=xmm[m.regField].hi;
                U32 used=opOff+3+m.length+1;
                if (op3b==0x08 || op3b==0x09 || op3b==0x0A || op3b==0x0B) {
                    bool isDouble=op3b==0x09 || op3b==0x0B;
                    bool scalar=op3b==0x0A || op3b==0x0B;
                    U32 lanes=isDouble?2:4;
                    U32 rc=(imm&4)?((mxcsr>>13)&3):(imm&3);
                    auto roundValue=[&](double value)->double {
                        if (!std::isfinite(value)) return value;
                        switch (rc) {
                            case 1: return std::floor(value);
                            case 2: return std::ceil(value);
                            case 3: return std::trunc(value);
                            default: return std::nearbyint(value);
                        }
                    };
                    U64 outLo=dLo,outHi=dHi;
                    U32 last=scalar?1:lanes;
                    for (U32 i=0;i<last;i++) {
                        if (isDouble) {
                            U64 bits=i==0?sLo:sHi;
                            U64 rounded=doubleToU64(roundValue(u64ToDouble(bits)));
                            if (i==0) outLo=rounded; else outHi=rounded;
                        } else {
                            U32 bits=(U32)(i<2?sLo>>(i*32):sHi>>((i-2)*32));
                            float value; std::memcpy(&value,&bits,4);
                            float rounded=(float)roundValue(value); U32 result;
                            std::memcpy(&result,&rounded,4);
                            if (i<2) outLo=(outLo&~(0xFFFFFFFFULL<<(i*32)))|((U64)result<<(i*32));
                            else outHi=(outHi&~(0xFFFFFFFFULL<<((i-2)*32)))|((U64)result<<((i-2)*32));
                        }
                    }
                    xmm[m.regField].lo=outLo; xmm[m.regField].hi=outHi;
                } else if (op3b==0x0C || op3b==0x0D || op3b==0x0E) {
                    U64 outLo=0,outHi=0;
                    if (op3b==0x0E) {
                        for (U32 i=0;i<8;i++) {
                            U16 d=(U16)(i<4?dLo>>(i*16):dHi>>((i-4)*16));
                            U16 s=(U16)(i<4?sLo>>(i*16):sHi>>((i-4)*16));
                            U16 value=(imm&(1U<<i))?s:d;
                            if(i<4)outLo|=(U64)value<<(i*16);else outHi|=(U64)value<<((i-4)*16);
                        }
                    } else {
                        U32 lanes=op3b==0x0C?4:2,width=op3b==0x0C?32:64;
                        for(U32 i=0;i<lanes;i++) {
                            U64 dv=width==32?((i<2?dLo>>(i*32):dHi>>((i-2)*32))&0xFFFFFFFFULL):(i?dHi:dLo);
                            U64 sv=width==32?((i<2?sLo>>(i*32):sHi>>((i-2)*32))&0xFFFFFFFFULL):(i?sHi:sLo);
                            U64 value=(imm&(1U<<i))?sv:dv;
                            if(width==32) {
                                if(i<2)outLo|=value<<(i*32);else outHi|=value<<((i-2)*32);
                            } else if(i==0)outLo=value;else outHi=value;
                        }
                    }
                    xmm[m.regField].lo=outLo;xmm[m.regField].hi=outHi;
                } else if (op3b==0x40 || op3b==0x41) {
                    bool isDouble=op3b==0x41; U32 lanes=isDouble?2:4;
                    double sumD=0.0; float sumF=0.0f;
                    for(U32 i=0;i<lanes;i++) if(imm&(1U<<(i+4))) {
                        if(isDouble) {
                            double a=u64ToDouble(i==0?dLo:dHi),b=u64ToDouble(i==0?sLo:sHi);
                            sumD += a*b;
                        } else {
                            U32 ab=(U32)(i<2?dLo>>(i*32):dHi>>((i-2)*32));
                            U32 bb=(U32)(i<2?sLo>>(i*32):sHi>>((i-2)*32));
                            float a,b;std::memcpy(&a,&ab,4);std::memcpy(&b,&bb,4);sumF += a*b;
                        }
                    }
                    U64 outLo=0,outHi=0;
                    for(U32 i=0;i<lanes;i++) if(imm&(1U<<i)) {
                        if(isDouble) { if(i==0)outLo=doubleToU64(sumD);else outHi=doubleToU64(sumD); }
                        else {
                            U32 bits;std::memcpy(&bits,&sumF,4);
                            if(i<2)outLo|=(U64)bits<<(i*32);else outHi|=(U64)bits<<((i-2)*32);
                        }
                    }
                    xmm[m.regField].lo=outLo;xmm[m.regField].hi=outHi;
                } else if (op3b==0x42) { // MPSADBW
                    U32 srcOffset=(imm&3)*4,dstOffset=((imm>>2)&1)*4;
                    U16 sums[8];
                    U64 outLo=0,outHi=0;
                    for(U32 lane=0;lane<8;lane++) {
                        U32 sum=0;
                        for(U32 byte=0;byte<4;byte++) {
                            U8 dv=(dstOffset+lane+byte<8)?(U8)(dLo>>((dstOffset+lane+byte)*8)):
                                  (U8)(dHi>>((dstOffset+lane+byte-8)*8));
                            U8 sv=(srcOffset+byte<8)?(U8)(sLo>>((srcOffset+byte)*8)):
                                  (U8)(sHi>>((srcOffset+byte-8)*8));
                            sum += dv>sv?dv-sv:sv-dv;
                        }
                        sums[lane]=(U16)sum;
                    }
                    for(U32 i=0;i<4;i++)outLo|=(U64)sums[i]<<(i*16);
                    for(U32 i=0;i<4;i++)outHi|=(U64)sums[i+4]<<(i*16);
                    xmm[m.regField].lo=outLo;xmm[m.regField].hi=outHi;
                } else if (op3b==0x20 || op3b==0x21 || op3b==0x22) {
                    if(op3b==0x20) {
                        U32 lane=imm&15,shift=(lane&7)*8;
                        // Register source is r32's LOW byte, never AH/CH/DH/BH
                        // (loadRM size-1 would apply legacy high-byte encoding).
                        U8 value=m.isReg?(U8)reg[m.rmIndex].u64:memory->readb(m.effAddr);
                        U64* dst=lane<8?&xmm[m.regField].lo:&xmm[m.regField].hi;
                        *dst=(*dst&~(0xFFULL<<shift))|((U64)value<<shift);
                    } else if(op3b==0x21) {
                        U32 sourceLane=(imm>>6)&3;
                        U32 source=m.isReg?
                            (U32)((sourceLane<2?xmm[m.rmIndex].lo>>(sourceLane*32):
                                   xmm[m.rmIndex].hi>>((sourceLane-2)*32))&0xFFFFFFFFULL):
                            memory->readd(m.effAddr);
                        U32 destLane=(imm>>4)&3;
                        if(destLane<2)xmm[m.regField].lo=(xmm[m.regField].lo&~(0xFFFFFFFFULL<<(destLane*32)))|((U64)source<<(destLane*32));
                        else xmm[m.regField].hi=(xmm[m.regField].hi&~(0xFFFFFFFFULL<<((destLane-2)*32)))|((U64)source<<((destLane-2)*32));
                        for(U32 i=0;i<4;i++)if(imm&(1U<<i)) {
                            if(i<2)xmm[m.regField].lo&=~(0xFFFFFFFFULL<<(i*32));
                            else xmm[m.regField].hi&=~(0xFFFFFFFFULL<<((i-2)*32));
                        }
                    } else {
                        U32 width=rexW?64:32,lane=imm&(rexW?1:3);U64 value=loadRM(m,width/8,rexPresent);
                        if(width==64) { if(lane==0)xmm[m.regField].lo=value;else xmm[m.regField].hi=value; }
                        else if(lane<2)xmm[m.regField].lo=(xmm[m.regField].lo&~(0xFFFFFFFFULL<<(lane*32)))|((U64)value<<(lane*32));
                        else xmm[m.regField].hi=(xmm[m.regField].hi&~(0xFFFFFFFFULL<<((lane-2)*32)))|((U64)value<<((lane-2)*32));
                    }
                } else if (op3b==0x17 || op3b==0x15 || op3b==0x16 || op3b==0x14) {
                    // PEXTRB/W/D/Q and EXTRACTPS write r/m, not the XMM reg.
                    U32 width=op3b==0x14?8:op3b==0x15?16:op3b==0x17?32:(rexW?64:32);
                    U32 lane=imm&(op3b==0x14?15:op3b==0x15?7:op3b==0x17?3:(rexW?1:3));
                    U64 source=lane*width<64?dLo>>(lane*width):dHi>>((lane*width)-64);
                    if(op3b==0x17) width=32;
                    storeRM(m,width/8,source,rexPresent);
                }
                rip+=used;return used;
            }
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

        // CVTSD2SS xmm, xmm/m64   F2 0F 5A /r — narrow double (src low qword)
        // to single, writing the result into dst's low 32 bits (high 32 of the
        // low qword preserved, per Intel). winex11/win32u hits this on DPI/
        // scaling math.
        if (op2 == 0x5A && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            float r = (float)u64ToDouble(srcBits);
            // float -> raw bits (u32ToFloat/floatToU32 helpers are declared
            // further down in the F3 block, so bit-cast inline here).
            U32 rBits; std::memcpy(&rBits, &r, sizeof(rBits));
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (U64)rBits;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSD2SI r32, xmm/m64   F2 0F 2D /r     (REX.W → r64)
        // CVTTSD2SI r32, xmm/m64  F2 0F 2C /r     (REX.W → r64) — truncate
        // Convert double to signed int with MXCSR.RC for 2D and truncation
        // for 2C. This fallback shares the same invalid-result handling.
        if ((op2 == 0x2D || op2 == 0x2C) && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double d = u64ToDouble(srcBits);
            U64 converted = cpu64ConvertFpToInt(d, rexW, op2 == 0x2C, mxcsr);
            if (rexW) reg[m.regField].setU64(converted);
            else      reg[m.regField].setU32((U32)converted);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CMPSD xmm, xmm/m64, imm8   F2 0F C2 /r ib — compare two doubles per
        // the imm8 predicate; write an all-ones (true) or all-zero (false)
        // 64-bit mask into dst's low qword (high qword preserved). The 8 core
        // predicates (imm8 & 7): 0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT,
        // 6 NLE, 7 ORD. winex11/win32u emits CMPSD ...,6 (NLE) for clamp math.
        if (op2 == 0xC2 && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            double a = u64ToDouble(xmm[m.regField].lo);
            double b = u64ToDouble(srcBits);
            bool unordered = std::isnan(a) || std::isnan(b);
            bool res;
            switch (imm & 7) {
                case 0: res = (a == b); break;                  // EQ
                case 1: res = (a <  b); break;                  // LT
                case 2: res = (a <= b); break;                  // LE
                case 3: res = unordered; break;                 // UNORD
                case 4: res = unordered || (a != b); break;     // NEQ
                case 5: res = unordered || !(a <  b); break;    // NLT
                case 6: res = unordered || !(a <= b); break;    // NLE
                default: res = !unordered; break;               // ORD
            }
            xmm[m.regField].lo = res ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;
            U32 used = opOff + 2 + m.length + 1; // +imm8
            rip += used;
            return used;
        }

        // CMPPS xmm, xmm/m128, imm8   0F C2 /r ib — packed single compare.
        // Each of the four dwords becomes all-ones/all-zero per the imm8
        // predicate. IL2CPP compiles vector float comparisons to this plus
        // MOVMSKPS. Without this the guest stops on the first such compare
        // (Gang Beasts reached GameAssembly initialization and hit it).
        if (op2 == 0xC2 && p.rep == 0 && !p.osize16) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 srcLo = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            U64 srcHi = m.isReg ? xmm[m.rmIndex].hi : memory->readq(m.effAddr + 8);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U64 dstBits[2] = { xmm[m.regField].lo, xmm[m.regField].hi };
            U64 srcBits[2] = { srcLo, srcHi };
            U64 outBits[2] = { 0, 0 };
            auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
            for (U32 half = 0; half < 2; half++) {
                for (U32 lane = 0; lane < 2; lane++) {
                    U32 shift = lane * 32;
                    float a = u32f((U32)(dstBits[half] >> shift));
                    float b = u32f((U32)(srcBits[half] >> shift));
                    bool unordered = std::isnan(a) || std::isnan(b);
                    bool res;
                    switch (imm & 7) {
                        case 0: res = (a == b); break;
                        case 1: res = (a <  b); break;
                        case 2: res = (a <= b); break;
                        case 3: res = unordered; break;
                        case 4: res = unordered || (a != b); break;
                        case 5: res = unordered || !(a <  b); break;
                        case 6: res = unordered || !(a <= b); break;
                        default: res = !unordered; break;
                    }
                    if (res) outBits[half] |= 0xFFFFFFFFULL << shift;
                }
            }
            xmm[m.regField].lo = outBits[0];
            xmm[m.regField].hi = outBits[1];
            U32 used = opOff + 2 + m.length + 1; // +imm8
            rip += used;
            return used;
        }

        // CMPPD xmm, xmm/m128, imm8   66 0F C2 /r ib — packed double compare.
        // Each of the two qwords becomes all-ones/all-zero.
        if (op2 == 0xC2 && p.rep == 0 && p.osize16) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 dstBits[2] = { xmm[m.regField].lo, xmm[m.regField].hi };
            U64 srcBits[2];
            if (m.isReg) { srcBits[0] = xmm[m.rmIndex].lo; srcBits[1] = xmm[m.rmIndex].hi; }
            else { srcBits[0] = memory->readq(m.effAddr); srcBits[1] = memory->readq(m.effAddr + 8); }
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U64 outBits[2] = { 0, 0 };
            for (U32 lane = 0; lane < 2; lane++) {
                double a = u64ToDouble(dstBits[lane]);
                double b = u64ToDouble(srcBits[lane]);
                bool unordered = std::isnan(a) || std::isnan(b);
                bool res;
                switch (imm & 7) {
                    case 0: res = (a == b); break;
                    case 1: res = (a <  b); break;
                    case 2: res = (a <= b); break;
                    case 3: res = unordered; break;
                    case 4: res = unordered || (a != b); break;
                    case 5: res = unordered || !(a <  b); break;
                    case 6: res = unordered || !(a <= b); break;
                    default: res = !unordered; break;
                }
                outBits[lane] = res ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;
            }
            xmm[m.regField].lo = outBits[0];
            xmm[m.regField].hi = outBits[1];
            U32 used = opOff + 2 + m.length + 1; // +imm8
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

        // ---- SSE scalar single-precision FP ----
        //
        // Mirror image of the F2 0F double-precision block. F3 prefix +
        // 0F xx selects the scalar single (float) form. Touches only the
        // low 32 bits (one float) of the XMM register; bits 32..127 are
        // left untouched on reg/reg moves and zeroed on memory loads.
        auto u32ToFloat = [](U32 bits) -> float {
            float f; std::memcpy(&f, &bits, sizeof(f)); return f;
        };
        auto floatToU32 = [](float f) -> U32 {
            U32 bits; std::memcpy(&bits, &f, sizeof(bits)); return bits;
        };

        // MOVSS xmm, xmm/m32   F3 0F 10 /r
        // MOVSS xmm/m32, xmm   F3 0F 11 /r
        if ((op2 == 0x10 || op2 == 0x11) && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            bool isStore = (op2 == 0x11);
            if (isStore) {
                U32 v = (U32)(xmm[m.regField].lo & 0xFFFFFFFFULL);
                if (m.isReg) {
                    // reg/reg: replace low 32 bits, leave rest.
                    U64 keep = xmm[m.rmIndex].lo & 0xFFFFFFFF00000000ULL;
                    xmm[m.rmIndex].lo = keep | v;
                } else {
                    memory->writed(m.effAddr, v);
                }
            } else {
                U32 v;
                if (m.isReg) {
                    v = (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL);
                    U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
                    xmm[m.regField].lo = keep | v;
                    // .hi unchanged for reg/reg
                } else {
                    v = memory->readd(m.effAddr);
                    xmm[m.regField].lo = (U64)v; // zero-extend; high32 of lo cleared
                    xmm[m.regField].hi = 0;      // memory form zeros high qword too
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Scalar single arithmetic:
        //   ADDSS  F3 0F 58
        //   MULSS  F3 0F 59
        //   SUBSS  F3 0F 5C
        //   DIVSS  F3 0F 5E
        //   SQRTSS F3 0F 51
        //   MINSS  F3 0F 5D
        //   MAXSS  F3 0F 5F
        if (p.rep == 0xF3 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            float a = u32ToFloat((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            float b = u32ToFloat(srcBits);
            float r;
            switch (op2) {
                case 0x58: r = a + b; break;
                case 0x59: r = a * b; break;
                case 0x5C: r = a - b; break;
                case 0x5E: r = a / b; break;
                case 0x51: r = std::sqrt(b); break; // SQRT uses src as input
                case 0x5D: r = (a < b) ? a : b; break;
                case 0x5F: r = (a > b) ? a : b; break;
                default:   r = a; break;
            }
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (U64)floatToU32(r);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSI2SS xmm, r/m32 (or r/m64 with REX.W)  F3 0F 2A /r
        if (op2 == 0x2A && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            float f;
            if (rexW) {
                S64 v = m.isReg ? (S64)reg[m.rmIndex].u64 : (S64)memory->readq(m.effAddr);
                f = (float)v;
            } else {
                S32 v = m.isReg ? (S32)reg[m.rmIndex].u32 : (S32)memory->readd(m.effAddr);
                f = (float)v;
            }
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (U64)floatToU32(f);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSS2SD xmm, xmm/m32   F3 0F 5A /r — widen single (src low 32 bits)
        // to double, writing the full low qword of dst. winex11/win32u hits
        // this converting single-precision metrics to double for layout math.
        if (op2 == 0x5A && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            double d = (double)u32ToFloat(srcBits);
            xmm[m.regField].lo = doubleToU64(d);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSS2SI  r32/r64, xmm/m32  F3 0F 2D /r
        // CVTTSS2SI r32/r64, xmm/m32  F3 0F 2C /r  — truncating variant
        // 2D follows MXCSR.RC; 2C truncates toward zero.
        if ((op2 == 0x2D || op2 == 0x2C) && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            float f = u32ToFloat(srcBits);
            U64 converted = cpu64ConvertFpToInt((double)f, rexW, op2 == 0x2C, mxcsr);
            if (rexW) reg[m.regField].setU64(converted);
            else      reg[m.regField].setU32((U32)converted);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CMPSS xmm, xmm/m32, imm8   F3 0F C2 /r ib — scalar single sibling of
        // CMPSD; writes an all-ones/all-zero 32-bit mask into dst's low 32 bits
        // (rest of the low qword preserved). Same imm8 predicates as CMPSD.
        if (op2 == 0xC2 && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            float a = u32ToFloat((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            float b = u32ToFloat(srcBits);
            bool unordered = std::isnan(a) || std::isnan(b);
            bool res;
            switch (imm & 7) {
                case 0: res = (a == b); break;
                case 1: res = (a <  b); break;
                case 2: res = (a <= b); break;
                case 3: res = unordered; break;
                case 4: res = unordered || (a != b); break;
                case 5: res = unordered || !(a <  b); break;
                case 6: res = unordered || !(a <= b); break;
                default: res = !unordered; break;
            }
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (res ? 0xFFFFFFFFULL : 0ULL);
            U32 used = opOff + 2 + m.length + 1; // +imm8
            rip += used;
            return used;
        }

        // UNPCKLPD xmm, xmm/m128  66 0F 14 /r — interleave low qwords.
        //   dst.hi = src.lo;  dst.lo unchanged.
        // Used by glibc/Wine to assemble a {lo, hi} pair of doubles from
        // two separate xmm.lo halves.
        if (op2 == 0x14 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            xmm[m.regField].hi = sLo;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UNPCKHPD xmm, xmm/m128  66 0F 15 /r — interleave high qwords.
        //   dst.lo = dst.hi;  dst.hi = src.hi.
        // Used by musl libm to pull the high double out of a packed pair
        // for follow-up scalar operations.
        if (op2 == 0x15 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sHi;
            if (m.isReg) sHi = xmm[m.rmIndex].hi;
            else         sHi = memory->readq(m.effAddr + 8);
            xmm[m.regField].lo = xmm[m.regField].hi;
            xmm[m.regField].hi = sHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // MOVLPD/MOVLPS xmm, m64  66 0F 12 /r  /  0F 12 /r — load m64 to
        //   xmm.lo, leave xmm.hi unchanged.
        // MOVHPD/MOVHPS xmm, m64  66 0F 16 /r  /  0F 16 /r — load m64 to
        //   xmm.hi, leave xmm.lo unchanged.
        // MOVDDUP        xmm, xmm/m64  F2 0F 12 /r — load m64, broadcast to
        //   BOTH halves of xmm. ld.so + libm use this heavily for scalar
        //   doubles fed into packed math.
        // Register-form 66 0F 12 /r and 0F 12 /r is MOVHLPS/MOVLPS-reg
        //   (dst.lo = src.hi for 0F 12 reg; dst.lo = src.lo for 66 0F 12 reg).
        if ((op2 == 0x12 || op2 == 0x16) && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 val;
            if (m.isReg) {
                // 0F 12 reg-form is MOVHLPS: dst.lo = src.hi
                // 66 0F 12 reg-form is MOVLPD (treated as MOVAPD low): dst.lo = src.lo
                // 0F 16 reg-form is MOVLHPS: dst.hi = src.lo
                // 66 0F 16 reg-form is MOVHPD (treated as MOVAPD high): dst.hi = src.hi
                if (op2 == 0x12) val = osize66 ? xmm[m.rmIndex].lo : xmm[m.rmIndex].hi;
                else             val = osize66 ? xmm[m.rmIndex].hi : xmm[m.rmIndex].lo;
            } else {
                val = memory->readq(m.effAddr);
            }
            if (op2 == 0x12) xmm[m.regField].lo = val;
            else             xmm[m.regField].hi = val;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x12 && p.rep == 0xF2) {
            // MOVDDUP: load m64 (or src.lo if reg), duplicate to both halves
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 val = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            xmm[m.regField].lo = val;
            xmm[m.regField].hi = val;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // MOVLPD m64, xmm  66 0F 13 /r  /  0F 13 /r — store xmm.lo to m64.
        // MOVHPD m64, xmm  66 0F 17 /r  /  0F 17 /r — store xmm.hi to m64.
        // (No register form for 0F 13 / 17.)
        if ((op2 == 0x13 || op2 == 0x17) && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                U64 val = (op2 == 0x13) ? xmm[m.regField].lo : xmm[m.regField].hi;
                memory->writeq(m.effAddr, val);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UNPCKLPS xmm, xmm/m128  0F 14 /r — interleave 32-bit floats from
        // low half: dst = {dst[0], src[0], dst[1], src[1]} (each 32-bit).
        // UNPCKHPS xmm, xmm/m128  0F 15 /r — from high half.
        if ((op2 == 0x14 || op2 == 0x15) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else         { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            U64 dLo = xmm[m.regField].lo, dHi = xmm[m.regField].hi;
            U32 d[4] = {(U32)dLo,(U32)(dLo>>32),(U32)dHi,(U32)(dHi>>32)};
            U32 s[4] = {(U32)sLo,(U32)(sLo>>32),(U32)sHi,(U32)(sHi>>32)};
            U32 r[4];
            if (op2 == 0x14) { r[0]=d[0]; r[1]=s[0]; r[2]=d[1]; r[3]=s[1]; }
            else             { r[0]=d[2]; r[1]=s[2]; r[2]=d[3]; r[3]=s[3]; }
            xmm[m.regField].lo = (U64)r[0] | ((U64)r[1] << 32);
            xmm[m.regField].hi = (U64)r[2] | ((U64)r[3] << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- Packed FP↔int converts ----
        //
        // All five variants share opcode bytes 0F 5B (FP conversions) or
        // 0F E6 (the F2/F3-specific double-precision variants), and the
        // prefix selects which form. Source is xmm/m128, dest is xmm.

        // CVTDQ2PS   0F 5B /r        — 4× S32 → 4× float
        // CVTPS2DQ   66 0F 5B /r     — 4× float → 4× S32 (round to nearest)
        // CVTTPS2DQ  F3 0F 5B /r     — 4× float → 4× S32 (truncate toward zero)
        if (op2 == 0x5B) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else         { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            U32 dwords[4] = { (U32)sLo, (U32)(sLo >> 32), (U32)sHi, (U32)(sHi >> 32) };
            U32 nDw[4];
            if (!osize66 && p.rep == 0) {
                // CVTDQ2PS: dwords are signed ints, write float bits.
                for (int i = 0; i < 4; i++) {
                    float f = (float)(S32)dwords[i];
                    U32 bits; std::memcpy(&bits, &f, 4); nDw[i] = bits;
                }
            } else if (osize66) {
                // CVTPS2DQ: dwords are float bits, use MXCSR.RC and the
                // integer-indefinite result for NaN, infinity, or overflow.
                for (int i = 0; i < 4; i++) {
                    float f; std::memcpy(&f, &dwords[i], 4);
                    nDw[i] = (U32)cpu64ConvertFpToInt((double)f, false, false, mxcsr);
                }
            } else if (p.rep == 0xF3) {
                // CVTTPS2DQ: float → int truncating.
                for (int i = 0; i < 4; i++) {
                    float f; std::memcpy(&f, &dwords[i], 4);
                    nDw[i] = (U32)cpu64ConvertFpToInt((double)f, false, true, mxcsr);
                }
            } else {
                goto unhandled; // F2 0F 5B is not defined
            }
            xmm[m.regField].lo = ((U64)nDw[0]) | (((U64)nDw[1]) << 32);
            xmm[m.regField].hi = ((U64)nDw[2]) | (((U64)nDw[3]) << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTDQ2PD  F3 0F E6 /r — low 2× S32 → 2× f64
        // CVTPD2DQ  F2 0F E6 /r — 2× f64 → low 2× S32 (rounded), hi qword = 0
        // CVTTPD2DQ 66 0F E6 /r — 2× f64 → low 2× S32 (truncate), hi qword = 0
        // fontconfig/freetype emit the 66-prefixed truncating form heavily.
        if (op2 == 0xE6 && (p.rep == 0xF3 || p.rep == 0xF2 || osize66)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else         { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            if (p.rep == 0xF3) {
                // CVTDQ2PD: take low 2 dwords of src as S32, write 2 f64s.
                S32 i0 = (S32)(U32)sLo;
                S32 i1 = (S32)(U32)(sLo >> 32);
                double d0 = (double)i0, d1 = (double)i1;
                U64 b0, b1;
                std::memcpy(&b0, &d0, 8); std::memcpy(&b1, &d1, 8);
                xmm[m.regField].lo = b0;
                xmm[m.regField].hi = b1;
            } else {
                // CVTPD2DQ (F2, round) / CVTTPD2DQ (66, truncate):
                // 2 f64s → 2 S32s in low qword, high qword zeroed.
                double d0, d1;
                std::memcpy(&d0, &sLo, 8); std::memcpy(&d1, &sHi, 8);
                U32 i0, i1;
                bool truncate = osize66;
                i0 = (U32)cpu64ConvertFpToInt(d0, false, truncate, mxcsr);
                i1 = (U32)cpu64ConvertFpToInt(d1, false, truncate, mxcsr);
                xmm[m.regField].lo = ((U64)i0) | (((U64)i1) << 32);
                xmm[m.regField].hi = 0;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // MOVNTI m32/m64, r32/r64  0F C3 /r  — non-temporal store of GPR.
        // glibc's memset/memcpy hot paths use this to bypass cache. We
        // treat it as a plain mov to memory; the cache-bypass hint is a
        // perf nicety the interpreter doesn't model.
        if (op2 == 0xC3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                U32 width = rexW ? 8 : 4;
                U64 v = rexW ? reg[m.regField].u64 : (U64)reg[m.regField].u32;
                if (width == 8) memory->writeq(m.effAddr, v);
                else            memory->writed(m.effAddr, (U32)v);
            }
            // reg/reg form of 0F C3 is illegal — ignore.
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UCOMISS xmm, xmm/m32   0F 2E /r   (no 66, no F2/F3 prefix)
        // COMISS  xmm, xmm/m32   0F 2F /r
        if ((op2 == 0x2E || op2 == 0x2F) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            float a = u32ToFloat((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            float b = u32ToFloat(srcBits);
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

    // 0F AE group: FXSAVE(/0) FXRSTOR(/1) LDMXCSR(/2) STMXCSR(/3) and the
    // mod==11 register forms MFENCE/LFENCE/SFENCE (/5,/6,/7).
    //
    // FXSAVE/FXRSTOR MUST round-trip the full legacy x87/SSE state: glibc's lazy PLT
    // resolver _dl_runtime_resolve_fxsave does FXSAVE before calling
    // _dl_fixup (which clobbers XMM0-7 internally) and FXRSTOR after, so the
    // caller's float/double arguments survive symbol resolution. When these
    // were no-ops, the FIRST call to any float-taking function through the
    // PLT (e.g. printf("%.5f", x)) saw XMM0 clobbered -> the argument read as
    // 0.0 -> "0.00000"; later calls bound the GOT and skipped the resolver,
    // so they were correct. That "first %f prints 0" was the real bug (NOT
    // x87 80-bit precision). The legacy 512-byte layout is used in 64-bit
    // mode; FIP/FDP are written as zero because this interpreter does not
    // track those pointers.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0xAE) {
        U8 aeModrm = fetchByte(rip + opOff + 2);
        U8 aeReg = (aeModrm >> 3) & 7;
        U8 aeMod = (aeModrm >> 6) & 3;
        ModRM m = decodeModRM(rip + opOff + 2, p, 0);
        U32 used = opOff + 2 + m.length;
        if (aeMod != 3 && aeReg == 0) {
            // FXSAVE m512byte legacy 64-bit layout.
            memory->writew(m.effAddr + 0, (U16)fpu.cw);
            memory->writew(m.effAddr + 2, (U16)fpu.SW());
            U8 abridgedTag = 0;
            for (U32 i = 0; i < 8; i++) {
                if (fpu.isMMXInUse || fpu.tags[i] != TAG_Empty)
                    abridgedTag |= (U8)(1U << i);
            }
            memory->writeb(m.effAddr + 4, abridgedTag);
            memory->writeb(m.effAddr + 5, 0);
            memory->writew(m.effAddr + 6, (U16)(fpu.envData[1] & 0x7FF));
            memory->writeq(m.effAddr + 8, 0);  // FIP: not tracked
            memory->writeq(m.effAddr + 16, 0); // FDP: not tracked
            memory->writed(m.effAddr + 24, mxcsr);
            memory->writed(m.effAddr + 28, 0xFFFF);
            for (int i = 0; i < 8; i++) {
                U64 low, high;
                fpu.ST80(fpu.STV(i), &low, &high);
                U64 slot = m.effAddr + 32 + i * 16;
                memory->writeq(slot, low);
                memory->writew(slot + 8, (U16)high);
                memory->writew(slot + 10, 0);
                memory->writed(slot + 12, 0);
            }
            for (int i = 0; i < 16; i++) {
                memory->writeq(m.effAddr + 160 + i * 16,     xmm[i].lo);
                memory->writeq(m.effAddr + 160 + i * 16 + 8, xmm[i].hi);
            }
            for (U64 offset = 416; offset < 512; offset += 8)
                memory->writeq(m.effAddr + offset, 0);
        } else if (aeMod != 3 && aeReg == 1) {
            // FXRSTOR m512byte restores MXCSR, x87 stack/control state and
            // all 16 XMM registers from the legacy 64-bit layout.
            U32 savedMxcsr = memory->readd(m.effAddr + 24);
            if (savedMxcsr & 0xFFFF0000) {
                if (raiseSyncFault(K_SIGSEGV, /*trapNo #GP*/13, 0, rip)) return 0;
                if (thread) thread->process->signalProcess(K_SIGSEGV);
                yield = true;
                return 0;
            }
            fpu.SetCW(memory->readw(m.effAddr + 0));
            fpu.SetSW(memory->readw(m.effAddr + 2));
            fpu.SetTagFromAbridged(memory->readb(m.effAddr + 4));
            fpu.isMMXInUse = false;
            fpu.envData[0] = 0;
            fpu.envData[1] = memory->readw(m.effAddr + 6) & 0x7FF;
            fpu.envData[2] = 0;
            fpu.envData[3] = 0;
            for (int i = 0; i < 8; i++) {
                U64 slot = m.effAddr + 32 + i * 16;
                fpu.LD80(fpu.STV(i), memory->readq(slot),
                         (S16)memory->readw(slot + 8));
            }
            mxcsr = savedMxcsr;
            for (int i = 0; i < 16; i++) {
                xmm[i].lo = memory->readq(m.effAddr + 160 + i * 16);
                xmm[i].hi = memory->readq(m.effAddr + 160 + i * 16 + 8);
            }
        } else if (aeMod != 3 && aeReg == 2) {
            U32 loadedMxcsr = memory->readd(m.effAddr);
            if (loadedMxcsr & 0xFFFF0000) {
                if (raiseSyncFault(K_SIGSEGV, /*trapNo #GP*/13, 0, rip)) return 0;
                if (thread) thread->process->signalProcess(K_SIGSEGV);
                yield = true;
                return 0;
            }
            mxcsr = loadedMxcsr;
        } else if (aeMod != 3 && aeReg == 3) {
            memory->writed(m.effAddr, mxcsr);
        }
        // Memory ordering for the mod==11 fences is moot in this single-step
        // interpreter; MXCSR is handled above.
        rip += used;
        return used;
    }

    // x87 FPU minimal subset. Reuses the shared FPU class (common/fpu.h) for
    // register-side state — we drive memory I/O through CPU64::memory ourselves
    // because the FPU::FLD_F64_EA family takes a 32-bit CPU* that we can't
    // satisfy. ModR/M "reg" field is the sub-opcode (/0../7) for memory forms;
    // for register forms (mod==11) the low 3 bits identify ST(i).
dsp_59:
    if (op == 0xD8 || op == 0xD9 || op == 0xDA || op == 0xDB ||
        op == 0xDC || op == 0xDD || op == 0xDE || op == 0xDF) {
        U8 modrmByte = fetchByte(rip + opOff + 1);
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U32 used = opOff + 1 + m.length;
        U8 sub = m.regField & 7; // x87 sub-opcode, ignore REX.R
        auto x87Value = [&]() -> double {
            return fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                             : (double)fpu.getF64(fpu.top);
        };
        auto storeX87Environment = [&](U64 addr) {
            memory->writed(addr + 0, fpu.CW());
            memory->writed(addr + 4, fpu.SW());
            memory->writed(addr + 8, (U32)fpu.GetTag(nullptr));
            for (int n = 0; n < 4; n++)
                memory->writed(addr + 12 + n * 4, fpu.envData[n]);
        };
        auto loadX87Environment = [&](U64 addr) {
            U32 control = memory->readd(addr + 0);
            fpu.sw = memory->readd(addr + 4);
            fpu.SetTag(memory->readd(addr + 8));
            for (int n = 0; n < 4; n++)
                fpu.envData[n] = memory->readd(addr + 12 + n * 4);
            fpu.SetCW((U16)control);
            fpu.top = (fpu.sw >> 11) & 7;
        };
        auto truncateX87Integer = [](double value, U32 bits) -> U64 {
            double lower = bits == 64 ? -0x1p63 : -(double)(1ULL << (bits - 1));
            double upperExclusive = bits == 64 ? 0x1p63 : (double)(1ULL << (bits - 1));
            if (!std::isfinite(value) || value < lower || value >= upperExclusive)
                return 1ULL << (bits - 1); // masked invalid conversion: integer indefinite
            S64 truncated = (S64)std::trunc(value);
            return (U64)truncated & (bits == 64 ? ~0ULL : ((1ULL << bits) - 1));
        };
        if (!m.isReg) {
            // memory form
            if (op == 0xD9 && sub == 0) {
                // FLD m32fp
                U32 bits = memory->readd(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_F32(bits, fpu.STV(0));
            } else if (op == 0xD8 && sub == 0) {
                // FADD m32fp — ST(0) += float32 at memory
                U32 bits = memory->readd(m.effAddr);
                fpu.FLD_F32(bits, 8);
                fpu.FADD(fpu.STV(0), 8);
            } else if (op == 0xD8 && sub == 1) {
                // FMUL m32fp — ST(0) *= float32 at memory
                U32 bits = memory->readd(m.effAddr);
                fpu.FLD_F32(bits, 8);
                fpu.FMUL(fpu.STV(0), 8);
            } else if (op == 0xD8 && sub == 4) {
                // FSUB m32fp — ST(0) -= float32 at memory
                U32 bits = memory->readd(m.effAddr);
                fpu.FLD_F32(bits, 8);
                fpu.FSUB(fpu.STV(0), 8);
            } else if (op == 0xD8 && sub == 5) {
                // FSUBR m32fp — ST(0) = memory - ST(0).
                fpu.FLD_F32(memory->readd(m.effAddr), 8);
                fpu.FSUBR(fpu.STV(0), 8);
            } else if (op == 0xD8 && sub == 6) {
                // FDIV m32fp — ST(0) /= float32 at memory
                U32 bits = memory->readd(m.effAddr);
                fpu.FLD_F32(bits, 8);
                fpu.FDIV(fpu.STV(0), 8);
            } else if (op == 0xD8 && sub == 7) {
                // FDIVR m32fp — ST(0) = memory / ST(0).
                fpu.FLD_F32(memory->readd(m.effAddr), 8);
                fpu.FDIVR(fpu.STV(0), 8);
            } else if (op == 0xD9 && sub == 2) {
                // FST m32fp — convert TOS to f32, store, no pop
                double d;
                if (fpu.isRegCached[fpu.top]) {
                    d = fpu.regCache[fpu.top].d;
                } else {
                    d = (double)fpu.getF64(fpu.top);
                }
                union { float f; U32 i; } u; u.f = (float)d;
                memory->writed(m.effAddr, u.i);
            } else if (op == 0xD9 && sub == 3) {
                // FSTP m32fp — convert TOS to f32, store, pop
                double d;
                if (fpu.isRegCached[fpu.top]) {
                    d = fpu.regCache[fpu.top].d;
                } else {
                    d = (double)fpu.getF64(fpu.top);
                }
                union { float f; U32 i; } u; u.f = (float)d;
                memory->writed(m.effAddr, u.i);
                fpu.FPOP();
            } else if (op == 0xDD && sub == 2) {
                // FST m64fp — store TOS without popping.
                U64 bits;
                if (fpu.isRegCached[fpu.top]) {
                    bits = fpu.regCache[fpu.top].l;
                } else {
                    union { double d; U64 u; } u; u.d = fpu.getF64(fpu.top);
                    bits = u.u;
                }
                memory->writeq(m.effAddr, bits);
            } else if (op == 0xD9 && sub == 4) {
                // FLDENV m28byte — 64-bit environment operand layout.
                loadX87Environment(m.effAddr);
            } else if (op == 0xD9 && sub == 6) {
                // FNSTENV m28byte stores first, then masks all exceptions.
                storeX87Environment(m.effAddr);
                fpu.SetCW((U16)(fpu.CW() | 0x003F));
            } else if (op == 0xD9 && sub == 5) {
                // D9 /5 = FLDCW m16 — load FPU control word. Our soft FPU
                // honors the rounding bits inside FROUND; we store the
                // raw value via cwSet for the few callers that read it
                // back via FNSTCW.
                U16 cw = (U16)memory->readw(m.effAddr);
                fpu.SetCW(cw);
            } else if (op == 0xD9 && sub == 7) {
                // D9 /7 = FNSTCW m16 — store FPU control word. Always
                // succeeds; no exception checks because we don't model
                // FPU exception bits.
                memory->writew(m.effAddr, (U16)fpu.CW());
            } else if (op == 0xDD && sub == 7) {
                // DD /7 = FNSTSW m16 — store status word to memory.
                memory->writew(m.effAddr, (U16)fpu.SW());
            } else if (op == 0xDD && sub == 0) {
                // FLD m64fp
                U64 bits = memory->readq(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_F64(bits, fpu.STV(0));
            } else if (op == 0xDD && sub == 1) {
                // FISTTP m64int — truncate toward zero, then pop.
                memory->writeq(m.effAddr, truncateX87Integer(x87Value(), 64));
                fpu.FPOP();
            } else if (op == 0xDD && sub == 3) {
                // FSTP m64fp — store TOS bits, pop
                U64 bits;
                if (fpu.isRegCached[fpu.top]) {
                    bits = fpu.regCache[fpu.top].l;
                } else {
                    union { double d; U64 u; } u; u.d = fpu.getF64(fpu.top);
                    bits = u.u;
                }
                memory->writeq(m.effAddr, bits);
                fpu.FPOP();
            } else if (op == 0xDD && sub == 4) {
                // FRSTOR m108byte — restore environment and all eight ST slots.
                loadX87Environment(m.effAddr);
                for (int n = 0; n < 8; n++) {
                    U64 slot = m.effAddr + 28 + n * 10;
                    fpu.LD80(fpu.STV(n), memory->readq(slot),
                             memory->readw(slot + 8));
                }
            } else if (op == 0xDD && sub == 6) {
                // FNSAVE m108byte — save then reinitialize the x87 state.
                storeX87Environment(m.effAddr);
                for (int n = 0; n < 8; n++) {
                    U64 low = 0, high = 0;
                    fpu.ST80(fpu.STV(n), &low, &high);
                    U64 slot = m.effAddr + 28 + n * 10;
                    memory->writeq(slot, low);
                    memory->writew(slot + 8, (U16)high);
                }
                fpu.FINIT();
            } else if (op == 0xDC && sub == 0) {
                // FADD m64fp — load operand into scratch slot 8, add to ST(0)
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FADD(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 1) {
                // FMUL m64fp
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FMUL(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 4) {
                // FSUB m64fp — ST(0) = ST(0) - m64
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FSUB(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 5) {
                // FSUBR m64fp — ST(0) = m64 - ST(0)
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FSUBR(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 6) {
                // FDIV m64fp — ST(0) = ST(0) / m64
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FDIV(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 7) {
                // FDIVR m64fp — ST(0) = m64 / ST(0)
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FDIVR(fpu.STV(0), 8);
            } else if (op == 0xDA && sub <= 7) {
                // DA /N with m32int operand:
                //   /0 FIADD, /1 FIMUL, /2 FICOM, /3 FICOMP,
                //   /4 FISUB, /5 FISUBR, /6 FIDIV, /7 FIDIVR.
                // Convert int32 to f64, then run the ST(0)-op variant via the
                // shared FPU helpers (which expect both operands in the FPU
                // stack), using scratch slot 8.
                S32 v = (S32)memory->readd(m.effAddr);
                fpu.FLD_I32(v, 8);
                if (sub == 0) {
                    fpu.FADD(fpu.STV(0), 8);
                } else if (sub == 1) {
                    fpu.FMUL(fpu.STV(0), 8);
                } else if (sub == 2 || sub == 3) {
                    // FICOM / FICOMP — set C3/C2/C0 from comparison
                    double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                                         : fpu.getF64(fpu.top);
                    double b = (double)v;
                    if (std::isnan(a) || std::isnan(b)) {
                        cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                    } else if (a == b) {
                        cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                    } else if (a < b) {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                    } else {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                    }
                    if (sub == 3) fpu.FPOP();
                } else if (sub == 4) {
                    fpu.FSUB(fpu.STV(0), 8);
                } else if (sub == 5) {
                    fpu.FSUBR(fpu.STV(0), 8);
                } else if (sub == 6) {
                    fpu.FDIV(fpu.STV(0), 8);
                } else /* sub == 7 */ {
                    fpu.FDIVR(fpu.STV(0), 8);
                }
            } else if (op == 0xDE && sub <= 7) {
                // DE /N with m16int operand: FIADD/FIMUL/FICOM/FICOMP/FISUB/FISUBR/FIDIV/FIDIVR
                // Same control flow as DA but the operand is signed 16-bit.
                S16 v = (S16)memory->readw(m.effAddr);
                fpu.FLD_I16(v, 8);
                if (sub == 0) {
                    fpu.FADD(fpu.STV(0), 8);
                } else if (sub == 1) {
                    fpu.FMUL(fpu.STV(0), 8);
                } else if (sub == 2 || sub == 3) {
                    double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                                         : fpu.getF64(fpu.top);
                    double b = (double)v;
                    if (std::isnan(a) || std::isnan(b)) {
                        cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                    } else if (a == b) {
                        cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                    } else if (a < b) {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                    } else {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                    }
                    if (sub == 3) fpu.FPOP();
                } else if (sub == 4) {
                    fpu.FSUB(fpu.STV(0), 8);
                } else if (sub == 5) {
                    fpu.FSUBR(fpu.STV(0), 8);
                } else if (sub == 6) {
                    fpu.FDIV(fpu.STV(0), 8);
                } else /* sub == 7 */ {
                    fpu.FDIVR(fpu.STV(0), 8);
                }
            } else if (op == 0xDB && sub == 0) {
                // FILD m32int — load signed 32-bit, push as f64
                S32 v = (S32)memory->readd(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_I32(v, fpu.STV(0));
            } else if (op == 0xDB && sub == 1) {
                // FISTTP m32int — truncate toward zero, then pop.
                memory->writed(m.effAddr, (U32)truncateX87Integer(x87Value(), 32));
                fpu.FPOP();
            } else if (op == 0xDB && (sub == 2 || sub == 3)) {
                // FIST m32int (sub=2) / FISTP m32int (sub=3) — round per CW
                S32 v;
                if (fpu.isRegCached[fpu.top]) {
                    v = (S32)fpu.FROUND(fpu.regCache[fpu.top].d);
                } else {
                    union { U64 u; double d; } u; u.d = fpu.getF64(fpu.top);
                    v = (S32)fpu.FROUND(u.d);
                }
                memory->writed(m.effAddr, (U32)v);
                if (sub == 3) fpu.FPOP();
            } else if (op == 0xDB && sub == 7) {
                // FSTP m80fp — store TOS as 80-bit extended precision, pop.
                // glibc's __printf_fp promotes double->long double here even
                // when the format spec is %f and spills the result via FSTP
                // m80 to generate high-precision digits. We MUST preserve all
                // 64 mantissa bits: route through FPU::ST80, which reads the
                // SoftFloat extFloat80_t register (FPU::getReg promotes a
                // cached double on the fly if useF64 ever left one there).
                // The old hand-rolled f64->f80 synthesis truncated to 52
                // mantissa bits and collapsed %.5f of pi to 0.00000.
                // 80-bit layout: 10 bytes LE, bytes 0..7 = explicit-bit
                // mantissa (signif), bytes 8..9 = sign(15)|biasedExp(14..0).
                U64 low; U64 high;
                fpu.ST80(fpu.top, &low, &high);
                memory->writeq(m.effAddr, low);
                memory->writew(m.effAddr + 8, (U16)high);
                fpu.FPOP();
            } else if (op == 0xDB && sub == 5) {
                // FLD m80fp — load 80-bit extended precision, push.
                // Inverse of FSTP m80; keep the full 80-bit value in the
                // SoftFloat register via FPU::LD80 instead of round-tripping
                // through double (which discarded the low mantissa bits).
                U64 low = memory->readq(m.effAddr);
                U16 high = (U16)memory->readw(m.effAddr + 8);
                fpu.PREP_PUSH();
                fpu.LD80(fpu.STV(0), low, high);
            } else if (op == 0xDF && sub == 0) {
                // FILD m16int — load signed 16-bit, push as f64
                S16 v = (S16)memory->readw(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_I16(v, fpu.STV(0));
            } else if (op == 0xDF && sub == 1) {
                // FISTTP m16int — truncate toward zero, then pop.
                memory->writew(m.effAddr, (U16)truncateX87Integer(x87Value(), 16));
                fpu.FPOP();
            } else if (op == 0xDF && (sub == 2 || sub == 3)) {
                // FIST m16int (sub=2) / FISTP m16int (sub=3)
                S16 v;
                if (fpu.isRegCached[fpu.top]) {
                    v = (S16)fpu.FROUND(fpu.regCache[fpu.top].d);
                } else {
                    union { U64 u; double d; } u; u.d = fpu.getF64(fpu.top);
                    v = (S16)fpu.FROUND(u.d);
                }
                memory->writew(m.effAddr, (U16)v);
                if (sub == 3) fpu.FPOP();
            } else if (op == 0xDF && sub == 5) {
                // FILD m64int — load signed 64-bit, push as f64
                S64 v = (S64)memory->readq(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_I64(v, fpu.STV(0));
            } else if (op == 0xDF && sub == 7) {
                // FISTP m64int — round per CW, store as i64, pop
                S64 v;
                if (fpu.isRegCached[fpu.top]) {
                    v = (S64)fpu.FROUND(fpu.regCache[fpu.top].d);
                } else {
                    union { U64 u; double d; } u; u.d = fpu.getF64(fpu.top);
                    v = (S64)fpu.FROUND(u.d);
                }
                memory->writeq(m.effAddr, (U64)v);
                fpu.FPOP();
            } else if ((op == 0xD8 || op == 0xDC) && (sub == 2 || sub == 3)) {
                // FCOM/FCOMP m32fp or m64fp — compare ST(0) to memory operand,
                // set C0/C2/C3. We inline because the FPU::FCOM helper takes
                // CPU*. Then pop if sub==3.
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                                     : fpu.getF64(fpu.top);
                double b;
                if (op == 0xD8) {
                    union { U32 i; float f; } u; u.i = memory->readd(m.effAddr);
                    b = (double)u.f;
                } else {
                    union { U64 i; double d; } u; u.i = memory->readq(m.effAddr);
                    b = u.d;
                }
                // FCOM result: C3=1/C2=1/C0=1 for unordered (NaN), C3=1/C2=0/C0=0
                // for equal, C3=0/C2=0/C0=1 for ST(0)<src, C3=0/C2=0/C0=0 for >.
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                if (sub == 3) fpu.FPOP();
            } else {
                goto unhandled;
            }
            rip += used;
            return used;
        } else {
            // register form (mod==11)
            U8 i = modrmByte & 7; // ST(i) selector
            U8 x87RegGroup = modrmByte & 0xF8;
            if ((op == 0xDA || op == 0xDB) &&
                (x87RegGroup == 0xC0 || x87RegGroup == 0xC8 ||
                 x87RegGroup == 0xD0 || x87RegGroup == 0xD8)) {
                // FCMOVcc ST(0),ST(i). Conditions are B/E/BE/U for DA and
                // their inverses NB/NE/NBE/NU for DB.
                U8 condition = x87RegGroup == 0xC0 ? 2
                             : x87RegGroup == 0xC8 ? 4
                             : x87RegGroup == 0xD0 ? 6 : 10;
                if (op == 0xDB) condition++;
                if (evalCC(condition)) fpu.FST(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xD9 && (modrmByte & 0xF8) == 0xC0) {
                // D9 C0+i = FLD ST(i)
                U32 src = fpu.STV(i);
                fpu.PREP_PUSH();
                U32 dst = fpu.STV(0);
                // duplicate src into dst. F64 fast path stores bits + cache flag.
                fpu.regCache[dst] = fpu.regCache[src];
                fpu.regs[dst] = fpu.regs[src];
                fpu.isRegCached[dst] = fpu.isRegCached[src];
                fpu.tags[dst] = TAG_Valid;
            } else if (op == 0xD9 && (modrmByte & 0xF8) == 0xC8) {
                // D9 C8+i = FXCH ST(0),ST(i)
                fpu.FXCH(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD9 && modrmByte == 0xE0) {
                // D9 E0 = FCHS — negate ST(0)
                fpu.FCHS();
            } else if (op == 0xD9 && modrmByte == 0xE1) {
                // D9 E1 = FABS — absolute value of ST(0)
                fpu.FABS();
            } else if (op == 0xD9 && modrmByte == 0xE4) {
                // D9 E4 = FTST — compare ST(0) to zero in x87 condition codes.
                fpu.FTST(nullptr);
            } else if (op == 0xD9 && modrmByte == 0xE5) {
                // D9 E5 = FXAM — classify ST(0) into C3/C2/C0 and sign into C1.
                fpu.FXAM();
            } else if (op == 0xD9 && modrmByte == 0xE8) {
                // D9 E8 = FLD1 — push +1.0 (FPU::FLD1 does PREP_PUSH itself)
                fpu.FLD1();
            } else if (op == 0xD9 && modrmByte == 0xE9) {
                // D9 E9 = FLDL2T — push log2(10)
                fpu.FLDL2T();
            } else if (op == 0xD9 && modrmByte == 0xEA) {
                // D9 EA = FLDL2E — push log2(e)
                fpu.FLDL2E();
            } else if (op == 0xD9 && modrmByte == 0xEB) {
                // D9 EB = FLDPI - push pi
                fpu.FLDPI();
            } else if (op == 0xD9 && modrmByte == 0xEC) {
                // D9 EC = FLDLG2 — push log10(2)
                fpu.FLDLG2();
            } else if (op == 0xD9 && modrmByte == 0xED) {
                // D9 ED = FLDLN2 — push ln(2)
                fpu.FLDLN2();
            } else if (op == 0xD9 && modrmByte == 0xEE) {
                // D9 EE = FLDZ — push +0.0 (FPU::FLDZ does PREP_PUSH itself)
                fpu.FLDZ();
            } else if (op == 0xD9 && modrmByte == 0xF5) {
                // D9 F5 = FPREM1 - partial remainder (IEEE)
                fpu.FPREM1();
            } else if (op == 0xD9 && modrmByte == 0xF6) {
                // D9 F6 = FDECSTP
                fpu.FDECSTP();
            } else if (op == 0xD9 && modrmByte == 0xF7) {
                // D9 F7 = FINCSTP
                fpu.FINCSTP();
            } else if (op == 0xD9 && modrmByte == 0xF0) {
                // D9 F0 = F2XM1 — 2^x - 1
                fpu.F2XM1();
            } else if (op == 0xD9 && modrmByte == 0xF1) {
                // D9 F1 = FYL2X — y * log2(x)
                fpu.FYL2X();
            } else if (op == 0xD9 && modrmByte == 0xF2) {
                // D9 F2 = FPTAN — partial tangent
                fpu.FPTAN();
            } else if (op == 0xD9 && modrmByte == 0xF3) {
                // D9 F3 = FPATAN — arctan(y/x)
                fpu.FPATAN();
            } else if (op == 0xD9 && modrmByte == 0xF4) {
                // D9 F4 = FXTRACT — extract exponent and significand
                fpu.FXTRACT();
            } else if (op == 0xD9 && modrmByte == 0xFA) {
                // D9 FA = FSQRT
                fpu.FSQRT();
            } else if (op == 0xD9 && modrmByte == 0xF8) {
                // D9 F8 = FPREM — partial remainder
                fpu.FPREM();
            } else if (op == 0xD9 && modrmByte == 0xF9) {
                // D9 F9 = FYL2XP1 — y * log2(x+1)
                fpu.FYL2XP1();
            } else if (op == 0xD9 && modrmByte == 0xFB) {
                // D9 FB = FSINCOS - ST(0)=sin(x), push cos(x)
                fpu.FSINCOS();
            } else if (op == 0xD9 && modrmByte == 0xFC) {
                // D9 FC = FRNDINT — round to integer
                fpu.FRNDINT();
            } else if (op == 0xD9 && modrmByte == 0xFD) {
                // D9 FD = FSCALE — scale by power of 2
                fpu.FSCALE();
            } else if (op == 0xD9 && modrmByte == 0xFE) {
                // D9 FE = FSIN — sine of ST(0), result in ST(0)
                fpu.FSIN();
            } else if (op == 0xD9 && modrmByte == 0xFF) {
                // D9 FF = FCOS — cosine of ST(0), result in ST(0)
                fpu.FCOS();
            } else if (op == 0xDF && modrmByte == 0xE0) {
                // DF E0 = FNSTSW AX — write status word into AX, preserving
                // the upper 48 bits of RAX (per AMD64 manual).
                reg[X64_RAX].setU16((U16)fpu.SW());
            } else if (op == 0xDD && (modrmByte & 0xF8) == 0xC0) {
                // DD C0+i = FFREE ST(i) — mark ST(i) as empty (TAG_Empty).
                fpu.tags[fpu.STV(i)] = TAG_Empty;
                fpu.isRegCached[fpu.STV(i)] = false;
            } else if (op == 0xDF && (modrmByte & 0xF8) == 0xC0) {
                // DF C0+i = FFREEP ST(i) — free the slot and pop ST(0).
                fpu.FFREE_STi(fpu.STV(i));
                fpu.FPOP();
            } else if (op == 0xDD && (modrmByte & 0xF8) == 0xD0) {
                // DD D0+i = FST ST(i) — copy ST(0) into ST(i), no pop.
                U32 src = fpu.STV(0);
                U32 dst = fpu.STV(i);
                fpu.regCache[dst] = fpu.regCache[src];
                fpu.regs[dst] = fpu.regs[src];
                fpu.isRegCached[dst] = fpu.isRegCached[src];
                fpu.tags[dst] = TAG_Valid;
            } else if (op == 0xDD && (modrmByte & 0xF8) == 0xD8) {
                // DD D8+i = FSTP ST(i) — copy ST(0) into ST(i), then pop.
                U32 src = fpu.STV(0);
                U32 dst = fpu.STV(i);
                fpu.regCache[dst] = fpu.regCache[src];
                fpu.regs[dst] = fpu.regs[src];
                fpu.isRegCached[dst] = fpu.isRegCached[src];
                fpu.tags[dst] = TAG_Valid;
                fpu.FPOP();
            } else if (op == 0xDD && ((modrmByte & 0xF8) == 0xE0 || (modrmByte & 0xF8) == 0xE8)) {
                // DD E0+i = FUCOM ST(i)  (sub-form E0)
                // DD E8+i = FUCOMP ST(i) (sub-form E8, pops afterwards)
                // Set C3/C2/C0 per the standard FCOM result; treat QNaN as
                // unordered (we already do the same for FCOM).
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                if ((modrmByte & 0xF8) == 0xE8) fpu.FPOP();
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xC0) {
                // DC C0+i = FADD ST(i), ST(0)
                fpu.FADD(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xC8) {
                // DC C8+i = FMUL ST(i), ST(0)
                fpu.FMUL(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xD0) {
                // DC D0+i = FCOM ST(i) — compare ST(0) with ST(i)
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si] ? fpu.regCache[si].d : fpu.getF64(si);
                cpu64_fpu_compare(fpu, a, b);
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xD8) {
                // DC D8+i = FCOMP ST(i) — compare, then pop
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si] ? fpu.regCache[si].d : fpu.getF64(si);
                cpu64_fpu_compare(fpu, a, b);
                fpu.FPOP();
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xE0) {
                // DC E0+i = FSUBR ST(i), ST(0) — ST(i) = ST(0) - ST(i)
                fpu.FSUBR(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xE8) {
                // DC E8+i = FSUB ST(i), ST(0) — ST(i) = ST(i) - ST(0)
                fpu.FSUB(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xF0) {
                // DC F0+i = FDIVR ST(i), ST(0) — ST(i) = ST(0) / ST(i)
                fpu.FDIVR(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xDC && (modrmByte & 0xF8) == 0xF8) {
                // DC F8+i = FDIV ST(i), ST(0) — ST(i) = ST(i) / ST(0)
                fpu.FDIV(fpu.STV(i), fpu.STV(0));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xC0) {
                // D8 C0+i = FADD ST(0), ST(i)
                fpu.FADD(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xC8) {
                // D8 C8+i = FMUL ST(0), ST(i)
                fpu.FMUL(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xE0) {
                // D8 E0+i = FSUB ST(0), ST(i)
                fpu.FSUB(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xE8) {
                // D8 E8+i = FSUBR ST(0), ST(i)
                fpu.FSUBR(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xF0) {
                // D8 F0+i = FDIV ST(0), ST(i)
                fpu.FDIV(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xF8) {
                // D8 F8+i = FDIVR ST(0), ST(i)
                fpu.FDIVR(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xD0) {
                // D8 D0+i = FCOM ST(0), ST(i) — set C0/C2/C3
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xD8) {
                // D8 D8+i = FCOMP ST(0), ST(i) — like FCOM, then pop
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                fpu.FPOP();
            } else if (op == 0xDE && (modrmByte & 0xC0) == 0xC0 && modrmByte != 0xD9) {
                // DE C0+i / C8+i / E0+i / E8+i / F0+i / F8+i — operate on
                // ST(i),ST(0), then pop. (DE D9 = FCOMPP is below.) The
                // shared FPU helpers expect (destSlot, srcSlot), so we
                // pass STV(i), STV(0).
                U32 si = fpu.STV(i);
                U32 s0 = fpu.STV(0);
                U8 grp = modrmByte & 0xF8;
                if (grp == 0xC0) {
                    fpu.FADD(si, s0);
                } else if (grp == 0xC8) {
                    fpu.FMUL(si, s0);
                } else if (grp == 0xE0) {
                    // FSUBRP: ST(i) = ST(0) - ST(i), pop
                    fpu.FSUBR(si, s0);
                } else if (grp == 0xE8) {
                    // FSUBP: ST(i) = ST(i) - ST(0), pop
                    fpu.FSUB(si, s0);
                } else if (grp == 0xF0) {
                    // FDIVRP: ST(i) = ST(0) / ST(i), pop
                    fpu.FDIVR(si, s0);
                } else if (grp == 0xF8) {
                    // FDIVP: ST(i) = ST(i) / ST(0), pop
                    fpu.FDIV(si, s0);
                } else {
                    goto unhandled;
                }
                fpu.FPOP();
            } else if (op == 0xDE && modrmByte == 0xD9) {
                // DE D9 = FCOMPP — compare ST(0) to ST(1), pop twice
                U32 s1 = fpu.STV(1);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[s1]      ? fpu.regCache[s1].d      : fpu.getF64(s1);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                fpu.FPOP();
                fpu.FPOP();
            } else if ((op == 0xDB || op == 0xDA) && ((modrmByte & 0xF8) == 0xF0 || (modrmByte & 0xF8) == 0xE8)) {
                // DB F0+i = FCOMI ST(0), ST(i) — set ZF/PF/CF in rflags.
                // DA F0+i = FUCOMI — same impl (we don't distinguish QNaN signaling).
                // DB E8+i = FUCOMI ST(0), ST(i). DA E8+i is reserved/invalid but
                // we accept it for symmetry; we don't distinguish signaling vs
                // quiet NaN here.
                // SDM table: unordered -> ZF=PF=CF=1; equal -> ZF=1, PF=CF=0;
                // ST(0)<ST(i) -> CF=1; ST(0)>ST(i) -> all zero. Other flags
                // (OF/SF/AF) are cleared per spec.
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                rflags &= ~(X64_CF | X64_PF | X64_ZF | X64_SF | X64_OF | X64_AF);
                if (std::isnan(a) || std::isnan(b)) {
                    rflags |= (X64_ZF | X64_PF | X64_CF);
                } else if (a == b) {
                    rflags |= X64_ZF;
                } else if (a < b) {
                    rflags |= X64_CF;
                }
                // a > b: no flags set
            } else if (op == 0xDF && ((modrmByte & 0xF8) == 0xE8 ||
                                      (modrmByte & 0xF8) == 0xF0)) {
                // DF E8+i = FUCOMIP; DF F0+i = FCOMIP. Compare then pop.
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                rflags &= ~(X64_CF | X64_PF | X64_ZF | X64_SF | X64_OF | X64_AF);
                if (std::isnan(a) || std::isnan(b)) {
                    rflags |= (X64_ZF | X64_PF | X64_CF);
                } else if (a == b) {
                    rflags |= X64_ZF;
                } else if (a < b) {
                    rflags |= X64_CF;
                }
                fpu.FPOP();
            } else if (op == 0xDA && modrmByte == 0xE9) {
                // DA E9 = FUCOMPP — compare ST(0) to ST(1), pop twice (same as FCOMPP for our purposes)
                U32 s1 = fpu.STV(1);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[s1]      ? fpu.regCache[s1].d      : fpu.getF64(s1);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                fpu.FPOP();
                fpu.FPOP();
            } else if (op == 0xDB && modrmByte == 0xE2) {
                // DB E2 = FNCLEX — clear the x87 exception flags. We don't model
                // FPU exception bits, so this just clears the relevant status
                // bits and is otherwise a no-op. winex11/win32u issues it before
                // FLDCW when setting up its rounding mode.
                fpu.SetSW(fpu.SW() & ~0x80ff); // clear ES + the 6 exception bits + SF
            } else if (op == 0xDB && modrmByte == 0xE3) {
                // DB E3 = FNINIT — reinitialize the x87 FPU to its default state.
                fpu.FINIT();
            } else if (op == 0xDB && (modrmByte == 0xE0 || modrmByte == 0xE1)) {
                // DB E0 = FNENI, DB E1 = FNDISI — 8087 enable/disable interrupt;
                // no-ops on all CPUs since the 80287. Accept and ignore.
            } else if (op == 0xD9 && modrmByte == 0xD0) {
                // D9 D0 = FNOP — explicit x87 no-op.
            } else {
                goto unhandled;
            }
            rip += used;
            return used;
        }
    }

unhandled:
    auto recoverInstructionLength = [&]() -> U32 {
        U32 pos = opOff;
        U8 first = op;
        U8 second = 0;
        U8 third = 0;
        U32 map = 0;
        if (first == 0x0F) {
            second = fetchByte(ipStart + pos + 1);
            if (second == 0x38 || second == 0x3A) {
                map = second == 0x38 ? 2 : 3;
                third = fetchByte(ipStart + pos + 2);
                pos += 3;
            } else {
                map = 1;
                pos += 2;
            }
        } else {
            pos++;
        }

        bool hasModrm = false;
        U32 immediate = 0;
        if (map == 2 || map == 3) {
            hasModrm = true;
            immediate = map == 3 ? 1 : 0;
        } else if (map == 1) {
            hasModrm = !((second >= 0x80 && second <= 0x8F) ||
                         second == 0x05 || second == 0x06 || second == 0x07 ||
                         second == 0x08 || second == 0x09 || second == 0x0B ||
                         (second >= 0x30 && second <= 0x35) || second == 0x37 ||
                         second == 0x77 || second == 0xA0 || second == 0xA1 ||
                         second == 0xA2 || second == 0xA8 || second == 0xA9 ||
                         second == 0xAA || (second >= 0xC8 && second <= 0xCF));
            if (second == 0x0F || second == 0xBA || second == 0xA4 ||
                second == 0xAC || second == 0xC2 || second == 0xC4 ||
                second == 0xC5 || second == 0xC6) immediate = 1;
        } else {
            hasModrm = (first <= 0x3B && (first & 7) <= 3) ||
                        first == 0x62 || first == 0x63 ||
                        (first >= 0x69 && first <= 0x6B) ||
                        (first >= 0x80 && first <= 0x8F) ||
                        first == 0xC0 || first == 0xC1 ||
                        (first >= 0xC4 && first <= 0xC7) ||
                        (first >= 0xD0 && first <= 0xD3) ||
                        (first >= 0xD8 && first <= 0xDF) ||
                        first == 0xF6 || first == 0xF7 || first == 0xFE || first == 0xFF;
            if ((first >= 0x70 && first <= 0x7F) || first == 0x6A ||
                first == 0x80 || first == 0x82 || first == 0x83 ||
                first == 0xA8 || first == 0xC0 || first == 0xC1 ||
                first == 0xC6 || first == 0xD4 || first == 0xD5 ||
                (first >= 0xE0 && first <= 0xE7) || first == 0xEB) immediate = 1;
            else if (first == 0x68 || first == 0x69 || first == 0x81 ||
                     first == 0xA9 || first == 0xC7 || first == 0xE8 || first == 0xE9)
                immediate = p.osize16 ? 2 : 4;
            else if (first >= 0xB8 && first <= 0xBF)
                immediate = (p.rex & 8) ? 8 : (p.osize16 ? 2 : 4);
            else if (first >= 0xB0 && first <= 0xB7) immediate = 1;
            if (first == 0xF6 || first == 0xF7) {
                U8 group = (fetchByte(ipStart + pos) >> 3) & 7;
                if (group <= 1) immediate = first == 0xF6 ? 1 : (p.osize16 ? 2 : 4);
            }
        }

        if (hasModrm) {
            U8 modrm = fetchByte(ipStart + pos++);
            U8 mod = modrm >> 6;
            U8 rm = modrm & 7;
            if (mod != 3 && rm == 4) {
                U8 sib = fetchByte(ipStart + pos++);
                if (mod == 0 && (sib & 7) == 5) pos += 4;
            }
            if (mod == 0 && rm == 5) pos += 4;
            else if (mod == 1) pos += 1;
            else if (mod == 2) pos += 4;
        }
        pos += immediate;
        return (pos <= 15 && pos > opOff) ? pos : 0;
    };

    if (bw64RecoverEnabled()) {
        U32 length = recoverInstructionLength();
        if (length && recoverFaultsAtRip < 3) {
            recoverFaultsAtRip++;
            bool firstAtRip = bw64RecoverClaimRip(64, ipStart);
            if (firstAtRip) {
                klog_fmt("recover: skipped CPU64 opcode at RIP=0x%llx len=%u bytes=%02x %02x %02x %02x",
                         (unsigned long long)ipStart, length,
                         fetchByte(ipStart), fetchByte(ipStart + 1),
                         fetchByte(ipStart + 2), fetchByte(ipStart + 3));
            }
            bw64RecoverNoteSkippedInstruction(firstAtRip);
            rip = ipStart + length;
            return length;
        }
        bw64RecoverNoteSigillFallback();
    }
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
    // BW64_UNIMPLDUMP: when a thread decodes a bogus opcode it has almost always
    // jumped to a garbage address (corruption / bad control transfer). Dump the
    // pid + GPRs + the top of the stack so we can see WHO called into here and
    // whether RIP/RSP are sane. Env-gated; off by default.
    if (std::getenv("BW64_UNIMPLDUMP")) {
        int pid = (thread && thread->process) ? (int)thread->process->id : -1;
        U64 rsp = reg[X64_RSP].u64;
        klog_fmt("  UNIMPLDUMP pid=%d rip=0x%llx rsp=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx",
                 pid, (unsigned long long)ipStart, (unsigned long long)rsp,
                 (unsigned long long)reg[X64_RAX].u64, (unsigned long long)reg[X64_RBX].u64,
                 (unsigned long long)reg[X64_RCX].u64, (unsigned long long)reg[X64_RDX].u64,
                 (unsigned long long)reg[X64_RSI].u64, (unsigned long long)reg[X64_RDI].u64,
                 (unsigned long long)reg[X64_RBP].u64);
        if (thread && thread->process) {
            klog_fmt("  UNIMPLDUMP exe='%s'", thread->process->exe.c_str());
        }
        // Top 8 stack qwords — the most recent one is usually the return address
        // of whoever CALLed into the bogus RIP.
        for (int i = 0; i < 8; i++) {
            U64 v = 0;
            if (memory) v = memory->readq(rsp + (U64)i * 8);
            klog_fmt("  UNIMPLDUMP   [rsp+0x%02x] = 0x%llx", i * 8, (unsigned long long)v);
        }
        // 32 bytes at RIP, and at the suspicious pointer registers, so we can see
        // whether RIP lands in real module code or a pattern-filled buffer.
        auto dumpMem = [&](const char* tag, U64 a) {
            if (!memory) return;
            U8 b[32];
            for (int i = 0; i < 32; i++) b[i] = memory->readb(a + (U64)i);
            klog_fmt("  UNIMPLDUMP %s @0x%llx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
                     tag, (unsigned long long)a,
                     b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7], b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],
                     b[16],b[17],b[18],b[19],b[20],b[21],b[22],b[23], b[24],b[25],b[26],b[27],b[28],b[29],b[30],b[31]);
        };
        dumpMem("RIP ", ipStart);
        dumpMem("RIP-", ipStart - 16);
        dumpMem("RBX ", reg[X64_RBX].u64);
        dumpMem("RBP ", reg[X64_RBP].u64);
    }
    return faultAtInstruction(K_SIGILL, 6);
}

void CPU64::run() {
    U64 nextPoll = (instructionCount | 0xFFFF) + 1;
    // Optional instruction tracer for bring-up debugging. Gated on env so it
    // never costs anything in normal runs. BOXEDWINE64_TRACE_FROM/TO bound the
    // instructionCount window; each traced step prints RIP + the leading
    // opcode bytes + the GPRs most relevant to pointer-corruption hunts.
    static const char* tf = std::getenv("BOXEDWINE64_TRACE_FROM");
    static const char* tt = std::getenv("BOXEDWINE64_TRACE_TO");
    U64 traceFrom = tf ? std::strtoull(tf, nullptr, 0) : (U64)-1;
    U64 traceTo   = tt ? std::strtoull(tt, nullptr, 0) : 0;
    bool tracing = (tf != nullptr);

    // BW64_WILDJUMP: arm a per-thread RIP-history ring that auto-dumps the
    // moment execution lands in low memory (a wild computed jump — e.g. the
    // freetype-load bug RIP=0x10xxx). The dump shows the last 32 (rip, opcode,
    // key GPRs) tuples so the *source* instruction that produced the bad target
    // (an indirect call/jmp/ret through a clobbered pointer) is visible. Cheap:
    // a 32-entry ring updated per step; the threshold check is one compare.
    static const bool wildJump = std::getenv("BW64_WILDJUMP") != nullptr;
    const U64 WILD_LO = 0x100000ull;   // anything below 1MB is never real code
    struct RipHist { U64 rip, rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp; U8 b0,b1,b2,b3; };
    RipHist ring[32] = {};
    int ringPos = 0;
    bool wildFired = false;

    while (!yield) {
        if (tracing && instructionCount >= traceFrom && instructionCount <= traceTo) {
            U64 r = rip;
            klog_fmt("TRACE #%llu RIP=0x%llx %02x %02x %02x %02x  "
                     "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx "
                     "xmm0=%llx:%llx xmm1=%llx:%llx",
                     (unsigned long long)instructionCount, (unsigned long long)r,
                     fetchByte(r), fetchByte(r+1), fetchByte(r+2), fetchByte(r+3),
                     (unsigned long long)reg[X64_RAX].u64, (unsigned long long)reg[X64_RBX].u64,
                     (unsigned long long)reg[X64_RCX].u64, (unsigned long long)reg[X64_RDX].u64,
                     (unsigned long long)reg[X64_RSI].u64, (unsigned long long)reg[X64_RDI].u64,
                     (unsigned long long)reg[X64_RSP].u64, (unsigned long long)reg[X64_RBP].u64,
                     (unsigned long long)xmm[0].hi, (unsigned long long)xmm[0].lo,
                     (unsigned long long)xmm[1].hi, (unsigned long long)xmm[1].lo);
        }
        if (wildJump) {
            RipHist& h = ring[ringPos];
            h.rip = rip;
            h.rax = reg[X64_RAX].u64; h.rbx = reg[X64_RBX].u64;
            h.rcx = reg[X64_RCX].u64; h.rdx = reg[X64_RDX].u64;
            h.rsi = reg[X64_RSI].u64; h.rdi = reg[X64_RDI].u64;
            h.rsp = reg[X64_RSP].u64; h.rbp = reg[X64_RBP].u64;
            h.b0 = fetchByte(rip);   h.b1 = fetchByte(rip+1);
            h.b2 = fetchByte(rip+2); h.b3 = fetchByte(rip+3);
            ringPos = (ringPos + 1) & 31;
            if (!wildFired && rip != 0 && rip < WILD_LO) {
                wildFired = true;
                klog_fmt("BW64_WILDJUMP: RIP=0x%llx entered low memory @ insn #%llu "
                         "— dumping last 32 instructions (oldest first):",
                         (unsigned long long)rip, (unsigned long long)instructionCount);
                for (int i = 0; i < 32; i++) {
                    RipHist& e = ring[(ringPos + i) & 31];
                    if (e.rip == 0 && e.b0 == 0 && e.rsp == 0) continue;
                    klog_fmt("  [%2d] RIP=0x%llx %02x %02x %02x %02x  "
                             "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx",
                             i, (unsigned long long)e.rip, e.b0, e.b1, e.b2, e.b3,
                             (unsigned long long)e.rax, (unsigned long long)e.rbx,
                             (unsigned long long)e.rcx, (unsigned long long)e.rdx,
                             (unsigned long long)e.rsi, (unsigned long long)e.rdi,
                             (unsigned long long)e.rsp, (unsigned long long)e.rbp);
                }
            }
        }
#ifdef BOXEDWINE_BLOCK_EXEC
        {
            U32 bn = tryBlockStep();
            if (bn) {
                instructionCount += bn;
                if (instructionCount >= nextPoll) {
                    nextPoll = instructionCount + 0x10000;
                    if (thread && thread->terminating) break;
                    if (thread && thread->pendingSignals) deliverPendingSignals();
                    blockStatsMaybeReport();
                }
                continue;
            }
        }
#endif
        U32 n = step();
        if (n == 0) break;
        instructionCount++;
        // A thread killed from outside (persistent-session app switch kills the
        // previous app) is only MARKED terminating — nothing interrupts a
        // CPU-bound guest loop that never blocks in a syscall (DOOM busy-spins
        // by design), so without this check run() never returns and the killed
        // process can never finish dying. One flag test every 64k instructions
        // is noise next to step()'s cost.
        //
        // SAME cadence, second job (M16, D3D Present fix): deliver any pending
        // cross-thread signal. In the MULTI-THREADED build each guest thread runs
        // its OWN cpu64->run() loop (normalPlatformMultiThreaded.cpp), which —
        // unlike the cooperative scheduler and the futex-wait path — never
        // delivered pendingSignals. A thread that BUSY-POLLS (non-blocking
        // recvmsg/read in a spin, never entering a futex wait) therefore never
        // received the wineserver's SIGUSR1 "APC nudge" carrying an async-I/O
        // completion. That was the D3D Present() hang: wined3d's render thread
        // spun on the wineserver socket for Present's completion whose APC nudge
        // sat undelivered in pendingSignals forever. Delivering here runs the
        // client's APC handler so Present can return. Cheap: only fires when
        // pendingSignals is set, gated to once per 64k instructions.
        // Threshold, not `& 0xFFFF == 0`: the block executor advances
        // instructionCount by whole blocks, which would skip exact multiples.
        if (instructionCount >= nextPoll) {
            nextPoll = instructionCount + 0x10000;
            if (thread && thread->terminating) {
                break;
            }
            if (thread && thread->pendingSignals) {
                deliverPendingSignals();
            }
#ifdef BOXEDWINE_BLOCK_EXEC
            blockStatsMaybeReport();
#endif
        }
    }
}


#ifdef BOXEDWINE_BLOCK_EXEC
static std::atomic<U64> g_bsBuilt{0}, g_bsExec{0}, g_bsInsn{0}, g_bsTotal{0}, g_bsNextReport{100000000ULL};
static std::atomic<U64> g_bsEndOp[512];
static bool g_blockFuzzPlantBug = false;
static int bw64BlockStatsLevel() {
    static int level = [] { const char* e = std::getenv("BW64_BLOCKSTATS"); return e ? std::atoi(e) : 0; }();
    return level;
}

// ---- Phase 2 prototype implementation (see cpu64.h). Semantics of every
// record kind are copied from the interpreter bodies below — same helpers
// (loadRM/storeRM/runAlu/doShift/evalCC), same flag behavior.

bool CPU64::decodeModRMRecipe(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes, BRecipe& out) {
    U8 modrm = fetchByte(modrmAddr);
    U8 mod = (modrm >> 6) & 0x3;
    U8 regF = (modrm >> 3) & 0x7;
    U8 rm  =  modrm        & 0x7;
    out.regField = (U8)(regF | ((p.rex & 0x04) ? 0x08 : 0));
    out.length = 1;
    out.seg = p.seg;
    out.asize32 = p.asize32;
    if (mod == 0x3) {
        out.isReg = true;
        out.rmIndex = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));
        return true;
    }
    if (mod == 0x0 && rm == 0x5) {
        S32 disp32 = (S32)fetchDword(modrmAddr + 1);
        out.length = 5;
        out.ripRel = true;
        out.ripTarget = modrmAddr + out.length + trailingImmBytes + (U64)(S64)disp32;
        return true;
    }
    bool haveBase = true;
    if (rm == 0x4) {
        U8 sib = fetchByte(modrmAddr + 1);
        out.length = 2;
        U8 scale = (sib >> 6) & 0x3;
        U8 idxF  = (sib >> 3) & 0x7;
        U8 baseF =  sib        & 0x7;
        if (!(idxF == 0x4 && !(p.rex & 0x02))) {
            out.idxIdx = (U8)(idxF | ((p.rex & 0x02) ? 0x08 : 0));
            out.scale = scale;
        }
        if (baseF == 0x5 && mod == 0x0) {
            haveBase = false;
        } else {
            out.baseIdx = (U8)(baseF | ((p.rex & 0x01) ? 0x08 : 0));
        }
    } else {
        out.baseIdx = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));
    }
    if (mod == 0x1) {
        out.disp = (S32)(S8)fetchByte(modrmAddr + out.length);
        out.length += 1;
    } else if (mod == 0x2 || (mod == 0x0 && rm == 0x4 && !haveBase)) {
        out.disp = (S32)fetchDword(modrmAddr + out.length);
        out.length += 4;
    }
    return true;
}

__attribute__((always_inline)) inline CPU64::ModRM CPU64::resolveRecipe(const BRecipe& r) {
    ModRM m;
    m.regField = r.regField;
    m.length = r.length;
    if (r.isReg) {
        m.isReg = true;
        m.rmIndex = r.rmIndex;
        return m;
    }
    if (r.ripRel) {
        m.isRipRel = true;
        m.effAddr = r.ripTarget;
        return m;
    }
    U64 ea = (U64)(S64)r.disp;
    if (r.baseIdx != 0xFF) ea += reg[r.baseIdx].u64;
    if (r.idxIdx != 0xFF) ea += reg[r.idxIdx].u64 << r.scale;
    if (r.asize32) ea &= 0xFFFFFFFFULL;
    if (r.seg == 0x64) ea += fsbase;
    else if (r.seg == 0x65) ea += gsbase;
    m.effAddr = ea;
    return m;
}

bool CPU64::buildBlock(U64 startRip, BBlock& b) {
    b.startRip = 0;
    b.n = 0;
    if (blockArenaCursor + BBLOCK_MAX_RECS > BBLOCK_ARENA_RECS) {
        blockArenaCursor = 0;
        blockEpoch++;                      // invalidates every existing block
    }
    b.recOff = blockArenaCursor;
    BRec* recs = &blockArena[b.recOff];
    U64 r = startRip;
    U32 endOp = 0xFFFF;
    while (b.n < BBLOCK_MAX_RECS) {
        Prefixes p;
        U64 savedRip = rip;
        rip = r;                      // consumePrefixes/fetch read via rip-relative helpers? they take absolute addrs; safe
        U32 opOff;
        {
            // consumePrefixes reads at this->rip
            opOff = consumePrefixes(p);
        }
        rip = savedRip;
        U8 op = fetchByte(r + opOff);
        if (bw64BlockStatsLevel() >= 2) {
            // provisional: overwritten each iteration, counted once at the end
            endOp = (op == 0x0F) ? (0x100 | fetchByte(r + opOff + 1)) : op;
        }
        if (p.lock) break;
        bool rexW = (p.rex & 0x08) != 0;
        U8 size = rexW ? 8 : 4;
        BRec& rec = recs[b.n];
        rec = BRec();
        rec.size = size;
        rec.rexPresent = (p.rex != 0) ? 1 : 0;

        // SSE forms where 66/F3 are MANDATORY prefixes (opcode selectors, not
        // operand-size/rep). Conditions mirror the interpreter's 0F handlers.
        if (op == 0x0F) {
            U8 op2 = fetchByte(r + opOff + 1);
            bool mov128 =
                ((op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29) && !p.rep) ||
                ((op2 == 0x6F || op2 == 0x7F) && (p.osize16 || p.rep == 0xF3));
            bool sse66 = p.osize16 && !p.rep;
            int kind = -1;
            if (mov128) kind = (op2 == 0x11 || op2 == 0x29 || op2 == 0x7F) ? BK_MOV128_ST : BK_MOV128_LD;
            else if (!p.rep && (op2 == 0x54 || op2 == 0x55 || op2 == 0x56 || op2 == 0x57)) kind = BK_PLOGIC;
            else if (sse66 && (op2 == 0xEF || op2 == 0xEB || op2 == 0xDB || op2 == 0xDF)) kind = BK_PLOGIC;
            else if (sse66 && (op2 == 0x74 || op2 == 0x75 || op2 == 0x76)) kind = BK_PCMPEQ;
            else if (sse66 && op2 == 0xD7) kind = BK_PMOVMSKB;
            if (kind >= 0) {
                if (!decodeModRMRecipe(r + opOff + 2, p, 0, rec.mr)) break;
                if (kind == BK_PMOVMSKB && !rec.mr.isReg) break;
                rec.kind = (U8)kind;
                rec.sub = (kind == BK_PCMPEQ) ? (U8)((op2 == 0x74) ? 8 : (op2 == 0x75) ? 16 : 32) : op2;
                rec.len = (U8)(opOff + 2 + rec.mr.length);
                b.n++;
                r += rec.len;
                continue;
            }
        }
        if (p.rep || p.osize16) break;             // 16-bit / string forms: interpreter

        if (op == 0x89 || op == 0x8B) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = (op == 0x89) ? BK_MOV_RM_R : BK_MOV_R_RM;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op <= 0x3D && ((op & 0x06) != 0x06) && (op & 0x7) <= 3) {
            U8 form = op & 0x7;
            if ((form & 1) == 0) rec.size = 1;
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = (form < 2) ? BK_ALU_RM_R : BK_ALU_R_RM;
            rec.sub = (op >> 3) & 0x7;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op <= 0x3D && ((op & 0x06) != 0x06) && ((op & 0x7) == 4 || (op & 0x7) == 5)) {
            // ALU AL/eAX/RAX, imm (04/05, 0C/0D, ... 3C/3D)
            rec.kind = BK_ALU_RM_IMM;
            rec.sub = (op >> 3) & 0x7;
            rec.mr.isReg = true; rec.mr.rmIndex = X64_RAX;
            if ((op & 1) == 0) {
                rec.size = 1;
                rec.imm = fetchByte(r + opOff + 1);
                rec.len = (U8)(opOff + 2);
            } else {
                U32 i32 = fetchDword(r + opOff + 1);
                rec.imm = (size == 8) ? (U64)(S64)(S32)i32 : (U64)i32;
                rec.len = (U8)(opOff + 5);
            }
        } else if (op == 0x80) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 1, rec.mr)) break;
            rec.kind = BK_ALU_RM_IMM;
            rec.size = 1;
            rec.sub = rec.mr.regField & 0x7;
            rec.imm = fetchByte(r + opOff + 1 + rec.mr.length);
            rec.len = (U8)(opOff + 1 + rec.mr.length + 1);
        } else if (op == 0x84 || op == 0x88 || op == 0x8A) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = (op == 0x84) ? BK_TEST_RM_R : (op == 0x88) ? BK_MOV_RM_R : BK_MOV_R_RM;
            rec.size = 1;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0xC6) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 1, rec.mr)) break;
            if ((rec.mr.regField & 0x7) != 0) break;
            rec.kind = BK_MOV_RM_IMM;
            rec.size = 1;
            rec.imm = fetchByte(r + opOff + 1 + rec.mr.length);
            rec.len = (U8)(opOff + 1 + rec.mr.length + 1);
        } else if (op == 0xA8 || op == 0xA9) {
            rec.kind = BK_TEST_RM_IMM;
            rec.mr.isReg = true; rec.mr.rmIndex = X64_RAX;
            if (op == 0xA8) {
                rec.size = 1;
                rec.imm = fetchByte(r + opOff + 1);
                rec.len = (U8)(opOff + 2);
            } else {
                U32 i32 = fetchDword(r + opOff + 1);
                rec.imm = (size == 8) ? (U64)(S64)(S32)i32 : (U64)i32;
                rec.len = (U8)(opOff + 5);
            }
        } else if (op == 0xF6 || op == 0xF7) {
            U32 immLen = (op == 0xF6) ? 1u : 4u;
            if (!decodeModRMRecipe(r + opOff + 1, p, immLen, rec.mr)) break;
            if ((rec.mr.regField & 0x7) != 0) break;   // only /0 TEST imm
            rec.kind = BK_TEST_RM_IMM;
            U64 immAddr = r + opOff + 1 + rec.mr.length;
            if (op == 0xF6) {
                rec.size = 1;
                rec.imm = fetchByte(immAddr);
            } else {
                U32 i32 = fetchDword(immAddr);
                rec.imm = (size == 8) ? (U64)(S64)(S32)i32 : (U64)i32;
            }
            rec.len = (U8)(opOff + 1 + rec.mr.length + immLen);
        } else if (op == 0xFF) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            U8 sub = rec.mr.regField & 0x7;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
            if (sub == 0 || sub == 1) {
                rec.kind = BK_INCDEC;
                rec.sub = sub;
            } else if (sub == 6) {
                rec.kind = BK_PUSH_RM;
            } else if (sub == 2 || sub == 4) {
                rec.kind = (sub == 2) ? BK_CALL_IND : BK_JMP_IND;
                b.n++;
                r += rec.len;
                break;                              // terminator
            } else {
                break;
            }
        } else if (op == 0x0F && fetchByte(r + opOff + 1) == 0x1F) {
            if (!decodeModRMRecipe(r + opOff + 2, p, 0, rec.mr)) break;
            rec.kind = BK_NOP;
            rec.len = (U8)(opOff + 2 + rec.mr.length);
        } else if (op == 0x90 && !(p.rex & 0x01)) {
            rec.kind = BK_NOP;
            rec.len = (U8)(opOff + 1);
        } else if (op == 0x81 || op == 0x83) {
            U32 trailing = (op == 0x81) ? 4u : 1u;
            if (!decodeModRMRecipe(r + opOff + 1, p, trailing, rec.mr)) break;
            rec.kind = BK_ALU_RM_IMM;
            rec.sub = rec.mr.regField & 0x7;
            U64 immAddr = r + opOff + 1 + rec.mr.length;
            U32 immLen;
            if (op == 0x83) {
                S8 i8 = (S8)fetchByte(immAddr);
                rec.imm = (U64)(S64)i8;
                if (size == 4) rec.imm &= 0xFFFFFFFFULL;
                immLen = 1;
            } else if (size == 4) {
                rec.imm = fetchDword(immAddr);
                immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(immAddr);
                rec.imm = (U64)(S64)i32;
                immLen = 4;
            }
            rec.len = (U8)(opOff + 1 + rec.mr.length + immLen);
        } else if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
            bool hasImm = (op == 0xC0 || op == 0xC1);
            if (op == 0xC0 || op == 0xD0 || op == 0xD2) rec.size = 1;
            if (!decodeModRMRecipe(r + opOff + 1, p, hasImm ? 1 : 0, rec.mr)) break;
            rec.sub = rec.mr.regField & 0x7;
            U8 mask = (rec.size == 8) ? 0x3F : 0x1F;
            if (op == 0xD2 || op == 0xD3) {
                rec.kind = BK_SHIFT_CL;
                rec.imm = mask;
                rec.len = (U8)(opOff + 1 + rec.mr.length);
            } else {
                rec.kind = BK_SHIFT_IMM;
                U8 count = hasImm ? fetchByte(r + opOff + 1 + rec.mr.length) : 1;
                rec.imm = count & mask;
                rec.len = (U8)(opOff + 1 + rec.mr.length + (hasImm ? 1 : 0));
            }
        } else if (op == 0x0F && fetchByte(r + opOff + 1) == 0xAF) {
            if (!decodeModRMRecipe(r + opOff + 2, p, 0, rec.mr)) break;
            rec.kind = BK_IMUL_R_RM;
            rec.len = (U8)(opOff + 2 + rec.mr.length);
        } else if (op == 0x85) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = BK_TEST_RM_R;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0x8D) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            if (rec.mr.isReg) break;               // LEA reg-form undefined
            rec.kind = BK_LEA;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0xC7) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 4, rec.mr)) break;
            if ((rec.mr.regField & 0x7) != 0) break;
            rec.kind = BK_MOV_RM_IMM;
            S32 imm32 = (S32)fetchDword(r + opOff + 1 + rec.mr.length);
            rec.imm = (size == 8) ? (U64)(S64)imm32 : (U64)(U32)imm32;
            rec.len = (U8)(opOff + 1 + rec.mr.length + 4);
        } else if (op >= 0xB8 && op <= 0xBF) {
            rec.kind = BK_MOV_R_IMM;
            rec.sub = (U8)((op - 0xB8) | ((p.rex & 0x01) ? 0x08 : 0));
            if (rexW) {
                rec.imm = fetchQword(r + opOff + 1);
                rec.len = (U8)(opOff + 1 + 8);
            } else {
                rec.imm = fetchDword(r + opOff + 1);
                rec.len = (U8)(opOff + 1 + 4);
            }
        } else if (op >= 0x50 && op <= 0x57) {
            rec.kind = BK_PUSH;
            rec.sub = (U8)((op - 0x50) | ((p.rex & 0x01) ? 0x08 : 0));
            rec.len = (U8)(opOff + 1);
        } else if (op >= 0x58 && op <= 0x5F) {
            rec.kind = BK_POP;
            rec.sub = (U8)((op - 0x58) | ((p.rex & 0x01) ? 0x08 : 0));
            rec.len = (U8)(opOff + 1);
        } else if (op == 0x0F && fetchByte(r + opOff + 1) >= 0x80 && fetchByte(r + opOff + 1) <= 0x8F) {
            rec.kind = BK_JCC8;                    // same executor as rel8
            rec.sub = fetchByte(r + opOff + 1) & 0xF;
            rec.jccDelta = (S32)fetchDword(r + opOff + 2);
            rec.len = (U8)(opOff + 2 + 4);
            b.n++;
            r += rec.len;
            break;                                  // terminator
        } else if (op >= 0x70 && op <= 0x7F) {
            rec.kind = BK_JCC8;
            rec.sub = op & 0xF;
            rec.jccDelta = (S32)(S8)fetchByte(r + opOff + 1);
            rec.len = (U8)(opOff + 2);
            b.n++;
            r += rec.len;
            break;                                  // terminator
        } else if (op == 0xE8 || op == 0xE9 || op == 0xEB) {
            rec.kind = (op == 0xE8) ? BK_CALL : BK_JMP;
            if (op == 0xEB) {
                rec.jccDelta = (S32)(S8)fetchByte(r + opOff + 1);
                rec.len = (U8)(opOff + 2);
            } else {
                rec.jccDelta = (S32)fetchDword(r + opOff + 1);
                rec.len = (U8)(opOff + 5);
            }
            b.n++;
            r += rec.len;
            break;                                  // terminator
        } else if (op == 0xC3) {
            rec.kind = BK_RET;
            rec.len = (U8)(opOff + 1);
            b.n++;
            r += rec.len;
            break;                                  // terminator
        } else if (op == 0x63) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = BK_MOVSXD;
            rec.sub = 4;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0x0F && (fetchByte(r + opOff + 1) & 0xF6) == 0xB6) {
            // 0F B6/B7 MOVZX, 0F BE/BF MOVSX
            U8 op2 = fetchByte(r + opOff + 1);
            if (!decodeModRMRecipe(r + opOff + 2, p, 0, rec.mr)) break;
            rec.kind = (op2 & 0x08) ? BK_MOVSX : BK_MOVZX;
            rec.sub = (op2 & 1) ? 2 : 1;
            rec.len = (U8)(opOff + 2 + rec.mr.length);
        } else {
            break;                                  // unsupported → end block here
        }
        b.n++;
        r += rec.len;
    }
    if (endOp < 512 && b.n < BBLOCK_MAX_RECS) {
        // the instruction at the end wasn't consumed (terminators did b.n++
        // AND advanced r, so only count when the last record isn't a Jcc)
        if (b.n == 0 || recs[b.n - 1].kind < BK_CALL && recs[b.n - 1].kind != BK_JCC8) g_bsEndOp[endOp]++;
    }
    if (b.n < 2) {
        return false;                               // not worth a block
    }
    // Register + snapshot page generations (register FIRST so no write can
    // slip between the gen read and future executions unnoticed).
    b.page0 = startRip >> 12;
    b.page1 = (r - 1) >> 12;
    KMemory64::blockPageRegister(b.page0);
    if (b.page1 != b.page0) KMemory64::blockPageRegister(b.page1);
    b.gen0 = KMemory64::blockPageGenOf(b.page0);
    b.gen1 = KMemory64::blockPageGenOf(b.page1);
    b.startRip = startRip;
    b.epoch = blockEpoch;
    blockArenaCursor += b.n;
    blockStatsBuilt++;
    return true;
}

bool CPU64::blockRecMayWrite(const BRec& rec) {
    switch (rec.kind) {
    case BK_PUSH: case BK_PUSH_RM:
        return true;
    case BK_MOV_RM_R: case BK_ALU_RM_R: case BK_ALU_RM_IMM:
    case BK_SHIFT_IMM: case BK_SHIFT_CL: case BK_MOV_RM_IMM:
    case BK_INCDEC: case BK_MOV128_ST:
        return !rec.mr.isReg;
    default:
        return false;   // terminators return before this check; loads/regs can't write
    }
}

inline U64 CPU64::blockRegRead(U8 idx, U32 size, bool rexPresent) {
    return (size == 1) ? (U64)readReg8(idx, rexPresent)
         : (size == 4) ? (U64)reg[idx].u32 : reg[idx].u64;
}

U32 CPU64::execBlock(const BBlock& b, U64 maxInsn) {
    U32 executed = 0;
    const BRec* recs = &blockArena[b.recOff];
    for (U16 i = 0; i < b.n && executed < maxInsn; i++) {
        const BRec& rec = recs[i];
        bool rexPresent = rec.rexPresent != 0;
        U32 size = rec.size;
        switch (rec.kind) {
        case BK_MOV_RM_R: {
            ModRM m = resolveRecipe(rec.mr);
            U64 src = blockRegRead(m.regField, size, rexPresent);
            storeRM(m, size, src, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_MOV_R_RM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 val = loadRM(m, size, rexPresent);
            if (size == 1) writeReg8(m.regField, (U8)val, rexPresent);
            else if (size == 4) reg[m.regField].setU32((U32)val);
            else reg[m.regField].setU64(val);
            rip += rec.len;
            break;
        }
        case BK_ALU_RM_R: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            U64 bv = blockRegRead(m.regField, size, rexPresent);
            runAlu(rec.sub, size, true, a, bv, m, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_ALU_R_RM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = blockRegRead(m.regField, size, rexPresent);
            U64 bv = loadRM(m, size, rexPresent);
            runAlu(rec.sub, size, false, a, bv, m, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_ALU_RM_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            runAlu(rec.sub, size, true, a, rec.imm, m, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_SHIFT_CL:
        case BK_SHIFT_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            U8 count = (rec.kind == BK_SHIFT_CL) ? (U8)(reg[X64_RCX].u8 & (U8)rec.imm) : (U8)rec.imm;
            U64 v = loadRM(m, size, rexPresent);
            U64 res = doShift(rflags, rec.sub, v, count, size);
            if (count != 0) storeRM(m, size, res, rexPresent);
            else if (size == 4 && m.isReg) reg[m.rmIndex].setU32((U32)v);
            rip += rec.len;
            break;
        }
        case BK_IMUL_R_RM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = (size == 4) ? (U64)reg[m.regField].u32 : reg[m.regField].u64;
            U64 bv = loadRM(m, size, rexPresent);
            S64 sa, sb;
            if (size == 4) { sa = (S64)(S32)a; sb = (S64)(S32)bv; }
            else { sa = (S64)a; sb = (S64)bv; }
            __int128 r128 = (__int128)sa * (__int128)sb;
            S64 res = (S64)r128;
            bool overflow;
            if (size == 4) overflow = (res != (S64)(S32)res);
            else overflow = (r128 != (__int128)res);
            rflags &= ~(X64_CF | X64_OF);
            if (overflow) rflags |= X64_CF | X64_OF;
            if (size == 4) reg[m.regField].setU32((U32)res);
            else reg[m.regField].setU64(res);
            rip += rec.len;
            break;
        }
        case BK_TEST_RM_R: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            U64 bv = blockRegRead(m.regField, size, rexPresent);
            flagsLogic(rflags, a & bv, size);
            rip += rec.len;
            break;
        }
        case BK_TEST_RM_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            flagsLogic(rflags, a & rec.imm, size);
            rip += rec.len;
            break;
        }
        case BK_INCDEC: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            U64 res = (rec.sub == 0) ? a + 1 : a - 1;
            U32 savedCF = rflags & X64_CF;
            if (rec.sub == 0) flagsAdd(rflags, a, 1, res, size);
            else              flagsSub(rflags, a, 1, res, size);
            rflags = (rflags & ~X64_CF) | savedCF;
            if (g_blockFuzzPlantBug && rec.sub == 0) rflags ^= X64_CF;
            storeRM(m, size, res, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_PUSH_RM: {
            ModRM m = resolveRecipe(rec.mr);
            push64(loadRM(m, 8, rexPresent));
            rip += rec.len;
            break;
        }
        case BK_NOP: {
            rip += rec.len;
            break;
        }
        case BK_MOV128_LD: {
            ModRM m = resolveRecipe(rec.mr);
            if (m.isReg) {
                xmm[m.regField] = xmm[m.rmIndex];
            } else {
                xmm[m.regField].lo = memory->readq(m.effAddr);
                xmm[m.regField].hi = memory->readq(m.effAddr + 8);
            }
            rip += rec.len;
            break;
        }
        case BK_MOV128_ST: {
            ModRM m = resolveRecipe(rec.mr);
            if (m.isReg) {
                xmm[m.rmIndex] = xmm[m.regField];
            } else {
                memory->writeq(m.effAddr,     xmm[m.regField].lo);
                memory->writeq(m.effAddr + 8, xmm[m.regField].hi);
            }
            rip += rec.len;
            break;
        }
        case BK_PLOGIC: {
            ModRM m = resolveRecipe(rec.mr);
            U64 srcLo, srcHi;
            if (m.isReg) { srcLo = xmm[m.rmIndex].lo; srcHi = xmm[m.rmIndex].hi; }
            else { srcLo = memory->readq(m.effAddr); srcHi = memory->readq(m.effAddr + 8); }
            U64 dLo = xmm[m.regField].lo, dHi = xmm[m.regField].hi;
            switch (rec.sub) {
                case 0x54: case 0xDB: xmm[m.regField].lo = dLo & srcLo;  xmm[m.regField].hi = dHi & srcHi;  break;
                case 0x55: case 0xDF: xmm[m.regField].lo = ~dLo & srcLo; xmm[m.regField].hi = ~dHi & srcHi; break;
                case 0x56: case 0xEB: xmm[m.regField].lo = dLo | srcLo;  xmm[m.regField].hi = dHi | srcHi;  break;
                default:              xmm[m.regField].lo = dLo ^ srcLo;  xmm[m.regField].hi = dHi ^ srcHi;  break;
            }
            rip += rec.len;
            break;
        }
        case BK_PCMPEQ: {
            ModRM m = resolveRecipe(rec.mr);
            U64 srcLo, srcHi;
            if (m.isReg) { srcLo = xmm[m.rmIndex].lo; srcHi = xmm[m.rmIndex].hi; }
            else { srcLo = memory->readq(m.effAddr); srcHi = memory->readq(m.effAddr + 8); }
            U32 elemBits = rec.sub;
            U64 mask = (1ULL << elemBits) - 1;
            U64 outLo = 0, outHi = 0;
            for (U32 off = 0; off < 64; off += elemBits) {
                if (((xmm[m.regField].lo >> off) & mask) == ((srcLo >> off) & mask)) outLo |= mask << off;
                if (((xmm[m.regField].hi >> off) & mask) == ((srcHi >> off) & mask)) outHi |= mask << off;
            }
            xmm[m.regField].lo = outLo;
            xmm[m.regField].hi = outHi;
            rip += rec.len;
            break;
        }
        case BK_PMOVMSKB: {
            ModRM m = resolveRecipe(rec.mr);
            U64 lo = xmm[m.rmIndex].lo, hi = xmm[m.rmIndex].hi;
            U32 msk = 0;
            for (U32 i = 0; i < 8; i++) {
                if (lo & (1ULL << (i * 8 + 7))) msk |= (1u << i);
                if (hi & (1ULL << (i * 8 + 7))) msk |= (1u << (i + 8));
            }
            reg[m.regField].setU32(msk);
            rip += rec.len;
            break;
        }
        case BK_CALL_IND: {
            ModRM m = resolveRecipe(rec.mr);
            U64 target = loadRM(m, 8, rexPresent);
            push64(rip + rec.len);
            rip = target;
            executed++;
            return executed;
        }
        case BK_JMP_IND: {
            ModRM m = resolveRecipe(rec.mr);
            rip = loadRM(m, 8, rexPresent);
            executed++;
            return executed;
        }
        case BK_LEA: {
            ModRM m = resolveRecipe(rec.mr);
            if (size == 4) reg[m.regField].setU32((U32)m.effAddr);
            else reg[m.regField].setU64(m.effAddr);
            rip += rec.len;
            break;
        }
        case BK_MOV_RM_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            storeRM(m, size, rec.imm, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_MOV_R_IMM: {
            if (size == 4) reg[rec.sub].setU32((U32)rec.imm);
            else reg[rec.sub].setU64(rec.imm);
            rip += rec.len;
            break;
        }
        case BK_PUSH: {
            push64(reg[rec.sub].u64);
            rip += rec.len;
            break;
        }
        case BK_POP: {
            reg[rec.sub].setU64(pop64());
            rip += rec.len;
            break;
        }
        case BK_JCC8: {
            rip += rec.len;
            if (evalCC(rec.sub)) rip += (U64)(S64)rec.jccDelta;
            executed++;
            return executed;
        }
        case BK_CALL: {
            U64 nextRip = rip + rec.len;
            push64(nextRip);
            rip = nextRip + (U64)(S64)rec.jccDelta;
            executed++;
            return executed;
        }
        case BK_JMP: {
            rip += rec.len + (U64)(S64)rec.jccDelta;
            executed++;
            return executed;
        }
        case BK_RET: {
            rip = pop64();
            executed++;
            return executed;
        }
        case BK_MOVZX: {
            ModRM m = resolveRecipe(rec.mr);
            U64 v = loadRM(m, rec.sub, rexPresent);
            if (size == 4) reg[m.regField].setU32((U32)v);
            else reg[m.regField].setU64(v);
            rip += rec.len;
            break;
        }
        case BK_MOVSX: {
            ModRM m = resolveRecipe(rec.mr);
            U64 raw = loadRM(m, rec.sub, rexPresent);
            S64 v = (rec.sub == 1) ? (S64)(S8)raw : (S64)(S16)raw;
            if (size == 4) reg[m.regField].setU32((U32)v);
            else reg[m.regField].setU64((U64)v);
            rip += rec.len;
            break;
        }
        case BK_MOVSXD: {
            ModRM m = resolveRecipe(rec.mr);
            U64 raw = loadRM(m, 4, rexPresent);
            if (size == 8) reg[m.regField].setU64((U64)(S64)(S32)raw);
            else reg[m.regField].setU32((U32)raw);
            rip += rec.len;
            break;
        }
        }
        executed++;
        // A store may patch a LATER record of this very block (self-modifying
        // code): stop at the next instruction so it is re-decoded. Only
        // records that can write memory pay the two generation loads.
        if (blockRecMayWrite(rec) &&
            (KMemory64::blockPageGenOf(b.page0) != b.gen0 ||
             KMemory64::blockPageGenOf(b.page1) != b.gen1)) {
            return executed;
        }
    }
    return executed;
}

// BW64_BLOCKSTATS=1: every guest thread folds its counters into process-wide
// totals at the run-loop poll cadence; a line is logged every ~100M guest
// instructions (wine sessions are usually killed, so no atexit report).
// BW64_BLOCKSTATS=2 also histograms the opcode that ENDED each block build
// (the "what to implement next" list), logged with the totals.

void CPU64::blockStatsMaybeReport() {
    if (!bw64BlockStatsLevel()) return;
    g_bsBuilt += blockStatsBuilt - bsFlushedBuilt; bsFlushedBuilt = blockStatsBuilt;
    g_bsExec  += blockStatsExec  - bsFlushedExec;  bsFlushedExec  = blockStatsExec;
    g_bsInsn  += blockStatsInsn  - bsFlushedInsn;  bsFlushedInsn  = blockStatsInsn;
    U64 total = (g_bsTotal += instructionCount - bsFlushedTotal); bsFlushedTotal = instructionCount;
    U64 due = g_bsNextReport.load();
    if (total < due || !g_bsNextReport.compare_exchange_strong(due, due + 100000000ULL)) return;
    U64 insn = g_bsInsn.load();
    klog_fmt("BLOCKSTATS: built=%llu execs=%llu insnsInBlocks=%llu totalInsns=%llu coverage=%.1f%%",
             (unsigned long long)g_bsBuilt.load(), (unsigned long long)g_bsExec.load(),
             (unsigned long long)insn, (unsigned long long)total, total ? 100.0 * insn / total : 0.0);
    if (bw64BlockStatsLevel() >= 2) {
        // top 16 block-ending opcodes (index: 0x000-0x0FF one-byte, 0x100-0x1FF 0F-prefixed)
        U64 snap[512];
        for (int i = 0; i < 512; i++) snap[i] = g_bsEndOp[i].load();
        for (int k = 0; k < 16; k++) {
            int best = -1;
            for (int i = 0; i < 512; i++) if (snap[i] && (best < 0 || snap[i] > snap[best])) best = i;
            if (best < 0) break;
            klog_fmt("BLOCKSTATS:   end-op %s%02x  x%llu", best >= 256 ? "0f " : "", best & 0xFF, (unsigned long long)snap[best]);
            snap[best] = 0;
        }
    }
}

U32 CPU64::tryBlockStep(U64 maxInsn) {
    U64 r = rip;
    U32 neg = (U32)((r ^ (r >> 10)) & 1023);
    if (blockNegCache[neg] == r) return 0;
    if (!blockTable) {
        blockTable = std::make_unique<BBlock[]>(BBLOCK_SLOTS);
        blockArena = std::make_unique<BRec[]>(BBLOCK_ARENA_RECS);
    }
    BBlock& b = blockTable[(U32)((r ^ (r >> 9)) & (BBLOCK_SLOTS - 1))];
    if (b.startRip == r && b.epoch == blockEpoch) {
        if (KMemory64::blockPageGenOf(b.page0) == b.gen0 &&
            KMemory64::blockPageGenOf(b.page1) == b.gen1) {
            U32 n = execBlock(b, maxInsn);
            blockStatsExec++;
            blockStatsInsn += n;
            return n;
        }
        b.startRip = 0; // stale — rebuild below
    }
    if (!buildBlock(r, b)) {
        blockNegCache[neg] = r;
        return 0;
    }
    U32 n = execBlock(b, maxInsn);
    blockStatsExec++;
    blockStatsInsn += n;
    return n;
}

// ---- --x64-blockfuzz ------------------------------------------------------
// Random sequences built ONLY from instruction forms the block builder
// accepts (plus a few it rejects, so mid-block interpreter fallback is
// exercised too). Each sequence runs twice on two identical address spaces:
// A through tryBlockStep (interpreter fallback when it returns 0), B through
// step() alone. Registers, RIP, flags, XMM and the data page must match.
// Register discipline: RSP/RBX/RDI are never written (RBX = data base,
// RDI = small index, RSP = stack inside the data page).
namespace {
struct FuzzRng {
    U64 s;
    U64 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    U32 below(U32 n) { return (U32)(next() % n); }
};
}

int CPU64::blockFuzz(U64 iterations, U64 seed) {
    const U64 CODE = 0x500000, DATA = 0x600000, END_PAD = 64;
    FuzzRng rng{seed ? seed : 0x9E3779B97F4A7C15ULL};
    int failures = 0;
    U64 kindsSeen = 0, blockInsns = 0, totalInsns = 0;
    // BW64_BLOCKFUZZ_SELFCHECK=1 plants a deliberate block-executor bug (INC
    // clobbers CF) to prove the fuzzer can see divergences.
    static const bool plant = std::getenv("BW64_BLOCKFUZZ_SELFCHECK") != nullptr;
    g_blockFuzzPlantBug = plant;
    for (U64 iter = 0; iter < iterations; iter++) {
        std::vector<U8> code;
        auto dstReg = [&]() -> U8 { for (;;) { U8 r = (U8)rng.below(16); if (r != X64_RSP && r != X64_RBX && r != X64_RDI) return r; } };
        auto anyReg = [&]() -> U8 { return (U8)rng.below(16); };
        // emit [prefixes] opcode bytes + modrm (+sib/disp). regField/rm are
        // full 4-bit numbers; memory forms use RBX(+RDI*scale)+disp8.
        auto emitModRM = [&](std::vector<U8>& out, U8 rexBase, bool w, U8 regF, bool mem, U8 rmReg,
                             std::initializer_list<U8> ops, bool reg8 = false) {
            U8 rex = rexBase | (w ? 0x08 : 0) | ((regF & 8) ? 0x04 : 0);
            bool sib = mem && rng.below(2);
            if (!mem) rex |= (rmReg & 8) ? 0x01 : 0;
            // 8-bit forms: half the time force a REX so SPL/BPL/SIL/DIL get used
            if (reg8 && rng.below(2)) rex |= 0x40;
            if (rex) out.push_back((U8)(0x40 | rex));
            for (U8 o : ops) out.push_back(o);
            if (!mem) {
                out.push_back((U8)(0xC0 | ((regF & 7) << 3) | (rmReg & 7)));
            } else if (sib) {
                out.push_back((U8)(0x44 | ((regF & 7) << 3)));                 // mod=01 rm=100
                out.push_back((U8)((rng.below(4) << 6) | (X64_RDI << 3) | X64_RBX));
                out.push_back((U8)rng.below(0x60));
            } else {
                out.push_back((U8)(0x43 | ((regF & 7) << 3)));                 // mod=01 rm=rbx
                out.push_back((U8)rng.below(0x60));
            }
        };
        U32 nInsn = 4 + rng.below(28);
        for (U32 k = 0; k < nInsn; k++) {
            std::vector<U8>& c = code;
            bool mem = rng.below(3) == 0;
            bool w = rng.below(2);
            U32 form = rng.below(34);
            kindsSeen |= 1ULL << form;
            switch (form) {
            case 0: emitModRM(c, 0, w, anyReg(), mem, dstReg(), {0x89}); if (!mem) { /* dest = rm */ } break;
            case 1: emitModRM(c, 0, w, dstReg(), mem, anyReg(), {0x8B}); break;
            case 2: { U8 op = (U8)((rng.below(8) << 3) | rng.below(4));             // ALU forms 0-3
                      bool destRm = (op & 7) < 2; bool r8 = !(op & 1);
                      if (r8) { // 8-bit: never touch BL/BH/DIL/SPL (rbx/rdi/rsp low bytes) -> use regs 0-2, 8-15
                          static const U8 ok8[] = {0, 1, 2, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15};
                          U8 a = ok8[rng.below(13)], b = ok8[rng.below(13)];
                          emitModRM(c, 0, false, destRm ? a : a, mem, b, {op}, true);
                      } else emitModRM(c, 0, w, destRm ? anyReg() : dstReg(), mem, destRm ? dstReg() : anyReg(), {op});
                      break; }
            case 3: { U8 op = (U8)((rng.below(8) << 3) | (4 + rng.below(2)));      // ALU acc,imm
                      if (w) c.push_back(0x48); c.push_back(op);
                      if (op & 1) for (int i = 0; i < 4; i++) c.push_back((U8)rng.next()); else c.push_back((U8)rng.next());
                      break; }
            case 4: emitModRM(c, 0, w, (U8)rng.below(8), mem, dstReg(), {0x81}); for (int i = 0; i < 4; i++) c.push_back((U8)rng.next()); break;
            case 5: emitModRM(c, 0, w, (U8)rng.below(8), mem, dstReg(), {0x83}); c.push_back((U8)rng.next()); break;
            case 6: { static const U8 ok8[] = {0, 1, 2, 8, 9, 10, 11, 12, 13, 14, 15};
                      emitModRM(c, 0, false, (U8)rng.below(8), mem, ok8[rng.below(11)], {0x80}, true); c.push_back((U8)rng.next()); break; }
            case 7: emitModRM(c, 0, w, anyReg(), mem, anyReg(), {0x85}); break;
            case 8: emitModRM(c, 0, false, anyReg(), mem, anyReg(), {0x84}, true); break;
            case 9: if (w) c.push_back(0x48); c.push_back(0xA9); for (int i = 0; i < 4; i++) c.push_back((U8)rng.next()); break;
            case 10: emitModRM(c, 0, w, 0, mem, anyReg(), {0xF7}); for (int i = 0; i < 4; i++) c.push_back((U8)rng.next()); break;
            case 11: emitModRM(c, 0, false, 0, mem, anyReg(), {0xF6}, true); c.push_back((U8)rng.next()); break;
            case 12: emitModRM(c, 0, w, 0, mem, dstReg(), {0xC7}); for (int i = 0; i < 4; i++) c.push_back((U8)rng.next()); break;
            case 13: { U8 r = dstReg(); c.push_back((U8)(0x40 | (w ? 8 : 0) | ((r & 8) ? 1 : 0))); c.push_back((U8)(0xB8 + (r & 7)));
                       for (int i = 0; i < (w ? 8 : 4); i++) c.push_back((U8)rng.next()); break; }
            case 14: emitModRM(c, 0, w, dstReg(), true, 0, {0x8D}); break;
            case 15: emitModRM(c, 0, w, dstReg(), mem, anyReg(), {0x0F, 0xAF}); break;
            case 16: emitModRM(c, 0, w, (U8)rng.below(8), mem, dstReg(), {0xC1}); c.push_back((U8)rng.below(80)); break;
            case 17: emitModRM(c, 0, w, (U8)rng.below(8), mem, dstReg(), {(U8)(0xD1 + 2 * rng.below(2))}); break;
            case 18: { static const U8 ok8[] = {0, 1, 2, 8, 9, 10, 11, 12, 13, 14, 15};
                       U8 op = (U8)(0xC0 + 0x10 * rng.below(2) + 2 * rng.below(2)); // C0, D0, D2 (C2 is RET: remap)
                       if (op == 0xC2) op = 0xC0;
                       emitModRM(c, 0, false, (U8)rng.below(8), mem, ok8[rng.below(11)], {op}, true);
                       if (op == 0xC0) c.push_back((U8)rng.below(20));
                       break; }
            case 19: emitModRM(c, 0, w, (U8)rng.below(2), mem, dstReg(), {0xFF}); break;          // INC/DEC
            case 20: { U8 r = anyReg(); if (r & 8) c.push_back(0x41); c.push_back((U8)(0x50 + (r & 7))); // PUSH ; POP
                       U8 d = dstReg(); if (d & 8) c.push_back(0x41); c.push_back((U8)(0x58 + (d & 7))); break; }
            case 21: emitModRM(c, 0, w, dstReg(), mem, anyReg(), {0x63}); break;
            case 22: { U8 op2 = (U8)(0xB6 + rng.below(2) + 8 * rng.below(2));
                       emitModRM(c, 0, w, dstReg(), mem, anyReg(), {0x0F, op2}, true); break; }
            case 23: emitModRM(c, 0, false, 0, mem, anyReg(), {0x0F, 0x1F}); break;
            case 24: c.push_back(0x90); break;
            case 25: { U8 op2 = (U8)(0x10 + rng.below(2) + 0x18 * rng.below(2));  // 10/11/28/29
                       if (rng.below(2)) c.push_back(0x66);
                       emitModRM(c, 0, false, (U8)rng.below(16), mem, (U8)rng.below(16), {0x0F, op2}); break; }
            case 26: { c.push_back(rng.below(2) ? 0x66 : 0xF3);
                       emitModRM(c, 0, false, (U8)rng.below(16), mem, (U8)rng.below(16), {0x0F, (U8)(rng.below(2) ? 0x6F : 0x7F)}); break; }
            case 27: { static const U8 ops[] = {0x54, 0x55, 0x56, 0x57};
                       emitModRM(c, 0, false, (U8)rng.below(16), mem, (U8)rng.below(16), {0x0F, ops[rng.below(4)]}); break; }
            case 28: { static const U8 ops[] = {0xEF, 0xEB, 0xDB, 0xDF, 0x74, 0x75, 0x76};
                       c.push_back(0x66);
                       emitModRM(c, 0, false, (U8)rng.below(16), mem, (U8)rng.below(16), {0x0F, ops[rng.below(7)]}); break; }
            case 29: c.push_back(0x66); emitModRM(c, 0, false, dstReg(), false, (U8)rng.below(16), {0x0F, 0xD7}); break;
            case 30: c.push_back((U8)(0x70 + rng.below(16))); c.push_back(0x00); break;       // Jcc +0 (both paths land next)
            case 31: c.push_back(0xEB); c.push_back(0x00); break;                              // JMP +0
            case 32: { c.push_back(0xE8); for (int i = 0; i < 4; i++) c.push_back(0); break; }  // CALL +0 (pushes)
            case 33: { c.push_back(0x66); emitModRM(c, 0, false, anyReg(), mem, dstReg(), {0x89}); break; } // 16-bit: block rejects
            }
        }
        size_t endOff = code.size();
        for (U64 i = 0; i < END_PAD; i++) code.push_back(0xF4);               // HLT (unsupported -> stop)

        // initial state
        U64 regs0[16]; for (int i = 0; i < 16; i++) regs0[i] = rng.next();
        regs0[X64_RBX] = DATA + 0x100;
        regs0[X64_RDI] = rng.below(8);
        regs0[X64_RSP] = DATA + 0xE00;
        U32 flags0 = 0x202 | ((U32)rng.next() & 0x8D5);
        Xmm xmm0[16]; for (int i = 0; i < 16; i++) { xmm0[i].lo = rng.next(); xmm0[i].hi = rng.next(); }
        std::vector<U8> data(0x1000); for (auto& b : data) b = (U8)rng.next();

        KMemory64 memA(nullptr), memB(nullptr);
        CPU64 a(&memA), b(&memB);
        KMemory64* mems[2] = {&memA, &memB};
        CPU64* cpus[2] = {&a, &b};
        for (int k = 0; k < 2; k++) {
            mems[k]->mmapAnonymousFixed(CODE, 0x2000, 7);
            mems[k]->mmapAnonymousFixed(DATA, 0x1000, 3);
            mems[k]->memcpyToGuest(CODE, code.data(), code.size());
            mems[k]->memcpyToGuest(DATA, data.data(), data.size());
            for (int i = 0; i < 16; i++) { cpus[k]->reg[i].setU64(regs0[i]); cpus[k]->xmm[i] = xmm0[i]; }
            cpus[k]->rflags = flags0;
        }
        const U64 endRip = CODE + endOff;
        for (int pass = 0; pass < 2; pass++) {          // pass 2 re-runs cached blocks
            a.rip = CODE; b.rip = CODE;
            for (U32 guard = 0; a.rip != endRip && guard < 4096; guard++) {
                if (!a.tryBlockStep()) { if (!a.step()) break; }
            }
            for (U32 guard = 0; b.rip != endRip && guard < 4096; guard++) {
                if (!b.step()) break;
            }
        }
        blockInsns += a.blockStatsInsn; totalInsns += b.instructionCount;
        bool ok = a.rip == b.rip && a.rflags == b.rflags;
        int badReg = -1, badXmm = -1;
        for (int i = 0; i < 16 && ok; i++) if (a.reg[i].u64 != b.reg[i].u64) { ok = false; badReg = i; }
        for (int i = 0; i < 16 && ok; i++) if (a.xmm[i].lo != b.xmm[i].lo || a.xmm[i].hi != b.xmm[i].hi) { ok = false; badXmm = i; }
        int badByte = -1;
        if (ok) {
            std::vector<U8> da(0x1000), db(0x1000);
            memA.memcpyFromGuest(da.data(), DATA, 0x1000);
            memB.memcpyFromGuest(db.data(), DATA, 0x1000);
            for (int i = 0; i < 0x1000; i++) if (da[i] != db[i]) { ok = false; badByte = i; break; }
        }
        if (!ok) {
            failures++;
            if (failures <= 10) {
                printf("BLOCKFUZZ MISMATCH iter=%llu rip A=0x%llx B=0x%llx flags A=0x%x B=0x%x badReg=%d (A=0x%llx B=0x%llx) badXmm=%d badByte=%d\n  code:",
                       (unsigned long long)iter, (unsigned long long)a.rip, (unsigned long long)b.rip, a.rflags, b.rflags,
                       badReg, badReg >= 0 ? (unsigned long long)a.reg[badReg].u64 : 0ULL,
                       badReg >= 0 ? (unsigned long long)b.reg[badReg].u64 : 0ULL, badXmm, badByte);
                for (size_t i = 0; i < endOff; i++) printf(" %02x", code[i]);
                printf("\n");
            }
        }
    }
    printf("BLOCKFUZZ: %llu sequences, %d mismatches (forms seen mask 0x%llx, %llu of %llu instructions ran in blocks)\n",
           (unsigned long long)iterations, failures, (unsigned long long)kindsSeen,
           (unsigned long long)blockInsns, (unsigned long long)totalInsns);
    return failures;
}
#endif // BOXEDWINE_BLOCK_EXEC

U64 CPU64::runBounded(U64 maxInsn) {
    static const char* tf = std::getenv("BOXEDWINE64_TRACE_FROM");
    static const char* tt = std::getenv("BOXEDWINE64_TRACE_TO");
    U64 traceFrom = tf ? std::strtoull(tf, nullptr, 0) : (U64)-1;
    U64 traceTo   = tt ? std::strtoull(tt, nullptr, 0) : 0;
    bool tracing = (tf != nullptr);

    U64 ran = 0;
    while (!yield && ran < maxInsn) {
        if (tracing && instructionCount >= traceFrom && instructionCount <= traceTo) {
            U64 r = rip;
            klog_fmt("TRACE #%llu RIP=0x%llx %02x %02x %02x %02x  "
                     "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx "
                     "xmm0=%llx:%llx xmm1=%llx:%llx",
                     (unsigned long long)instructionCount, (unsigned long long)r,
                     fetchByte(r), fetchByte(r+1), fetchByte(r+2), fetchByte(r+3),
                     (unsigned long long)reg[X64_RAX].u64, (unsigned long long)reg[X64_RBX].u64,
                     (unsigned long long)reg[X64_RCX].u64, (unsigned long long)reg[X64_RDX].u64,
                     (unsigned long long)reg[X64_RSI].u64, (unsigned long long)reg[X64_RDI].u64,
                     (unsigned long long)reg[X64_RSP].u64, (unsigned long long)reg[X64_RBP].u64,
                     (unsigned long long)xmm[0].hi, (unsigned long long)xmm[0].lo,
                     (unsigned long long)xmm[1].hi, (unsigned long long)xmm[1].lo);
        }
#ifdef BOXEDWINE_BLOCK_EXEC
        {
            U32 bn = tryBlockStep(maxInsn - ran);
            if (bn) {
                instructionCount += bn;
                ran += bn;
                continue;
            }
        }
#endif
        U32 n = step();
        if (n == 0) break;
        instructionCount++;
        ran++;
    }
#ifdef BOXEDWINE_BLOCK_EXEC
    blockStatsMaybeReport();
#endif
    return ran;
}

#endif // BOXEDWINE_GUEST_X64
