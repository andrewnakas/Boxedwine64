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
#include <vector>

// Minimal in-process smoke test for the x86-64 interpreter. Builds tiny
// programs directly in guest memory (no ELF round-trip), runs them, and
// checks register state. Each case prints PASS / FAIL with a short
// diagnostic. Invoked via `boxedwine --x64-selftest`.
//
// This is deliberately scoped narrower than the full loader path: it
// validates CPU64 + KMemory64 in isolation so failures point at the
// interpreter rather than the loader/stack-build code.

namespace {

constexpr U64 CODE_BASE = 0x400000;
constexpr U64 STACK_TOP = 0x800000;

struct TestResult {
    int passed = 0;
    int failed = 0;
};

void loadCode(KMemory64& mem, U64 addr, const std::vector<U8>& bytes) {
    mem.memcpyToGuest(addr, bytes.data(), bytes.size());
}

// Run a single program. The CPU yields when it hits 0xF4 (HLT, used here
// as "halt" marker) or runs out of instructions. We add a HLT handler
// inline in step() — but for now we use an exit syscall as the natural
// stopping point. Caller passes an opcode buffer that ends in
// "mov rax,60; xor rdi,rdi; syscall" (exit(0)).
void runAndCheck(TestResult& r, const char* name,
                 const std::vector<U8>& code,
                 std::function<bool(CPU64&)> verify) {
    KMemory64 mem(nullptr);
    mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7); // RWX
    mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3); // RW
    loadCode(mem, CODE_BASE, code);

    CPU64 cpu(&mem);
    cpu.rip = CODE_BASE;
    cpu.reg[X64_RSP].setU64(STACK_TOP - 16);

    // runBounded enforces an instruction cap so a buggy test can't
    // peg the host. cpu.run() has an unbounded while-loop and would hang.
    const U64 INSN_LIMIT = 200000;
    cpu.runBounded(INSN_LIMIT);
    bool hung = (!cpu.yield && cpu.instructionCount >= INSN_LIMIT);
    if (hung) {
        printf("  TIMEOUT: %s  (final RIP=0x%llx instr=%llu RAX=0x%llx R15=0x%llx)\n",
               name,
               (unsigned long long)cpu.rip,
               (unsigned long long)cpu.instructionCount,
               (unsigned long long)cpu.reg[X64_RAX].u64,
               (unsigned long long)cpu.reg[X64_R15].u64);
        r.failed++;
        fflush(stdout);
        return;
    }

    bool ok = verify(cpu);
    if (ok) {
        printf("  PASS: %s\n", name);
        r.passed++;
    } else {
        printf("  FAIL: %s  (RIP=0x%llx instr=%llu RAX=0x%llx R15=0x%llx RCX=0x%llx RDI=0x%llx)\n",
               name,
               (unsigned long long)cpu.rip,
               (unsigned long long)cpu.instructionCount,
               (unsigned long long)cpu.reg[X64_RAX].u64,
               (unsigned long long)cpu.reg[X64_R15].u64,
               (unsigned long long)cpu.reg[X64_RCX].u64,
               (unsigned long long)cpu.reg[X64_RDI].u64);
        r.failed++;
    }
    fflush(stdout);
}

// Exit suffix. ksyscall64's exit handler doesn't preserve RAX (it writes
// the syscall return value back into it), so we stash RAX into R15 first
// — R15 is callee-saved and untouched by everything below.
//   49 89 C7                 mov r15, rax
//   48 C7 C0 3C 00 00 00     mov rax, 60     ; exit
//   0F 05                    syscall
const std::vector<U8> EXIT_SYSCALL = {
    0x49, 0x89, 0xC7,
    0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,
    0x0F, 0x05,
};

std::vector<U8> withExit(std::vector<U8> code) {
    code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
    return code;
}

} // anonymous namespace

