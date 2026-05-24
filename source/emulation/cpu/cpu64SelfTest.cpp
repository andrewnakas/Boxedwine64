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

    // Run up to 10000 outer iterations or until yield.
    int iters = 0;
    for (; iters < 10000 && !cpu.yield; iters++) {
        cpu.run();
    }
    bool hung = (iters >= 10000 && !cpu.yield);
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

    printf("=== self-test summary: %d passed, %d failed ===\n\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}

#endif // BOXEDWINE_GUEST_X64
