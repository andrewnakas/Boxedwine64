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

    printf("=== self-test summary: %d passed, %d failed ===\n\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}

#endif // BOXEDWINE_GUEST_X64