int runX64SelfTest() {
    printf("\n=== CPU64 self-test ===\n");
    TestResult r;

    // Test 1: mov imm64. After: RAX = 0x1122334455667788.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // mov rax, 0x1122334455667788
        };
        runAndCheck(r, "mov rax, imm64", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x1122334455667788ULL;
        });
    }

    // Test 2: add + sub. RAX = 5 + 3 - 2 = 6.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00, // mov rax, 5
            0x48, 0x83, 0xC0, 0x03,                    // add rax, 3
            0x48, 0x83, 0xE8, 0x02,                    // sub rax, 2
        };
        runAndCheck(r, "add/sub imm8", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 6;
        });
    }

    // Test 3: 32-bit dest zero-extends. mov eax, 0xFFFFFFFF then read full rax.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // mov rax, -1
            0xB8, 0x42, 0x00, 0x00, 0x00,                                // mov eax, 0x42
        };
        runAndCheck(r, "mov eax zero-extends", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x42ULL;
        });
    }

    // Test 4: push + pop. RAX should round-trip a value through stack.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x99, 0x00, 0x00, 0x00, // mov rax, 0x99
            0x50,                                       // push rax
            0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, // mov rax, 0
            0x58,                                       // pop rax
        };
        runAndCheck(r, "push/pop rax", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x99;
        });
    }

    // Test 5: conditional jump. xor sets ZF; jz taken.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                          // xor rax, rax
            0x74, 0x07,                                 // jz +7
            0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
            0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // mov rax, 0xAA (target)
        };
        runAndCheck(r, "xor sets ZF, jz taken", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xAA;
        });
    }

    // Test 6: call + ret. Layout: jmp over func, then call func, then exit.
    //   offset 0:  EB 08            jmp +8 (over func, to entry continuation)
    //   offset 2:  48 C7 C0 77...   mov rax, 0x77    (func body, 7 bytes)
    //   offset 9:  C3               ret              (func end)
    //   offset A:  E8 F3 FF FF FF   call rel32 = -13 → target = 0xF + -13 = 2 ✓
    //   offset F:  (withExit appends here)
    {
        std::vector<U8> code = {
            0xEB, 0x08,
            0x48, 0xC7, 0xC0, 0x77, 0x00, 0x00, 0x00,
            0xC3,
            0xE8, 0xF3, 0xFF, 0xFF, 0xFF,
        };
        runAndCheck(r, "call/ret", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x77;
        });
    }

    // Test 7: LEA RIP-relative.  lea rax, [rip+0]  → RAX = next-RIP.
    {
        std::vector<U8> code = {
            0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00, // lea rax, [rip+0]
        };
        runAndCheck(r, "lea rax, [rip+0]", withExit(code), [](CPU64& c) {
            // After LEA, RAX should equal the address of the instruction
            // immediately following the LEA — that's CODE_BASE+7.
            return c.reg[X64_R15].u64 == CODE_BASE + 7;
        });
    }

    // Test 8: cmp + setcc. cmp 5,3 → CF=0 ZF=0 → sete al = 0; setg al = 1.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00, // mov rax, 5
            0x48, 0x83, 0xF8, 0x03,                    // cmp rax, 3
            0x0F, 0x9F, 0xC0,                          // setg al
            0x48, 0x0F, 0xB6, 0xC0,                    // movzx rax, al
        };
        runAndCheck(r, "cmp/setg/movzx", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1;
        });
    }

    // Test 9: shift. RAX = 1 << 10 = 0x400.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
            0x48, 0xC1, 0xE0, 0x0A,                    // shl rax, 10
        };
        runAndCheck(r, "shl rax, 10", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x400;
        });
    }

    // Test 10: REP STOSQ. Fill 4 qwords with RAX at a stack-page address that
    // is mapped (STACK_TOP-0x800). Verifier checks RCX drained to 0 *and* the
    // first qword equals the pattern.
    //   48 B8 ...  mov rax, 0x1111222233334444   (10 bytes)
    //   48 BF ...  mov rdi, STACK_TOP-0x800      (10 bytes)
    //   48 C7 C1 04 00 00 00   mov rcx, 4         (7 bytes)
    //   F3 48 AB   rep stosq                       (3 bytes)
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xB8, 0x44, 0x44, 0x33, 0x33, 0x22, 0x22, 0x11, 0x11,
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0xC7, 0xC1, 0x04, 0x00, 0x00, 0x00,
            0xF3, 0x48, 0xAB,
        };
        // Verifier: R15 still holds the source pattern, and RDI advanced by
        // 4*8=32 bytes (REP STOSQ wrote 4 qwords). We can't check RCX directly
        // because SYSCALL writes the saved-RIP into RCX per the AMD64 spec.
        runAndCheck(r, "rep stosq", withExit(code), [dst](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x1111222233334444ULL &&
                   c.reg[X64_RDI].u64 == dst + 32;
        });
    }

    // Test 11: MUL r/m64. RDX:RAX = 7 * 6 = 42. RDX should be 0.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x07, 0x00, 0x00, 0x00,             // mov rax, 7
            0x48, 0xC7, 0xC3, 0x06, 0x00, 0x00, 0x00,             // mov rbx, 6
            0x48, 0xF7, 0xE3,                                       // mul rbx
        };
        runAndCheck(r, "mul rbx (7*6=42)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 42 && c.reg[X64_RDX].u64 == 0;
        });
    }

    // Test 12: IDIV r/m64. Signed 64-bit divide. -100 / 7 = -14 rem -2.
    //   CQO sign-extends RAX into RDX:RAX, then IDIV.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x9C, 0xFF, 0xFF, 0xFF,             // mov rax, -100 (sign-ext from imm32)
            0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00,             // mov rbx, 7
            0x48, 0x99,                                             // cqo
            0x48, 0xF7, 0xFB,                                       // idiv rbx
        };
        // After IDIV: RAX = -14 = 0xFFFFFFFFFFFFFFF2, RDX = -2 = 0xFFFFFFFFFFFFFFFE.
        // We stash RAX into R15 via the exit prologue (which expects RAX to
        // be set first), so check both.
        runAndCheck(r, "cqo + idiv (-100/7)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == (U64)(S64)-14 &&
                   c.reg[X64_RDX].u64 == (U64)(S64)-2;
        });
    }

    // Test 13: XCHG rax, rbx round-trips two values.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00,             // mov rax, 0xAA
            0x48, 0xC7, 0xC3, 0xBB, 0x00, 0x00, 0x00,             // mov rbx, 0xBB
            0x48, 0x87, 0xD8,                                       // xchg rax, rbx
        };
        // After XCHG: RAX=0xBB, RBX=0xAA.
        runAndCheck(r, "xchg rax,rbx", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xBB && c.reg[X64_RBX].u64 == 0xAA;
        });
    }

    // Test 14: PUSH imm + POP rax round-trip.
    {
        std::vector<U8> code = {
            0x68, 0x78, 0x56, 0x34, 0x12,                           // push 0x12345678
            0x58,                                                     // pop rax
        };
        runAndCheck(r, "push imm32 / pop rax", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x12345678;
        });
    }

    // Test 15: BSWAP rax. 0x0102030405060708 → 0x0807060504030201.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // mov rax, 0x0102030405060708
            0x48, 0x0F, 0xC8,                                            // bswap rax
        };
        runAndCheck(r, "bswap rax", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0807060504030201ULL;
        });
    }

    // Test 16: CMPXCHG hits. RAX==dest → ZF=1, dest gets r value.
    //   mov rax, 5; mov rbx, 5; mov rcx, 99; cmpxchg rbx, rcx
    //   After: ZF=1, RBX=99. R15 holds RAX (still 5).
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,             // mov rax, 5
            0x48, 0xC7, 0xC3, 0x05, 0x00, 0x00, 0x00,             // mov rbx, 5
            0x48, 0xC7, 0xC1, 0x63, 0x00, 0x00, 0x00,             // mov rcx, 99
            0x48, 0x0F, 0xB1, 0xCB,                                 // cmpxchg rbx, rcx
        };
        runAndCheck(r, "cmpxchg (match)", withExit(code), [](CPU64& c) {
            return c.reg[X64_RBX].u64 == 99 && c.reg[X64_R15].u64 == 5;
        });
    }

    // Test 17: XADD. After: RBX = old(RBX) + old(RAX) = 13; RAX = old(RBX) = 10.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00,             // mov rax, 3
            0x48, 0xC7, 0xC3, 0x0A, 0x00, 0x00, 0x00,             // mov rbx, 10
            0x48, 0x0F, 0xC1, 0xC3,                                 // xadd rbx, rax
        };
        runAndCheck(r, "xadd rbx, rax", withExit(code), [](CPU64& c) {
            return c.reg[X64_RBX].u64 == 13 && c.reg[X64_R15].u64 == 10;
        });
    }

    // Test 18: ROL rax, 4. Rotate 0x1234567890ABCDEF left 4 → 0x234567890ABCDEF1.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x90, 0x78, 0x56, 0x34, 0x12, // mov rax, 0x1234567890ABCDEF
            0x48, 0xC1, 0xC0, 0x04,                                       // rol rax, 4
        };
        runAndCheck(r, "rol rax, 4", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x234567890ABCDEF1ULL;
        });
    }

    // Test 19: ROR rax, 8. Rotate 0x1122334455667788 right 8 → 0x8811223344556677.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // mov rax, 0x1122334455667788
            0x48, 0xC1, 0xC8, 0x08,                                       // ror rax, 8
        };
        runAndCheck(r, "ror rax, 8", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x8811223344556677ULL;
        });
    }

    // Test 20: NOT rax. ~0x00000000FFFFFFFF = 0xFFFFFFFF00000000.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,                     // mov rax, -1 (sign-ext) actually 0xFFFFFFFFFFFFFFFF
            0x48, 0xC1, 0xE8, 0x20,                                       // shr rax, 32  → 0xFFFFFFFF
            0x48, 0xF7, 0xD0,                                             // not rax
        };
        runAndCheck(r, "not rax", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFF00000000ULL;
        });
    }

    // Test 21: NEG rax. -7 in two's complement = 0xFFFFFFFFFFFFFFF9.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x07, 0x00, 0x00, 0x00,                     // mov rax, 7
            0x48, 0xF7, 0xD8,                                             // neg rax
        };
        runAndCheck(r, "neg rax", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF9ULL;
        });
    }

    // Test 22: BTS rax, 7. Start with 0; set bit 7 → 0x80; CF=0 (was clear).
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                             // xor rax, rax
            0x48, 0x0F, 0xBA, 0xE8, 0x07,                                 // bts rax, 7
        };
        runAndCheck(r, "bts rax, 7", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x80ULL;
        });
    }

    // Test 22b: BTR rax, 7. Start with 0xFF; clear bit 7 → 0x7F.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0x00, 0x00, 0x00,                     // mov rax, 0xFF
            0x48, 0x0F, 0xBA, 0xF0, 0x07,                                 // btr rax, 7
        };
        runAndCheck(r, "btr rax, 7 (0xFF → 0x7F)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x7FULL;
        });
    }

    // Test 22c: BTC rax, 0. Toggle bit 0 of zero → 1.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                             // xor rax, rax
            0x48, 0x0F, 0xBA, 0xF8, 0x00,                                 // btc rax, 0
        };
        runAndCheck(r, "btc rax, 0 (0 → 1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1ULL;
        });
    }

    // Test 22d: BT (read-only) — verify CF reflects the bit. rax=0x20, bit 5 set;
    // bt sets CF=1; setc cl captures it; movzx rcx; mov rax,rcx → 1.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x20, 0x00, 0x00, 0x00,                     // mov rax, 0x20
            0x48, 0x0F, 0xBA, 0xE0, 0x05,                                 // bt rax, 5
            0x0F, 0x92, 0xC1,                                             // setc cl
            0x48, 0x0F, 0xB6, 0xC1,                                       // movzx rcx, cl
            0x48, 0x89, 0xC8,                                             // mov rax, rcx
        };
        runAndCheck(r, "bt+setc (bit 5 of 0x20 → CF=1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1ULL;
        });
    }

    // Test 22e: CMOVE taken — xor sets ZF; cmove copies rbx into rax.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                             // xor rax, rax        ; ZF=1
            0x48, 0xC7, 0xC3, 0x42, 0x00, 0x00, 0x00,                     // mov rbx, 0x42
            0x48, 0x0F, 0x44, 0xC3,                                       // cmove rax, rbx
        };
        runAndCheck(r, "cmove taken (ZF=1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x42ULL;
        });
    }

    // Test 22f: CMOVNE not taken — same ZF=1 setup, but cmovne is condition NE.
    // rax must keep its zero value because the move is not performed.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                             // xor rax, rax        ; ZF=1
            0x48, 0xC7, 0xC3, 0x42, 0x00, 0x00, 0x00,                     // mov rbx, 0x42
            0x48, 0x0F, 0x45, 0xC3,                                       // cmovne rax, rbx     ; not taken
        };
        runAndCheck(r, "cmovne not taken (ZF=1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0ULL;
        });
    }

    // Test 22g: CMOVL signed — rax=-1, cmp rax,5 → SF=1, OF=0, SF≠OF → L true.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,                     // mov rax, -1 (sign-extended)
            0x48, 0x83, 0xF8, 0x05,                                       // cmp rax, 5
            0x48, 0xC7, 0xC3, 0xAA, 0x00, 0x00, 0x00,                     // mov rbx, 0xAA
            0x48, 0x0F, 0x4C, 0xC3,                                       // cmovl rax, rbx
        };
        runAndCheck(r, "cmovl taken (-1 < 5 signed)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xAAULL;
        });
    }

    // Test 22h: CMOVB unsigned — rax=3, cmp rax,5 → CF=1 → B (below) true.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00,                     // mov rax, 3
            0x48, 0x83, 0xF8, 0x05,                                       // cmp rax, 5
            0x48, 0xC7, 0xC3, 0xCC, 0x00, 0x00, 0x00,                     // mov rbx, 0xCC
            0x48, 0x0F, 0x42, 0xC3,                                       // cmovb rax, rbx
        };
        runAndCheck(r, "cmovb taken (3 < 5 unsigned)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xCCULL;
        });
    }

    // Test 22i: CMOVcc 32-bit not-taken zero-extension quirk. When the
    // destination is the 32-bit name of a register, *any* write (including the
    // implicit no-op write when CMOVcc is not taken) must zero the upper 32
    // bits. Start with rax = 0xFFFFFFFFFFFFFFFF, run a not-taken cmove eax,ebx,
    // and verify the upper 32 bits are now zero.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // mov rax, -1
            0x48, 0x83, 0xF8, 0x00,                                       // cmp rax, 0  ; ZF=0
            0xBB, 0x42, 0x00, 0x00, 0x00,                                 // mov ebx, 0x42
            0x0F, 0x44, 0xC3,                                             // cmove eax, ebx  ; not taken
        };
        runAndCheck(r, "cmove eax (not taken) zero-extends upper 32", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFULL;
        });
    }

    // Test 22j: SETE — cmp rax,rax forces ZF=1; sete al; movzx → 1.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                             // xor rax, rax
            0x48, 0x39, 0xC0,                                             // cmp rax, rax  ; ZF=1
            0x0F, 0x94, 0xC0,                                             // sete al
            0x48, 0x0F, 0xB6, 0xC0,                                       // movzx rax, al
        };
        runAndCheck(r, "sete (equal → 1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1ULL;
        });
    }

    // Test 22k: SETB — unsigned below, opposite-of-setg pair.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00,                     // mov rax, 3
            0x48, 0x83, 0xF8, 0x05,                                       // cmp rax, 5
            0x0F, 0x92, 0xC0,                                             // setb al
            0x48, 0x0F, 0xB6, 0xC0,                                       // movzx rax, al
        };
        runAndCheck(r, "setb (3 < 5 unsigned → 1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1ULL;
        });
    }

    // Test 22l: SETL — signed less; rax=-1, cmp rax,0 → SF=1, OF=0 → L true.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,                     // mov rax, -1
            0x48, 0x83, 0xF8, 0x00,                                       // cmp rax, 0
            0x0F, 0x9C, 0xC0,                                             // setl al
            0x48, 0x0F, 0xB6, 0xC0,                                       // movzx rax, al
        };
        runAndCheck(r, "setl (-1 < 0 signed → 1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1ULL;
        });
    }

    // Test 23: POPCNT. popcnt(0xFF) = 8.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC3, 0xFF, 0x00, 0x00, 0x00,                     // mov rbx, 0xFF
            0xF3, 0x48, 0x0F, 0xB8, 0xC3,                                 // popcnt rax, rbx
        };
        runAndCheck(r, "popcnt rax, rbx (0xFF→8)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 8;
        });
    }

    // Test 24: BSF rax, rbx where rbx=0x100 → result 8 (low bit set is bit 8).
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC3, 0x00, 0x01, 0x00, 0x00,                     // mov rbx, 0x100
            0x48, 0x0F, 0xBC, 0xC3,                                       // bsf rax, rbx
        };
        runAndCheck(r, "bsf rax, rbx (0x100→8)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 8;
        });
    }

    // Test 25: SHLD eax, ebx, 4.
    //   eax = 0xAAAA0000, ebx = 0x0000BBBB
    //   shld eax, ebx, 4 → eax = (0xAAAA0000 << 4) | (0xBBBB >> 28) = 0xAAA00000 | 0x0 = 0xAAA00000
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0xAA, 0xAA,                                 // mov eax, 0xAAAA0000
            0xBB, 0xBB, 0xBB, 0x00, 0x00,                                 // mov ebx, 0x0000BBBB
            0x0F, 0xA4, 0xD8, 0x04,                                       // shld eax, ebx, 4
        };
        runAndCheck(r, "shld eax, ebx, 4", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xAAA00000ULL;
        });
    }

    // Test 26: RDTSC. Just verify EDX:EAX nonzero (host clock should be nonzero).
    {
        std::vector<U8> code = {
            0x0F, 0x31,                                                   // rdtsc
            0x48, 0x09, 0xD0,                                             // or rax, rdx (collapse into RAX so R15 captures it)
        };
        runAndCheck(r, "rdtsc", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 != 0;
        });
    }

    // Test 27: Iterative fibonacci. fib(10+1) = 89, computed with
    // a/b/swap-via-xchg. Exercises ALU + loop branch + xchg in composition.
    //   xor  rax, rax            ; a = 0
    //   mov  rbx, 1              ; b = 1
    //   mov  rcx, 10             ; n
    // loop:
    //   add  rax, rbx
    //   xchg rax, rbx
    //   dec  rcx
    //   jnz  loop                ; rel8 = -10
    //   mov  rax, rbx            ; result -> rax so it lands in r15
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                             // xor rax, rax
            0x48, 0xC7, 0xC3, 0x01, 0x00, 0x00, 0x00,                     // mov rbx, 1
            0x48, 0xC7, 0xC1, 0x0A, 0x00, 0x00, 0x00,                     // mov rcx, 10
            0x48, 0x01, 0xD8,                                             // add rax, rbx
            0x48, 0x93,                                                   // xchg rax, rbx
            0x48, 0xFF, 0xC9,                                             // dec rcx
            0x75, 0xF6,                                                   // jnz -10 → back to add
            0x48, 0x89, 0xD8,                                             // mov rax, rbx
        };
        runAndCheck(r, "fib(10+1)=89", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 89;
        });
    }

    // Test 28: REP MOVSB. Copy 16 bytes from src to dst within the mapped
    // stack page. Verify byte 7 lands as 0x88.
    //   Layout (offsets from start of code blob):
    //     0..6:  lea rsi, [rip + 29]       ; rip_after=7, src target=36
    //     7..14: lea rdi, [rsp - 0x100]
    //    15..21: mov rcx, 16
    //    22..23: rep movsb
    //    24..26: xor rax, rax
    //    27..33: mov al, [rsp - 0x100 + 7]
    //    34..35: jmp +16   (skip over src bytes so exit suffix runs)
    //    36..51: <16 src bytes>
    {
        std::vector<U8> code = {
            0x48, 0x8D, 0x35, 0x1D, 0x00, 0x00, 0x00,                     //  0..6 lea rsi, [rip+29]
            0x48, 0x8D, 0xBC, 0x24, 0x00, 0xFF, 0xFF, 0xFF,               //  7..14 lea rdi, [rsp-0x100]
            0x48, 0xC7, 0xC1, 0x10, 0x00, 0x00, 0x00,                     // 15..21 mov rcx, 16
            0xF3, 0xA4,                                                   // 22..23 rep movsb
            0x48, 0x31, 0xC0,                                             // 24..26 xor rax, rax
            0x8A, 0x84, 0x24, 0x07, 0xFF, 0xFF, 0xFF,                     // 27..33 mov al, [rsp-0x100+7]
            0xEB, 0x10,                                                   // 34..35 jmp +16 (over src)
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,               // 36..43 src
            0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,               // 44..51 src
        };
        runAndCheck(r, "rep movsb (16B)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x88;
        });
    }

    // Test 29: REPNE SCASB to find byte 0x42 in a sequence.
    //   Layout:
    //     0..6:  lea rdi, [rip + 16]      ; rip_after=7, target=23
    //     7..13: mov rcx, 6
    //    14..15: mov al, 0x42
    //    16..17: repne scasb
    //    18..20: mov rax, rdi
    //    21..22: jmp +6   (skip src to land on exit suffix)
    //    23..28: src
    //   REPNE stops when ZF set. After matching 0x42 at index 3, RDI
    //   points one past match → src + 4 = CODE_BASE + 27.
    {
        std::vector<U8> code = {
            0x48, 0x8D, 0x3D, 0x10, 0x00, 0x00, 0x00,                     //  0..6 lea rdi, [rip+16]
            0x48, 0xC7, 0xC1, 0x06, 0x00, 0x00, 0x00,                     //  7..13 mov rcx, 6
            0xB0, 0x42,                                                   // 14..15 mov al, 0x42
            0xF2, 0xAE,                                                   // 16..17 repne scasb
            0x48, 0x89, 0xF8,                                             // 18..20 mov rax, rdi
            0xEB, 0x06,                                                   // 21..22 jmp +6
            0x11, 0x22, 0x33, 0x42, 0x55, 0x66,                           // 23..28 src
        };
        runAndCheck(r, "repne scasb finds 0x42", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == (CODE_BASE + 27);
        });
    }

    // Test 30: Recursive function call. fact(5) = 120.
    //   _start:
    //     mov rax, 5
    //     call fact
    //     ; result in rax, will be stashed to r15 by exit suffix
    //     jmp end
    //   fact:
    //     cmp rax, 1
    //     jg .rec
    //     ret              ; fact(0)=1 or fact(1)=1 (base ≤1)
    //   .rec:
    //     push rax
    //     dec rax
    //     call fact
    //     pop rcx          ; rcx = original n
    //     ; multiply rax by rcx using IMUL (two-operand)
    //     imul rax, rcx
    //     ret
    //   end:
    //
    // Build it from layout:
    //  0:  48 C7 C0 05 00 00 00          mov rax, 5
    //  7:  E8 09 00 00 00                call rel32 = +9 → 12+9=21? need to retarget
    //  Let me lay it out so call target is correct.
    //
    //  Plan addresses:
    //   _start at 0
    //   fact   at 16
    //   exit code appended after _start return path
    //
    //   0:  48 C7 C0 05 00 00 00          mov rax, 5        (7 bytes)
    //   7:  E8 04 00 00 00                call +4 → 12      WRONG, fact is at 16
    //
    //  Recompute: want call at offset 7 (5 bytes), so call rip = 12, want
    //  target 16, disp = 4.
    //
    //  Then continue with EB <jmp over fact>...
    //
    //   0:  48 C7 C0 05 00 00 00          mov rax, 5         [0..6]
    //   7:  E8 09 00 00 00                call rel32 → fact  [7..11], next rip=12, want 16+5=21. disp=9. ✓
    //  12:  EB 16                          jmp +22 → 36 (end) [12..13]
    //  Wait, fact starts at 16 so we need to skip from 14 to 16: place 2 nops.
    //
    //  Simpler layout — put fact first, then _start:
    //
    //  0:  EB 14                          jmp +20 → 22 (_start) [0..1]
    //  fact at 2:
    //   2:  48 83 F8 01                   cmp rax, 1                [2..5]
    //   6:  7F 01                         jg +1 → 9                 [6..7]
    //   8:  C3                            ret                       [8]
    //   9:  50                            push rax                  [9]
    //  10:  48 FF C8                      dec rax                   [10..12]
    //  13:  E8 EA FF FF FF                call rel32 = -22 → 0+(-22) wait, current rip=18, target=2, disp=-16
    //         actually E8 takes rel32; disp = target - (rip after instr) = 2 - 18 = -16 = 0xFFFFFFF0
    //   correct: 0xF0 0xFF 0xFF 0xFF
    //  13:  E8 F0 FF FF FF                call rel32 = -16 → 2     [13..17]
    //  18:  59                            pop rcx                   [18]
    //  19:  48 0F AF C1                   imul rax, rcx             [19..22]  but wait, _start was at 22!
    //  Need more room.
    //
    //  Let me just put fact at offset 2, _start later.
    //  Recompute with _start at 24:
    //   0:  EB 16                jmp +22 → 24                       [0..1]
    //   2:  48 83 F8 01          cmp rax, 1                          [2..5]
    //   6:  7F 01                jg +1 → 9                           [6..7]
    //   8:  C3                   ret                                 [8]
    //   9:  50                   push rax                            [9]
    //  10:  48 FF C8             dec rax                             [10..12]
    //  13:  E8 F0 FF FF FF       call rel32 = -16 → 2 (rip_after=18)  [13..17]
    //  18:  59                   pop rcx                             [18]
    //  19:  48 0F AF C1          imul rax, rcx                       [19..22]
    //  23:  C3                   ret                                 [23]
    //  24:  48 C7 C0 05 00 00 00 mov rax, 5                          [24..30]
    //  31:  E8 CE FF FF FF       call rel32 = -50 → 2 (rip_after=36, target=2, disp=-34=0xFFFFFFDE)
    //       wait: 36 + (-34) = 2. ✓. disp bytes: DE FF FF FF
    //  36:  (withExit appended)
    {
        std::vector<U8> code = {
            0xEB, 0x16,                                                   //  0: jmp +22
            0x48, 0x83, 0xF8, 0x01,                                       //  2: cmp rax, 1
            0x7F, 0x01,                                                   //  6: jg +1
            0xC3,                                                         //  8: ret
            0x50,                                                         //  9: push rax
            0x48, 0xFF, 0xC8,                                             // 10: dec rax
            0xE8, 0xF0, 0xFF, 0xFF, 0xFF,                                 // 13: call -16
            0x59,                                                         // 18: pop rcx
            0x48, 0x0F, 0xAF, 0xC1,                                       // 19: imul rax, rcx
            0xC3,                                                         // 23: ret
            0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,                     // 24: mov rax, 5
            0xE8, 0xDE, 0xFF, 0xFF, 0xFF,                                 // 31: call -34 → 2
        };
        runAndCheck(r, "fact(5)=120 (recursive)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 120;
        });
    }

    // Test 31: CPUID leaf 1. Verify EDX has SSE2 bit set (bit 26).
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,   // mov rax, 1
            0x48, 0x31, 0xC9,                            // xor rcx, rcx
            0x0F, 0xA2,                                  // cpuid
            0x48, 0x89, 0xD0,                            // mov rax, rdx   (so R15 captures EDX after exit)
        };
        runAndCheck(r, "cpuid leaf 1 → EDX bit26 (SSE2)", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & (1ULL << 26)) != 0;
        });
    }
    // Test 32: PXOR xmm0,xmm0 then MOVD eax,xmm0 → 0.
    // Build: load xmm0 with a known nonzero via memory, pxor it, movd eax.
    {
        std::vector<U8> code = {
            // mov rax, 0xdeadbeef
            0x48, 0xC7, 0xC0, 0xEF, 0xBE, 0xAD, 0xDE,
            // movd xmm0, eax           (66 0F 6E /r — xmm0=eax)
            0x66, 0x0F, 0x6E, 0xC0,
            // pxor xmm0, xmm0          (66 0F EF /r)
            0x66, 0x0F, 0xEF, 0xC0,
            // movd eax, xmm0           (66 0F 7E /r — eax=xmm0)
            0x66, 0x0F, 0x7E, 0xC0,
        };
        runAndCheck(r, "pxor xmm0; movd eax,xmm0 → 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }
    // Test 33: MOVQ round-trip rax → xmm1 → rcx via REX.W MOVD.
    {
        std::vector<U8> code = {
            // mov rax, 0x1122334455667788
            0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
            // movq xmm1, rax           (66 REX.W 0F 6E /r → xmm1=rax)
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            // movq rcx, xmm1           (66 REX.W 0F 7E /r → rcx=xmm1.lo)
            0x66, 0x48, 0x0F, 0x7E, 0xC9,
            // mov rax, rcx
            0x48, 0x89, 0xC8,
        };
        runAndCheck(r, "movq rax↔xmm1 round trip", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x1122334455667788ULL;
        });
    }

    // Test 34: PCMPEQB + PMOVMSKB. Build xmm0 = 16 bytes alternating
    // 0x42/0x00, xmm1 = all 0x00. PCMPEQB xmm0,xmm1 → byte i is 0xFF iff
    // xmm0[i]==0 → bytes 1,3,5,7,9,11,13,15 → mask 0b1010101010101010 = 0xAAAA.
    {
        std::vector<U8> code = {
            // mov rax, 0x0042004200420042
            0x48, 0xB8, 0x42, 0x00, 0x42, 0x00, 0x42, 0x00, 0x42, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            // duplicate to high qword via pshufd-like trick: use punpcklqdq?
            // simpler: load same value into xmm0.hi via movq then unpck. But we
            // don't have unpck. Just set bytes 8..15 too: write via second movq.
            0x66, 0x0F, 0x6E, 0xC8,                                       // movd xmm1, eax (low 4 bytes only)
            // pxor xmm1, xmm1 to clear  (66 0F EF /r)
            0x66, 0x0F, 0xEF, 0xC9,
            // pcmpeqb xmm0, xmm1  (66 0F 74 /r). Lo will give pattern.
            0x66, 0x0F, 0x74, 0xC1,
            // pmovmskb eax, xmm0  (66 0F D7 /r)
            0x66, 0x0F, 0xD7, 0xC0,
        };
        // xmm0.lo = 0x0042004200420042 → bytes 0..7: 42,00,42,00,42,00,42,00
        // xmm0.hi = 0 (untouched by REX-less movq)
        // After pcmpeqb against zero: bytes 0..7 mask: 0,FF,0,FF,0,FF,0,FF
        //                              bytes 8..15: all 0xFF (xmm0.hi was 0)
        // pmovmskb: bits = 1010 1010 (low 8) | 1111 1111 (high 8) = 0xFFAA
        runAndCheck(r, "pcmpeqb + pmovmskb → 0xFFAA", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0xFFFFULL) == 0xFFAAULL;
        });
    }
    // Test 35: PSHUFD with imm=0 broadcasts low dword to all four positions.
    // Load xmm0.lo = 0xAAAAAAAA_BBBBBBBB, PSHUFD with 0 → xmm0 all dwords = BBBBBBBB.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xBB, 0xBB, 0xBB, 0xBB, 0xAA, 0xAA, 0xAA, 0xAA, // mov rax,...
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x66, 0x0F, 0x70, 0xC0, 0x00,                                // pshufd xmm0, xmm0, 0
            // After: xmm0.lo = 0xBBBBBBBBBBBBBBBB, xmm0.hi = 0xBBBBBBBBBBBBBBBB
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "pshufd imm=0 broadcasts low dword", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xBBBBBBBBBBBBBBBBULL;
        });
    }
    // Test 36: PADDD: xmm0 = [0x10000000, 0x20000000, 0, 0]; xmm1 = same;
    // after PADDD xmm0,xmm1 the low dword = 0x20000000, second = 0x40000000.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, // mov rax, 0x2000000010000000
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0xFE, 0xC1,                                       // paddd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "paddd doubles each dword", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4000000020000000ULL;
        });
    }
    // Test 37: PSUBD then PCMPGTD. xmm0 = [5, 5], xmm1 = [3, 7].
    // PSUBD xmm0,xmm1 → [2, -2] (i.e. 0xFFFFFFFE in lane 1).
    // PCMPGTD xmm0, (zeroed xmm2) → lane 0 (2>0)=all 1s, lane 1 (-2>0)=0.
    // Result.lo = 0x00000000_FFFFFFFF.
    {
        std::vector<U8> code = {
            // xmm0.lo = 0x0000000500000005
            0x48, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            // xmm1.lo = 0x0000000700000003
            0x48, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0xFA, 0xC1,                                       // psubd xmm0, xmm1
            // xmm2 = 0  (pxor)
            0x66, 0x0F, 0xEF, 0xD2,
            0x66, 0x0F, 0x66, 0xC2,                                       // pcmpgtd xmm0, xmm2
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "psubd then pcmpgtd vs zero → 0x00000000_FFFFFFFF",
            withExit(code), [](CPU64& c) {
                return c.reg[X64_R15].u64 == 0x00000000FFFFFFFFULL;
            });
    }
    // Test 38: PUNPCKLBW. xmm0.lo = 0x0807060504030201, xmm1.lo = 0xFFEEDDCCBBAA9988.
    // After punpcklbw xmm0,xmm1: interleave low 8 bytes of each.
    // out[i*2]   = d[i] = 01,02,03,04,05,06,07,08
    // out[i*2+1] = s[i] = 88,99,AA,BB,CC,DD,EE,FF
    // Result.lo = bytes 0..7: 01 88 02 99 03 AA 04 BB
    //          = 0xBB04_AA03_9902_8801
    {
        std::vector<U8> code = {
            // xmm0.lo = 0x0807060504030201
            0x48, 0xB8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            // xmm1.lo = 0xFFEEDDCCBBAA9988
            0x48, 0xB8, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0x60, 0xC1,                                       // punpcklbw xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "punpcklbw interleaves low bytes", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xBB04AA0399028801ULL;
        });
    }
    // Test 39: PSLLD xmm0, 4 — shift each dword left by 4.
    // xmm0.lo = 0x00000001_00000002 → 0x00000010_00000020.
    {
        std::vector<U8> code = {
            // rax = 0x0000000100000002
            0x48, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            // pslld xmm0, 4  → 66 0F 72 /6 ib, ModR/M = 11_110_000 = 0xF0
            0x66, 0x0F, 0x72, 0xF0, 0x04,
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pslld xmm0,4 doubles dwords by 16", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000001000000020ULL;
        });
    }
    // Test 40: PSRAW xmm0, 2 — arithmetic right-shift word lanes by 2.
    // xmm0.lo = 0x8000_4000_C000_0004 → words: 0x0004, 0xC000, 0x4000, 0x8000.
    //   0x0004 >> 2 = 0x0001
    //   0xC000 (signed -16384) >> 2 = 0xF000 (signed -4096)
    //   0x4000 >> 2 = 0x1000
    //   0x8000 (signed -32768) >> 2 = 0xE000 (signed -8192)
    // Result = 0xE000_1000_F000_0001
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x04, 0x00, 0x00, 0xC0, 0x00, 0x40, 0x00, 0x80,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            // psraw xmm0, 2  → 66 0F 71 /4 ib, ModR/M = 11_100_000 = 0xE0
            0x66, 0x0F, 0x71, 0xE0, 0x02,
            0x66, 0x48, 0x0F, 0x7E, 0xC0,
        };
        runAndCheck(r, "psraw xmm0,2 sign-extends negatives", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xE0001000F0000001ULL;
        });
    }
    // Test 41: PMINUB. xmm0.lo = 0x10_20_30_40_50_60_70_80,
    //                   xmm1.lo = 0x80_70_60_50_40_30_20_10.
    // Per-byte min → 0x10_20_30_40_40_30_20_10.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0xDA, 0xC1,                                       // pminub xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pminub picks per-byte minimum", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x1020304040302010ULL;
        });
    }
    // Test 42: PMAXSW. Words in xmm0 = {0x0001, 0xFFFF (-1), 0x7FFF, 0x8000 (-32768)}.
    //          Words in xmm1 = {0x0000, 0x0000, 0x0000, 0x0000}.
    // Per-word signed max → {0x0001, 0x0000, 0x7FFF, 0x0000}.
    // Stored little-endian as 0x0000_7FFF_0000_0001.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x80,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x66, 0x0F, 0xEF, 0xC9,                                       // pxor xmm1, xmm1
            0x66, 0x0F, 0xEE, 0xC1,                                       // pmaxsw xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pmaxsw signed-word max with zero clamps negatives", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x00007FFF00000001ULL;
        });
    }
    // Test 43: PSADBW. xmm0 = 16×0xFF, xmm1 = 16×0x00.
    // SAD low half = 8 × 255 = 2040 = 0x7F8. Result.lo = 0x7F8, .hi = 0x7F8.
    // We only read the low qword back into rax — expect 0x7F8.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax (lo)
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax (we'll zero this next)
            // pxor xmm1, xmm1 (zero xmm1 entirely so both lo+hi are 0).
            0x66, 0x0F, 0xEF, 0xC9,
            // pshufd xmm0, xmm0, 0x44 = 01_00_01_00 — copy low 64 bits to both
            // halves so xmm0 = 16 × 0xFF.
            0x66, 0x0F, 0x70, 0xC0, 0x44,
            0x66, 0x0F, 0xF6, 0xC1,                                       // psadbw xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0 (low qword)
        };
        runAndCheck(r, "psadbw 16×0xFF vs 0 → 2040 per half", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x7F8ULL;
        });
    }
    // Test 44: PMULLW. xmm0 = {3, 4, 5, 6}, xmm1 = {2, 2, 2, 2}.
    // Low halves of products: {6, 8, 10, 12} → 0x000C_000A_0008_0006.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0xD5, 0xC1,                                       // pmullw xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pmullw multiplies low 16 bits of words", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x000C000A00080006ULL;
        });
    }
    // Test 45: PMULUDQ. xmm0.lo = 0x0000_0001_0000_0007 → dword0 = 7.
    //                   xmm1.lo = 0x0000_0001_0000_0005 → dword0 = 5.
    // Result low qword = 7 * 5 = 35 = 0x23.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0xF4, 0xC1,                                       // pmuludq xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pmuludq unsigned dword multiply 7*5=35", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x23ULL;
        });
    }
    // Test 46: PACKUSWB. xmm0 words = {0x0050, 0x01FF, 0xFFFE (-2), 0x00FE},
    //                   xmm1 = 0.
    // PACKUSWB saturates signed words → unsigned bytes [0..255].
    // dst low half → bytes [50, FF, 00, FE]; src low half (zero) → [00,00,00,00].
    // Result.lo = 0x0000_0000_FE00_FF50.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x50, 0x00, 0xFF, 0x01, 0xFE, 0xFF, 0xFE, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x66, 0x0F, 0xEF, 0xC9,                                       // pxor xmm1, xmm1
            0x66, 0x0F, 0x67, 0xC1,                                       // packuswb xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "packuswb saturates words to unsigned bytes", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x00000000FE00FF50ULL;
        });
    }
    // Test 47: PMULHUW. xmm0 = {0x8000, 0x4000, 0x0001, 0x0002}, xmm1 same.
    // High 16 of (a*b):
    //   0x8000 * 0x8000 = 0x40000000 → high = 0x4000
    //   0x4000 * 0x4000 = 0x10000000 → high = 0x1000
    //   0x0001 * 0x0001 = 0x00000001 → high = 0x0000
    //   0x0002 * 0x0002 = 0x00000004 → high = 0x0000
    // Result.lo lanes (LSB-first) = {0x4000, 0x1000, 0x0000, 0x0000}
    // = 0x0000_0000_1000_4000.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00, 0x80, 0x00, 0x40, 0x01, 0x00, 0x02, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax (same)
            0x66, 0x0F, 0xE4, 0xC1,                                       // pmulhuw xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pmulhuw stores high 16 of unsigned word products", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000010004000ULL;
        });
    }
    // Test 48: PSLLD (variable form). xmm0.lo = {1, 2}, xmm1.lo = 4 (count).
    // After psld xmm0, xmm1: {16, 32} = 0x0000_0020_0000_0010.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax (cnt=4)
            0x66, 0x0F, 0xF2, 0xC1,                                       // pslld xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pslld variable shift by xmm1 count=4", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000002000000010ULL;
        });
    }
    // Test 49: MOVMSKPS. xmm0.lo = 0x80000000_00000000 → lane0 sign=0, lane1 sign=1.
    //                    xmm0.hi = 0x80000000_80000000 → lane2 sign=1, lane3 sign=1.
    // Result = 0b1110 = 0xE.
    {
        std::vector<U8> code = {
            // Build xmm0.lo = 0x8000000000000000
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax (sets lo, zeros hi)
            // Build rcx = 0x8000000080000000, then punpcklqdq to set xmm0.hi.
            0x48, 0xB9, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80,
            0x66, 0x48, 0x0F, 0x6E, 0xC9,                                // movq xmm1, rcx
            // movlhps xmm0, xmm1  — 0F 16 /r — sets xmm0.hi := xmm1.lo.
            0x0F, 0x16, 0xC1,
            0x0F, 0x50, 0xC0,                                             // movmskps eax, xmm0
            0x49, 0x89, 0xC7,                                             // mov r15, rax
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,                     // mov rax, 60
            0x0F, 0x05,                                                   // syscall (exit)
        };
        runAndCheck(r, "movmskps extracts 4 sign bits", code, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xEULL;
        });
    }
    // Test 50: CLD/STD. After STD, RFLAGS DF bit (0x400) set; after CLD it
    // is cleared. We test by reading the byte at offset 0x46 in rflags by
    // pushing rflags and popping it into rax.
    {
        std::vector<U8> code = {
            0xFD,                         // std
            0x9C,                         // pushfq
            0x58,                         // pop rax            ; rax = rflags with DF=1
            0xFC,                         // cld                 ; restore DF=0 before exiting
            0x49, 0x89, 0xC7,             // mov r15, rax
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00, // mov rax, 60
            0x0F, 0x05,                   // syscall (exit)
        };
        runAndCheck(r, "std sets DF in rflags (bit 0x400)", code, [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0x400ULL) != 0;
        });
    }
    // Test 51: SHUFPS imm=0xE4 (lanes 0,1,2,3 = 0,1,2,3) — identity shuffle.
    // After: xmm0 unchanged. xmm0.lo = 0x0808080804040404,
    //                       xmm1.lo = 0x0202020201010101.
    // imm=0xE4 takes lanes 0,1 from xmm0 and 2,3 from xmm1.
    // Expected: xmm0.lo unchanged, xmm0.hi = xmm1.hi (we set xmm1.hi via
    // punpcklqdq, but simpler: read xmm0.lo back).
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x08, 0x08,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x0F, 0xC6, 0xC1, 0xE4,                                       // shufps xmm0, xmm1, 0xE4
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "shufps identity imm=0xE4 preserves xmm0.lo", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0808080804040404ULL;
        });
    }
    // Test 52: PEXTRW lane 0 of xmm0. xmm0.lo = 0x...1234 → rax = 0x1234.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x34, 0x12, 0x78, 0x56, 0x00, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x66, 0x0F, 0xC5, 0xC0, 0x00,                                 // pextrw eax, xmm0, 0
        };
        runAndCheck(r, "pextrw lane0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x1234ULL;
        });
    }
    // Test 53: PINSRW: zero xmm0, insert 0xABCD at lane 2 → xmm0.lo = 0xABCD_0000_0000_0000.
    {
        std::vector<U8> code = {
            0x66, 0x0F, 0xEF, 0xC0,                                       // pxor xmm0, xmm0
            0x48, 0xC7, 0xC1, 0xCD, 0xAB, 0x00, 0x00,                     // mov rcx, 0xABCD
            0x66, 0x0F, 0xC4, 0xC1, 0x02,                                 // pinsrw xmm0, ecx, 2
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pinsrw lane2 places word in high dword of low qword", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xABCD00000000ULL;
        });
    }
    // Test 54 — end-to-end "hello world" via the write+exit syscalls.
    // Issues write(1, msg, 3) where msg is appended inline to the code
    // buffer, then exits with the write return value stashed in r15.
    // Validates: syscall dispatch, sys_write64 host-stdout tee, RIP-relative
    // LEA, syscall return into RAX, and the SysV ABI register layout.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,             // mov rax, 1            ; SYS_write
            0x48, 0xC7, 0xC7, 0x01, 0x00, 0x00, 0x00,             // mov rdi, 1            ; fd=stdout
            0x48, 0x8D, 0x35, 0x15, 0x00, 0x00, 0x00,             // lea rsi, [rip+0x15]   ; → msg
            0x48, 0xC7, 0xC2, 0x03, 0x00, 0x00, 0x00,             // mov rdx, 3            ; len
            0x0F, 0x05,                                            // syscall               ; write
            0x49, 0x89, 0xC7,                                      // mov r15, rax          ; r15 = bytes written
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,             // mov rax, 60           ; SYS_exit
            0x0F, 0x05,                                            // syscall               ; exit(r15)
            'h', 'i', '\n',                                        // msg: "hi\n"
        };
        runAndCheck(r, "write(1,\"hi\\n\",3) end-to-end via syscall", code, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 3;
        });
    }
    // Test 55 — applyRelativeRelocations end-to-end.
    // Models a relocated module loaded at LOAD_RELOC. Lays out a 4-entry
    // dynamic array (RELA, RELASZ, RELAENT, NULL) and a 3-entry RELA table
    // entirely inside one mapped page at LOAD_RELOC. Each RELA entry says
    // "store (load_base + addend) at offset 0x800+i*8". After the call,
    // destination words should contain LOAD_RELOC + i*0x100 instead of
    // the sentinel we pre-filled.
    {
        const U64 LOAD_RELOC = 0x10000000;  // pretend load slide
        const U64 DYN_OFF    = 0x100;       // dyn array at relocated address
        const U64 RELA_OFF   = 0x200;       // RELA table at relocated address
        const U64 DEST_OFF   = 0x800;       // relocation destinations
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3); // RW

        // RELA table at LOAD_RELOC + RELA_OFF.
        for (U64 i = 0; i < 3; i++) {
            k_Elf64_Rela rela{};
            rela.r_offset = DEST_OFF + i * 8;                // unrelocated
            rela.r_info   = ((U64)0 << 32) | k_R_X86_64_RELATIVE;
            rela.r_addend = (S64)(i * 0x100);
            mem.memcpyToGuest(LOAD_RELOC + RELA_OFF + i * sizeof(rela),
                              &rela, sizeof(rela));
            mem.writeq(LOAD_RELOC + rela.r_offset, 0xDEADBEEFCAFEBABEULL);
        }

        // Dyn array at LOAD_RELOC + DYN_OFF.
        k_Elf64_Dyn dyn[4]{};
        dyn[0].d_tag = k_DT_RELA;    dyn[0].d_un.d_ptr = RELA_OFF; // unrelocated
        dyn[1].d_tag = k_DT_RELASZ;  dyn[1].d_un.d_val = 3 * sizeof(k_Elf64_Rela);
        dyn[2].d_tag = k_DT_RELAENT; dyn[2].d_un.d_val = sizeof(k_Elf64_Rela);
        dyn[3].d_tag = k_DT_NULL;    dyn[3].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        U64 applied = ElfLoader64::applyRelativeRelocations(
            &mem, info, LOAD_RELOC, "selftest");

        bool ok = (applied == 3);
        for (U64 i = 0; i < 3 && ok; i++) {
            U64 got = mem.readq(LOAD_RELOC + DEST_OFF + i * 8);
            U64 want = LOAD_RELOC + i * 0x100;
            if (got != want) {
                printf("  relocation %llu: got 0x%llx want 0x%llx\n",
                       (unsigned long long)i,
                       (unsigned long long)got,
                       (unsigned long long)want);
                ok = false;
            }
        }
        if (ok) {
            printf("  PASS: applyRelativeRelocations: 3 R_X86_64_RELATIVE entries fixed up\n");
            r.passed++;
        } else {
            printf("  FAIL: applyRelativeRelocations (applied=%llu)\n",
                   (unsigned long long)applied);
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: setupStaticTls copies template image and writes TCB self-pointer.
    //   - Image at IMAGE_OFF contains 16 bytes of known data + 8 BSS bytes
    //     (filesz=16, memsz=24, align=8 ⇒ imageSize=24).
    //   - Block mapped at BLOCK_OFF; after setupStaticTls, BLOCK[0..16] must
    //     match the source image, BLOCK[16..24] must be zero, TCB at
    //     BLOCK_OFF+24 must contain BLOCK_OFF+24 (self-pointer).
    {
        const U64 BASE      = 0x20000000;
        const U64 IMAGE_OFF = 0x100;
        const U64 BLOCK_OFF = 0x400;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(BASE, 0x1000, 3);

        // Write a known 16-byte image.
        U8 src[16];
        for (int i = 0; i < 16; i++) src[i] = (U8)(0xA0 + i);
        mem.memcpyToGuest(BASE + IMAGE_OFF, src, sizeof(src));

        // Poison the block region so we can verify it actually gets written.
        for (int i = 0; i < 32; i++) mem.writeb(BASE + BLOCK_OFF + i, 0xFF);

        Elf64TlsInfo tls;
        tls.present = true;
        tls.vaddr   = 0;
        tls.filesz  = 16;
        tls.memsz   = 24;
        tls.align   = 8;

        U64 fsbase = ElfLoader64::setupStaticTls(
            &mem, tls, BASE + IMAGE_OFF, BASE + BLOCK_OFF);

        bool ok = true;
        // Image bytes copied verbatim.
        for (int i = 0; i < 16 && ok; i++) {
            U8 got = mem.readb(BASE + BLOCK_OFF + i);
            if (got != src[i]) {
                printf("  image byte %d: got 0x%02x want 0x%02x\n", i, got, src[i]);
                ok = false;
            }
        }
        // BSS portion zeroed (bytes 16..23).
        for (int i = 16; i < 24 && ok; i++) {
            U8 got = mem.readb(BASE + BLOCK_OFF + i);
            if (got != 0) {
                printf("  bss byte %d: got 0x%02x want 0\n", i, got);
                ok = false;
            }
        }
        // TCB self-pointer at offset 24 (= imageSize-aligned end of image).
        U64 expectedTcb = BASE + BLOCK_OFF + 24;
        U64 gotTcb = mem.readq(expectedTcb);
        if (fsbase != expectedTcb || gotTcb != expectedTcb) {
            printf("  TCB self-pointer: fsbase=0x%llx gotTcb=0x%llx want=0x%llx\n",
                   (unsigned long long)fsbase,
                   (unsigned long long)gotTcb,
                   (unsigned long long)expectedTcb);
            ok = false;
        }
        if (ok) {
            printf("  PASS: setupStaticTls: image copied, bss zeroed, TCB self-pointer set\n");
            r.passed++;
        } else {
            printf("  FAIL: setupStaticTls\n");
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: applySymbolRelocations — synthetic module with one symbol-bound
    // entry in DT_RELA (GLOB_DAT), one in DT_RELA (R_X86_64_64 with addend),
    // one RELATIVE (which the symbol pass must ignore), and one JUMP_SLOT in
    // DT_JMPREL. Resolves against an injected symbol table; verifies the
    // GOT/PLT slots end up with the correct (symbol + addend) bytes.
    {
        const U64 LOAD_RELOC = 0x30000000;
        const U64 DYN_OFF    = 0x100;
        const U64 RELA_OFF   = 0x200;
        const U64 JMPREL_OFF = 0x300;
        const U64 SYMTAB_OFF = 0x400;
        const U64 STRTAB_OFF = 0x500;
        const U64 DEST_OFF   = 0x800;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);

        // String table: leading NUL (ELF requires strtab[0]==0), then names.
        // Offsets: 1="puts", 6="global_var", 17="missing_sym"
        const char* strs = "\0puts\0global_var\0missing_sym";
        const U32 STR_LEN = 1 + 5 + 11 + 12; // = 29
        mem.memcpyToGuest(LOAD_RELOC + STRTAB_OFF, (const void*)strs, STR_LEN);

        // Symbol table: sym[0] = null sym, sym[1] = "puts", sym[2] = "global_var",
        // sym[3] = "missing_sym" (used by the unresolved-counter check below).
        k_Elf64_Sym syms[4]{};
        syms[1].st_name = 1;   // "puts"
        syms[2].st_name = 6;   // "global_var"
        syms[3].st_name = 17;  // "missing_sym"
        mem.memcpyToGuest(LOAD_RELOC + SYMTAB_OFF, syms, sizeof(syms));

        // RELA table: 3 entries.
        //   [0] GLOB_DAT, sym=2 (global_var), addend=0, dst=DEST_OFF+0
        //   [1] R_X86_64_64, sym=1 (puts), addend=8, dst=DEST_OFF+8
        //   [2] RELATIVE, addend=0x123, dst=DEST_OFF+0x10  (ignored by symbol pass)
        k_Elf64_Rela rela[3]{};
        rela[0].r_offset = DEST_OFF + 0x00;
        rela[0].r_info   = ((U64)2 << 32) | k_R_X86_64_GLOB_DAT;
        rela[0].r_addend = 0;
        rela[1].r_offset = DEST_OFF + 0x08;
        rela[1].r_info   = ((U64)1 << 32) | k_R_X86_64_64;
        rela[1].r_addend = 8;
        rela[2].r_offset = DEST_OFF + 0x10;
        rela[2].r_info   = ((U64)0 << 32) | k_R_X86_64_RELATIVE;
        rela[2].r_addend = 0x123;
        mem.memcpyToGuest(LOAD_RELOC + RELA_OFF, rela, sizeof(rela));

        // JMPREL table: 1 entry.
        //   JUMP_SLOT, sym=1 (puts), addend=0, dst=DEST_OFF+0x18
        k_Elf64_Rela plt[1]{};
        plt[0].r_offset = DEST_OFF + 0x18;
        plt[0].r_info   = ((U64)1 << 32) | k_R_X86_64_JUMP_SLOT;
        plt[0].r_addend = 0;
        mem.memcpyToGuest(LOAD_RELOC + JMPREL_OFF, plt, sizeof(plt));

        // Dyn array.
        k_Elf64_Dyn dyn[10]{};
        dyn[0].d_tag = k_DT_RELA;     dyn[0].d_un.d_ptr = RELA_OFF;
        dyn[1].d_tag = k_DT_RELASZ;   dyn[1].d_un.d_val = sizeof(rela);
        dyn[2].d_tag = k_DT_RELAENT;  dyn[2].d_un.d_val = sizeof(k_Elf64_Rela);
        dyn[3].d_tag = k_DT_JMPREL;   dyn[3].d_un.d_ptr = JMPREL_OFF;
        dyn[4].d_tag = k_DT_PLTRELSZ; dyn[4].d_un.d_val = sizeof(plt);
        dyn[5].d_tag = k_DT_SYMTAB;   dyn[5].d_un.d_ptr = SYMTAB_OFF;
        dyn[6].d_tag = k_DT_STRTAB;   dyn[6].d_un.d_ptr = STRTAB_OFF;
        dyn[7].d_tag = k_DT_SYMENT;   dyn[7].d_un.d_val = sizeof(k_Elf64_Sym);
        dyn[8].d_tag = k_DT_NULL;     dyn[8].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        std::unordered_map<std::string, U64> symbols;
        symbols["puts"]       = 0xDEADC0DEULL;
        symbols["global_var"] = 0xABCD1234ULL;

        // Poison destinations.
        mem.writeq(LOAD_RELOC + DEST_OFF + 0x00, 0xFFFFFFFFFFFFFFFFULL);
        mem.writeq(LOAD_RELOC + DEST_OFF + 0x08, 0xFFFFFFFFFFFFFFFFULL);
        mem.writeq(LOAD_RELOC + DEST_OFF + 0x18, 0xFFFFFFFFFFFFFFFFULL);

        U64 resolved = 0, unresolved = 0;
        ElfLoader64::applySymbolRelocations(
            &mem, info, LOAD_RELOC, symbols, "symtest",
            &resolved, &unresolved);

        U64 got0 = mem.readq(LOAD_RELOC + DEST_OFF + 0x00);
        U64 got1 = mem.readq(LOAD_RELOC + DEST_OFF + 0x08);
        U64 got3 = mem.readq(LOAD_RELOC + DEST_OFF + 0x18);
        bool ok = (resolved == 3) && (unresolved == 0) &&
                  (got0 == 0xABCD1234ULL) &&
                  (got1 == 0xDEADC0DEULL + 8) &&
                  (got3 == 0xDEADC0DEULL);
        if (ok) {
            printf("  PASS: applySymbolRelocations: 2 RELA + 1 JMPREL resolved against synthetic symtab\n");
            r.passed++;
        } else {
            printf("  FAIL: applySymbolRelocations resolved=%llu unresolved=%llu got0=0x%llx got1=0x%llx got3=0x%llx\n",
                   (unsigned long long)resolved,
                   (unsigned long long)unresolved,
                   (unsigned long long)got0,
                   (unsigned long long)got1,
                   (unsigned long long)got3);
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: applySymbolRelocations — unresolved symbol path. Replays the
    // setup above but uses sym index 3 (missing_sym) and an EMPTY caller
    // symbol map. Must report unresolved=1 without writing the destination.
    {
        const U64 LOAD_RELOC = 0x31000000;
        const U64 DYN_OFF    = 0x100;
        const U64 RELA_OFF   = 0x200;
        const U64 SYMTAB_OFF = 0x400;
        const U64 STRTAB_OFF = 0x500;
        const U64 DEST_OFF   = 0x800;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);

        const char* strs = "\0missing_sym";
        mem.memcpyToGuest(LOAD_RELOC + STRTAB_OFF, (const void*)strs, 13);

        k_Elf64_Sym syms[2]{};
        syms[1].st_name = 1;
        mem.memcpyToGuest(LOAD_RELOC + SYMTAB_OFF, syms, sizeof(syms));

        k_Elf64_Rela rela[1]{};
        rela[0].r_offset = DEST_OFF;
        rela[0].r_info   = ((U64)1 << 32) | k_R_X86_64_GLOB_DAT;
        rela[0].r_addend = 0;
        mem.memcpyToGuest(LOAD_RELOC + RELA_OFF, rela, sizeof(rela));

        k_Elf64_Dyn dyn[8]{};
        dyn[0].d_tag = k_DT_RELA;     dyn[0].d_un.d_ptr = RELA_OFF;
        dyn[1].d_tag = k_DT_RELASZ;   dyn[1].d_un.d_val = sizeof(rela);
        dyn[2].d_tag = k_DT_RELAENT;  dyn[2].d_un.d_val = sizeof(k_Elf64_Rela);
        dyn[3].d_tag = k_DT_SYMTAB;   dyn[3].d_un.d_ptr = SYMTAB_OFF;
        dyn[4].d_tag = k_DT_STRTAB;   dyn[4].d_un.d_ptr = STRTAB_OFF;
        dyn[5].d_tag = k_DT_SYMENT;   dyn[5].d_un.d_val = sizeof(k_Elf64_Sym);
        dyn[6].d_tag = k_DT_NULL;     dyn[6].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        std::unordered_map<std::string, U64> emptySymbols;

        const U64 POISON = 0xFFFFFFFFFFFFFFFFULL;
        mem.writeq(LOAD_RELOC + DEST_OFF, POISON);

        U64 resolved = 0, unresolved = 0;
        ElfLoader64::applySymbolRelocations(
            &mem, info, LOAD_RELOC, emptySymbols, "missing-symtest",
            &resolved, &unresolved);

        U64 got = mem.readq(LOAD_RELOC + DEST_OFF);
        bool ok = (resolved == 0) && (unresolved == 1) && (got == POISON);
        if (ok) {
            printf("  PASS: applySymbolRelocations: unresolved symbol reported, destination unchanged\n");
            r.passed++;
        } else {
            printf("  FAIL: applySymbolRelocations unresolved-path resolved=%llu unresolved=%llu got=0x%llx\n",
                   (unsigned long long)resolved,
                   (unsigned long long)unresolved,
                   (unsigned long long)got);
            r.failed++;
        }
        fflush(stdout);
    }

    // ---- SSE2 scalar FP coverage ----
    // Helper-style pattern: load doubles into xmm via "mov rax, imm64;
    // movq xmm, rax", perform the op, write the resulting bits back to
    // rax via "movq rax, xmm0", stash in r15 for the verifier.
    //
    // Bit patterns for the test doubles (IEEE-754 binary64):
    //   2.0  = 0x4000000000000000
    //   3.0  = 0x4008000000000000
    //   5.0  = 0x4014000000000000
    //   6.0  = 0x4018000000000000  (2 * 3)
    //   8.0  = 0x4020000000000000
    //   1.5  = 0x3FF8000000000000  (3 / 2)

    // Test 56 — ADDSD xmm0, xmm1: 2.0 + 3.0 = 5.0
    {
        std::vector<U8> code = {
            // mov rax, 0x4000000000000000 (2.0)
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                         // movq xmm0, rax
            // mov rax, 0x4008000000000000 (3.0)
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                         // movq xmm1, rax
            0xF2, 0x0F, 0x58, 0xC1,                               // addsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                         // movq rax, xmm0
        };
        runAndCheck(r, "addsd 2.0 + 3.0 = 5.0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4014000000000000ULL;
        });
    }

    // Test 57 — MULSD xmm0, xmm1: 2.0 * 3.0 = 6.0
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40, // 2.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40, // 3.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            0xF2, 0x0F, 0x59, 0xC1,                               // mulsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,
        };
        runAndCheck(r, "mulsd 2.0 * 3.0 = 6.0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4018000000000000ULL;
        });
    }

    // Test 58 — SUBSD xmm0, xmm1: 5.0 - 3.0 = 2.0
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x40, // 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40, // 3.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            0xF2, 0x0F, 0x5C, 0xC1,                               // subsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,
        };
        runAndCheck(r, "subsd 5.0 - 3.0 = 2.0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4000000000000000ULL;
        });
    }

    // Test 59 — DIVSD xmm0, xmm1: 3.0 / 2.0 = 1.5
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40, // 3.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40, // 2.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            0xF2, 0x0F, 0x5E, 0xC1,                               // divsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,
        };
        runAndCheck(r, "divsd 3.0 / 2.0 = 1.5", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x3FF8000000000000ULL;
        });
    }

    // Test 60 — SQRTSD xmm0, xmm1: sqrt(4.0) = 2.0  (4.0 = 0x4010000000000000)
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x40, // 4.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                         // movq xmm1, rax
            0xF2, 0x0F, 0x51, 0xC1,                               // sqrtsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                         // movq rax, xmm0
        };
        runAndCheck(r, "sqrtsd sqrt(4.0) = 2.0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4000000000000000ULL;
        });
    }

    // Test 61 — CVTSI2SD with REX.W: int64 42 → double 42.0
    // 42.0 = 0x4045000000000000
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00,             // mov rax, 42
            0xF2, 0x48, 0x0F, 0x2A, 0xC0,                         // cvtsi2sd xmm0, rax
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                         // movq rax, xmm0
        };
        runAndCheck(r, "cvtsi2sd int 42 -> 42.0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4045000000000000ULL;
        });
    }

    // Test 62 — CVTSD2SI with REX.W: double 42.0 → int 42
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x45,0x40, // 42.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                         // movq xmm0, rax
            0xF2, 0x48, 0x0F, 0x2D, 0xC0,                         // cvtsd2si rax, xmm0
        };
        runAndCheck(r, "cvtsd2si 42.0 -> 42", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 42;
        });
    }

    // Test 63 — UCOMISD: 5.0 vs 3.0 → greater. Use setcc-style readback
    // by clearing rax and setting al=1 iff CF after the compare. Greater
    // case: CF=0, so al stays 0.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x40, // 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40, // 3.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            0x48, 0x31, 0xC0,                                     // xor rax, rax
            0x66, 0x0F, 0x2E, 0xC1,                               // ucomisd xmm0, xmm1
            0x0F, 0x92, 0xC0,                                     // setb al (1 if CF)
        };
        runAndCheck(r, "ucomisd 5.0 vs 3.0 → CF=0 (greater)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // Test 64 — UCOMISD: 2.0 vs 5.0 → less. CF=1, so setb al sets al=1.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40, // 2.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x40, // 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            0x48, 0x31, 0xC0,                                     // xor rax, rax
            0x66, 0x0F, 0x2E, 0xC1,                               // ucomisd
            0x0F, 0x92, 0xC0,                                     // setb al
        };
        runAndCheck(r, "ucomisd 2.0 vs 5.0 → CF=1 (less)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1;
        });
    }

    // Test 65 — UCOMISD equal: ZF=1. Use sete al.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x40, // 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x40, // 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,
            0x48, 0x31, 0xC0,                                     // xor rax, rax
            0x66, 0x0F, 0x2E, 0xC1,                               // ucomisd
            0x0F, 0x94, 0xC0,                                     // sete al
        };
        runAndCheck(r, "ucomisd 5.0 vs 5.0 → ZF=1 (equal)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1;
        });
    }

    // Test 66 — XGETBV(0) returns EAX=3 (x87+SSE enabled), EDX=0.
    // Encoding: xor ecx,ecx; xgetbv; shl rdx,32; or rax,rdx.
    {
        std::vector<U8> code = {
            0x31, 0xC9,             // xor ecx, ecx
            0x0F, 0x01, 0xD0,       // xgetbv
            0x48, 0xC1, 0xE2, 0x20, // shl rdx, 32
            0x48, 0x09, 0xD0,       // or rax, rdx
        };
        runAndCheck(r, "xgetbv(0) → EDX:EAX = 0:3", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x3;
        });
    }

    // Test 67 — RDTSCP returns ECX=0 (CPU 0). We don't check the TSC value
    // since it's monotonic but synthetic; just verify ECX was zeroed.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC1, 0xFF, 0x00, 0x00, 0x00, // mov rcx, 0xFF (sentinel)
            0x0F, 0x01, 0xF9,                          // rdtscp
            0x48, 0x89, 0xC8,                          // mov rax, rcx (capture ECX→RAX)
        };
        runAndCheck(r, "rdtscp → ECX = 0 (cpu id)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // Test 68 — PSHUFB reverses the low 8 bytes.
    //   xmm0.lo = 0x0706050403020100 (bytes: 0x00,0x01,...,0x07)
    //   xmm1.lo = 0x0001020304050607 (shuffle ctrl: pick src[7],src[6],...,src[0])
    //   PSHUFB xmm0, xmm1
    //   xmm0.lo should be 0x0001020304050607 (reversed bytes)
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x00, 0xC1,                                // pshufb xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "pshufb reverses low 8 bytes", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0001020304050607ULL;
        });
    }

    // Test 69 — PSHUFB zero-byte semantics: ctrl byte with high bit set → 0.
    //   xmm0.lo = 0xDEADBEEFCAFEBABE
    //   xmm1.lo = all 0x80 (every ctrl byte requests "write zero")
    //   PSHUFB xmm0, xmm1 → xmm0.lo should be 0
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xBE, 0xBA, 0xFE, 0xCA, 0xEF, 0xBE, 0xAD, 0xDE,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x00, 0xC1,                                // pshufb xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "pshufb high-bit ctrl produces zero bytes", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // Test 70 — PALIGNR with imm=8 takes the high 8 bytes of src and low 8
    // of dest. With dest.lo=0xAA..., src.lo=0xBB..., src.hi=0, dest.hi=0:
    //   concatenated = src(16B) || dest(16B), shifted right by 8 bytes
    //   the resulting low 16B starts at byte 8 of the concatenation, which is
    //   src.hi (0) followed by dest.lo, so low qword = dest.lo (0xAA...).
    // But our movq sets .hi=0 for both, so result.lo = 0 (src.hi),
    // result.hi = dest.lo (0xAA...). We read back .lo, expecting 0.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08,                          // palignr xmm0, xmm1, 8
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "palignr imm=8 shifts concatenated 32B right by 8", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // Test 71 — PALIGNR imm=0 returns src unchanged (low 16B = src).
    // src.lo = 0xCAFEBABEDEADBEEF, after movq src.hi=0. dest is don't-care.
    // Result.lo = src.lo, so read back rax = 0xCAFEBABEDEADBEEF.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0xBE, 0xBA, 0xFE, 0xCA,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x00,                          // palignr xmm0, xmm1, 0
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "palignr imm=0 copies src to dest", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xCAFEBABEDEADBEEFULL;
        });
    }

    // Test 72 — PCMPEQQ: low qwords differ → low result = 0; high qwords both
    // 0 (from movq) → high result = -1. We read back .lo expecting 0.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00,                     // mov rax, 0xAA
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x48, 0xC7, 0xC0, 0xBB, 0x00, 0x00, 0x00,                     // mov rax, 0xBB
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x29, 0xC1,                                 // pcmpeqq xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pcmpeqq low qwords differ → low lane = 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // Test 73 — PCMPEQQ both qwords equal → both lanes -1, .lo readback = -1.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x42, 0x00, 0x00, 0x00,                     // mov rax, 0x42
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x29, 0xC1,                                 // pcmpeqq xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "pcmpeqq both qwords equal → low lane all-ones", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFFFULL;
        });
    }

    // Test 74 — PTEST sets ZF when (dst & src) == 0.
    //   xmm0 = {0x0F00, 0}, xmm1 = {0x00F0, 0}; AND = 0 → ZF=1.
    //   We readback rflags via pushfq+pop rax then mask ZF (0x40).
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x00, 0x0F, 0x00, 0x00,                     // mov rax, 0x0F00
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x48, 0xC7, 0xC0, 0xF0, 0x00, 0x00, 0x00,                     // mov rax, 0x00F0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x17, 0xC1,                                 // ptest xmm0, xmm1
            0x9C,                                                         // pushfq
            0x58,                                                         // pop rax
            0x48, 0x25, 0x40, 0x00, 0x00, 0x00,                           // and rax, 0x40 (ZF)
        };
        runAndCheck(r, "ptest disjoint bits → ZF=1", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x40;
        });
    }

    // Test 75 — PTEST clears ZF when (dst & src) != 0.
    //   xmm0 = xmm1 = {0xFF, 0} → AND non-zero → ZF=0.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0x00, 0x00, 0x00,                     // mov rax, 0xFF
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x17, 0xC1,                                 // ptest xmm0, xmm1
            0x9C,                                                         // pushfq
            0x58,                                                         // pop rax
            0x48, 0x25, 0x40, 0x00, 0x00, 0x00,                           // and rax, 0x40 (ZF)
        };
        runAndCheck(r, "ptest overlapping bits → ZF=0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // ---- SSE scalar single-precision FP (MOVSS + ADDSS family) ----
    //
    // Pattern: load 32-bit float bits into low dword of an xmm via movd,
    // then run the scalar op, then read low dword back into eax.
    //   movd xmm0, eax   = 66 0F 6E /r  (we use it in 32-bit form, no REX.W)
    //   movd eax, xmm0   = 66 0F 7E /r
    // For two-source ops we use cvtsi2ss to load the second operand from
    // an integer constant directly into xmm1.

    // Test 76 — ADDSS 2.0 + 3.0 = 5.0
    //   IEEE-754 bits: 2.0f=0x40000000, 3.0f=0x40400000, 5.0f=0x40A00000
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0x00, 0x40,           // mov eax, 0x40000000 (2.0f)
            0x66, 0x0F, 0x6E, 0xC0,                 // movd xmm0, eax
            0xB8, 0x00, 0x00, 0x40, 0x40,           // mov eax, 0x40400000 (3.0f)
            0x66, 0x0F, 0x6E, 0xC8,                 // movd xmm1, eax
            0xF3, 0x0F, 0x58, 0xC1,                 // addss xmm0, xmm1
            0x66, 0x0F, 0x7E, 0xC0,                 // movd eax, xmm0
        };
        runAndCheck(r, "addss 2.0f + 3.0f = 5.0f", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x40A00000ULL;
        });
    }

    // Test 77 — MULSS 2.0 * 3.0 = 6.0 (bits 0x40C00000)
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0x00, 0x40,           // mov eax, 2.0f
            0x66, 0x0F, 0x6E, 0xC0,                 // movd xmm0, eax
            0xB8, 0x00, 0x00, 0x40, 0x40,           // mov eax, 3.0f
            0x66, 0x0F, 0x6E, 0xC8,                 // movd xmm1, eax
            0xF3, 0x0F, 0x59, 0xC1,                 // mulss xmm0, xmm1
            0x66, 0x0F, 0x7E, 0xC0,                 // movd eax, xmm0
        };
        runAndCheck(r, "mulss 2.0f * 3.0f = 6.0f", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x40C00000ULL;
        });
    }

    // Test 78 — SUBSS 5.0 - 3.0 = 2.0 (0x40000000)
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0xA0, 0x40,           // mov eax, 5.0f
            0x66, 0x0F, 0x6E, 0xC0,                 // movd xmm0, eax
            0xB8, 0x00, 0x00, 0x40, 0x40,           // mov eax, 3.0f
            0x66, 0x0F, 0x6E, 0xC8,                 // movd xmm1, eax
            0xF3, 0x0F, 0x5C, 0xC1,                 // subss xmm0, xmm1
            0x66, 0x0F, 0x7E, 0xC0,                 // movd eax, xmm0
        };
        runAndCheck(r, "subss 5.0f - 3.0f = 2.0f", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x40000000ULL;
        });
    }

    // Test 79 — DIVSS 3.0 / 2.0 = 1.5 (0x3FC00000)
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0x40, 0x40,           // mov eax, 3.0f
            0x66, 0x0F, 0x6E, 0xC0,                 // movd xmm0, eax
            0xB8, 0x00, 0x00, 0x00, 0x40,           // mov eax, 2.0f
            0x66, 0x0F, 0x6E, 0xC8,                 // movd xmm1, eax
            0xF3, 0x0F, 0x5E, 0xC1,                 // divss xmm0, xmm1
            0x66, 0x0F, 0x7E, 0xC0,                 // movd eax, xmm0
        };
        runAndCheck(r, "divss 3.0f / 2.0f = 1.5f", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x3FC00000ULL;
        });
    }

    // Test 80 — SQRTSS sqrt(4.0) = 2.0
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0x80, 0x40,           // mov eax, 4.0f
            0x66, 0x0F, 0x6E, 0xC8,                 // movd xmm1, eax
            0xF3, 0x0F, 0x51, 0xC1,                 // sqrtss xmm0, xmm1
            0x66, 0x0F, 0x7E, 0xC0,                 // movd eax, xmm0
        };
        runAndCheck(r, "sqrtss sqrt(4.0f) = 2.0f", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x40000000ULL;
        });
    }

    // Test 81 — CVTSI2SS: int 42 → 42.0f (0x42280000)
    {
        std::vector<U8> code = {
            0xB8, 0x2A, 0x00, 0x00, 0x00,           // mov eax, 42
            0xF3, 0x0F, 0x2A, 0xC0,                 // cvtsi2ss xmm0, eax
            0x66, 0x0F, 0x7E, 0xC0,                 // movd eax, xmm0
        };
        runAndCheck(r, "cvtsi2ss int 42 -> 42.0f", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x42280000ULL;
        });
    }

    // Test 82 — CVTSS2SI: 42.0f → int 42
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0x28, 0x42,           // mov eax, 0x42280000 (42.0f)
            0x66, 0x0F, 0x6E, 0xC0,                 // movd xmm0, eax
            0xF3, 0x0F, 0x2D, 0xC0,                 // cvtss2si eax, xmm0
        };
        runAndCheck(r, "cvtss2si 42.0f -> int 42", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 42;
        });
    }

    // Test 83 — UCOMISS: 5.0 vs 3.0 → CF=0 (greater)
    {
        std::vector<U8> code = {
            0xB8, 0x00, 0x00, 0xA0, 0x40,           // mov eax, 5.0f
            0x66, 0x0F, 0x6E, 0xC0,                 // movd xmm0, eax
            0xB8, 0x00, 0x00, 0x40, 0x40,           // mov eax, 3.0f
            0x66, 0x0F, 0x6E, 0xC8,                 // movd xmm1, eax
            0x0F, 0x2E, 0xC1,                       // ucomiss xmm0, xmm1
            0x48, 0x31, 0xC0,                       // xor rax, rax
            0x0F, 0x92, 0xC0,                       // setb al  (CF=1 ⇒ less; here CF=0 ⇒ al=0)
        };
        runAndCheck(r, "ucomiss 5.0f vs 3.0f → CF=0 (greater)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // ---- Packed FP <-> int converts ----

    // Test 84 — CVTDQ2PD: low 2 dwords of src are S32 ints {3,4}.
    //   Result: xmm0.lo = double(3) bits = 0x4008000000000000,
    //           xmm0.hi = double(4) bits = 0x4010000000000000.
    //   We read .lo back into rax and check.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,   // mov rax, (3)|(4<<32)
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                  // movq xmm1, rax
            0xF3, 0x0F, 0xE6, 0xC1,                                        // cvtdq2pd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                  // movq rax, xmm0
        };
        runAndCheck(r, "cvtdq2pd low {3,4} -> {3.0, 4.0}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4008000000000000ULL;
        });
    }

    // Test 85 — CVTPD2DQ: src = {3.0, 4.0} → low.lo = 3 | 4<<32, hi qword = 0.
    {
        std::vector<U8> code = {
            // Build xmm1 = {3.0, 4.0}. Use movq for .lo, then need .hi.
            // Easier: load both doubles via writing through a temp + movdqu...
            // We use a different approach: cvtsi2sd twice to fill .lo,
            // then unpcklpd to shift it. But that's more bytes.
            // Simplest: write to stack and use movdqu via SS:[rsp].
            //
            // Use movq for xmm1.lo=3.0, then F2 0F 12 (movddup) won't help.
            // We use a 2-step build with cvtsi2sd to populate .lo with 3.0,
            // then move .lo into .hi using SHUFPD imm=0, then cvtsi2sd
            // again for the new .lo = 4.0.
            // Wait — SHUFPD with imm=0 copies src.lo into both halves of dst,
            // so dst.hi becomes src.lo. So: cvtsi2sd xmm1, 3 (lo=3.0);
            // shufpd xmm1, xmm1, 0 (hi=lo=3.0); now lo=3.0 and we need 4.0.
            // Then mov rax=4, cvtsi2sd xmm1, rax — that only overwrites lo,
            // leaves hi=3.0. That's wrong; we want hi=4.0.
            //
            // Simpler: use unpcklpd to combine xmm0=3.0 and xmm1=4.0.
            // unpcklpd xmm0, xmm1 produces {xmm0.lo, xmm1.lo} = {3.0, 4.0}.
            0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00,                      // mov rax, 3
            0xF2, 0x48, 0x0F, 0x2A, 0xC0,                                  // cvtsi2sd xmm0, rax  (3.0 → xmm0.lo)
            0x48, 0xC7, 0xC0, 0x04, 0x00, 0x00, 0x00,                      // mov rax, 4
            0xF2, 0x48, 0x0F, 0x2A, 0xC8,                                  // cvtsi2sd xmm1, rax  (4.0 → xmm1.lo)
            0x66, 0x0F, 0x14, 0xC1,                                        // unpcklpd xmm0, xmm1  ({3.0, 4.0})
            0xF2, 0x0F, 0xE6, 0xC8,                                        // cvtpd2dq xmm1, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC8,                                  // movq rax, xmm1
        };
        runAndCheck(r, "cvtpd2dq {3.0, 4.0} -> {3, 4} packed S32", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == ((U64)3 | ((U64)4 << 32));
        });
    }

    // Test 86 — CVTDQ2PS: src.lo = (1)|(2<<32) → result.lo = 1.0f|2.0f<<32
    //   1.0f=0x3F800000, 2.0f=0x40000000 → expected lo = 0x400000003F800000.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,    // mov rax, 1|2<<32
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                   // movq xmm1, rax
            0x0F, 0x5B, 0xC1,                                               // cvtdq2ps xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                   // movq rax, xmm0
        };
        runAndCheck(r, "cvtdq2ps {1,2,0,0} -> {1.0f, 2.0f, 0.0f, 0.0f}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x400000003F800000ULL;
        });
    }

    // Test 87 — CVTPS2DQ: src.lo = 1.0f|2.0f<<32 → result.lo = 1|2<<32
    //   src.hi was zeroed by movq, so dst lanes 2/3 = 0.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40,    // mov rax, bits of {1.0f, 2.0f}
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                   // movq xmm1, rax
            0x66, 0x0F, 0x5B, 0xC1,                                         // cvtps2dq xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                   // movq rax, xmm0
        };
        runAndCheck(r, "cvtps2dq {1.0f, 2.0f, 0, 0} -> {1, 2, 0, 0}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == ((U64)1 | ((U64)2 << 32));
        });
    }

    // Test 88 — MOVNTI: store rax (0xDEADBEEFCAFEBABE) to [rsp-8], then load
    // it back to r15 to verify. We bias on the stack pointer using a small
    // negative disp so we don't have to manage rsp explicitly.
    {
        std::vector<U8> code = {
            // rax = 0xDEADBEEFCAFEBABE
            0x48, 0xB8, 0xBE, 0xBA, 0xFE, 0xCA, 0xEF, 0xBE, 0xAD, 0xDE,
            // movnti [rsp-8], rax    →   REX.W 0F C3 /r with disp8
            //   ModR/M: mod=01 reg=000 rm=100 (SIB) → 0x44; SIB=0x24 (rsp,no-idx); disp=-8(0xF8)
            0x48, 0x0F, 0xC3, 0x44, 0x24, 0xF8,
            // mov r15, [rsp-8]       →  4C 8B 7C 24 F8
            0x4C, 0x8B, 0x7C, 0x24, 0xF8,
            // syscall exit(0) via the existing exit prologue (withExit appends
            // mov r15, rax — but we want the loaded value, so use a direct
            // raw exit syscall here instead).
            0xB8, 0x3C, 0x00, 0x00, 0x00,                                 // mov eax, 60 (exit)
            0x48, 0x31, 0xFF,                                             // xor rdi, rdi
            0x0F, 0x05,                                                   // syscall
        };
        // Use 'code' directly (no withExit) since we already issue the
        // exit syscall ourselves; the runner checks r15.
        runAndCheck(r, "movnti store + load roundtrip preserves 64 bits", code, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xDEADBEEFCAFEBABEULL;
        });
    }

    // Test 89 — MFENCE/LFENCE/SFENCE: all three are no-ops; verify they
    // don't disturb register state by setting r15, fencing 3 times, and
    // reading r15 back.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x42, 0x00, 0x00, 0x00,                     // mov rax, 0x42
            0x0F, 0xAE, 0xE8,                                             // lfence
            0x0F, 0xAE, 0xF0,                                             // mfence
            0x0F, 0xAE, 0xF8,                                             // sfence
        };
        runAndCheck(r, "lfence/mfence/sfence are no-ops", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x42;
        });
    }

    printf("=== self-test summary: %d passed, %d failed ===\n\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}

#endif // BOXEDWINE_GUEST_X64
