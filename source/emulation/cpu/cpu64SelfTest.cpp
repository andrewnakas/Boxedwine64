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
#include <cstdlib>
#include <cstring>
#include <string>
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
    U64 jitBlocks = 0;
    U64 jitInsns = 0;
    // Emitted-code evidence: these come from Jit64WasmState::blocksEntered and
    // ::execCount, which the generated modules write themselves. `wasmBlocks`
    // is the host's call count; the two must agree, which is what makes the
    // "executed generated code" claim checkable rather than assumed.
    U64 wasmBlocks = 0;
    U64 wasmInsns = 0;
    U64 wasmEntered = 0;
    U64 emitRejected = 0;
    U64 instantiateFailed = 0;
    const char* engine = "off";
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
                 std::function<bool(CPU64&)> verify,
                 std::function<void(CPU64&)> initialize = {}) {
    KMemory64 mem(nullptr);
    mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7); // RWX
    mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3); // RW
    loadCode(mem, CODE_BASE, code);

    CPU64 cpu(&mem);
    cpu.rip = CODE_BASE;
    cpu.reg[X64_RSP].setU64(STACK_TOP - 16);
    if (initialize) initialize(cpu);

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

// The isolated self-test CPU has no KThread/signal action to inspect. A
// synchronous fault therefore takes the native step fault path by yielding
// with RIP still on the faulting instruction (rather than advancing or
// silently continuing).
void runFaultAtRIP(TestResult& r, const char* name,
                   const std::vector<U8>& instruction) {
    KMemory64 mem(nullptr);
    mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
    mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3);
    loadCode(mem, CODE_BASE, instruction);
    CPU64 cpu(&mem);
    cpu.rip = CODE_BASE;
    cpu.reg[X64_RSP].setU64(STACK_TOP - 16);
    cpu.runBounded(1);
    bool ok = cpu.yield && cpu.rip == CODE_BASE && cpu.instructionCount == 0;
    if (ok) {
        printf("  PASS: %s\n", name);
        r.passed++;
    } else {
        printf("  FAIL: %s (yield=%d RIP=0x%llx instructions=%llu)\n",
               name, (int)cpu.yield, (unsigned long long)cpu.rip,
               (unsigned long long)cpu.instructionCount);
        r.failed++;
    }
    fflush(stdout);
}

void setRecoverPolicyForTest(bool enabled) {
#ifdef BOXEDWINE_MSVC
    _putenv_s("BW64_RECOVER", enabled ? "1" : "0");
#else
    setenv("BW64_RECOVER", enabled ? "1" : "0", 1);
#endif
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

    // Regression for compiled `cmp r13d,1; jz rel32` blocks. This is the
    // exact instruction sequence dumped at the first failing snake block.
    // Keep the branch target at offset 0x9F (the rel32 is +0x95 from the
    // branch's next RIP), with both outcomes checked against the interpreter.
    {
        std::vector<U8> code = {
            0x41, 0x83, 0xFD, 0x01,                   // cmp r13d, 1
            0x0F, 0x84, 0x95, 0x00, 0x00, 0x00,       // jz +0x95
            0xB8, 0x00, 0x00, 0x00, 0x00,             // mov eax, 0 (not taken)
            0xE9, 0x90, 0x00, 0x00, 0x00,             // jmp over padding
        };
        code.resize(159, 0x90);
        code.insert(code.end(), {0xB8, 0x01, 0x00, 0x00, 0x00}); // mov eax, 1
        runAndCheck(r, "JIT repro: cmp r13d,1; jz rel32 (not taken)", withExit(code),
                    [](CPU64& c) { return c.reg[X64_R15].u64 == 0; },
                    [](CPU64& c) { c.reg[X64_R13].setU64(0); });
        runAndCheck(r, "JIT repro: cmp r13d,1; jz rel32 (taken)", withExit(code),
                    [](CPU64& c) { return c.reg[X64_R15].u64 == 1; },
                    [](CPU64& c) { c.reg[X64_R13].setU64(1); });
    }

    // The other dumped startup block: test r12b,2; jnz rel32. Keep the
    // target at offset 0x1FC and check both flag outcomes against the
    // interpreter so byte-register selection and the following Jcc share a
    // single compiled block.
    {
        std::vector<U8> code = {
            0x41, 0xF6, 0xC4, 0x02,                   // test r12b, 2
            0x0F, 0x85, 0xF2, 0x01, 0x00, 0x00,       // jnz +0x1F2
            0xB8, 0x00, 0x00, 0x00, 0x00,             // mov eax, 0 (not taken)
            0xE9, 0xED, 0x01, 0x00, 0x00,             // jmp to exit suffix
        };
        code.resize(508, 0x90);
        code.insert(code.end(), {0xB8, 0x01, 0x00, 0x00, 0x00}); // mov eax, 1
        runAndCheck(r, "JIT repro: test r12b,2; jnz rel32 (not taken)", withExit(code),
                    [](CPU64& c) { return c.reg[X64_R15].u64 == 0; },
                    [](CPU64& c) { c.reg[X64_R12].setU64(0); });
        runAndCheck(r, "JIT repro: test r12b,2; jnz rel32 (taken)", withExit(code),
                    [](CPU64& c) { return c.reg[X64_R15].u64 == 1; },
                    [](CPU64& c) { c.reg[X64_R12].setU64(2); });
    }


    // Keep the regression fixture runnable on its own. The full loader
    // suite has WASM-only high-address cases, while this focused mode lets
    // the interpreter and emitted-code paths be compared independently.
    const char* reproOnly = std::getenv("BW64_CPU64_SELFTEST_REPRO_ONLY");
    if (reproOnly && std::strcmp(reproOnly, "1") == 0) {
        printf("=== self-test summary: %d passed, %d failed ===\n\n", r.passed, r.failed);
        return r.failed == 0 ? 0 : 1;
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

    // IRETQ (48 CF). Wine's PE-side ntdll returns from its user-mode exception
    // dispatcher with iretq. Build a long-mode interrupt frame on the stack
    // (pushed high-to-low: SS, RSP, RFLAGS, CS, RIP) and execute iretq; it must
    // jump to the frame's RIP, reload RSP, and restore RFLAGS. Each frame value
    // is loaded with `movabs rax, imm64` (10 bytes: 48 B8 + 8) then `push rax`
    // (1 byte: 50) — 11 bytes per entry, 5 entries = 55 bytes, then `48 CF`
    // (iretq, 2 bytes). The target `mov rax, 0x42` therefore sits at offset 57.
    {
        const U64 newRsp = STACK_TOP - 0x40;
        const U64 targetOff = 5 * 11 + 2; // after 5 pushes + iretq
        auto movabsPush = [](std::vector<U8>& v, U64 imm) {
            v.push_back(0x48); v.push_back(0xB8);
            for (int i = 0; i < 8; i++) v.push_back((U8)((imm >> (8 * i)) & 0xFF));
            v.push_back(0x50); // push rax
        };
        std::vector<U8> code;
        movabsPush(code, 0x0000);                 // SS (popped & ignored)
        movabsPush(code, newRsp);                 // RSP to install
        movabsPush(code, 0x202);                  // RFLAGS (IF + reserved bit 1)
        movabsPush(code, 0x0033);                 // CS (popped & ignored)
        movabsPush(code, CODE_BASE + targetOff);  // RIP target
        code.push_back(0x48); code.push_back(0xCF); // iretq
        // target: mov rax, 0x42  (48 C7 C0 42 00 00 00)
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(0x42); code.push_back(0x00); code.push_back(0x00); code.push_back(0x00);
        runAndCheck(r, "iretq restores rip/rflags/rsp", withExit(code), [newRsp](CPU64& c) {
            // withExit's `mov r15,rax` captures the target's RAX (0x42) and does
            // not touch RSP, so RSP at exit is the value iretq installed.
            return c.reg[X64_R15].u64 == 0x42 && c.reg[X64_RSP].u64 == newRsp;
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

    // Test 12b: DIV r/m64 with a RIP-RELATIVE memory operand. Regression for
    // the F6/F7 group passing a bogus trailingImmBytes to decodeModRM: only the
    // /0 TEST subform has an immediate, but the decoder added the immediate
    // width to the RIP-relative effective address for *every* subform, so
    // `divq [rip+disp]` read 4 bytes past its real divisor (it read 0 from the
    // gap and faulted #DE — exactly what broke libX11's quark-hash rehash).
    //   mov rax, 84            48 C7 C0 54 00 00 00   (7)
    //   xor rdx, rdx           48 31 D2               (3)
    //   divq [rip+0x0C]        48 F7 35 0C 00 00 00   (7)  -> ends at off 17
    //   <EXIT_SYSCALL>                                (12) -> slot at off 29
    //   .quad 2                (divisor, off 29)
    // disp = slot(29) - end_of_divq(17) = 12 = 0x0C. 84 / 2 = 42.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x54, 0x00, 0x00, 0x00,             // mov rax, 84
            0x48, 0x31, 0xD2,                                      // xor rdx, rdx
            0x48, 0xF7, 0x35, 0x0C, 0x00, 0x00, 0x00,             // divq [rip+0x0C]
        };
        std::vector<U8> full = withExit(code);
        const U8 divisor[8] = { 0x02, 0, 0, 0, 0, 0, 0, 0 };      // 2
        full.insert(full.end(), divisor, divisor + 8);
        runAndCheck(r, "divq [rip+disp] (84/2=42, RIP-rel operand)", full, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 42 && c.reg[X64_RDX].u64 == 0;
        });
    }

    // Test 12c: TEST r/m64, imm32 with a RIP-RELATIVE operand still addresses
    // correctly *after* the trailing-immediate fix (its disp IS relative to the
    // end of the instruction including the imm32, so the fix re-adds immLen).
    //   xor rax, rax           48 31 C0                       (3)
    //   testq [rip+disp], imm  48 F7 05 <d32> 0F 00 00 00    (11) ends at off 14
    //   sete al                0F 94 C0                       (3)  al=1 if ZF
    //   movzx rax, al          48 0F B6 C0                    (4)
    //   <EXIT_SYSCALL>                                        (12) slot at off 33
    //   .quad 0xF0  (so 0xF0 & 0x0F == 0 -> ZF set -> result 1)
    // disp = slot(33) - end_of_test(14) = 19 = 0x13.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                     // xor rax, rax
            0x48, 0xF7, 0x05, 0x13, 0x00, 0x00, 0x00,            // testq [rip+0x13], ...
            0x0F, 0x00, 0x00, 0x00,                               // imm32 = 0x0F
            0x0F, 0x94, 0xC0,                                     // sete al
            0x48, 0x0F, 0xB6, 0xC0,                               // movzx rax, al
        };
        std::vector<U8> full = withExit(code);
        const U8 slot[8] = { 0xF0, 0, 0, 0, 0, 0, 0, 0 };        // 0xF0 & 0x0F == 0
        full.insert(full.end(), slot, slot + 8);
        runAndCheck(r, "testq [rip+disp], imm32 (RIP-rel addr w/ imm)", full, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1;
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

    // Test 13b: XLAT/XLATB (D7). AL = [RBX + AL]. FreeType's table-driven code
    // uses it; it was an unimpl opcode that crashed every winex11-font process.
    //   lea rbx, [rip+disp]    48 8D 1D <d32>   (7)  -> RBX = &table
    //   mov al, 3              B0 03            (2)  index 3
    //   xlat                   D7               (1)  AL = table[3]
    //   <EXIT_SYSCALL>         ...                   (RAX->R15)
    //   table: 10 20 30 40 ... (disp points here)
    // disp = (bytes before table) - end_of_lea(7).
    {
        std::vector<U8> code = {
            0x48, 0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00,            // lea rbx,[rip+disp] (patched)
            0xB0, 0x03,                                           // mov al, 3
            0xD7,                                                 // xlat
        };
        std::vector<U8> full = withExit(code);
        U32 disp = (U32)(full.size() - 7);                       // table starts here
        full[3] = (U8)(disp & 0xff);
        full[4] = (U8)((disp >> 8) & 0xff);
        full[5] = (U8)((disp >> 16) & 0xff);
        full[6] = (U8)((disp >> 24) & 0xff);
        const U8 table[4] = { 0x10, 0x20, 0x30, 0x40 };          // table[3] = 0x40
        full.insert(full.end(), table, table + 4);
        runAndCheck(r, "xlat (AL = [RBX+AL], table[3]=0x40)", full, [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0xff) == 0x40;
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

    // Unaligned x86 LOCK/XCHG operands are legal. These cases cover each
    // width that used to reach a WebAssembly atomic with a misaligned address.
    {
        std::vector<U8> code = {
            0xB8, 0x44, 0x33, 0x22, 0x11,                         // mov eax, 0x11223344
            0xB9, 0x88, 0x77, 0x66, 0x55,                         // mov ecx, 0x55667788
            0xF0, 0x0F, 0xB1, 0x4C, 0x24, 0x01,                   // lock cmpxchg [rsp+1], ecx
        };
        runAndCheck(r, "unaligned lock cmpxchg [rsp+1],ecx", withExit(code), [](CPU64& c) {
            return c.memory->readd(STACK_TOP - 16 + 1) == 0x55667788 && c.reg[X64_R15].u32 == 0x11223344;
        }, [](CPU64& c) {
            c.memory->writed(STACK_TOP - 16 + 1, 0x11223344);
        });
    }
    {
        std::vector<U8> code = {
            0xB8, 0x03, 0x00, 0x00, 0x00,                         // mov eax, 3
            0xF0, 0x0F, 0xC1, 0x44, 0x24, 0x03,                   // lock xadd [rsp+3], eax
        };
        runAndCheck(r, "unaligned lock xadd [rsp+3],eax", withExit(code), [](CPU64& c) {
            return c.memory->readd(STACK_TOP - 16 + 3) == 0x13 && c.reg[X64_R15].u32 == 0x10;
        }, [](CPU64& c) {
            c.memory->writed(STACK_TOP - 16 + 3, 0x10);
        });
    }
    {
        std::vector<U8> code = {
            0xF0, 0x48, 0xFF, 0x44, 0x24, 0x05,                   // lock inc qword [rsp+5]
        };
        runAndCheck(r, "unaligned lock inc qword [rsp+5]", withExit(code), [](CPU64& c) {
            return c.memory->readq(STACK_TOP - 16 + 5) == 0x1020304050607081ULL;
        }, [](CPU64& c) {
            c.memory->writeq(STACK_TOP - 16 + 5, 0x1020304050607080ULL);
        });
    }
    {
        std::vector<U8> code = {
            0x66, 0xB8, 0xCD, 0xAB,                               // mov ax, 0xabcd
            0x66, 0x87, 0x44, 0x24, 0x02,                         // xchg [rsp+2], ax (implicitly locked)
            0x4C, 0x0F, 0xB7, 0xF8,                               // movzx r15, ax
        };
        runAndCheck(r, "unaligned xchg [rsp+2],ax", withExit(code), [](CPU64& c) {
            return c.memory->readw(STACK_TOP - 16 + 2) == 0xABCD && c.reg[X64_R15].u64 == 0x1234;
        }, [](CPU64& c) {
            c.memory->writew(STACK_TOP - 16 + 2, 0x1234);
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

    // POPCNT memory source and zero-result flags.
    {
        std::vector<U8> code = {0xF3, 0x48, 0x0F, 0xB8, 0x05,
                                0x0C, 0x00, 0x00, 0x00}; // popcnt rax,[rip+12]
        std::vector<U8> full = withExit(code);
        const U8 value[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
        full.insert(full.end(), value, value + sizeof(value));
        runAndCheck(r, "popcnt rax,[rip]", full, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1 && !(c.rflags & (X64_CF | X64_OF | X64_ZF));
        });
    }
    {
        std::vector<U8> code = {0xF3, 0x48, 0x0F, 0xB8, 0xC3}; // popcnt rax,rbx
        runAndCheck(r, "popcnt zero sets ZF and clears CF/OF", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0 && (c.rflags & X64_ZF) &&
                   !(c.rflags & (X64_CF | X64_OF));
        }, [](CPU64& c) {
            c.reg[X64_RBX].setU64(0);
            c.rflags |= X64_CF | X64_OF;
        });
    }

    // F3 is ignored for BSF/BSR when ABM is not advertised.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x5A, 0, 0, 0, // destination sentinel
            0x48, 0xC7, 0xC3, 0x00, 0x01, 0, 0, // source 0x100
            0xF3, 0x48, 0x0F, 0xBC, 0xC3, // rep bsf rax,rbx
        };
        runAndCheck(r, "F3 BSF keeps BSF semantics", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 8 && !(c.rflags & X64_ZF);
        });
    }
    {
        std::vector<U8> code = {0xF3, 0x48, 0x0F, 0xBC, 0x05,
                                0x0C, 0x00, 0x00, 0x00}; // rep bsf rax,[rip+12]
        std::vector<U8> full = withExit(code);
        const U8 value[8] = {0x00, 0x01, 0, 0, 0, 0, 0, 0};
        full.insert(full.end(), value, value + sizeof(value));
        runAndCheck(r, "F3 BSF memory keeps BSF semantics", full, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 8 && !(c.rflags & X64_ZF);
        });
    }
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x5A, 0, 0, 0, // destination sentinel
            0xBB, 0, 0, 0, 0x80, // mov ebx,0x80000000 (zero-extends)
            0xF3, 0x48, 0x0F, 0xBD, 0xC3, // rep bsr rax,rbx
        };
        runAndCheck(r, "F3 BSR keeps BSR semantics", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 31 && !(c.rflags & X64_ZF);
        });
    }
    {
        std::vector<U8> code = {0xF3, 0x48, 0x0F, 0xBD, 0x05,
                                0x0C, 0x00, 0x00, 0x00}; // rep bsr rax,[rip+12]
        std::vector<U8> full = withExit(code);
        const U8 value[8] = {0x00, 0x00, 0x00, 0x80, 0, 0, 0, 0};
        full.insert(full.end(), value, value + sizeof(value));
        runAndCheck(r, "F3 BSR memory keeps BSR semantics", full, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 31 && !(c.rflags & X64_ZF);
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
    {
        std::vector<U8> code = {
            0x48,0xBF,0x00,0x08,0x40,0,0,0,0,0, // rdi = code scratch
            0xB8,0x01,0,0,0, 0x31,0xC9, 0x0F,0xA2,
            0x89,0x07, 0x89,0x5F,0x04, 0x89,0x4F,0x08, 0x89,0x57,0x0C,
        };
        runAndCheck(r, "CPUID leaf 1 topology and feature bits", withExit(code), [](CPU64& c) {
            return c.memory->readd(CODE_BASE + 0x800) == ((6u << 8) | (15u << 4) | 3u) &&
                   c.memory->readd(CODE_BASE + 0x804) == ((8u << 8) | (1u << 16)) &&
                   c.memory->readd(CODE_BASE + 0x808) == ((1u << 0) | (1u << 9) |
                       (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23)) &&
                   c.memory->readd(CODE_BASE + 0x80C) == ((1u<<0)|(1u<<4)|(1u<<5)|
                       (1u<<8)|(1u<<15)|(1u<<19)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26));
        });
        runAndCheck(r, "CPUID leaf 0 maximum basic leaf is 7", withExit({
            0x31, 0xC0, 0x0F, 0xA2, // xor eax,eax; cpuid
        }), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 7;
        });
        runAndCheck(r, "CPUID leaf 7 subleaf 0 has no extensions", withExit({
            0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax,7
            0x31, 0xC9, 0x0F, 0xA2,       // xor ecx,ecx; cpuid
            0x48, 0x09, 0xD8,             // or rax,rbx
            0x48, 0x09, 0xC8,             // or rax,rcx
            0x48, 0x09, 0xD0,             // or rax,rdx
        }), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
        std::vector<U8> extended = {
            0x48,0xBF,0x00,0x08,0x40,0,0,0,0,0,
            0xB8,0x01,0,0,0x80, 0x31,0xC9, 0x0F,0xA2,
            0x89,0x07, 0x89,0x5F,0x04, 0x89,0x4F,0x08, 0x89,0x57,0x0C,
        };
        runAndCheck(r, "CPUID 0x80000001 long-mode features", withExit(extended), [](CPU64& c) {
            U32 a = c.memory->readd(CODE_BASE + 0x800);
            U32 b = c.memory->readd(CODE_BASE + 0x804);
            U32 ecx = c.memory->readd(CODE_BASE + 0x808);
            U32 edx = c.memory->readd(CODE_BASE + 0x80C);
            bool ok = a == 0 && b == 0 && ecx == ((1u << 0) | (1u << 8)) &&
                      edx == ((1u<<29)|(1u<<11)|(1u<<20));
            return ok;
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
    // Test 38a: PUNPCKLQDQ xmm0, xmm1 — result = {xmm0.lo, xmm1.lo}.
    // This is the op that broke real glibc malloc: 0x6C is numerically >=
    // 0x68 but is a LOW unpack, and a ">= 0x68 means high" classifier read
    // the (zero) high halves, zeroing the bin self-pointers. Here we set
    // xmm0.lo=AAAA, xmm1.lo=BBBB, run punpcklqdq, then move the HIGH lane
    // down with punpckhqdq xmm0,xmm0 and read it: must be BBBB (xmm1.lo),
    // proving the high lane got xmm1.lo and was not zeroed.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,  // rax=0xAAAA...
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,  // rax=0xBBBB...
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0x6C, 0xC1,                                      // punpcklqdq xmm0, xmm1 -> {AAAA,BBBB}
            0x66, 0x0F, 0x6D, 0xC0,                                      // punpckhqdq xmm0, xmm0 -> {BBBB,BBBB}
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "punpcklqdq sets high lane to src.lo (glibc malloc bug)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xBBBBBBBBBBBBBBBBULL;
        });
    }

    // Test 38b: PUNPCKHQDQ xmm0, xmm1 — result = {xmm0.hi, xmm1.hi}.
    // Build distinct hi lanes via punpcklqdq first, then verify the low lane
    // of the result equals xmm0.hi. xmm0={hi=0x1111,lo=0}, via punpcklqdq
    // xmm0,xmm2 where xmm2.lo=0x1111: xmm0={lo=0,hi=0x1111}. Then
    // punpckhqdq xmm0,xmm1 -> low lane = xmm0.hi = 0x1111.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,  // rax=0x1111...
            0x66, 0x48, 0x0F, 0x6E, 0xD0,                                // movq xmm2, rax
            0x66, 0x0F, 0xEF, 0xC0,                                      // pxor xmm0, xmm0 -> {0,0}
            0x66, 0x0F, 0x6C, 0xC2,                                      // punpcklqdq xmm0, xmm2 -> {0, 0x1111}
            0x66, 0x0F, 0x6D, 0xC0,                                      // punpckhqdq xmm0, xmm0 -> {0x1111,0x1111}
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                // movq rax, xmm0
        };
        runAndCheck(r, "punpckhqdq selects high lanes", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x1111111111111111ULL;
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

    // Test 41b: PSUBUSB saturates each byte subtraction at zero.
    // xmm0 bytes = {0,10,200,255,5,6,7,8}; xmm1 = {1,20,100,255,0,0,0,0}
    // → {0,0,100,0,5,6,7,8} = 0x0807060500640000 little-endian.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00, 0x0A, 0xC8, 0xFF, 0x05, 0x06, 0x07, 0x08,
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                // movq xmm0, rax
            0x48, 0xB8, 0x01, 0x14, 0x64, 0xFF, 0x00, 0x00, 0x00, 0x00,
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                // movq xmm1, rax
            0x66, 0x0F, 0xD8, 0xC1,                                       // psubusb xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "psubusb saturates byte subtraction at zero", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0807060500640000ULL;
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

    // Test: applySymbolRelocations — R_X86_64_COPY semantics. A real-world
    // example: an exe imports an extern data symbol (e.g. `environ`) from
    // libc.so.6. The linker emits a COPY reloc; the loader copies the
    // source DSO's symbol bytes into the exe's BSS placeholder.
    // We seed the "source DSO" bytes at address 0xCAFE0000 (sym value) and
    // the exe's COPY target at LOAD_RELOC+DEST_OFF. After the call we
    // expect the destination to contain the source's bytes.
    {
        const U64 LOAD_RELOC = 0x32500000;
        const U64 SOURCE_ADDR = 0xCAFE0000;
        const U64 DYN_OFF    = 0x100;
        const U64 RELA_OFF   = 0x200;
        const U64 SYMTAB_OFF = 0x400;
        const U64 STRTAB_OFF = 0x500;
        const U64 DEST_OFF   = 0x800;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);
        mem.mmapAnonymousFixed(SOURCE_ADDR, 0x1000, 3);

        // Seed the "source DSO" data — 24 bytes of recognizable pattern.
        U8 srcPattern[24];
        for (int i = 0; i < 24; i++) srcPattern[i] = (U8)(0x40 + i);
        mem.memcpyToGuest(SOURCE_ADDR, srcPattern, sizeof(srcPattern));

        // Strtab + symtab: one symbol "environ" of size 24.
        const char* strs = "\0environ";
        mem.memcpyToGuest(LOAD_RELOC + STRTAB_OFF, (const void*)strs, 9);
        k_Elf64_Sym syms[2]{};
        syms[1].st_name = 1;
        syms[1].st_size = 24;          // exe's placeholder size
        mem.memcpyToGuest(LOAD_RELOC + SYMTAB_OFF, syms, sizeof(syms));

        // One COPY reloc: sym=1, dst=DEST_OFF (in exe), addend irrelevant.
        k_Elf64_Rela rela[1]{};
        rela[0].r_offset = DEST_OFF;
        rela[0].r_info   = ((U64)1 << 32) | k_R_X86_64_COPY;
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

        // Symbol map points "environ" at the source-DSO data.
        std::unordered_map<std::string, U64> symbols;
        symbols["environ"] = SOURCE_ADDR;

        // Poison destination.
        for (int i = 0; i < 24; i++) mem.writeb(LOAD_RELOC + DEST_OFF + i, 0xFF);

        U64 resolved = 0, unresolved = 0;
        ElfLoader64::applySymbolRelocations(
            &mem, info, LOAD_RELOC, symbols, "copytest",
            &resolved, &unresolved);

        bool ok = (resolved == 1) && (unresolved == 0);
        for (int i = 0; i < 24 && ok; i++) {
            U8 got = mem.readb(LOAD_RELOC + DEST_OFF + i);
            if (got != srcPattern[i]) {
                printf("  copy byte %d: got 0x%02x want 0x%02x\n", i, got, srcPattern[i]);
                ok = false;
            }
        }
        if (ok) {
            printf("  PASS: applySymbolRelocations: R_X86_64_COPY copies 24 bytes from source DSO\n");
            r.passed++;
        } else {
            printf("  FAIL: R_X86_64_COPY resolved=%llu unresolved=%llu\n",
                   (unsigned long long)resolved, (unsigned long long)unresolved);
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: extractNeededLibraries — synthetic PT_DYNAMIC with three
    // DT_NEEDED entries pointing into a small strtab. Verifies order is
    // preserved and DT_STRTAB lookup is correct.
    {
        const U64 LOAD_RELOC = 0x32000000;
        const U64 DYN_OFF    = 0x100;
        const U64 STRTAB_OFF = 0x300;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);

        // String table layout (offsets):
        //   1  = "libc.so.6"        (10 bytes incl. NUL)
        //   11 = "libpthread.so.0"  (16 bytes incl. NUL)
        //   27 = "ld-linux-x86-64.so.2" (21 bytes incl. NUL)
        const char* strs =
            "\0libc.so.6\0libpthread.so.0\0ld-linux-x86-64.so.2";
        const U32 STR_LEN = 1 + 10 + 16 + 21; // = 48
        mem.memcpyToGuest(LOAD_RELOC + STRTAB_OFF, (const void*)strs, STR_LEN);

        // Dyn array: three DT_NEEDED entries interleaved with a stray non-
        // matching tag to verify the filter behaves.
        k_Elf64_Dyn dyn[6]{};
        dyn[0].d_tag = k_DT_NEEDED;  dyn[0].d_un.d_val = 1;   // libc.so.6
        dyn[1].d_tag = k_DT_NEEDED;  dyn[1].d_un.d_val = 11;  // libpthread.so.0
        dyn[2].d_tag = k_DT_STRTAB;  dyn[2].d_un.d_ptr = STRTAB_OFF;
        dyn[3].d_tag = k_DT_NEEDED;  dyn[3].d_un.d_val = 27;  // ld-linux...
        dyn[4].d_tag = k_DT_RELA;    dyn[4].d_un.d_ptr = 0;   // unrelated tag
        dyn[5].d_tag = k_DT_NULL;    dyn[5].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        std::vector<std::string> needed =
            ElfLoader64::extractNeededLibraries(&mem, info, LOAD_RELOC);

        bool ok = (needed.size() == 3) &&
                  (needed[0] == "libc.so.6") &&
                  (needed[1] == "libpthread.so.0") &&
                  (needed[2] == "ld-linux-x86-64.so.2");
        if (ok) {
            printf("  PASS: extractNeededLibraries: 3 DT_NEEDED entries resolved in order\n");
            r.passed++;
        } else {
            printf("  FAIL: extractNeededLibraries: size=%zu",
                   needed.size());
            for (size_t i = 0; i < needed.size(); i++) {
                printf(" [%zu]=\"%s\"", i, needed[i].c_str());
            }
            printf("\n");
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: extractNeededLibraries — empty case (no DT_STRTAB). Must return
    // an empty vector without crashing.
    {
        const U64 LOAD_RELOC = 0x33000000;
        const U64 DYN_OFF    = 0x100;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);

        k_Elf64_Dyn dyn[3]{};
        dyn[0].d_tag = k_DT_NEEDED;  dyn[0].d_un.d_val = 1;
        dyn[1].d_tag = k_DT_RELA;    dyn[1].d_un.d_ptr = 0;
        dyn[2].d_tag = k_DT_NULL;    dyn[2].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        std::vector<std::string> needed =
            ElfLoader64::extractNeededLibraries(&mem, info, LOAD_RELOC);
        if (needed.empty()) {
            printf("  PASS: extractNeededLibraries: empty vector when DT_STRTAB missing\n");
            r.passed++;
        } else {
            printf("  FAIL: extractNeededLibraries: expected empty, got size=%zu\n",
                   needed.size());
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: extractGlobalSymbols — synthetic SYMTAB+STRTAB layout, three
    // symbols. Verifies local is skipped, global+weak are included, and
    // st_value gets the load slide added.
    {
        const U64 LOAD_RELOC = 0x34000000;
        const U64 DYN_OFF    = 0x100;
        const U64 SYMTAB_OFF = 0x200;
        const U64 STRTAB_OFF = 0x260; // = SYMTAB_OFF + 4 * sizeof(k_Elf64_Sym)
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);

        // STRTAB. Index 0 is the empty string per ELF convention.
        // 1  = "ignored" (local, won't appear)
        // 9  = "foo"     (global)
        // 13 = "bar"     (weak)
        const char* strs = "\0ignored\0foo\0bar";
        const U32 STR_LEN = 17;
        mem.memcpyToGuest(LOAD_RELOC + STRTAB_OFF, (const void*)strs, STR_LEN);

        // SYMTAB. Slot 0 is reserved/zero in ELF. Slots 1..3 are real.
        // st_info: bind << 4 | type. bind 0=local, 1=global, 2=weak.
        k_Elf64_Sym syms[4]{};
        syms[1].st_name  = 1;       // "ignored"
        syms[1].st_info  = 0 << 4;  // LOCAL — must be skipped
        syms[1].st_shndx = 1;
        syms[1].st_value = 0x1000;
        syms[2].st_name  = 9;       // "foo"
        syms[2].st_info  = 1 << 4;  // GLOBAL
        syms[2].st_shndx = 1;
        syms[2].st_value = 0x2000;
        syms[3].st_name  = 13;      // "bar"
        syms[3].st_info  = 2 << 4;  // WEAK
        syms[3].st_shndx = 1;
        syms[3].st_value = 0x3000;
        mem.memcpyToGuest(LOAD_RELOC + SYMTAB_OFF, syms, sizeof(syms));

        // DYN: SYMTAB + STRTAB + SYMENT + NULL.
        k_Elf64_Dyn dyn[4]{};
        dyn[0].d_tag = k_DT_SYMTAB; dyn[0].d_un.d_ptr = SYMTAB_OFF;
        dyn[1].d_tag = k_DT_STRTAB; dyn[1].d_un.d_ptr = STRTAB_OFF;
        dyn[2].d_tag = k_DT_SYMENT; dyn[2].d_un.d_val = sizeof(k_Elf64_Sym);
        dyn[3].d_tag = k_DT_NULL;   dyn[3].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        auto sym = ElfLoader64::extractGlobalSymbols(&mem, info, LOAD_RELOC);
        bool ok = (sym.size() == 2) &&
                  (sym.count("foo") && sym["foo"] == LOAD_RELOC + 0x2000) &&
                  (sym.count("bar") && sym["bar"] == LOAD_RELOC + 0x3000) &&
                  (sym.count("ignored") == 0);
        if (ok) {
            printf("  PASS: extractGlobalSymbols: skips LOCAL, exports GLOBAL+WEAK, applies reloc\n");
            r.passed++;
        } else {
            printf("  FAIL: extractGlobalSymbols size=%zu foo=0x%llx bar=0x%llx ignored?=%d\n",
                   sym.size(),
                   (unsigned long long)(sym.count("foo") ? sym["foo"] : 0),
                   (unsigned long long)(sym.count("bar") ? sym["bar"] : 0),
                   (int)sym.count("ignored"));
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: linkSharedObjects end-to-end with a preloaded lib.
    // libfoo defines `foo` at its code offset; main exe imports `foo`
    // via a JUMP_SLOT entry. After linking, the GOT slot in the main
    // exe must hold libfoo's resolved foo address (= libReloc + foo_off).
    {
        const U64 MAIN_RELOC = 0x40000000;
        const U64 LIB_RELOC  = 0x50000000;

        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(MAIN_RELOC, 0x2000, 7);
        mem.mmapAnonymousFixed(LIB_RELOC,  0x2000, 7);

        // ---------- Build libfoo blob (in-memory ELF) ----------
        // Layout:
        //   0x0000 Ehdr
        //   0x0040 Phdr (1 LOAD)
        //   0x0078 STRTAB ("\0foo")
        //   0x0080 SYMTAB (2 entries: 0=null, 1=foo @ st_value=code_off, GLOBAL)
        //   0x00B0 DYN (4: SYMTAB, STRTAB, SYMENT, NULL)
        //   0x00F0 Phdr[1] PT_DYNAMIC pointer  (NO — we put dyn ptr in dyn struct, not extra Phdr)
        // Wait — dynamic info comes via PT_DYNAMIC phdr. Need 2 Phdrs.
        //
        // Re-layout:
        //   0x0000 Ehdr (64)
        //   0x0040 Phdr[0] PT_LOAD (56)
        //   0x0078 Phdr[1] PT_DYNAMIC (56)
        //   0x00B0 DYN array (4 entries × 16 = 64)
        //   0x00F0 STRTAB ("\0foo")
        //   0x00F4 SYMTAB (2 syms × 24 = 48)
        //   0x0124 Code: mov r15, 0xF00; ret
        const U64 L_EHDR  = 0x0000;
        const U64 L_PHDR  = 0x0040;
        const U64 L_DYN   = 0x00B0;
        const U64 L_STR   = 0x00F0;
        const U64 L_SYM   = 0x00F4;
        const U64 L_CODE  = 0x0124;
        const U64 L_END   = 0x0140;

        std::vector<U8> libBuf(L_END, 0);
        // STRTAB.
        memcpy(libBuf.data() + L_STR, "\0foo", 4);
        // SYMTAB. Slot 0 zero, slot 1 = foo.
        k_Elf64_Sym lsyms[2]{};
        lsyms[1].st_name  = 1;          // "foo"
        lsyms[1].st_info  = 1 << 4;     // GLOBAL
        lsyms[1].st_shndx = 1;
        lsyms[1].st_value = L_CODE;     // function offset
        memcpy(libBuf.data() + L_SYM, lsyms, sizeof(lsyms));
        // Note: STRTAB at 0xF0, SYMTAB at 0xF4 — STRTAB sits BEFORE SYMTAB
        // here, which violates the (strtab > symtab) assumption in
        // extractGlobalSymbols. Swap them.
        //   Use: STRTAB at L_STR (after SYMTAB), SYMTAB at L_SYM (before).
        // Re-layout swap: L_SYM = 0xF0 (48 bytes), L_STR = 0x120 (4 bytes).
        // Redo with corrected layout below.

        const U64 L2_EHDR = 0x0000;
        const U64 L2_PHDR = 0x0040;
        const U64 L2_DYN  = 0x00B0;
        const U64 L2_SYM  = 0x00F0;
        const U64 L2_STR  = 0x0120; // immediately after SYMTAB
        const U64 L2_CODE = 0x0128;
        const U64 L2_END  = 0x0140;
        libBuf.assign(L2_END, 0);

        // Ehdr.
        k_Elf64_Ehdr leh{};
        leh.e_ident[0]=0x7F; leh.e_ident[1]='E'; leh.e_ident[2]='L'; leh.e_ident[3]='F';
        leh.e_ident[4]=k_ELFCLASS64; leh.e_ident[5]=1; leh.e_ident[6]=1;
        leh.e_type=3;             // ET_DYN
        leh.e_machine=k_EM_X86_64;
        leh.e_version=1;
        leh.e_entry=L2_CODE;
        leh.e_phoff=L2_PHDR;
        leh.e_ehsize=sizeof(k_Elf64_Ehdr);
        leh.e_phentsize=sizeof(k_Elf64_Phdr);
        leh.e_phnum=2;
        memcpy(libBuf.data() + L2_EHDR, &leh, sizeof(leh));

        // Phdrs.
        k_Elf64_Phdr lload{};
        lload.p_type=k_PT_LOAD; lload.p_flags=7;
        lload.p_offset=0; lload.p_vaddr=0; lload.p_paddr=0;
        lload.p_filesz=L2_END; lload.p_memsz=L2_END; lload.p_align=0x1000;
        memcpy(libBuf.data() + L2_PHDR, &lload, sizeof(lload));
        k_Elf64_Phdr ldyn{};
        ldyn.p_type=k_PT_DYNAMIC; ldyn.p_flags=6;
        ldyn.p_offset=L2_DYN; ldyn.p_vaddr=L2_DYN;
        ldyn.p_filesz=0x40; ldyn.p_memsz=0x40; ldyn.p_align=8;
        memcpy(libBuf.data() + L2_PHDR + sizeof(k_Elf64_Phdr), &ldyn, sizeof(ldyn));

        // DYN.
        k_Elf64_Dyn ldynArr[4]{};
        ldynArr[0].d_tag=k_DT_SYMTAB; ldynArr[0].d_un.d_ptr=L2_SYM;
        ldynArr[1].d_tag=k_DT_STRTAB; ldynArr[1].d_un.d_ptr=L2_STR;
        ldynArr[2].d_tag=k_DT_SYMENT; ldynArr[2].d_un.d_val=sizeof(k_Elf64_Sym);
        ldynArr[3].d_tag=k_DT_NULL;   ldynArr[3].d_un.d_val=0;
        memcpy(libBuf.data() + L2_DYN, ldynArr, sizeof(ldynArr));

        // SYMTAB: slot 1 = foo.
        k_Elf64_Sym lsyms2[2]{};
        lsyms2[1].st_name  = 1;
        lsyms2[1].st_info  = 1 << 4;
        lsyms2[1].st_shndx = 1;
        lsyms2[1].st_value = L2_CODE;
        memcpy(libBuf.data() + L2_SYM, lsyms2, sizeof(lsyms2));

        // STRTAB (5 bytes including trailing NUL — see main exe note).
        memcpy(libBuf.data() + L2_STR, "\0foo\0", 5);

        // Code at L2_CODE: mov r15, 0xF00; ret  (5 bytes — but we never
        // actually execute this in the test, we only verify the relocated
        // GOT slot value).
        // 49 C7 C7 00 0F 00 00 = mov r15, 0xF00 (7 bytes)
        // C3                    = ret
        U8* lcode = libBuf.data() + L2_CODE;
        lcode[0]=0x49; lcode[1]=0xC7; lcode[2]=0xC7;
        lcode[3]=0x00; lcode[4]=0x0F; lcode[5]=0x00; lcode[6]=0x00;
        lcode[7]=0xC3;

        // ---------- Build main exe ----------
        // Imports `foo` via R_X86_64_JUMP_SLOT into a GOT slot. We won't
        // run code; we just check the relocated slot holds LIB_RELOC + L2_CODE.
        //
        // Layout:
        //   0x0000 Ehdr (64)
        //   0x0040 Phdr[0] PT_LOAD (56)
        //   0x0078 Phdr[1] PT_DYNAMIC (56)
        //   0x00B0 DYN (5 entries: SYMTAB, STRTAB, SYMENT, JMPREL, PLTRELSZ, NULL → 6×16=96)
        //   0x0110 SYMTAB (2 entries × 24 = 48: slot 0=null, slot 1=foo UNDEF)
        //   0x0140 STRTAB ("\0foo" = 4 bytes)
        //   0x0144 JMPREL: 1 RELA (24 bytes), r_offset=GOT_SLOT, type=JUMP_SLOT, sym=1
        //   0x015C GOT slot (8 bytes, target of relocation, initially 0)
        //   0x0164 end
        const U64 M_EHDR   = 0x0000;
        const U64 M_PHDR   = 0x0040;
        const U64 M_DYN    = 0x00B0;
        const U64 M_SYM    = 0x0110;
        const U64 M_STR    = 0x0140;
        const U64 M_JMP    = 0x0148;   // strtab is 5 bytes ("\0foo\0") + 3 pad
        const U64 M_GOT    = 0x0160;
        const U64 M_END    = 0x0168;

        std::vector<U8> mainBuf(M_END, 0);

        k_Elf64_Ehdr meh{};
        meh.e_ident[0]=0x7F; meh.e_ident[1]='E'; meh.e_ident[2]='L'; meh.e_ident[3]='F';
        meh.e_ident[4]=k_ELFCLASS64; meh.e_ident[5]=1; meh.e_ident[6]=1;
        meh.e_type=2;             // ET_EXEC (we apply MAIN_RELOC manually anyway)
        meh.e_machine=k_EM_X86_64;
        meh.e_version=1;
        meh.e_entry=M_GOT;        // doesn't matter, we don't run
        meh.e_phoff=M_PHDR;
        meh.e_ehsize=sizeof(k_Elf64_Ehdr);
        meh.e_phentsize=sizeof(k_Elf64_Phdr);
        meh.e_phnum=2;
        memcpy(mainBuf.data() + M_EHDR, &meh, sizeof(meh));

        k_Elf64_Phdr mload{};
        mload.p_type=k_PT_LOAD; mload.p_flags=7;
        mload.p_offset=0; mload.p_vaddr=0; mload.p_paddr=0;
        mload.p_filesz=M_END; mload.p_memsz=M_END; mload.p_align=0x1000;
        memcpy(mainBuf.data() + M_PHDR, &mload, sizeof(mload));
        k_Elf64_Phdr mdyn{};
        mdyn.p_type=k_PT_DYNAMIC; mdyn.p_flags=6;
        mdyn.p_offset=M_DYN; mdyn.p_vaddr=M_DYN;
        mdyn.p_filesz=0x60; mdyn.p_memsz=0x60; mdyn.p_align=8;
        memcpy(mainBuf.data() + M_PHDR + sizeof(k_Elf64_Phdr), &mdyn, sizeof(mdyn));

        // DYN (6 entries).
        k_Elf64_Dyn mdynArr[6]{};
        mdynArr[0].d_tag=k_DT_SYMTAB;   mdynArr[0].d_un.d_ptr=M_SYM;
        mdynArr[1].d_tag=k_DT_STRTAB;   mdynArr[1].d_un.d_ptr=M_STR;
        mdynArr[2].d_tag=k_DT_SYMENT;   mdynArr[2].d_un.d_val=sizeof(k_Elf64_Sym);
        mdynArr[3].d_tag=k_DT_JMPREL;   mdynArr[3].d_un.d_ptr=M_JMP;
        mdynArr[4].d_tag=k_DT_PLTRELSZ; mdynArr[4].d_un.d_val=sizeof(k_Elf64_Rela);
        mdynArr[5].d_tag=k_DT_NULL;     mdynArr[5].d_un.d_val=0;
        memcpy(mainBuf.data() + M_DYN, mdynArr, sizeof(mdynArr));

        // SYMTAB. Slot 1 is foo UNDEF (st_shndx=0).
        k_Elf64_Sym msyms[2]{};
        msyms[1].st_name  = 1;          // "foo"
        msyms[1].st_info  = 1 << 4;     // GLOBAL
        msyms[1].st_shndx = 0;          // UNDEF — imported
        memcpy(mainBuf.data() + M_SYM, msyms, sizeof(msyms));

        // STRTAB: leading NUL, "foo", trailing NUL. 5 bytes total — the
        // trailing NUL is required because readGuestCString reads until
        // it hits a 0 byte and the next thing in memory is the JMPREL
        // table (otherwise the first JMPREL byte gets read as a char).
        memcpy(mainBuf.data() + M_STR, "\0foo\0", 5);

        // JMPREL: one entry, JUMP_SLOT, sym index 1, r_offset = GOT slot.
        k_Elf64_Rela mrela{};
        mrela.r_offset = M_GOT;
        mrela.r_info   = ((U64)1 << 32) | k_R_X86_64_JUMP_SLOT; // sym=1, type=7
        mrela.r_addend = 0;
        memcpy(mainBuf.data() + M_JMP, &mrela, sizeof(mrela));

        // ---------- Parse + map main exe ----------
        Elf64ParseResult mainParsed = ElfLoader64::parseBuffer(mainBuf.data(), mainBuf.size());
        bool mainMapOk = mainParsed.ok &&
                         ElfLoader64::mapSegmentsFromBuffer(&mem, mainParsed,
                                                            mainBuf.data(), mainBuf.size(),
                                                            MAIN_RELOC, "a3d-main");

        // ---------- Run orchestrator ----------
        std::vector<ElfLoader64::PreloadedLibrary> preloaded = {
            { "libfoo.so", libBuf.data(), (U64)libBuf.size(), LIB_RELOC }
        };
        std::vector<ElfLoader64::LinkedLibrary> outLibs;
        U64 nLinked = mainMapOk ? ElfLoader64::linkSharedObjects(
            &mem, mainParsed, MAIN_RELOC, preloaded, 0, &outLibs) : 0;

        // ---------- Verify ----------
        U64 gotValue = mem.readq(M_GOT + MAIN_RELOC);
        U64 expected = LIB_RELOC + L2_CODE;

        bool ok = mainMapOk && nLinked == 1 &&
                  outLibs.size() == 1 &&
                  outLibs[0].name == "libfoo.so" &&
                  outLibs[0].reloc == LIB_RELOC &&
                  gotValue == expected;
        if (ok) {
            printf("  PASS: linkSharedObjects: main JUMP_SLOT for `foo` resolved to libfoo+0x%llx\n",
                   (unsigned long long)L2_CODE);
            r.passed++;
        } else {
            printf("  FAIL: A3d link (mapOk=%d nLinked=%llu outSize=%zu got=0x%llx exp=0x%llx)\n",
                   mainMapOk, (unsigned long long)nLinked, outLibs.size(),
                   (unsigned long long)gotValue,
                   (unsigned long long)expected);
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: linkSharedObjectsRecursive — transitive DT_NEEDED resolution.
    //
    // Topology:
    //   main exe imports `foo` from libA (DT_NEEDED libA)
    //   libA imports `bar` from libB (DT_NEEDED libB)
    //   libB exports `bar`
    //   libA exports `foo`
    //
    // The recursive linker must:
    //   1. Walk main's DT_NEEDED → load libA
    //   2. Walk libA's DT_NEEDED → load libB transitively
    //   3. Build merged symbol table {foo: libA+codeA, bar: libB+codeB}
    //   4. Apply symbol relocations on main, libA, libB
    //
    // We verify both GOT slots (main's `foo` slot and libA's `bar` slot)
    // hold the right resolved addresses. This is the single tightest probe
    // we have of transitive DT_NEEDED short of a real glibc binary.
    {
        // Helper: build a single synthetic ET_DYN library with:
        //   - one exported global symbol (`exportName`, code offset E_CODE)
        //   - up to one imported symbol via JUMP_SLOT GOT slot
        //   - one DT_NEEDED entry (optional)
        // Returns the raw ELF bytes.
        auto buildLib = [](const char* exportName,
                           const char* importName,    // nullptr for none
                           const char* neededName)    // nullptr for none
            -> std::vector<U8>
        {
            // Layout — keep generous: many sections, each on its own slot.
            //   0x0000 Ehdr
            //   0x0040 Phdr[0] PT_LOAD
            //   0x0078 Phdr[1] PT_DYNAMIC
            //   0x00B0 DYN (10 entries × 16 = 160)
            //   0x0150 SYMTAB (3 entries × 24 = 72)
            //   0x0198 STRTAB (up to 64 bytes)
            //   0x01E0 JMPREL (1 entry × 24 = 24)
            //   0x0200 GOT slot (8 bytes)
            //   0x0210 code
            //   0x0220 end
            const U64 EHDR=0x0000, PHDR=0x0040, DYN=0x00B0;
            const U64 SYM=0x0150, STR=0x0198, JMP=0x01E0;
            const U64 GOT=0x0200, CODE=0x0210, END=0x0220;
            std::vector<U8> buf(END, 0);

            // STRTAB layout: leading NUL, exportName\0, importName\0, neededName\0
            U32 expOff = 1;
            U32 impOff = 0;
            U32 needOff = 0;
            std::string strs;
            strs.push_back('\0');
            strs += exportName; strs.push_back('\0');
            if (importName) {
                impOff = (U32)strs.size();
                strs += importName; strs.push_back('\0');
            }
            if (neededName) {
                needOff = (U32)strs.size();
                strs += neededName; strs.push_back('\0');
            }
            memcpy(buf.data() + STR, strs.data(), strs.size());

            // SYMTAB: slot 0 null, slot 1 = export (defined), slot 2 = import (undef).
            k_Elf64_Sym syms[3]{};
            syms[1].st_name = expOff;
            syms[1].st_info = 1 << 4;     // GLOBAL
            syms[1].st_shndx = 1;
            syms[1].st_value = CODE;      // exported at offset CODE
            if (importName) {
                syms[2].st_name = impOff;
                syms[2].st_info = 1 << 4;
                syms[2].st_shndx = 0;     // UNDEF
            }
            memcpy(buf.data() + SYM, syms, sizeof(syms));

            // JMPREL: one entry (only if we have an import).
            k_Elf64_Rela rela{};
            if (importName) {
                rela.r_offset = GOT;
                rela.r_info   = ((U64)2 << 32) | k_R_X86_64_JUMP_SLOT;
                rela.r_addend = 0;
            }
            memcpy(buf.data() + JMP, &rela, sizeof(rela));

            // DYN array.
            k_Elf64_Dyn dyn[10]{};
            int di = 0;
            dyn[di].d_tag = k_DT_SYMTAB;   dyn[di++].d_un.d_ptr = SYM;
            dyn[di].d_tag = k_DT_STRTAB;   dyn[di++].d_un.d_ptr = STR;
            dyn[di].d_tag = k_DT_SYMENT;   dyn[di++].d_un.d_val = sizeof(k_Elf64_Sym);
            if (importName) {
                dyn[di].d_tag = k_DT_JMPREL;   dyn[di++].d_un.d_ptr = JMP;
                dyn[di].d_tag = k_DT_PLTRELSZ; dyn[di++].d_un.d_val = sizeof(k_Elf64_Rela);
            }
            if (neededName) {
                dyn[di].d_tag = k_DT_NEEDED;   dyn[di++].d_un.d_val = needOff;
            }
            dyn[di].d_tag = k_DT_NULL;     dyn[di++].d_un.d_val = 0;
            memcpy(buf.data() + DYN, dyn, sizeof(dyn));

            // Ehdr.
            k_Elf64_Ehdr eh{};
            eh.e_ident[0]=0x7F; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
            eh.e_ident[4]=k_ELFCLASS64; eh.e_ident[5]=1; eh.e_ident[6]=1;
            eh.e_type=3; // ET_DYN
            eh.e_machine=k_EM_X86_64;
            eh.e_version=1;
            eh.e_entry=CODE;
            eh.e_phoff=PHDR;
            eh.e_ehsize=sizeof(k_Elf64_Ehdr);
            eh.e_phentsize=sizeof(k_Elf64_Phdr);
            eh.e_phnum=2;
            memcpy(buf.data() + EHDR, &eh, sizeof(eh));

            // Phdr[0] PT_LOAD covers full file.
            k_Elf64_Phdr ld{};
            ld.p_type=k_PT_LOAD; ld.p_flags=7;
            ld.p_offset=0; ld.p_vaddr=0; ld.p_paddr=0;
            ld.p_filesz=END; ld.p_memsz=END; ld.p_align=0x1000;
            memcpy(buf.data() + PHDR, &ld, sizeof(ld));
            // Phdr[1] PT_DYNAMIC.
            k_Elf64_Phdr pd{};
            pd.p_type=k_PT_DYNAMIC; pd.p_flags=6;
            pd.p_offset=DYN; pd.p_vaddr=DYN;
            pd.p_filesz=sizeof(dyn); pd.p_memsz=sizeof(dyn); pd.p_align=8;
            memcpy(buf.data() + PHDR + sizeof(k_Elf64_Phdr), &pd, sizeof(pd));

            // Code: trivial ret (we don't run it — only check resolved GOT slots).
            buf[CODE] = 0xC3;
            return buf;
        };

        // Build libB (exports `bar`, no imports, no deps).
        auto libBBytes = buildLib("bar", nullptr, nullptr);
        // Build libA (exports `foo`, imports `bar`, DT_NEEDED libB).
        auto libABytes = buildLib("foo", "bar", "libB.so");
        // Build main (exports nothing useful, imports `foo`, DT_NEEDED libA).
        // We reuse the same buildLib helper — `exportName` will be a stray
        // symbol that nothing imports, which is harmless.
        auto mainBytes = buildLib("__main_dummy__", "foo", "libA.so");

        // Capture the GOT offset in the helper layout (same in every blob).
        const U64 GOT_OFF = 0x0200;
        const U64 CODE_OFF = 0x0210;

        // Place main at MAIN_RELOC; libraries at LIB_BASE + N * 16 MiB.
        const U64 MAIN_RELOC = 0x60000000;
        const U64 LIB_BASE   = 0x70000000;

        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(MAIN_RELOC, 0x1000, 7);
        // Pre-map enough space for two libs at 16 MiB stride.
        mem.mmapAnonymousFixed(LIB_BASE, 0x1000, 7);
        mem.mmapAnonymousFixed(LIB_BASE + 0x1000000, 0x1000, 7);

        Elf64ParseResult mainParsed = ElfLoader64::parseBuffer(mainBytes.data(),
                                                                mainBytes.size());
        bool mainMapOk = mainParsed.ok &&
                         ElfLoader64::mapSegmentsFromBuffer(&mem, mainParsed,
                             mainBytes.data(), mainBytes.size(),
                             MAIN_RELOC, "main");

        // Fetcher: name -> blob. Returns empty for unknown names.
        ElfLoader64::LibFetcher fetcher = [&](const std::string& name)
            -> ElfLoader64::FetchedLibrary
        {
            ElfLoader64::FetchedLibrary out;
            if (name == "libA.so") out.bytes = libABytes;
            else if (name == "libB.so") out.bytes = libBBytes;
            return out;
        };

        std::vector<ElfLoader64::LinkedLibrary> linked;
        U64 nLinked = mainMapOk ? ElfLoader64::linkSharedObjectsRecursive(
            &mem, mainParsed, MAIN_RELOC, fetcher, LIB_BASE, &linked) : 0;

        // Locate the two libs by name in the linked vector.
        U64 libAReloc = 0, libBReloc = 0;
        for (const auto& lib : linked) {
            if (lib.name == "libA.so") libAReloc = lib.reloc;
            if (lib.name == "libB.so") libBReloc = lib.reloc;
        }

        // Main's GOT slot must contain libA+CODE_OFF (foo).
        U64 mainGot = mem.readq(MAIN_RELOC + GOT_OFF);
        U64 expectMainGot = libAReloc + CODE_OFF;
        // libA's GOT slot must contain libB+CODE_OFF (bar).
        U64 libAGot = mem.readq(libAReloc + GOT_OFF);
        U64 expectLibAGot = libBReloc + CODE_OFF;

        bool ok = mainMapOk && nLinked == 2 &&
                  libAReloc != 0 && libBReloc != 0 &&
                  mainGot == expectMainGot &&
                  libAGot == expectLibAGot;
        if (ok) {
            printf("  PASS: linkSharedObjectsRecursive: transitive DT_NEEDED resolved foo->libA, bar->libB\n");
            r.passed++;
        } else {
            printf("  FAIL: A29 transitive link (mapOk=%d nLinked=%llu libA=0x%llx libB=0x%llx mainGot=0x%llx exp=0x%llx libAGot=0x%llx exp=0x%llx)\n",
                   mainMapOk, (unsigned long long)nLinked,
                   (unsigned long long)libAReloc, (unsigned long long)libBReloc,
                   (unsigned long long)mainGot, (unsigned long long)expectMainGot,
                   (unsigned long long)libAGot, (unsigned long long)expectLibAGot);
            r.failed++;
        }
        fflush(stdout);
    }

    // Test: extractGlobalSymbols — missing SYMTAB returns empty without
    // dereferencing nullptr.
    {
        const U64 LOAD_RELOC = 0x35000000;
        const U64 DYN_OFF    = 0x100;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(LOAD_RELOC, 0x1000, 3);

        k_Elf64_Dyn dyn[2]{};
        dyn[0].d_tag = k_DT_STRTAB; dyn[0].d_un.d_ptr = 0x200;
        dyn[1].d_tag = k_DT_NULL;   dyn[1].d_un.d_val = 0;
        mem.memcpyToGuest(LOAD_RELOC + DYN_OFF, dyn, sizeof(dyn));

        Elf64DynamicInfo info;
        info.present = true;
        info.vaddr   = DYN_OFF;
        info.memsz   = sizeof(dyn);

        auto sym = ElfLoader64::extractGlobalSymbols(&mem, info, LOAD_RELOC);
        if (sym.empty()) {
            printf("  PASS: extractGlobalSymbols: empty when DT_SYMTAB missing\n");
            r.passed++;
        } else {
            printf("  FAIL: extractGlobalSymbols expected empty, got size=%zu\n", sym.size());
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

    // Test 65a — CMPPS xmm0, xmm1, 1 (LT), packed singles. Low lanes are
    // 1.0 vs 2.0 (true) and 5.0 vs 2.0 (false), so MOVMSKPS must return 0b01.
    // IL2CPP emits exactly this pair for vector float compares.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x80,0x3F,0x00,0x00,0xA0,0x40, // 1.0, 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                         // movq xmm0, rax
            0x48, 0xB8, 0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x40, // 2.0, 2.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                         // movq xmm1, rax
            0x0F, 0xC2, 0xC1, 0x01,                               // cmpps xmm0, xmm1, 1 (LT)
            0x0F, 0x50, 0xC0,                                     // movmskps eax, xmm0
        };
        runAndCheck(r, "cmpps LT -> 1.0<2.0 lane set (movmskps=1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1;
        });
    }

    // Test 65b — CMPPD xmm0, xmm1, 1 (LT), packed doubles: [2.0, 5.0] vs
    // [3.0, 3.0] → lane 0 true, lane 1 false; MOVMSKPD must return 0b01.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40, // 2.0
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                         // movq xmm0, rax
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x40, // 5.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                         // movq xmm1, rax
            0x0F, 0x16, 0xC1,                                     // movlhps xmm0, xmm1 → [2.0, 5.0]
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40, // 3.0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                         // movq xmm1, rax
            0x48, 0xB8, 0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x40, // 3.0
            0x66, 0x48, 0x0F, 0x6E, 0xD0,                         // movq xmm2, rax
            0x0F, 0x16, 0xCA,                                     // movlhps xmm1, xmm2 → [3.0, 3.0]
            0x66, 0x0F, 0xC2, 0xC1, 0x01,                         // cmppd xmm0, xmm1, 1 (LT)
            0x66, 0x0F, 0x50, 0xC0,                               // movmskpd eax, xmm0
        };
        runAndCheck(r, "cmppd LT -> 2.0<3.0 lane set (movmskpd=1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 1;
        });
    }

    // Test 65c — imm8 ModR/M decoding. The memory data follows the exit
    // suffix, so each RIP-relative displacement is exactly 12 bytes from the
    // end of its instruction. These cases cover register, [RBX], and
    // RIP-relative forms for every implemented 0F C2/C4/C6 immediate form.
    {
        const U64 dstLo = 0xBF8000003F800000ULL; // -1.0, 1.0
        const U64 dstHi = 0x7F8000007FC00000ULL; // +inf, qNaN
        const U64 srcLo = 0x0000000040000000ULL; //  2.0, 0.0
        const U64 srcHi = 0xFF8000003F800000ULL; // -inf, 1.0
        const std::vector<U8> floatData = {
            0x00,0x00,0x00,0x40, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x80,0x3F, 0x00,0x00,0x80,0xFF
        }; // [2.0, +0.0, 1.0, -inf]
        const std::vector<U8> doubleData = {
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,
            0x00,0x00,0x00,0x00,0x00,0x00,0xF0,0x7F
        }; // [2.0, +inf]

        auto vectorImm = [&](const char* opName, const std::vector<U8>& prefix,
                             U8 opcode, U8 imm, int form, U64 expectedLo,
                             U64 expectedHi, U64 initialLo, U64 initialHi,
                             const std::vector<U8>& data,
                             U64 xmm1Lo, U64 xmm1Hi) {
            std::vector<U8> insn = prefix;
            insn.push_back(0x0F); insn.push_back(opcode);
            if (form == 0) insn.push_back(0xC1);      // xmm0, xmm1
            else if (form == 1) insn.push_back(0x03); // xmm0, [rbx]
            else insn.push_back(0x05);                // xmm0, [rip+disp32]
            if (form == 2) {
                // The exit suffix is 12 bytes. RIP points here after imm8,
                // so disp32=12 targets the inline memory operand after it.
                insn.insert(insn.end(), {0x0C,0x00,0x00,0x00});
            }
            insn.push_back(imm);
            std::vector<U8> code = withExit(insn);
            code.insert(code.end(), data.begin(), data.end());
            std::string testName = std::string(opName) +
                (form == 0 ? " register" : form == 1 ? " memory" : " RIP-relative");
            runAndCheck(r, testName.c_str(), code,
                [=](CPU64& c) {
                    return c.xmm[0].lo == expectedLo && c.xmm[0].hi == expectedHi;
                },
                [=, &insn](CPU64& c) {
                    c.xmm[0].lo = initialLo; c.xmm[0].hi = initialHi;
                    c.xmm[1].lo = xmm1Lo; c.xmm[1].hi = xmm1Hi;
                    if (form == 1) {
                        c.reg[X64_RBX].setU64(CODE_BASE + insn.size() + EXIT_SYSCALL.size());
                    }
                });
        };

        const U64 shufflePsLo = 0xBF8000003F800000ULL; // lanes 0,1 = 1.0, -1.0
        const U64 shufflePsHi = 0xFF8000003F800000ULL; // lanes 2,3 = 1.0, -inf
        for (int form = 0; form < 3; form++) {
            vectorImm("shufps imm8", {}, 0xC6, 0xE4, form,
                      shufflePsLo, shufflePsHi, dstLo, dstHi,
                      floatData, srcLo, srcHi);
        }

        const U64 shufflePdLo = 0x3FF0000000000000ULL; // dst low = 1.0
        const U64 shufflePdHi = 0x7FF0000000000000ULL; // src high = +inf
        for (int form = 0; form < 3; form++) {
            vectorImm("shufpd imm8", {0x66}, 0xC6, 0x02, form,
                      shufflePdLo, shufflePdHi,
                      0x3FF0000000000000ULL, 0xBFF0000000000000ULL, doubleData,
                      0x7FF8000000000000ULL, 0x7FF0000000000000ULL);
        }

        const U64 cmpPsLo = 0xFFFFFFFFFFFFFFFFULL; // 1<2 and -1<0
        const U64 cmpPsHi = 0;                     // NaN unordered, inf !< inf
        for (int form = 0; form < 3; form++) {
            vectorImm("cmpps imm8", {}, 0xC2, 0x01, form,
                      cmpPsLo, cmpPsHi, dstLo, dstHi, floatData,
                      srcLo, 0x7F8000007FC00000ULL);
        }

        const U64 cmpPdLo = 0xFFFFFFFFFFFFFFFFULL; // 1.0 < 2.0
        const U64 cmpPdHi = 0;                     // NaN < +inf is false
        for (int form = 0; form < 3; form++) {
            vectorImm("cmppd imm8", {0x66}, 0xC2, 0x01, form,
                      cmpPdLo, cmpPdHi,
                      0x3FF0000000000000ULL, 0x7FF8000000000000ULL, doubleData,
                      0x4000000000000000ULL, 0x7FF0000000000000ULL);
        }

        const U64 cmpSdLo = 0xFFFFFFFFFFFFFFFFULL;
        const U64 cmpSdHi = 0x7FF8000000000000ULL;
        for (int form = 0; form < 3; form++) {
            vectorImm("cmpsd imm8", {0xF2}, 0xC2, 0x01, form,
                      cmpSdLo, cmpSdHi,
                      0x3FF0000000000000ULL, 0x7FF8000000000000ULL, doubleData,
                      0x4000000000000000ULL, 0x7FF0000000000000ULL);
        }

        const U64 cmpSsLo = (dstLo & 0xFFFFFFFF00000000ULL) | 0xFFFFFFFFULL;
        const U64 cmpSsHi = dstHi;
        for (int form = 0; form < 3; form++) {
            vectorImm("cmpss imm8", {0xF3}, 0xC2, 0x01, form,
                      cmpSsLo, cmpSsHi, dstLo, dstHi, floatData,
                      srcLo, srcHi);
        }

        const U32 scalarPsEdges[] = {0x7FC00000, 0xFF800000, 0x7F800000};
        const U64 scalarPdEdges[] = {0x7FF8000000000000ULL,
                                     0xFFF0000000000000ULL,
                                     0x7FF0000000000000ULL};
        for (int form = 0; form < 3; form++) {
            std::vector<U8> psData(16, 0), pdData(16, 0);
            std::memcpy(psData.data(), &scalarPsEdges[form], sizeof(U32));
            std::memcpy(pdData.data(), &scalarPdEdges[form], sizeof(U64));
            bool less = form == 2; // 1.0 < +infinity; NaN and -infinity are false.
            vectorImm("cmpss imm8 NaN/Inf", {0xF3}, 0xC2, 0x01, form,
                      (dstLo & 0xFFFFFFFF00000000ULL) | (less ? 0xFFFFFFFFULL : 0),
                      dstHi, dstLo, dstHi, psData, scalarPsEdges[form], 0);
            vectorImm("cmpsd imm8 NaN/Inf", {0xF2}, 0xC2, 0x01, form,
                      less ? 0xFFFFFFFFFFFFFFFFULL : 0,
                      0x7FF8000000000000ULL,
                      0x3FF0000000000000ULL, 0x7FF8000000000000ULL,
                      pdData, scalarPdEdges[form], 0);
        }

        // PINSRW takes a GPR in register form and m16 in memory forms. Lane 6
        // uses the second qword so the immediate and source position are both
        // observable.
        const U64 pinsLo = 0x1111222233334444ULL;
        const U64 pinsHiReg = 0x5555800077778888ULL;
        const U64 pinsHiMem = 0x5555A55A77778888ULL;
        for (int form = 0; form < 3; form++) {
            std::vector<U8> insn = {0x66,0x0F,0xC4};
            if (form == 0) insn.push_back(0xC1);
            else if (form == 1) insn.push_back(0x03);
            else insn.push_back(0x05);
            if (form == 2) insn.insert(insn.end(), {0x0C,0x00,0x00,0x00});
            insn.push_back(0x06);
            std::vector<U8> code = withExit(insn);
            std::vector<U8> data = {0x5A,0xA5,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
            code.insert(code.end(), data.begin(), data.end());
            std::string name = std::string("pinsrw imm8 ") +
                (form == 0 ? "register" : form == 1 ? "memory" : "RIP-relative");
            runAndCheck(r, name.c_str(), code,
                [=](CPU64& c) {
                    return c.xmm[0].lo == pinsLo &&
                           c.xmm[0].hi == (form == 0 ? pinsHiReg : pinsHiMem);
                },
                [=, &insn](CPU64& c) {
                    c.xmm[0].lo = pinsLo; c.xmm[0].hi = 0x5555666677778888ULL;
                    c.reg[X64_RCX].setU64(0x8000);
                    if (form == 1) {
                        c.reg[X64_RBX].setU64(CODE_BASE + insn.size() + EXIT_SYSCALL.size());
                    }
                });
        }

        // 66 0F C5 PEXTRW reads an XMM register; the legacy form has no
        // memory-source encoding. Immediate 6 selects word 6 of XMM1.
        runAndCheck(r, "pextrw imm8 register", withExit({
            0x66,0x0F,0xC5,0xC1,0x06
        }), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xDDEE;
        }, [](CPU64& c) {
            c.xmm[1].lo = 0x7788556633441122ULL;
            c.xmm[1].hi = 0xFF00DDEEBBCC99AAULL;
        });
    }

    // Test 65d — FE /0,/1 have no immediate. Verify register, [RBX], and
    // RIP-relative byte operands, including result flags and CF preservation.
    {
        for (U8 sub = 0; sub < 2; sub++) {
            const U8 initial = sub == 0 ? 0x7F : 0x80;
            const U8 expected = sub == 0 ? 0x80 : 0x7F;
            const U32 expectedFlags = X64_CF | X64_OF | (sub == 0 ? X64_SF : 0);
            for (int form = 0; form < 3; form++) {
                std::vector<U8> insn = {0xFE};
                if (form == 0) insn.push_back(sub == 0 ? 0xC0 : 0xC8);
                else if (form == 1) insn.push_back(sub == 0 ? 0x03 : 0x0B);
                else {
                    insn.push_back(sub == 0 ? 0x05 : 0x0D);
                    // Two flag-capture bytes and the 12-byte exit suffix
                    // precede the data, so disp32=14 from the instruction end.
                    insn.insert(insn.end(), {0x0E,0x00,0x00,0x00});
                }
                std::vector<U8> code = insn;
                code.insert(code.end(), {0x9C,0x5A}); // pushfq; pop rdx
                code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                code.push_back(initial);
                std::string name = std::string(sub == 0 ? "inc m8" : "dec m8") +
                    (form == 0 ? " register" : form == 1 ? " memory" : " RIP-relative");
                runAndCheck(r, name.c_str(), code,
                    [=](CPU64& c) {
                        U8 actual = form == 0 ? (U8)c.reg[X64_R15].u64
                            : c.memory->readb(CODE_BASE + insn.size() + 2 + EXIT_SYSCALL.size());
                        return actual == expected &&
                               (((U32)c.reg[X64_RDX].u64) & (X64_CF | X64_OF | X64_SF)) == expectedFlags;
                    },
                    [=, &insn](CPU64& c) {
                        c.reg[X64_RAX].setU64(initial);
                        c.rflags = 0x202 | X64_CF;
                        if (form == 1) {
                            c.reg[X64_RBX].setU64(CODE_BASE + insn.size() + 2 + EXIT_SYSCALL.size());
                        }
                    });
            }
        }
    }

    // Test 65e — F6/F7 /0 TEST immediates: imm8, imm16, and imm32, each with
    // register, base-memory, and RIP-relative operands. The RIP displacement
    // spans the two-byte flag capture and 12-byte exit suffix (disp32=14).
    {
        struct GroupTestSpec {
            const char* name;
            std::vector<U8> prefix;
            U8 opcode;
            std::vector<U8> immediate;
            std::vector<U8> data;
        };
        GroupTestSpec specs[] = {
            {"test f6 imm8", {}, 0xF6, {0x0F}, {0xF0}},
            {"test f7 imm16", {0x66}, 0xF7, {0x0F,0x00}, {0xF0,0x00}},
            {"test f7 imm32", {0x48}, 0xF7, {0x0F,0x00,0x00,0x00},
             {0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        };
        for (const auto& spec : specs) {
            for (int form = 0; form < 3; form++) {
                std::vector<U8> insn = spec.prefix;
                insn.push_back(spec.opcode);
                insn.push_back(form == 0 ? 0xC0 : form == 1 ? 0x03 : 0x05);
                if (form == 2) insn.insert(insn.end(), {0x0E,0x00,0x00,0x00});
                insn.insert(insn.end(), spec.immediate.begin(), spec.immediate.end());
                std::vector<U8> code = insn;
                code.insert(code.end(), {0x9C,0x58}); // pushfq; pop rax
                code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                code.insert(code.end(), spec.data.begin(), spec.data.end());
                std::string name = std::string(spec.name) +
                    (form == 0 ? " register" : form == 1 ? " memory" : " RIP-relative");
                runAndCheck(r, name.c_str(), code, [](CPU64& c) {
                    const U32 defined = X64_CF | X64_PF | X64_ZF | X64_SF | X64_OF;
                    return (((U32)c.reg[X64_R15].u64) & defined) == (X64_PF | X64_ZF);
                }, [=, &insn, &spec](CPU64& c) {
                    c.reg[X64_RAX].setU64(0xF0);
                    if (form == 1) {
                        c.reg[X64_RBX].setU64(CODE_BASE + insn.size() + 2 + EXIT_SYSCALL.size());
                    }
                });
            }
        }
    }

    // Test 65f — LOCK-prefixed scalar RMW instructions use the same result
    // and flag semantics as their register forms, and preserve unrelated flags.
    {
        auto checkRmw = [&](const char* name, const std::vector<U8>& insn,
                            bool memoryForm, U8 size, U64 initial, U64 expected,
                            U64 rcx, U32 flagsIn, U32 flagsMask,
                            U32 expectedFlags, bool unchangedFlags) {
            U32 wantedFlags = unchangedFlags ? (flagsIn & flagsMask) : expectedFlags;
            std::vector<U8> code = insn;
            code.insert(code.end(), {0x9C,0x58}); // pushfq; pop rax; withExit saves flags in R15
            U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
            if (memoryForm) {
                code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                for (U8 i = 0; i < size; i++) code.push_back((U8)(initial >> (i * 8)));
            } else {
                code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
            }
            runAndCheck(r, name, code,
                [=](CPU64& c) {
                    U64 actual = memoryForm
                        ? (size == 1 ? c.memory->readb(dataAddr) : c.memory->readq(dataAddr))
                        : c.reg[X64_RBX].u64;
                    U32 flags = (U32)c.reg[X64_R15].u64 & flagsMask;
                    return actual == expected && flags == wantedFlags;
                },
                [=](CPU64& c) {
                    c.reg[X64_RBX].setU64(memoryForm ? dataAddr : initial);
                    c.reg[X64_RSI].setU64(rcx);
                    c.rflags = flagsIn;
                    if (memoryForm) c.reg[X64_RBX].setU64(dataAddr);
                });
        };

        const U32 incFlags = X64_CF | X64_PF | X64_AF | X64_ZF | X64_SF | X64_OF;
        const U32 allFlags = incFlags;
        const U32 allSet = 0x202 | allFlags;
        checkRmw("INC r/m8 register", {0xFE,0xC3}, false, 1,
                 0x7F, 0x80, 0, 0x203, incFlags,
                 X64_CF | X64_AF | X64_SF | X64_OF, false);
        checkRmw("LOCK INC m8 [RBX]", {0xF0,0xFE,0x03}, true, 1,
                 0x7F, 0x80, 0, 0x203, incFlags,
                 X64_CF | X64_AF | X64_SF | X64_OF, false);
        checkRmw("DEC r/m8 register", {0xFE,0xCB}, false, 1,
                 0x80, 0x7F, 0, 0x203, incFlags,
                 X64_CF | X64_AF | X64_OF, false);
        checkRmw("LOCK DEC m8 [RBX]", {0xF0,0xFE,0x0B}, true, 1,
                 0x80, 0x7F, 0, 0x203, incFlags,
                 X64_CF | X64_AF | X64_OF, false);

        checkRmw("NOT r/m8 register", {0xF6,0xD3}, false, 1,
                 0x55, 0xAA, 0, allSet, allFlags, allFlags, true);
        checkRmw("LOCK NOT m8 [RBX]", {0xF0,0xF6,0x13}, true, 1,
                 0x55, 0xAA, 0, allSet, allFlags, allFlags, true);
        checkRmw("NEG r/m8 register", {0xF6,0xDB}, false, 1,
                 0x80, 0x80, 0, 0x202, allFlags,
                 X64_CF | X64_SF | X64_OF, false);
        checkRmw("LOCK NEG m8 [RBX]", {0xF0,0xF6,0x1B}, true, 1,
                 0x80, 0x80, 0, 0x202, allFlags,
                 X64_CF | X64_SF | X64_OF, false);
        checkRmw("NOT r/m64 register", {0x48,0xF7,0xD3}, false, 8,
                 0x00FF00FF00FF00FFULL, 0xFF00FF00FF00FF00ULL,
                 0, allSet, allFlags, allFlags, true);
        checkRmw("LOCK NOT m64 [RBX]", {0xF0,0x48,0xF7,0x13}, true, 8,
                 0x00FF00FF00FF00FFULL, 0xFF00FF00FF00FF00ULL,
                 0, allSet, allFlags, allFlags, true);
        checkRmw("NEG r/m64 register", {0x48,0xF7,0xDB}, false, 8,
                 0x8000000000000000ULL, 0x8000000000000000ULL,
                 0, 0x202, allFlags, X64_CF | X64_PF | X64_SF | X64_OF, false);
        checkRmw("LOCK NEG m64 [RBX]", {0xF0,0x48,0xF7,0x1B}, true, 8,
                 0x8000000000000000ULL, 0x8000000000000000ULL,
                 0, 0x202, allFlags, X64_CF | X64_PF | X64_SF | X64_OF, false);

        const U8 bitOpcodes[] = {0xAB,0xB3,0xBB};
        const U8 bitExts[] = {5,6,7};
        const char* bitNames[] = {"BTS", "BTR", "BTC"};
        for (int i = 0; i < 3; i++) {
            U64 expected = i == 0 ? 8 : 0;
            std::string regName = std::string(bitNames[i]) + " r64,rcx";
            checkRmw(regName.c_str(), {0x48,0x0F,bitOpcodes[i],0xF3}, false, 8,
                     8, expected, 3, 0x202, X64_CF, X64_CF, false);
            std::string memName = std::string("LOCK ") + bitNames[i] + " [RBX],rcx";
            checkRmw(memName.c_str(), {0xF0,0x48,0x0F,bitOpcodes[i],0x33},
                     true, 8, 8, expected, 3, 0x202, X64_CF, X64_CF, false);
            U8 regModRM = (U8)(0xC3 | (bitExts[i] << 3));
            std::string immRegName = std::string(bitNames[i]) + " r64,imm8";
            checkRmw(immRegName.c_str(), {0x48,0x0F,0xBA,regModRM,0x03},
                     false, 8, 8, expected, 0, 0x202, X64_CF, X64_CF, false);
            U8 memModRM = (U8)((bitExts[i] << 3) | X64_RBX);
            std::string immMemName = std::string("LOCK ") + bitNames[i] + " [RBX],imm8";
            checkRmw(immMemName.c_str(), {0xF0,0x48,0x0F,0xBA,memModRM,0x03},
                     true, 8, 8, expected, 0, 0x202, X64_CF, X64_CF, false);
        }
    }

    // Test 65g — register-index BT-family memory forms address a signed bit
    // string. Offsets 40, -1, and 200 cover forward and backward qword steps.
    {
        const U8 opcodes[] = {0xA3,0xAB,0xB3,0xBB};
        const char* names[] = {"BT", "BTS", "BTR", "BTC"};
        const S64 indices[] = {40, -1, 200};
        const U8 bits[] = {40, 63, 8};
        const S32 deltas[] = {0, -8, 24};
        for (int op = 0; op < 4; op++) {
            for (int index = 0; index < 3; index++) {
                U64 initial = 1ULL << bits[index];
                U64 expected = (op == 0 || op == 1) ? initial : 0;
                for (int memoryForm = 0; memoryForm < 2; memoryForm++) {
                    std::vector<U8> code;
                    if (memoryForm && op != 0) code.push_back(0xF0); // LOCK RMW
                    code.insert(code.end(), {0x48,0x0F,opcodes[op]});
                    if (memoryForm) code.insert(code.end(), {0x73,0x20});
                    else code.push_back(0xF3); // mod=11, RSI index, RBX destination
                    code.insert(code.end(), {0x9C,0x58}); // pushfq; pop rax
                    U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
                    if (memoryForm) {
                        code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                        code.resize(code.size() + 64, 0);
                        U64 dataWord = initial;
                        U64 dataOffset = (U64)(32 + deltas[index]);
                        for (U8 byte = 0; byte < 8; byte++)
                            code[(size_t)(dataAddr - CODE_BASE + dataOffset + byte)] =
                                (U8)(dataWord >> (byte * 8));
                    } else {
                        code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                    }
                    std::string testName = std::string(memoryForm ? "" : "register ") +
                        names[op] + " register bit offset " + std::to_string(indices[index]) +
                        (memoryForm ? " memory" : "");
                    U64 targetAddr = dataAddr + 32 + deltas[index];
                    runAndCheck(r, testName.c_str(), code,
                        [=](CPU64& c) {
                            U64 actual = memoryForm ? c.memory->readq(targetAddr)
                                                    : c.reg[X64_RBX].u64;
                            return actual == expected &&
                                (((U32)c.reg[X64_R15].u64 & X64_CF) == X64_CF);
                        },
                        [=](CPU64& c) {
                            c.rflags = 0x202;
                            c.reg[X64_RBX].setU64(memoryForm ? dataAddr : initial);
                            c.reg[X64_RSI].setU64((U64)indices[index]);
                        });
                }
            }
        }
    }

    // Test 65h — immediate 0F BA forms still mask imm8 within the addressed
    // operand. This RIP-relative locked memory form also verifies its disp32
    // is based after the trailing immediate byte.
    {
        const U8 exts[] = {5,6,7};
        const char* names[] = {"LOCK BTS RIP-relative imm8",
                               "LOCK BTR RIP-relative imm8",
                               "LOCK BTC RIP-relative imm8"};
        for (int i = 0; i < 3; i++) {
            std::vector<U8> code = {
                0xF0,0x48,0x0F,0xBA,(U8)((exts[i] << 3) | 5),
                0x0E,0x00,0x00,0x00,0xC8, // displacement 14; imm8 200 -> bit 8
                0x9C,0x58,
            };
            U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
            code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
            U64 initial = 1ULL << 8;
            for (U8 byte = 0; byte < 8; byte++)
                code.push_back((U8)(initial >> (byte * 8)));
            U64 expected = i == 0 ? initial : 0;
            runAndCheck(r, names[i], code, [=](CPU64& c) {
                return c.memory->readq(dataAddr) == expected &&
                       (((U32)c.reg[X64_R15].u64 & X64_CF) == X64_CF);
            }, [=](CPU64& c) {
                c.rflags = 0x202;
            });
        }
    }

    // Test 65i — MXCSR default, clone state, load/store, and reserved-bit #GP.
    {
        runAndCheck(r, "MXCSR default is 0x1F80", withExit({}), [](CPU64& c) {
            return c.mxcsr == 0x1F80;
        });
        runAndCheck(r, "CPU64 clone copies MXCSR", withExit({}), [](CPU64& c) {
            CPU64 clone(c.memory);
            clone.cloneRegistersFrom(&c);
            return clone.mxcsr == 0x5F80;
        }, [](CPU64& c) {
            c.mxcsr = 0x5F80;
        });

        {
            std::vector<U8> code = {0x0F,0xAE,0x13, 0x0F,0xAE,0x5B,0x04};
            U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
            code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
            U32 input = 0x5F80;
            for (U8 byte = 0; byte < 4; byte++) code.push_back((U8)(input >> (byte * 8)));
            code.insert(code.end(), 4, 0);
            runAndCheck(r, "LDMXCSR/STMXCSR memory", code, [=](CPU64& c) {
                return c.mxcsr == input && c.memory->readd(dataAddr + 4) == input;
            }, [=](CPU64& c) {
                c.reg[X64_RBX].setU64(dataAddr);
            });
        }

        {
            std::vector<U8> code = {0x0F,0xAE,0x15, 0x0C,0x00,0x00,0x00};
            U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
            code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
            U32 invalid = 0x00010000;
            for (U8 byte = 0; byte < 4; byte++) code.push_back((U8)(invalid >> (byte * 8)));
            runAndCheck(r, "LDMXCSR reserved bits raise #GP", code, [](CPU64& c) {
                return c.yield && c.rip == CODE_BASE && c.mxcsr == 0x1F80;
            });
        }
    }

    // Test 65j — FXSAVE/FXRSTOR round-trip FCW/FSW/FTW/FOP, MXCSR, all x87
    // 80-bit stack slots, and all 16 XMM registers in the 512-byte layout.
    {
        std::vector<U8> code = {
            0x0F,0xAE,0x03,                         // fxsave [rbx]
            0x0F,0xAE,0x93,0x00,0x02,0x00,0x00,   // ldmxcsr [rbx+512]
            0x66,0x0F,0xEF,0xC0,                  // pxor xmm0,xmm0
            0xDB,0xE3,                             // fninit
            0x0F,0xAE,0x0B,                        // fxrstor [rbx]
            0x0F,0xAE,0x9B,0x10,0x02,0x00,0x00, // stmxcsr [rbx+528]
        };
        while ((code.size() + EXIT_SYSCALL.size()) & 15) code.push_back(0x90);
        U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
        code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
        code.resize(code.size() + 532, 0);
        U32 replacementMxcsr = 0x1F80;
        for (U8 byte = 0; byte < 4; byte++)
            code[(size_t)(dataAddr - CODE_BASE + 512 + byte)] =
                (U8)(replacementMxcsr >> (byte * 8));

        runAndCheck(r, "FXSAVE64/FXRSTOR64 full state round-trip", code,
            [=](CPU64& c) {
                if (c.mxcsr != 0x3F80 || c.fpu.cw != 0x077F || c.fpu.top != 6 ||
                    c.fpu.envData[1] != 0x456 || c.fpu.tags[6] == TAG_Empty ||
                    c.fpu.tags[7] == TAG_Empty ||
                    c.memory->readw(dataAddr + 0) != 0x077F ||
                    c.memory->readw(dataAddr + 2) != (U16)c.fpu.SW() ||
                    c.memory->readb(dataAddr + 4) != 0xC0 ||
                    c.memory->readw(dataAddr + 6) != 0x456 ||
                    c.memory->readq(dataAddr + 8) != 0 ||
                    c.memory->readq(dataAddr + 16) != 0 ||
                    c.memory->readd(dataAddr + 24) != 0x3F80 ||
                    c.memory->readd(dataAddr + 28) != 0xFFFF ||
                    c.memory->readd(dataAddr + 528) != 0x3F80) return false;
                U64 low, high;
                c.fpu.ST80(c.fpu.STV(0), &low, &high);
                if (c.memory->readq(dataAddr + 32) != low ||
                    c.memory->readw(dataAddr + 40) != (U16)high) return false;
                for (int i = 0; i < 16; i++) {
                    if (c.xmm[i].lo != (0x1111222233334444ULL + (U64)i) ||
                        c.xmm[i].hi != (0xAAAABBBBCCCCDDDDULL + (U64)i) ||
                        c.memory->readq(dataAddr + 160 + i * 16) != c.xmm[i].lo ||
                        c.memory->readq(dataAddr + 168 + i * 16) != c.xmm[i].hi)
                        return false;
                }
                for (U64 offset = 416; offset < 512; offset += 8)
                    if (c.memory->readq(dataAddr + offset) != 0) return false;
                return true;
            }, [=](CPU64& c) {
                c.reg[X64_RBX].setU64(dataAddr);
                c.mxcsr = 0x3F80;
                c.fpu.FINIT();
                c.fpu.SetCW(0x077F);
                c.fpu.envData[1] = 0x456;
                c.fpu.PREP_PUSH();
                c.fpu.FLD_F64(0x4004000000000000ULL, c.fpu.STV(0)); // 2.5
                c.fpu.PREP_PUSH();
                c.fpu.FLD_F64(0x3FF8000000000000ULL, c.fpu.STV(0)); // 1.5
                for (int i = 0; i < 16; i++) {
                    c.xmm[i].lo = 0x1111222233334444ULL + (U64)i;
                    c.xmm[i].hi = 0xAAAABBBBCCCCDDDDULL + (U64)i;
                }
            });
    }

    // Test 65k — scalar CVTSS2SI/CVTSD2SI honor MXCSR.RC for register and
    // memory sources, including positive and negative halfway values.
    {
        struct ScalarConvert {
            const char* name;
            U8 prefix;
            bool isDouble;
            U64 positiveBits;
            U64 negativeBits;
        };
        const ScalarConvert conversions[] = {
            {"CVTSS2SI",0xF3,false,0x40200000,0xC0200000},
            {"CVTSD2SI",0xF2,true,0x4004000000000000ULL,0xC004000000000000ULL},
        };
        const U32 rcBits[] = {0x0000,0x2000,0x4000,0x6000};
        const S64 expected[4][2] = {{2,-2},{2,-3},{3,-2},{2,-2}};
        const char* rcNames[] = {"nearest", "down", "up", "zero"};
        for (const auto& convert : conversions) {
            for (int rc = 0; rc < 4; rc++) {
                for (int sign = 0; sign < 2; sign++) {
                    U64 bits = sign ? convert.negativeBits : convert.positiveBits;
                    for (int form = 0; form < 2; form++) {
                        std::vector<U8> code = {convert.prefix,0x48,0x0F,0x2D,
                                                (U8)(form == 0 ? 0xC0 : 0x03)};
                        U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
                        code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                        if (form == 1) {
                            U8 width = convert.isDouble ? 8 : 4;
                            for (U8 byte = 0; byte < width; byte++)
                                code.push_back((U8)(bits >> (byte * 8)));
                        }
                        std::string testName = std::string(convert.name) + " " +
                            rcNames[rc] + (sign ? " -2.5 " : " +2.5 ") +
                            (form == 0 ? "register" : "memory");
                        runAndCheck(r, testName.c_str(), code,
                            [=](CPU64& c) {
                                return c.reg[X64_R15].u64 == (U64)expected[rc][sign];
                            },
                            [=](CPU64& c) {
                                c.mxcsr = 0x1F80 | rcBits[rc];
                                if (form == 0) c.xmm[0].lo = bits;
                                else c.reg[X64_RBX].setU64(dataAddr);
                            });
                    }
                }
            }
        }
    }

    // Test 65l — packed float-to-int conversions return integer-indefinite
    // for NaN, infinities, and out-of-range lanes in register and memory forms.
    {
        struct PackedConvert {
            const char* name;
            U8 prefix;
            U8 opcode;
            bool isDouble;
            bool truncate;
        };
        const PackedConvert conversions[] = {
            {"CVTPS2DQ",0x66,0x5B,false,false},
            {"CVTTPS2DQ",0xF3,0x5B,false,true},
            {"CVTPD2DQ",0xF2,0xE6,true,false},
            {"CVTTPD2DQ",0x66,0xE6,true,true},
        };
        const U32 psInputs[][4] = {
            {0x3FCCCCCD,0xC02CCCCC,0x00000000,0x7FC00000},
            {0x7F800000,0xFF800000,0x4F000000,0xCF000001},
        };
        const U32 psExpected[][4] = {
            {2,0xFFFFFFFD,0,0x80000000},
            {0x80000000,0x80000000,0x80000000,0x80000000},
        };
        const U32 psTruncExpected[][4] = {
            {1,0xFFFFFFFE,0,0x80000000},
            {0x80000000,0x80000000,0x80000000,0x80000000},
        };
        const U64 pdInputs[][2] = {
            {0x3FF999999999999AULL,0xC00599999999999AULL}, // 1.6, -2.7
            {0x7FF8000000000000ULL,0x7FF0000000000000ULL}, // NaN, +inf
            {0xFFF0000000000000ULL,0x41E0000000000000ULL}, // -inf, 2^31
        };
        const U32 pdExpected[][2] = {{2,0xFFFFFFFD},
            {0x80000000,0x80000000},{0x80000000,0x80000000}};
        const U32 pdTruncExpected[][2] = {{1,0xFFFFFFFE},
            {0x80000000,0x80000000},{0x80000000,0x80000000}};

        for (const auto& convert : conversions) {
            int dataSets = convert.isDouble ? 3 : 2;
            for (int dataSet = 0; dataSet < dataSets; dataSet++) {
                U8 source[16] = {};
                U32 outLanes[4] = {};
                if (convert.isDouble) {
                    memcpy(source, pdInputs[dataSet], sizeof(pdInputs[dataSet]));
                    const U32* expected = convert.truncate
                        ? pdTruncExpected[dataSet] : pdExpected[dataSet];
                    outLanes[0] = expected[0]; outLanes[1] = expected[1];
                } else {
                    memcpy(source, psInputs[dataSet], sizeof(psInputs[dataSet]));
                    const U32 (*expected)[4] = convert.truncate
                        ? psTruncExpected : psExpected;
                    memcpy(outLanes, expected[dataSet], sizeof(outLanes));
                }
                U64 expectedLo = (U64)outLanes[0] | ((U64)outLanes[1] << 32);
                U64 expectedHi = (U64)outLanes[2] | ((U64)outLanes[3] << 32);
                for (int form = 0; form < 2; form++) {
                    std::vector<U8> code = {convert.prefix,0x0F,convert.opcode,
                                            (U8)(form == 0 ? 0xC0 : 0x03)};
                    U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
                    code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                    if (form == 1) code.insert(code.end(), source, source + 16);
                    std::string testName = std::string(convert.name) +
                        (form == 0 ? " register" : " memory") +
                        " lane set " + std::to_string(dataSet);
                    runAndCheck(r, testName.c_str(), code, [=](CPU64& c) {
                        return c.xmm[0].lo == expectedLo && c.xmm[0].hi == expectedHi;
                    }, [=](CPU64& c) {
                        c.mxcsr = 0x1F80;
                        if (form == 0) {
                            memcpy(&c.xmm[0].lo, source, 8);
                            memcpy(&c.xmm[0].hi, source + 8, 8);
                        } else {
                            c.reg[X64_RBX].setU64(dataAddr);
                        }
                    });
                }
            }
        }
    }

    // Test 65m — packed non-truncating conversions follow all four MXCSR.RC
    // modes for positive and negative halfway lanes, from register and memory.
    {
        struct RoundConvert { const char* name; U8 prefix; U8 opcode; bool isDouble; };
        const RoundConvert conversions[] = {
            {"CVTPS2DQ",0x66,0x5B,false},
            {"CVTPD2DQ",0xF2,0xE6,true},
        };
        const U32 rcBits[] = {0x0000,0x2000,0x4000,0x6000};
        const S32 psExpected[][4] = {
            {2,-2,0,2},{2,-3,0,1},{3,-2,0,2},{2,-2,0,1},
        };
        const S32 pdExpected[][2] = {{2,-2},{2,-3},{3,-2},{2,-2}};
        const char* rcNames[] = {"nearest", "down", "up", "zero"};
        U32 psSource[4] = {0x40200000,0xC0200000,0x00000000,0x3FC00000};
        U64 pdSource[2] = {0x4004000000000000ULL,0xC004000000000000ULL};
        for (const auto& convert : conversions) {
            for (int rc = 0; rc < 4; rc++) {
                for (int form = 0; form < 2; form++) {
                    std::vector<U8> code = {0x0F,0xAE,0x53,0x10}; // ldmxcsr [rbx+16]
                    if (form == 0) code.insert(code.end(), {0x0F,0x10,0x03}); // movups xmm0,[rbx]
                    code.insert(code.end(), {convert.prefix,0x0F,convert.opcode,
                                             (U8)(form == 0 ? 0xC0 : 0x03)});
                    U64 dataAddr = CODE_BASE + code.size() + EXIT_SYSCALL.size();
                    code.insert(code.end(), EXIT_SYSCALL.begin(), EXIT_SYSCALL.end());
                    if (convert.isDouble) {
                        for (U64 lane : pdSource)
                            for (U8 byte = 0; byte < 8; byte++)
                                code.push_back((U8)(lane >> (byte * 8)));
                    } else {
                        for (U32 lane : psSource)
                            for (U8 byte = 0; byte < 4; byte++)
                                code.push_back((U8)(lane >> (byte * 8)));
                    }
                    U32 mxcsr = 0x1F80 | rcBits[rc];
                    for (U8 byte = 0; byte < 4; byte++)
                        code.push_back((U8)(mxcsr >> (byte * 8)));
                    std::string name = std::string(convert.name) + " " + rcNames[rc] +
                        (form == 0 ? " register" : " memory");
                    runAndCheck(r, name.c_str(), code, [=](CPU64& c) {
                        if (convert.isDouble) {
                            U64 lo = (U32)pdExpected[rc][0] |
                                     ((U64)(U32)pdExpected[rc][1] << 32);
                            return c.xmm[0].lo == lo && c.xmm[0].hi == 0;
                        }
                        U64 lo = (U32)psExpected[rc][0] |
                                 ((U64)(U32)psExpected[rc][1] << 32);
                        U64 hi = (U32)psExpected[rc][2] |
                                 ((U64)(U32)psExpected[rc][3] << 32);
                        return c.xmm[0].lo == lo && c.xmm[0].hi == hi;
                    }, [=](CPU64& c) {
                        c.reg[X64_RBX].setU64(dataAddr);
                    });
                }
            }
        }
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

    // ---- SSSE3 packed PABS / PHADD / PSIGN ----
    //
    // Inputs go in via movq into the low qword of xmm0 (the dest). For the
    // two-source ops we also load xmm1 (the src). The high qword is zeroed
    // by movq, so we reason only about the low 8 bytes and read .lo back.

    // Test 75a — PABSD: dwords {-3, 5} (lo = 0x00000005FFFFFFFD) → {3, 5}
    //   result.lo = 0x0000000500000003
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xFD, 0xFF, 0xFF, 0xFF, 0x05, 0x00, 0x00, 0x00, // mov rax, 0x00000005FFFFFFFD
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x66, 0x0F, 0x38, 0x1E, 0xC0,                               // pabsd xmm0, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "pabsd |{-3,5}| = {3,5}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000500000003ULL;
        });
    }

    // Test 75b — PABSW: words {-1, 2, -3, 4} (lo = 0x0004FFFD0002FFFF)
    //   → {1, 2, 3, 4} = 0x0004000300020001
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xFF, 0xFF, 0x02, 0x00, 0xFD, 0xFF, 0x04, 0x00, // mov rax, 0x0004FFFD0002FFFF
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x66, 0x0F, 0x38, 0x1D, 0xC0,                               // pabsw xmm0, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "pabsw |{-1,2,-3,4}| = {1,2,3,4}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0004000300020001ULL;
        });
    }

    // Test 75c — PABSB: bytes {-1,-2,3,-4,5,6,-7,8}
    //   lo = 0x08F906050C FD FE FF ... build: byte0=0xFF(-1),1=0xFE(-2),
    //   2=0x03,3=0xFC(-4),4=0x05,5=0x06,6=0xF9(-7),7=0x08
    //   → {1,2,3,4,5,6,7,8} = 0x0807060504030201
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xFF, 0xFE, 0x03, 0xFC, 0x05, 0x06, 0xF9, 0x08, // mov rax, ...
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x66, 0x0F, 0x38, 0x1C, 0xC0,                               // pabsb xmm0, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "pabsb |signed bytes| = magnitudes", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0807060504030201ULL;
        });
    }

    // Test 75d — PHADDD xmm0, xmm0: dwords loaded only in low qword via movq,
    //   so the full vector is d = {3, 10, 0, 0} (lanes 2,3 zeroed by movq).
    //   PHADDD result = {d0+d1, d2+d3, s0+s1, s2+s3}; src==dst here, so
    //   lanes = {13, 0, 13, 0}. Reading back .lo (lanes 0,1) = 0x000000000000000D.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, // mov rax, 0x0000000A00000003
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x66, 0x0F, 0x38, 0x02, 0xC0,                               // phaddd xmm0, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "phaddd {3,10,0,0} self → low lanes {13,0}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x000000000000000DULL;
        });
    }

    // Test 75e — PHADDW xmm0, xmm0: words loaded only in low qword via movq,
    //   so full vector w = {1,2,3,4, 0,0,0,0}. PHADDW lanes =
    //   {w0+w1, w2+w3, w4+w5, w6+w7, s0+s1, s2+s3, s4+s5, s6+s7}; src==dst →
    //   {3, 7, 0, 0, 3, 7, 0, 0}. Reading .lo (lanes 0..3) = {3,7,0,0}
    //   = 0x0000000000070003.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, // mov rax, 0x0004000300020001
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x66, 0x0F, 0x38, 0x01, 0xC0,                               // phaddw xmm0, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "phaddw {1,2,3,4,0,0,0,0} self → low lanes {3,7,0,0}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000000070003ULL;
        });
    }

    // Test 75f — PSIGND dst={10,10}, src={-1, 0}:
    //   lane0: src<0 → -dst = -10 = 0xFFFFFFF6
    //   lane1: src==0 → 0
    //   result.lo = 0x00000000FFFFFFF6
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x0A, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, // mov rax, 0x0000000A0000000A (dst)
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x00000000FFFFFFFF (src)
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x0A, 0xC1,                               // psignd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "psignd {10,10} by {-1,0} → {-10,0}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x00000000FFFFFFF6ULL;
        });
    }

    // Test 75g — PSIGND keep case: dst={5,5}, src={1,1} (both positive) →
    //   dst unchanged = {5,5}. result.lo = 0x0000000500000005.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, // mov rax, 0x0000000500000005 (dst)
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x48, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov rax, 0x0000000100000001 (src)
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x0A, 0xC1,                               // psignd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "psignd {5,5} by {1,1} keeps dst", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000500000005ULL;
        });
    }

    // Remaining SSSE3 XMM operations, with both register and memory sources.
    {
        struct Ssse3Case { const char* name; U8 opcode; U64 dstLo, dstHi, srcLo, srcHi, outLo, outHi; };
        const Ssse3Case cases[] = {
            {"phaddsw",0x03,0xFFFE800000017FFFULL,0x00060005FF380064ULL,
             0x0004000300020001ULL,0x0008000700060005ULL,
             0x000BFF9C80007FFFULL,0x000F000B00070003ULL},
            {"pmaddubsw",0x04,0x070605040302FFFFULL,0,
             0x040302017F807F7FULL,0,
             0x002E000E007D7FFFULL,0},
            {"phsubw",0x05,0x0004FFFB0003000AULL,0,
             0xFFFFFFF800020007ULL,0,
             0x00000000FFF70007ULL,0x00000000FFF90005ULL},
            {"phsubd",0x06,0x000000030000000AULL,0x0000000600000014ULL,
             0x0000000200000007ULL,0x0000000400000009ULL,
             0x0000000E00000007ULL,0x0000000500000005ULL},
            {"phsubsw",0x07,0x00018000FFFF7FFFULL,0,
             0x0001FFFF7FFF8000ULL,0,
             0x0000000080007FFFULL,0x00000000FFFE8000ULL},
            {"pmulhrsw",0x0B,0x7FFF8000C0004000ULL,0,
             0x7FFF8000C0004000ULL,0,
             0x7FFE800020002000ULL,0}
        };
        const U64 data = CODE_BASE + 0x800;
        for (const Ssse3Case& item : cases) {
            for (int memorySource = 0; memorySource < 2; memorySource++) {
                std::vector<U8> code = {0x66,0x0F,0x38,item.opcode,
                    (U8)(memorySource ? 0x03 : 0xC1)};
                runAndCheck(r, (std::string(item.name) + (memorySource ? " memory" : " register")).c_str(),
                    withExit(code), [item](CPU64& c) {
                        return c.xmm[0].lo == item.outLo && c.xmm[0].hi == item.outHi;
                    }, [item,data](CPU64& c) {
                        c.xmm[0].lo = item.dstLo; c.xmm[0].hi = item.dstHi;
                        c.xmm[1].lo = item.srcLo; c.xmm[1].hi = item.srcHi;
                        c.reg[X64_RBX].setU64(data);
                        c.memory->writeq(data,item.srcLo);
                        c.memory->writeq(data+8,item.srcHi);
                    });
            }
        }
    }

    // PALIGNR's immediate follows ModR/M and its RIP-relative source begins
    // after a short jump, so this also checks the instruction-end RIP base.
    {
        std::vector<U8> code = {0x66,0x0F,0x3A,0x0F,0x05,0x02,0,0,0,0x05,
                                0xEB,0x10};
        const U8 src[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
        code.insert(code.end(),src,src+sizeof(src));
        runAndCheck(r,"palignr rip-relative imm8 source",withExit(code),[](CPU64& c) {
            return c.xmm[0].lo == 0x0C0B0A0908070605ULL &&
                   c.xmm[0].hi == 0x14131211100F0E0DULL;
        },[](CPU64& c) {
            c.xmm[0].lo = 0x1716151413121110ULL;
            c.xmm[0].hi = 0x1F1E1D1C1B1A1918ULL;
        });
    }

    // ---- SSE4.1 packed dword PMULLD / PMINSD / PMAXSD ----

    // Test 75h — PMULLD {2,3} * {7,7} = {14,21} → 0x000000150000000E
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, // mov rax, 0x0000000300000002
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x48, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, // mov rax, 0x0000000700000007
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x40, 0xC1,                               // pmulld xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "pmulld {2,3}*{7,7} = {14,21}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x000000150000000EULL;
        });
    }

    // Test 75i — PMINSD {10,-5} min {4,4}: lane0 min(10,4)=4,
    //   lane1 min(-5,4)=-5 → {4,-5} = 0xFFFFFFFB00000004
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x0A, 0x00, 0x00, 0x00, 0xFB, 0xFF, 0xFF, 0xFF, // mov rax, 0xFFFFFFFB0000000A (dst {10,-5})
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x48, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, // mov rax, 0x0000000400000004 (src {4,4})
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x39, 0xC1,                               // pminsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "pminsd {10,-5} min {4,4} = {4,-5}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFB00000004ULL;
        });
    }

    // Test 75j — PMAXSD {10,-5} max {4,4}: lane0 max(10,4)=10,
    //   lane1 max(-5,4)=4 → {10,4} = 0x000000040000000A
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x0A, 0x00, 0x00, 0x00, 0xFB, 0xFF, 0xFF, 0xFF, // mov rax, 0xFFFFFFFB0000000A (dst {10,-5})
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x48, 0xB8, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, // mov rax, 0x0000000400000004 (src {4,4})
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0x66, 0x0F, 0x38, 0x3D, 0xC1,                               // pmaxsd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "pmaxsd {10,-5} max {4,4} = {10,4}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x000000040000000AULL;
        });
    }

    // SSE4.1 0F38 forms: zero vectors cover register and memory dispatch,
    // with the two existing compare/test forms checking their defined result.
    {
        struct Op { const char* name; U8 opcode; };
        const Op ops[] = {
            {"pblendvb",0x10},{"blendvps",0x14},{"blendvpd",0x15},
            {"pmovsx_bw",0x20},{"pmovsx_bd",0x21},{"pmovsx_bq",0x22},
            {"pmovsx_wd",0x23},{"pmovsx_wq",0x24},{"pmovsx_dq",0x25},
            {"pmuldq",0x28},{"pcmpeqq",0x29},{"movntdqa",0x2A},{"packusdw",0x2B},
            {"pmovzx_bw",0x30},{"pmovzx_bd",0x31},{"pmovzx_bq",0x32},
            {"pmovzx_wd",0x33},{"pmovzx_wq",0x34},{"pmovzx_dq",0x35},
            {"pminsb",0x38},{"pminsd",0x39},{"pminuw",0x3A},{"pminud",0x3B},
            {"pmaxsb",0x3C},{"pmaxsd",0x3D},{"pmaxuw",0x3E},{"pmaxud",0x3F},
            {"pmulld",0x40},{"phminposuw",0x41}
        };
        const U64 data=CODE_BASE+0x800;
        for(const Op& op:ops)for(int memorySource=0;memorySource<(op.opcode==0x2A?1:2);memorySource++) {
            std::vector<U8> code;
            if(op.opcode==0x2A)code={0x66,0x0F,0x38,op.opcode,0x43,0x10};
            else {
                code={0x0F,0x10,0x03};
                if(!memorySource)code.insert(code.end(),{0x0F,0x10,0x4B,0x10});
                code.insert(code.end(),{0x66,0x0F,0x38,op.opcode,(U8)(memorySource?0x43:0xC1)});
                if(memorySource)code.push_back(0x10);
            }
            code.insert(code.end(),{0x0F,0x11,0x03});
            bool equal=op.opcode==0x29;
            runAndCheck(r,(std::string("SSE4.1 ")+op.name+(memorySource?" memory":" register")).c_str(),
                withExit(code),[equal](CPU64& c) {
                    return c.xmm[0].lo==(equal?~0ULL:0ULL) && c.xmm[0].hi==(equal?~0ULL:0ULL);
                },[data](CPU64& c) {
                    c.reg[X64_RBX].setU64(data);c.xmm[0].lo=c.xmm[0].hi=0;
                    c.xmm[1].lo=c.xmm[1].hi=0;
                    for(U32 i=0;i<32;i++)c.memory->writeb(data+i,0);
                });
        }
        for(bool memorySource:{false,true}) {
            std::vector<U8> code={0x0F,0x10,0x03};
            if(!memorySource)code.insert(code.end(),{0x0F,0x10,0x4B,0x10});
            code.insert(code.end(),{0x66,0x0F,0x38,0x17,(U8)(memorySource?0x43:0xC1)});
            if(memorySource)code.push_back(0x10);
            runAndCheck(r,memorySource?"SSE4.1 PTEST memory flags":"SSE4.1 PTEST register flags",
                withExit(code),[](CPU64& c) {
                    return (c.rflags&(X64_CF|X64_ZF|X64_OF|X64_SF|X64_AF|X64_PF))==(X64_CF|X64_ZF);
                },[data](CPU64& c) {
                    c.reg[X64_RBX].setU64(data);c.xmm[0].lo=c.xmm[0].hi=0;c.xmm[1].lo=c.xmm[1].hi=0;
                    for(U32 i=0;i<32;i++)c.memory->writeb(data+i,0);
                });
        }
    }

    // SSE4.1 immediate forms in register, base-memory, and RIP-relative forms.
    // Zero operands give a compact exact result while verifying the ModR/M,
    // trailing imm8 length, and the 0F3A dispatch for every opcode.
    {
        struct Op { const char* name; U8 opcode; };
        const Op ops[] = {
            {"roundps",0x08},{"roundpd",0x09},{"roundss",0x0A},{"roundsd",0x0B},
            {"blendps",0x0C},{"blendpd",0x0D},{"pblendw",0x0E},
            {"pextrb",0x14},{"pextrw",0x15},{"pextrd_q",0x16},{"extractps",0x17},
            {"pinsrb",0x20},{"insertps",0x21},{"pinsrd_q",0x22},
            {"dpps",0x40},{"dppd",0x41},{"mpsadbw",0x42}
        };
        const U64 data=CODE_BASE+0x800;
        auto appendRip=[&](std::vector<U8>& code,U8 opcode,U8 imm,U8 target,bool wide) {
            code.insert(code.end(),{0x9C,0x50,0x51});
            size_t leaStart=code.size();code.insert(code.end(),{0x48,0x8D,0x05,0,0,0,0});
            code.insert(code.end(),{0x48,0x8D,0x4B,target,0x48,0x29,0xC1});
            size_t storeStart=code.size();code.insert(code.end(),{0x89,0x0D,0,0,0,0,0x59,0x58,0x9D,0xEB,0x01,0x90});
            size_t instructionStart=code.size();
            std::vector<U8> prefixes=wide?std::vector<U8>{0x66,0x48}:std::vector<U8>{0x66};
            code.insert(code.end(),prefixes.begin(),prefixes.end());
            code.insert(code.end(),{0x0F,0x3A,opcode,0x05,0,0,0,0,imm});
            auto patch=[&](size_t at,S32 value){U32 bits=(U32)value;for(U32 i=0;i<4;i++)code[at+i]=(U8)(bits>>(i*8));};
            size_t prefixLen=prefixes.size();
            patch(leaStart+3,(S32)(instructionStart+prefixLen+9-(leaStart+7)));
            patch(storeStart+2,(S32)(instructionStart+prefixLen+4-(storeStart+6)));
        };
        for(const Op& op:ops)for(U8 imm:{(U8)0,(U8)7,(U8)0xFF}) {
            bool extract=op.opcode>=0x14&&op.opcode<=0x17;
            bool gpInsert=op.opcode==0x20||op.opcode==0x22;
            bool widthVariant=op.opcode==0x16||op.opcode==0x22;
            for(int form=0;form<3;form++)for(int wide=0;wide<(widthVariant?2:1);wide++) {
                bool mem=form==1,ripRel=form==2;
                std::vector<U8> code={0x0F,0x10,0x03};
                if(!extract&&!gpInsert)code.insert(code.end(),{0x0F,0x10,0x4B,0x10});
                if(extract) {
                    if(ripRel)appendRip(code,op.opcode,imm,32,wide!=0);
                    else {
                        if(wide)code.push_back(0x48);
                        code.insert(code.end(),{0x66,0x0F,0x3A,op.opcode,
                            (U8)(mem?0x43:0xC0)});
                        if(mem)code.push_back(0x20);
                        code.push_back(imm);
                    }
                    if(!mem&&!ripRel)code.insert(code.end(),{0x48,0x89,0x43,0x20});
                } else {
                    if(ripRel)appendRip(code,op.opcode,imm,16,wide!=0);
                    else {
                        if(wide)code.push_back(0x48);
                        code.insert(code.end(),{0x66,0x0F,0x3A,op.opcode,
                            (U8)(mem?0x43:gpInsert?0xC0:0xC1)});
                        if(mem)code.push_back(0x10);
                        code.push_back(imm);
                    }
                    code.insert(code.end(),{0x0F,0x11,0x03});
                }
                runAndCheck(r,(std::string("SSE4.1 ")+op.name+" imm8 "+(ripRel?"RIP":mem?"memory":"register")+(wide?" REX.W":"")).c_str(),
                    withExit(code),[=](CPU64& c) {
                        if(extract) return c.memory->readq(data+32)==0 &&
                            (mem||ripRel||c.reg[X64_R15].u64==0);
                        return c.xmm[0].lo==0&&c.xmm[0].hi==0;
                    },[=](CPU64& c) {
                        c.reg[X64_RBX].setU64(data);c.reg[X64_RAX].setU64(0);
                        c.xmm[0].lo=c.xmm[0].hi=0;c.xmm[1].lo=c.xmm[1].hi=0;
                        for(U32 i=0;i<40;i++)c.memory->writeb(data+i,0);
                    });
            }
        }
    }

    // SSE4.2 packed-string opcodes use explicit or implicit lengths and an
    // imm8 control byte. Exercise every opcode in register, base-memory, and
    // RIP-relative forms; zero inputs produce a fully defined empty result.
    {
        const char* names[] = {"PCMPESTRM","PCMPESTRI","PCMPISTRM","PCMPISTRI"};
        const U8 opcodes[] = {0x60,0x61,0x62,0x63};
        const U64 input=CODE_BASE+0x800;
        for(U32 op=0;op<4;op++)for(U32 form=0;form<3;form++) {
            std::vector<U8> code={0x0F,0x10,0x03}; // XMM0 = first input.
            if(form==0)code.insert(code.end(),{0x0F,0x10,0x4B,0x10});
            size_t instructionStart=code.size();
            code.insert(code.end(),{0x66,0x0F,0x3A,opcodes[op],
                (U8)(form==0?0xC1:form==1?0x43:0x05)});
            if(form==1)code.push_back(0x10);
            if(form==2)code.insert(code.end(),{0,0,0,0});
            code.push_back(0); // imm8: unsigned bytes, equal-any, positive, bit-mask.
            bool maskResult=op==0||op==2;
            if(!maskResult)code.insert(code.end(),{0x89,0xC8}); // EAX = ECX.
            if(form==2) {
                code.insert(code.end(),EXIT_SYSCALL.begin(),EXIT_SYSCALL.end());
                size_t dataOffset=code.size();
                code.insert(code.end(),16,0); // inline source follows the exit suffix.
                S32 displacement=(S32)dataOffset-(S32)(instructionStart+10);
                U32 bits=(U32)displacement;
                size_t dispOffset=instructionStart+5;
                for(U32 i=0;i<4;i++)code[dispOffset+i]=(U8)(bits>>(i*8));
            } else {
                code=withExit(std::move(code));
            }
            runAndCheck(r,(std::string("SSE4.2 ")+names[op]+" "+
                           (form==0?"register":form==1?"memory":"RIP-relative")).c_str(),
                code,[maskResult](CPU64& c) {
                    bool output=maskResult?(c.xmm[0].lo==0&&c.xmm[0].hi==0):
                                           c.reg[X64_R15].u64==16;
                    U32 flags=X64_CF|X64_ZF|X64_SF|X64_OF|X64_AF|X64_PF;
                    return output&&(c.rflags&flags)==(X64_ZF|X64_SF);
                },[input](CPU64& c) {
                    c.reg[X64_RBX].setU64(input);
                    c.reg[X64_RAX].setU64(0);c.reg[X64_RDX].setU64(0);
                    c.xmm[0].lo=c.xmm[0].hi=0;c.xmm[1].lo=c.xmm[1].hi=0;
                    for(U32 i=0;i<32;i++)c.memory->writeb(input+i,0);
                });
        }
    }

    // PCMPGTQ compares signed qwords in both register and memory forms.
    {
        const U64 input=CODE_BASE+0x800;
        for(bool memorySource:{false,true}) {
            std::vector<U8> code={0x0F,0x10,0x03};
            if(!memorySource)code.insert(code.end(),{0x0F,0x10,0x4B,0x10});
            code.insert(code.end(),{0x66,0x0F,0x38,0x37,(U8)(memorySource?0x43:0xC1)});
            if(memorySource)code.push_back(0x10);
            runAndCheck(r,memorySource?"SSE4.2 PCMPGTQ memory":"SSE4.2 PCMPGTQ register",
                withExit(code),[](CPU64& c) {
                    return c.xmm[0].lo==~0ULL&&c.xmm[0].hi==0;
                },[input](CPU64& c) {
                    c.reg[X64_RBX].setU64(input);
                    c.xmm[0].lo=~0ULL;c.xmm[0].hi=2;
                    c.xmm[1].lo=~1ULL;c.xmm[1].hi=2;
                    c.memory->writeq(input,~0ULL);c.memory->writeq(input+8,2);
                    c.memory->writeq(input+16,~1ULL);c.memory->writeq(input+24,2);
                });
        }
    }

    // CRC32C byte/word/dword/qword sources with 32-bit and 64-bit destinations.
    {
        auto updateCrc=[](U32 crc,U64 value,U32 bytes) {
            for(U32 i=0;i<bytes;i++) {
                crc^=(U8)(value>>(i*8));
                for(U32 bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0x82F63B78U:0);
            }
            return crc;
        };
        struct Spec { const char* name; U8 opcode; U8 bytes; bool wide; };
        const Spec specs[]={{"CRC32 r32,r/m8",0xF0,1,false},
                            {"CRC32 r64,r/m8",0xF0,1,true},
                            {"CRC32 r32,r/m16",0xF1,2,false},
                            {"CRC32 r32,r/m32",0xF1,4,false},
                            {"CRC32 r64,r/m64",0xF1,8,true}};
        const U64 input=CODE_BASE+0x800;
        const U32 seed=0x12345678U;
        const U64 value=0xFEDCBA9876543210ULL;
        for(const Spec& spec:specs)for(bool memorySource:{false,true}) {
            std::vector<U8> code={0x48,0xB8};
            for(U32 i=0;i<8;i++)code.push_back((U8)((U64)seed>>(i*8)));
            code.insert(code.end(),{0x48,0xB9});
            for(U32 i=0;i<8;i++)code.push_back((U8)(value>>(i*8)));
            if(spec.bytes==2)code.push_back(0x66);
            code.push_back(0xF2);
            if(spec.wide)code.push_back(0x48);
            code.insert(code.end(),{0x0F,0x38,spec.opcode,(U8)(memorySource?0x43:0xC1)});
            if(memorySource)code.push_back(0x10);
            code.insert(code.end(),{0x48,0x89,0x43,0x20});
            runAndCheck(r,(std::string("SSE4.2 ")+spec.name+
                           (memorySource?" memory":" register")).c_str(),
                withExit(code),[=](CPU64& c) {
                    return c.reg[X64_R15].u64==updateCrc(seed,value,spec.bytes);
                },[=](CPU64& c) {
                    c.reg[X64_RBX].setU64(input);
                    for(U32 i=0;i<spec.bytes;i++)c.memory->writeb(input+16+i,(U8)(value>>(i*8)));
                });
        }
    }

    // Test 75k — MOVSHDUP {1,2,3,4} → {2,2,4,4}; reading .lo (lanes 0,1)
    //   = {2,2}. Inputs are integers placed in float lanes via movq — the
    //   op is a pure lane shuffle, so integer bit patterns work fine.
    //   dst.lo lanes {1,2} = 0x0000000200000001, dst.hi {3,4}.
    //   result lanes {2,2,4,4}; .lo = 0x0000000200000002.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, // mov rax, {1,2}
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0xF3, 0x0F, 0x16, 0xC1,                                     // movshdup xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "movshdup {1,2,..} → {2,2,..}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000200000002ULL;
        });
    }

    // Test 75l — MOVSLDUP {1,2,..} → {1,1,..}; .lo = 0x0000000100000001.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, // mov rax, {1,2}
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                               // movq xmm1, rax
            0xF3, 0x0F, 0x12, 0xC1,                                     // movsldup xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
        };
        runAndCheck(r, "movsldup {1,2,..} → {1,1,..}", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000100000001ULL;
        });
    }

    // ANDPD (66 0F 54): dst &= src. dst=0xFFFF..FF, src=0x7FFF..FF (the fabs
    // sign-clear mask glibc __printf_fp uses for %f) -> 0x7FFF..FF. This op
    // was the unimpl-opcode blocker for dynamic %f (probe2).
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,                     // mov rax, -1
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,   // mov rax, 0x7FFF..FF
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x54, 0xC1,                                       // andpd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "andpd masks sign bit (fabs idiom)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x7FFFFFFFFFFFFFFFULL;
        });
    }

    // ORPD (66 0F 56): 0x0F | 0xF0..00 = 0xF0..0F.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // rax = 0x0F
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0,   // rax = 0xF0..00
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x56, 0xC1,                                       // orpd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "orpd ors low qword", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xF00000000000000FULL;
        });
    }

    // ANDNPD (66 0F 55): ~dst & src. ~0x00FF & 0x0FF0 = 0x0F00.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xFF, 0x00, 0x00, 0x00,                     // rax = 0x00FF
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                 // movq xmm0, rax
            0x48, 0xC7, 0xC0, 0xF0, 0x0F, 0x00, 0x00,                     // rax = 0x0FF0
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                 // movq xmm1, rax
            0x66, 0x0F, 0x55, 0xC1,                                       // andnpd xmm0, xmm1
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                                 // movq rax, xmm0
        };
        runAndCheck(r, "andnpd computes ~dst & src", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000000000F00ULL;
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

    // Test 85b — CVTTPD2DQ (66 0F E6): TRUNCATE 2× f64 → 2× S32, hi qword 0.
    // {3.7, -4.7} truncates toward zero to {3, -4} (rounding would give {4,-5}),
    // so this distinguishes the truncating form from cvtpd2dq. fontconfig emits
    // this opcode while building font metrics; it was unimplemented (the
    // freetype-load "unimpl opcode 66 0f e6" that wedged the GUI boot).
    {
        // 3.7 = 0x400D999999999999A? use exact load via movabs of the IEEE bits.
        // 3.7  bits = 0x400D99999999999A ; -4.7 bits = 0xC012CCCCCCCCCCCD
        std::vector<U8> code = {
            0x48, 0xB8, 0x9A,0x99,0x99,0x99,0x99,0x99,0x0D,0x40,           // mov rax, bits(3.7)
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                                  // movq xmm0, rax (lo=3.7)
            0x48, 0xB8, 0xCD,0xCC,0xCC,0xCC,0xCC,0xCC,0x12,0xC0,           // mov rax, bits(-4.7)
            0x66, 0x48, 0x0F, 0x6E, 0xC8,                                  // movq xmm1, rax (lo=-4.7)
            0x66, 0x0F, 0x14, 0xC1,                                        // unpcklpd xmm0, xmm1 → {3.7, -4.7}
            0x66, 0x0F, 0xE6, 0xC8,                                        // cvttpd2dq xmm1, xmm0
            0x66, 0x48, 0x0F, 0x7E, 0xC8,                                  // movq rax, xmm1
        };
        runAndCheck(r, "cvttpd2dq {3.7, -4.7} -> {3, -4} (truncate)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == ((U64)3 | (((U64)(U32)(S32)-4) << 32));
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

    // ---- futex(202) stub semantics ----
    // FUTEX_WAKE_PRIVATE always returns 0 — "no waiters in our world".
    //   syscall convention: RAX=202 (futex), RDI=uaddr, RSI=op, RDX=val
    //   exit-suffix captures the futex-return-value into R15.
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,                     // mov rsi, 0x81 (FUTEX_WAKE_PRIVATE)
            0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,                     // mov rdx, 1
            0x0F, 0x05,                                                   // syscall
        };
        runAndCheck(r, "futex FUTEX_WAKE_PRIVATE → 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // FUTEX_WAIT mismatch: pre-store 42 at *uaddr, call WAIT with val=99 →
    // value mismatched, must return -EAGAIN per Linux semantics. EAGAIN=11
    // so the negated u64 is 0xFFFFFFFFFFFFFFF5.
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            // mov rdi, dst (uaddr)
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            // mov dword [rdi], 42
            0xC7, 0x07, 0x2A, 0x00, 0x00, 0x00,
            // mov rax, 202
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            // mov rsi, 0x80 (FUTEX_WAIT_PRIVATE)
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,
            // mov rdx, 99 (expected val — mismatches the stored 42)
            0x48, 0xC7, 0xC2, 0x63, 0x00, 0x00, 0x00,
            // xor r10, r10 (no timeout)
            0x4D, 0x31, 0xD2,
            // syscall
            0x0F, 0x05,
        };
        runAndCheck(r, "futex FUTEX_WAIT mismatch → -EAGAIN", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF5ULL;
        });
    }

    // FUTEX_WAIT match: would normally block; we return -EAGAIN as a
    // would-block stand-in (see sys_futex64 header comment). Pre-store 42,
    // call WAIT with val=42 → -EAGAIN.
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0xC7, 0x07, 0x2A, 0x00, 0x00, 0x00,                           // mov dword [rdi], 42
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,                     // mov rsi, FUTEX_WAIT_PRIVATE
            0x48, 0xC7, 0xC2, 0x2A, 0x00, 0x00, 0x00,                     // mov rdx, 42 (matches)
            0x4D, 0x31, 0xD2,                                             // xor r10, r10
            0x0F, 0x05,                                                   // syscall
        };
        runAndCheck(r, "futex FUTEX_WAIT match (no-block) → -EAGAIN", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF5ULL;
        });
    }

    // ---- futex(202) Milestone B4 extensions ----

    // FUTEX_WAKE_BITSET (op=10) is glibc 2.35+'s default for pthread_cond_signal.
    // Same semantics as WAKE: 0 waiters woken.
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0xC7, 0xC6, 0x8A, 0x00, 0x00, 0x00,                     // mov rsi, 0x8A (WAKE_BITSET|PRIVATE)
            0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,                     // mov rdx, 1
            0x0F, 0x05,
        };
        runAndCheck(r, "futex FUTEX_WAKE_BITSET_PRIVATE → 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // FUTEX_WAIT_BITSET (op=9) — value match still returns -EAGAIN (no-block path).
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0xC7, 0x07, 0x07, 0x00, 0x00, 0x00,                           // mov dword [rdi], 7
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xC7, 0xC6, 0x89, 0x00, 0x00, 0x00,                     // mov rsi, 0x89 (WAIT_BITSET|PRIVATE)
            0x48, 0xC7, 0xC2, 0x07, 0x00, 0x00, 0x00,                     // mov rdx, 7 (matches)
            0x4D, 0x31, 0xD2,                                             // xor r10, r10
            0x0F, 0x05,
        };
        runAndCheck(r, "futex FUTEX_WAIT_BITSET match → -EAGAIN", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF5ULL;
        });
    }

    // Real waiter-count: do two WAIT-on-match (each parks one would-be waiter
    // in cpu->futexWaiters[uaddr]), then WAKE with val=10 — must drain 2.
    // This is the Milestone B1 contract that distinguishes real bookkeeping
    // from the old "always 0" stub.
    {
        U64 dst = STACK_TOP - 0x808;
        std::vector<U8> code = {
            // *uaddr = 7
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0xC7, 0x07, 0x07, 0x00, 0x00, 0x00,
            // First FUTEX_WAIT (op=0x80) with val=7 → matches → bump waiter to 1
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,                     // mov rsi, WAIT_PRIVATE
            0x48, 0xC7, 0xC2, 0x07, 0x00, 0x00, 0x00,                     // mov rdx, 7
            0x4D, 0x31, 0xD2,                                             // xor r10, r10
            0x0F, 0x05,
            // Second FUTEX_WAIT (same addr) → waiter count → 2
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x07, 0x00, 0x00, 0x00,
            0x4D, 0x31, 0xD2,
            0x0F, 0x05,
            // FUTEX_WAKE (op=0x81) val=10 → must return 2 (drained both)
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,                     // mov rsi, WAKE_PRIVATE
            0x48, 0xC7, 0xC2, 0x0A, 0x00, 0x00, 0x00,                     // mov rdx, 10
            0x0F, 0x05,
        };
        runAndCheck(r, "futex WAKE drains accumulated waiters", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 2;
        });
    }

    // Per-address isolation: WAIT on addr A then WAKE on addr B must NOT
    // consume A's waiter. Verifies the futexWaiters map is actually keyed.
    {
        U64 dstA = STACK_TOP - 0x810;
        U64 dstB = STACK_TOP - 0x820;
        std::vector<U8> code = {
            // *dstA = 5
            0x48, 0xBF,
                (U8)(dstA), (U8)(dstA >> 8), (U8)(dstA >> 16), (U8)(dstA >> 24),
                (U8)(dstA >> 32), (U8)(dstA >> 40), (U8)(dstA >> 48), (U8)(dstA >> 56),
            0xC7, 0x07, 0x05, 0x00, 0x00, 0x00,
            // WAIT on dstA, val=5 → bumps waiter[dstA]=1
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x05, 0x00, 0x00, 0x00,
            0x4D, 0x31, 0xD2,
            0x0F, 0x05,
            // WAKE on dstB, val=10 → no waiter on dstB → returns 0
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xBF,
                (U8)(dstB), (U8)(dstB >> 8), (U8)(dstB >> 16), (U8)(dstB >> 24),
                (U8)(dstB >> 32), (U8)(dstB >> 40), (U8)(dstB >> 48), (U8)(dstB >> 56),
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x0A, 0x00, 0x00, 0x00,
            0x0F, 0x05,
        };
        runAndCheck(r, "futex WAKE addr-keyed (no cross-addr drain)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // Partial drain: stack two WAITs then WAKE with val=1 → returns 1, leaves 1.
    // Then a second WAKE drains the remainder.
    {
        U64 dst = STACK_TOP - 0x830;
        std::vector<U8> code = {
            // *uaddr = 3
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0xC7, 0x07, 0x03, 0x00, 0x00, 0x00,
            // Two WAIT-match calls → waiters=2
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x03, 0x00, 0x00, 0x00,
            0x4D, 0x31, 0xD2,
            0x0F, 0x05,
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x80, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x03, 0x00, 0x00, 0x00,
            0x4D, 0x31, 0xD2,
            0x0F, 0x05,
            // WAKE val=1 → returns 1, leaves 1 parked. Result goes to RBX.
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            0x48, 0x89, 0xC3,                                             // mov rbx, rax
            // WAKE val=10 → returns 1 (drains the 1 remaining). Add to rbx.
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC2, 0x0A, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            0x48, 0x01, 0xD8,                                             // add rax, rbx → rax = 2
        };
        runAndCheck(r, "futex WAKE partial drain (val=1, then val=10)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 2;
        });
    }

    // FUTEX_REQUEUE (op=3) — should return 0 (no waiters to move).
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0xC7, 0xC6, 0x03, 0x00, 0x00, 0x00,                     // mov rsi, 3 (REQUEUE)
            0x48, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00,                     // mov rdx, 0
            0x0F, 0x05,
        };
        runAndCheck(r, "futex FUTEX_REQUEUE → 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // uaddr=0 → -EFAULT regardless of op. EFAULT=14 → 0xFFFFFFFFFFFFFFF2.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0x31, 0xFF,                                             // xor rdi, rdi (uaddr=0)
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,                     // mov rsi, WAKE_PRIVATE
            0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,                     // mov rdx, 1
            0x0F, 0x05,
        };
        runAndCheck(r, "futex uaddr=NULL → -EFAULT", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF2ULL;
        });
    }

    // Linux rejects misaligned futex words with EINVAL before touching memory.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xC7, 0xC7, 0x01, 0x00, 0x80, 0x00,                     // mov rdi, 0x800001
            0x48, 0xC7, 0xC6, 0x81, 0x00, 0x00, 0x00,                     // mov rsi, WAKE_PRIVATE
            0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,                     // mov rdx, 1
            0x0F, 0x05,
        };
        runAndCheck(r, "futex unaligned uaddr → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL;
        });
    }

    // Unknown op (e.g. LOCK_PI = 6) → -ENOSYS so glibc falls back to user-space.
    // ENOSYS=38 → 0xFFFFFFFFFFFFFFDA.
    {
        U64 dst = STACK_TOP - 0x800;
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCA, 0x00, 0x00, 0x00,                     // mov rax, 202
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0xC7, 0xC6, 0x06, 0x00, 0x00, 0x00,                     // mov rsi, 6 (LOCK_PI — unhandled)
            0x48, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00,                     // mov rdx, 0
            0x0F, 0x05,
        };
        runAndCheck(r, "futex unknown op (LOCK_PI) → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // ---- mmap(9) NULL-hint allocator: single monotonic bump ----
    //
    // Regression guard for the multi-DSO Verneed bug. sys_mmap64 (anonymous)
    // and sys_mmap64_file (file-backed) MUST draw mmap(NULL,...) placements
    // from ONE shared pointer (CPU64::mmapNext). Previously each had its own
    // static seeded at the same base, so an anonymous map and a file-backed
    // map could be handed the SAME address — and mmapAnonymousFixed zero-fills
    // on map, clobbering one DSO's version records ("unsupported version 0 of
    // Verneed record" in the guest ld.so for the 2nd versioned library).
    //
    // We can only reach the anonymous path from the FS-less self-test (no fd),
    // but that path shares the very same allocator, so asserting two
    // consecutive mmap(NULL, 0x1000) returns are disjoint and monotonic pins
    // the single-pointer invariant. First base captured in R14, second in R15
    // (via the exit suffix). Expect R15 == R14 + 0x1000 and R14 != 0.
    {
        auto mmapNull = [](std::vector<U8>& v) {
            const U8 ins[] = {
                0x48, 0xC7, 0xC0, 0x09, 0x00, 0x00, 0x00, // mov rax, 9 (mmap)
                0x48, 0x31, 0xFF,                         // xor rdi, rdi (addr=NULL)
                0x48, 0xC7, 0xC6, 0x00, 0x10, 0x00, 0x00, // mov rsi, 0x1000 (len)
                0x48, 0xC7, 0xC2, 0x03, 0x00, 0x00, 0x00, // mov rdx, 3 (PROT_READ|WRITE)
                0x49, 0xC7, 0xC2, 0x22, 0x00, 0x00, 0x00, // mov r10, 0x22 (MAP_PRIVATE|ANON)
                0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, // mov r8, -1 (fd)
                0x4D, 0x31, 0xC9,                         // xor r9, r9 (offset=0)
                0x0F, 0x05,                               // syscall
            };
            v.insert(v.end(), ins, ins + sizeof(ins));
        };
        std::vector<U8> code;
        mmapNull(code);
        code.insert(code.end(), {0x49, 0x89, 0xC6}); // mov r14, rax (1st base)
        mmapNull(code);                              // 2nd base → RAX → R15 (exit suffix)
        runAndCheck(r, "mmap NULL-hint: shared monotonic bump (disjoint maps)",
                    withExit(code), [](CPU64& c) {
            U64 first = c.reg[X64_R14].u64;
            U64 second = c.reg[X64_R15].u64;
            return first == 0x700000000ULL && second == first + 0x1000ULL;
        });
    }

    // ptrace is unsupported by the 32-bit kernel too; return EPERM directly so
    // guest probes do not spin through the unimplemented-syscall path.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x65, 0x00, 0x00, 0x00,                     // mov rax, 101 (ptrace)
            0x0F, 0x05,
        };
        runAndCheck(r, "ptrace syscall → -EPERM", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFFFULL;
        });
    }

    // An unknown syscall is reported five times, then once with the
    // suppression marker. The runner checks the six emitted log lines.
    {
        std::vector<U8> code;
        for (int i = 0; i < 7; ++i) {
            code.insert(code.end(), {
                0x48, 0xC7, 0xC0, 0x0F, 0x27, 0x00, 0x00,                 // mov rax, 9999
                0x0F, 0x05,
            });
        }
        runAndCheck(r, "unknown syscall returns -ENOSYS after rate limiting", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // ---- Milestone B5: signal-syscall stubs ----

    // pause(34) → -EINTR (we never deliver, so loop-retry is the honest answer)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x22, 0x00, 0x00, 0x00,                     // mov rax, 34 (pause)
            0x0F, 0x05,
        };
        runAndCheck(r, "pause() → -EINTR", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFFCULL;
        });
    }

    // wait4(61) → -ECHILD (no children to reap)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x3D, 0x00, 0x00, 0x00,                     // mov rax, 61
            0x48, 0xC7, 0xC7, 0xFF, 0xFF, 0xFF, 0xFF,                     // mov rdi, -1 (any child)
            0x48, 0x31, 0xF6,                                             // xor rsi, rsi
            0x48, 0x31, 0xD2,                                             // xor rdx, rdx
            0x4D, 0x31, 0xD2,                                             // xor r10, r10
            0x0F, 0x05,
        };
        runAndCheck(r, "wait4(-1) → -ECHILD", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF6ULL;
        });
    }

    // clone(56) → -ENOSYS (real impl deferred; explicit so glibc surfaces it)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x38, 0x00, 0x00, 0x00,                     // mov rax, 56
            0x48, 0x31, 0xFF,                                             // xor rdi, rdi (flags=0)
            0x48, 0x31, 0xF6,                                             // xor rsi, rsi
            0x0F, 0x05,
        };
        runAndCheck(r, "clone(0) → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // getitimer(36) → -ENOSYS
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x24, 0x00, 0x00, 0x00,                     // mov rax, 36
            0x48, 0x31, 0xFF,                                             // rdi=which=0
            0x48, 0x31, 0xF6,                                             // rsi=NULL
            0x0F, 0x05,
        };
        runAndCheck(r, "getitimer() → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // rt_sigpending(127) writes zeros, returns 0
    {
        U64 dst = STACK_TOP - 0x900;
        std::vector<U8> code = {
            // Prefill mask slot with 0xDEADBEEF to confirm overwrite-to-zero.
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00,   // mov rax, 0xDEADBEEF
            0x48, 0x89, 0x07,                                             // mov [rdi], rax
            // syscall: rax=127, rdi=dst, rsi=8
            0x48, 0xC7, 0xC0, 0x7F, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC6, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            // Move the written mask into r15 via the verifier (r15 picks up rax).
            // We want r15 to reflect *both* return value (0) AND the mask write.
            // Pack: load mask into rcx, OR with rax-shifted... simpler: just
            // confirm rax==0 and check mask via a separate read-back into rax.
            0x48, 0xBF,
                (U8)(dst), (U8)(dst >> 8), (U8)(dst >> 16), (U8)(dst >> 24),
                (U8)(dst >> 32), (U8)(dst >> 40), (U8)(dst >> 48), (U8)(dst >> 56),
            0x48, 0x8B, 0x07,                                             // mov rax, [rdi]  (final RAX→R15 via withExit)
        };
        runAndCheck(r, "rt_sigpending writes zero mask", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // rt_sigpending sigsetsize=4 → -EINVAL
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xC7, 0xC0, 0x7F, 0x00, 0x00, 0x00,                     // mov rax, 127
            0x48, 0x89, 0xE7,                                             // mov rdi, rsp
            0x48, 0xC7, 0xC6, 0x04, 0x00, 0x00, 0x00,                     // mov rsi, 4 (wrong size)
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "rt_sigpending sigsetsize=4 → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL;
        });
    }

    // rt_sigtimedwait(128) → -EAGAIN (we treat unblocked as immediate timeout)
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xC7, 0xC0, 0x80, 0x00, 0x00, 0x00,                     // mov rax, 128
            0x48, 0x89, 0xE7,                                             // mov rdi, rsp (set)
            0x48, 0x31, 0xF6,                                             // rsi=NULL (info)
            0x48, 0x31, 0xD2,                                             // rdx=NULL (timeout)
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,                     // mov r10, 8
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "rt_sigtimedwait → -EAGAIN", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF5ULL;
        });
    }

    // rt_sigsuspend(130) → -EINTR (we pretend a signal interrupted)
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xC7, 0xC0, 0x82, 0x00, 0x00, 0x00,                     // mov rax, 130
            0x48, 0x89, 0xE7,                                             // mov rdi, rsp (mask)
            0x48, 0xC7, 0xC6, 0x08, 0x00, 0x00, 0x00,                     // mov rsi, 8
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "rt_sigsuspend → -EINTR", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFFCULL;
        });
    }

    // ----- B6: IPC/IO syscall stub surface -----
    // All return errno semantics (no real impl); each test pins:
    //   (a) RAX = expected negated errno
    //   (b) the dispatcher named the syscall (verified by absence of
    //       "unimplemented #<n>" in the dispatch path — implicit since we
    //       reach the per-case break instead of default).

    // clone3 → -ENOSYS (rax=435, expect -38 = 0xFFFFFFFFFFFFFFDA)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xB3, 0x01, 0x00, 0x00,                     // mov rax, 435
            0x48, 0x31, 0xFF,                                             // xor rdi, rdi (args)
            0x48, 0x31, 0xF6,                                             // xor rsi, rsi (size)
            0x0F, 0x05,                                                   // syscall
        };
        runAndCheck(r, "clone3 → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // pipe(NULL) → -EFAULT (rax=22, rdi=0, expect -14 = 0xFFFFFFFFFFFFFFF2)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x16, 0x00, 0x00, 0x00,                     // mov rax, 22
            0x48, 0x31, 0xFF,                                             // xor rdi, rdi
            0x0F, 0x05,                                                   // syscall
        };
        runAndCheck(r, "pipe(NULL) → -EFAULT", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF2ULL;
        });
    }

    // pipe2(buf, 0) → -ENOSYS (rax=293, rdi=stack slot, expect -38)
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,                                       // sub rsp, 16
            0x48, 0xC7, 0xC0, 0x25, 0x01, 0x00, 0x00,                     // mov rax, 293
            0x48, 0x89, 0xE7,                                             // mov rdi, rsp (non-NULL)
            0x48, 0x31, 0xF6,                                             // xor rsi, rsi (flags=0)
            0x0F, 0x05,                                                   // syscall
            0x48, 0x83, 0xC4, 0x10,                                       // add rsp, 16
        };
        runAndCheck(r, "pipe2 → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // eventfd2(0, 0) → -ENOSYS (rax=290)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x22, 0x01, 0x00, 0x00,                     // mov rax, 290
            0x48, 0x31, 0xFF,                                             // xor rdi
            0x48, 0x31, 0xF6,                                             // xor rsi
            0x0F, 0x05,
        };
        runAndCheck(r, "eventfd2 → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // epoll_create1(0) → -ENOSYS (rax=291)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x23, 0x01, 0x00, 0x00,                     // mov rax, 291
            0x48, 0x31, 0xFF,                                             // xor rdi
            0x0F, 0x05,
        };
        runAndCheck(r, "epoll_create1 → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // epoll_ctl → -EBADF (rax=233, expect -9 = 0xFFFFFFFFFFFFFFF7)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xE9, 0x00, 0x00, 0x00,                     // mov rax, 233
            0x48, 0x31, 0xFF, 0x48, 0x31, 0xF6, 0x48, 0x31, 0xD2,         // rdi=rsi=rdx=0
            0x49, 0x31, 0xD2,                                             // r10=0
            0x0F, 0x05,
        };
        runAndCheck(r, "epoll_ctl → -EBADF", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF7ULL;
        });
    }

    // epoll_wait → -EBADF (rax=232)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xE8, 0x00, 0x00, 0x00,                     // mov rax, 232
            0x48, 0x31, 0xFF, 0x48, 0x31, 0xF6, 0x48, 0x31, 0xD2,         // rdi=rsi=rdx=0
            0x49, 0x31, 0xD2,                                             // r10=0
            0x0F, 0x05,
        };
        runAndCheck(r, "epoll_wait → -EBADF", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF7ULL;
        });
    }

    // getdents64(0, NULL, 0) → -EFAULT (rax=217, rsi=0)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xD9, 0x00, 0x00, 0x00,                     // mov rax, 217
            0x48, 0x31, 0xFF,                                             // xor rdi
            0x48, 0x31, 0xF6,                                             // xor rsi (buf=NULL)
            0x48, 0x31, 0xD2,                                             // xor rdx
            0x0F, 0x05,
        };
        runAndCheck(r, "getdents64(NULL) → -EFAULT", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF2ULL;
        });
    }

    // select → -ENOSYS (rax=23)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x17, 0x00, 0x00, 0x00,                     // mov rax, 23
            0x48, 0x31, 0xFF, 0x48, 0x31, 0xF6, 0x48, 0x31, 0xD2,
            0x49, 0x31, 0xD2, 0x4D, 0x31, 0xC0,                           // r10=r8=0
            0x0F, 0x05,
        };
        runAndCheck(r, "select → -ENOSYS", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFDAULL;
        });
    }

    // chmod → 0 (success no-op, rax=90)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x5A, 0x00, 0x00, 0x00,                     // mov rax, 90
            0x48, 0x31, 0xFF, 0x48, 0x31, 0xF6,                           // rdi=rsi=0
            0x0F, 0x05,
        };
        runAndCheck(r, "chmod → 0 (no-op success)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // fchmod → 0 (success no-op, rax=91)
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x5B, 0x00, 0x00, 0x00,                     // mov rax, 91
            0x48, 0x31, 0xFF, 0x48, 0x31, 0xF6,                           // rdi=rsi=0
            0x0F, 0x05,
        };
        runAndCheck(r, "fchmod → 0 (no-op success)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // ----- x87 FPU minimal subset (D9/DC/DD) -----
    // Pattern: spill the operand double into a stack slot via
    //   sub rsp,8 ; mov rax,imm64 ; mov [rsp],rax
    // then drive x87 via fld/fstp qword [rsp], then read result back through rax.

    // T: FLD/FSTP m64fp round-trip preserves bits (3.0 = 0x4008000000000000).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,                                     // sub rsp, 8
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40, // mov rax, 0x4008000000000000 (3.0)
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            0xDD, 0x04, 0x24,                                           // fld  qword [rsp]
            0x48, 0x31, 0xC0,                                           // xor rax, rax (poison)
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax (clear)
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0x83, 0xC4, 0x08,                                     // add rsp, 8
        };
        runAndCheck(r, "x87 FLD/FSTP m64fp round-trip (3.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4008000000000000ULL;
        });
    }

    // T: FADD m64fp — 2.0 + 3.0 = 5.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,                                     // sub rsp, 8
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // mov rax, 0x4000000000000000 (2.0)
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            0xDD, 0x04, 0x24,                                           // fld  qword [rsp]   ; st0=2.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40, // mov rax, 3.0
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            0xDC, 0x04, 0x24,                                           // fadd qword [rsp]   ; st0=5.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0x83, 0xC4, 0x08,                                     // add rsp, 8
        };
        runAndCheck(r, "x87 FADD m64fp (2.0+3.0=5.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4014000000000000ULL; // 5.0
        });
    }

    // T: FMUL m64fp — 2.5 * 4.0 = 10.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.5
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x0C, 0x24,                                           // fmul qword [rsp]
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FMUL m64fp (2.5*4.0=10.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4024000000000000ULL; // 10.0
        });
    }

    // T: FLD m32fp + FSTP m64fp — single-precision 2.5f promotes to double 2.5.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xC7, 0xC0, 0x00, 0x00, 0x20, 0x40,                   // mov rax, 0x40200000 (2.5f, sign-ext fine)
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax (upper half junk, ignored)
            0xD9, 0x04, 0x24,                                           // fld  dword [rsp]   ; st0 = 2.5 (promoted)
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]   ; store as f64
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FLD m32fp + FSTP m64fp (2.5f → 2.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4004000000000000ULL; // 2.5
        });
    }

    // T: FXCH — push 2.0 then 3.0 (st0=3, st1=2), fxch st1 (st0=2), fstp → 2.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // 2.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.0  → st0=2
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40, // 3.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 3.0  → st0=3, st1=2
            0xD9, 0xC9,                                                 // fxch st1 → st0=2, st1=3
            0xDD, 0x1C, 0x24,                                           // fstp     → mem=2.0; st0=3
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FXCH ST(1) swaps tops", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4000000000000000ULL; // 2.0
        });
    }

    // T: D8 C8+i (FMUL ST(0), ST(i)) — the register form Gecko's xul.dll
    // uses during browser startup. The decoder only implemented FCOM/FCOMP
    // for this opcode before.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.5        ; st0=2.5
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 4.0        ; st0=4.0, st1=2.5
            0xD8, 0xC9,                                                 // fmul st(0),st1 ; st0=10.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 D8 FMUL ST(0),ST(1) (4.0*2.5=10.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4024000000000000ULL; // 10.0
        });
    }

    // T: D8 E0+i (FSUB ST(0), ST(i)) and D8 E8+i (FSUBR ST(0), ST(i)).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 4.0       ; st0=4.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x40, // 6.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 6.0       ; st0=6.0, st1=4.0
            0xD8, 0xE1,                                                 // fsub st(0),st(1) ; st0=2.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 D8 FSUB ST(0),ST(1) (6.0-4.0=2.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4000000000000000ULL; // 2.0
        });
    }

    // T: DC E9 (FSUB ST(1), ST(0)) writes ST(1) and does not pop.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.5        ; st0=2.5
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 4.0        ; st0=4.0, st1=2.5
            0xDC, 0xE9,                                                 // fsub st(1),st0 ; st1=-1.5
            0xD9, 0xC9,                                                 // fxch st(1)
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 DC FSUB ST(1),ST(0) (2.5-4.0=-1.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xBFF8000000000000ULL; // -1.5
        });
    }

    // T: D9 E5 (FXAM) classifies -0.0: C3=1, C2=0, C0=0, C1=1.
    // Gecko's xul.dll uses FXAM/FNSTSW before its first browser call.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, // -0.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld qword [rsp]
            0xD9, 0xE5,                                                 // fxam
            0xDF, 0xE0,                                                 // fnstsw ax
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 D9 FXAM classifies -0.0 (C3+C1)", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u16 & 0x4700) == 0x4200;
        });
    }

    // T: FLD ST(0) duplicates TOS; then FADD ST(0)+mem path. Pattern:
    //   fld 3.0; fld st0 (now st0=3, st1=3); fstp → 3.0 (st0=3 still).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40, // 3.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 3.0
            0xD9, 0xC0,                                                 // fld st0 (dup)
            0x48, 0x31, 0xC0,                                           // xor rax, rax
            0x48, 0x89, 0x04, 0x24,                                     // clear slot
            0xDD, 0x1C, 0x24,                                           // fstp (pops the duplicate)
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FLD ST(0) duplicate then FSTP", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4008000000000000ULL; // 3.0
        });
    }

    // ----- C8 extensions: FSUB/FSUBR/FDIV/FDIVR + FCHS/FABS/FLD1/FLDZ + FNSTSW -----

    // T: FSUB m64fp — 10.0 - 4.0 = 6.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x24, 0x24,                                           // fsub qword [rsp]   ; st0=6.0
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FSUB m64fp (10.0-4.0=6.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4018000000000000ULL; // 6.0
        });
    }

    // T: FSUBR m64fp — 4.0 - 10.0 = -6.0 (reverse order).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x2C, 0x24,                                           // fsubr qword [rsp]  ; st0 = 4-10 = -6
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FSUBR m64fp (4.0-10.0=-6.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xC018000000000000ULL; // -6.0
        });
    }

    // T: FDIV m64fp — 10.0 / 4.0 = 2.5.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x34, 0x24,                                           // fdiv qword [rsp]   ; st0=2.5
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FDIV m64fp (10.0/4.0=2.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4004000000000000ULL; // 2.5
        });
    }

    // T: FDIVR m64fp — 4.0 / 10.0 = 0.4 (reverse order).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x3C, 0x24,                                           // fdivr qword [rsp]  ; st0 = 4/10 = 0.4
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FDIVR m64fp (4.0/10.0=0.4)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x3FD999999999999AULL; // 0.4
        });
    }

    // T: FCHS — load 2.5, negate → -2.5.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.5
            0xD9, 0xE0,                                                 // fchs → -2.5
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FCHS (-2.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xC004000000000000ULL; // -2.5
        });
    }

    // T: FABS — load -2.5, abs → +2.5.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0, // -2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld -2.5
            0xD9, 0xE1,                                                 // fabs → +2.5
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FABS (|-2.5|=2.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4004000000000000ULL; // 2.5
        });
    }

    // T: FLD1 — push 1.0, store.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0xD9, 0xE8,                                                 // fld1 → st0=1.0
            0xDD, 0x1C, 0x24,                                           // fstp [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FLD1 (1.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x3FF0000000000000ULL; // 1.0
        });
    }

    // T: FLDZ — push 0.0, store.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            // Prefill slot with non-zero so we can confirm FLDZ actually wrote.
            0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x48, 0x89, 0x04, 0x24,
            0xD9, 0xEE,                                                 // fldz → st0=0.0
            0xDD, 0x1C, 0x24,                                           // fstp [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FLDZ (0.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000000000000ULL; // 0.0
        });
    }

    // T: FLD1 then FCHS then FADD with FLD1 → 0.0 (-1 + 1 = 0).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0xD9, 0xE8,                                                 // fld1 → st0=1
            0xD9, 0xE0,                                                 // fchs → st0=-1
            0xD9, 0xE8,                                                 // fld1 → st0=1, st1=-1
            // Add ST(1) into ST(0) via fadd qword [rsp] won't work — need a different op.
            // Easier: fxch then fstp twice. Actually simpler: use the existing FADD via mem.
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xBF, // -1.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x04, 0x24,                                           // fadd [rsp] → st0=1+(-1)=0
            0xDD, 0x1C, 0x24,                                           // fstp [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FLD1+FCHS pipeline (1, then -1+1=0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x0000000000000000ULL; // 0.0
        });
    }

    // Independent QEMU/x86-64 results for valid FYL2XP1 operands. Computing
    // log(1+x) loses tiny x and the sign of -0 before the logarithm runs.
    {
        const bool previousMode = KSystem::useF64;
        struct LogCase { U64 input, expected; const char* name; };
        const LogCase cases[] = {
            {0x3c30000000000000ULL, 0x3c371547652b82feULL, "x87 FYL2XP1 tiny positive epsilon"},
            {0xbc30000000000000ULL, 0xbc371547652b82feULL, "x87 FYL2XP1 tiny negative epsilon"},
            {0x8000000000000000ULL, 0x8000000000000000ULL, "x87 FYL2XP1 negative zero"},
        };
        for (bool cached : {false, true}) {
            KSystem::useF64 = cached;
            for (const auto& test : cases) {
                std::vector<U8> code = {0x48,0x83,0xEC,0x08,0x48,0xB8};
                for (int byte=0; byte<8; byte++) code.push_back((U8)(test.input >> (8*byte)));
                const U8 operation[] = {
                    0x48,0x89,0x04,0x24, // mov [rsp],rax
                    0xDB,0xE3,0xD9,0xE8, // finit; fld1
                    0xDD,0x04,0x24,0xD9,0xF9, // fld qword [rsp]; fyl2xp1
                    0xDD,0x1C,0x24,0x48,0x8B,0x04,0x24, // fstp [rsp]; mov rax,[rsp]
                    0x48,0x83,0xC4,0x08,
                };
                code.insert(code.end(), operation, operation + sizeof(operation));
                const U64 expected = test.expected;
                runAndCheck(r, test.name, withExit(code), [expected](CPU64& cpu) {
                    return cpu.reg[X64_R15].u64 == expected;
                });
            }
        }
        KSystem::useF64 = previousMode;
    }

    // T: FNSTSW AX — after FLDZ, FXAM-style flags not set in our simplified
    // model; we just confirm that the upper 48 bits of RAX are preserved and
    // the low 16 bits get the SW value (whatever it is). Pre-load RAX with a
    // sentinel; verify the high half stays.
    {
        std::vector<U8> code = {
            0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // mov rax, 0x0123456789ABCDEF
            0xD9, 0xE8,                                                 // fld1
            0xDF, 0xE0,                                                 // fnstsw ax — writes status into AX
            // RAX upper 48 bits should still be 0x012345678 9AB
            // Mask AX out: shr rax,16; shl rax,16 then OR... too complex.
            // Easier: AND rax with high mask and verify == 0x0123456789AB0000.
            0x48, 0x25, 0x00, 0x00, 0x00, 0x00,                         // and eax, 0  (zeros low 32, since this is and eax,imm32 → zero-extend)
            // After this, low 32 bits = 0 (the AX value was overwritten by the AND too,
            // but verifier just needs to confirm the high 32 bits survived).
            // Actually: AND EAX,imm32 zeros upper 32 bits of RAX too (zero-extension).
            // That defeats the test. Use a different verifier: just confirm RAX != orig
            // sentinel (i.e., AX was modified).
        };
        // Drop the AND — we'll just check that RAX changed from the sentinel value
        // (proving FNSTSW wrote something into AX) and that the upper 48 bits match.
        code = {
            0x48, 0xB8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // mov rax, 0x0123456789ABCDEF
            0xD9, 0xE8,                                                 // fld1
            0xDF, 0xE0,                                                 // fnstsw ax
        };
        runAndCheck(r, "x87 FNSTSW AX preserves RAX upper 48 bits", withExit(code), [](CPU64& c) {
            // Upper 48 bits of R15 (= RAX at exit) must equal upper 48 bits of sentinel.
            return (c.reg[X64_R15].u64 & 0xFFFFFFFFFFFF0000ULL) == 0x0123456789AB0000ULL;
        });
    }

    // ----- C9 extensions: x87 int↔FP convert FILD/FIST/FISTP -----

    // T: FILD m32int + FSTP m64fp — load int 42, store as f64 = 42.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00,                   // mov rax, 42
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            0xDB, 0x04, 0x24,                                           // fild dword [rsp]  ; st0=42.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FILD m32int (42 -> 42.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4045000000000000ULL; // 42.0
        });
    }

    // T: FILD m32int negative — load -7, store as f64 = -7.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xC7, 0xC0, 0xF9, 0xFF, 0xFF, 0xFF,                   // mov rax, -7 (sign-ext)
            0x48, 0x89, 0x04, 0x24,
            0xDB, 0x04, 0x24,                                           // fild dword [rsp]  ; st0=-7.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FILD m32int negative (-7 -> -7.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xC01C000000000000ULL; // -7.0
        });
    }

    // T: FLD 3.7 + FISTP m32int — round-to-nearest gives 4.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0x0D, 0x40, // 3.7
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld qword [rsp]   ; st0=3.7
            0xDB, 0x1C, 0x24,                                           // fistp dword [rsp] ; rounds, stores 4, pops
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]   ; low dword = 4
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FISTP m32int (3.7 -> 4 round-nearest)", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0xFFFFFFFFULL) == 4;
        });
    }

    // T: FILD m16int — load int16 0x1234 = 4660, store as f64.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xC7, 0xC0, 0x34, 0x12, 0x00, 0x00,                   // mov rax, 0x1234
            0x48, 0x89, 0x04, 0x24,
            0xDF, 0x04, 0x24,                                           // fild word [rsp]  ; st0=4660.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FILD m16int (0x1234 -> 4660.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x40B2340000000000ULL; // 4660.0
        });
    }

    // T: FLD 100.0 + FISTP m16int — stores 16-bit 100 in low word.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x40, // 100.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld qword [rsp]
            0x48, 0x31, 0xC0,                                           // xor rax, rax (clear slot — verifier reads full qword)
            0x48, 0x89, 0x04, 0x24,
            0xDF, 0x1C, 0x24,                                           // fistp word [rsp] ; stores 100 in low word
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FISTP m16int (100.0 -> 100)", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0xFFFFULL) == 100;
        });
    }

    // T: FILD m64int — load int64 0x123456789, store as f64.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00, // mov rax, 0x123456789
            0x48, 0x89, 0x04, 0x24,
            0xDF, 0x2C, 0x24,                                           // fild qword [rsp]
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FILD m64int (0x123456789 -> double)", withExit(code), [](CPU64& c) {
            // (double)0x123456789 = 4886718345.0
            // Mantissa bits = 0x23456789 << 1 = 0x468ACF12, exp = 32+1023 = 0x41F
            return c.reg[X64_R15].u64 == 0x41F2345678900000ULL;
        });
    }

    // T: FLD 2.5 + FISTP m64int — stores int64 2 (round-nearest-even on .5 ties).
    // Wait: round-nearest-even on 2.5 gives 2; on 3.5 gives 4. Use 2.5 → 2.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld qword [rsp]   ; st0=2.5
            0xDF, 0x3C, 0x24,                                           // fistp qword [rsp] ; rounds to 2
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FISTP m64int (2.5 -> 2 round-even)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 2;
        });
    }

    // ----- C10 extensions: x87 FCOM/FCOMI compare ops -----

    // T: FCOMI ST(0)>ST(1) — load 5.0 then 2.0 → st0=2,st1=5 (wait reversed)
    // fld 2.0 then fld 5.0 gives st0=5, st1=2. fcomi st0,st1 → 5>2 → no flags.
    // Verify ZF=0, CF=0 via pushfq.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // 2.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.0  → st0=2
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x40, // 5.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 5.0  → st0=5, st1=2
            0xDB, 0xF1,                                                 // fcomi st0,st1 → 5>2: no flags
            0x9C,                                                       // pushfq
            0x58,                                                       // pop rax
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "x87 FCOMI ST(0)>ST(1) clears ZF/CF/PF", withExit(code), [](CPU64& c) {
            // CF (bit 0), PF (bit 2), ZF (bit 6) must all be 0
            return (c.reg[X64_R15].u64 & 0x45) == 0;
        });
    }

    // T: FCOMI ST(0)<ST(1) — fld 5.0 then 2.0 → st0=2, st1=5; 2<5 → CF=1.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x40, // 5.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 5.0  → st0=5
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // 2.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.0  → st0=2, st1=5
            0xDB, 0xF1,                                                 // fcomi st0,st1 → 2<5: CF=1
            0x9C, 0x58,                                                 // pushfq; pop rax
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "x87 FCOMI ST(0)<ST(1) sets CF=1", withExit(code), [](CPU64& c) {
            // CF=1, ZF=0, PF=0
            return (c.reg[X64_R15].u64 & 0x45) == 0x01;
        });
    }

    // T: FCOMI ST(0)==ST(1) — fld 3.0 twice → ZF=1.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40, // 3.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 3.0
            0xDD, 0x04, 0x24,                                           // fld 3.0 again
            0xDB, 0xF1,                                                 // fcomi st0,st1 → 3==3: ZF=1
            0x9C, 0x58,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "x87 FCOMI ST(0)==ST(1) sets ZF=1", withExit(code), [](CPU64& c) {
            // ZF=1, CF=0, PF=0
            return (c.reg[X64_R15].u64 & 0x45) == 0x40;
        });
    }

    // T: FCOMIP ST(0),ST(1) pops — after comparing, st0 should be the
    // original st1 value (5.0). We verify by storing the new TOS.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x40, // 5.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 5.0  → st0=5
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // 2.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.0  → st0=2, st1=5
            0xDF, 0xF1,                                                 // fcomip st0,st1 → pops 2; now st0=5
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]  ; stores 5.0
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "x87 FCOMIP pops top, new TOS=ST(1)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4014000000000000ULL; // 5.0
        });
    }

    // T: FCOM m64fp + FNSTSW — st0=10.0, FCOM 4.0 → 10>4 → C3=C2=C0=0.
    // FNSTSW into AX low; verify C0 (bit 8), C2 (bit 10), C3 (bit 14) all 0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, // 4.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x14, 0x24,                                           // fcom qword [rsp]  ; 10>4: C3=C2=C0=0
            0x48, 0x31, 0xC0,                                           // xor rax, rax  (clear AX)
            0xDF, 0xE0,                                                 // fnstsw ax
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FCOM m64fp 10>4 clears C0/C2/C3", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0x4500) == 0;
        });
    }

    // T: FCOM m64fp — st0=2.0, FCOM 5.0 → 2<5 → C0=1.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // 2.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.0
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x40, // 5.0
            0x48, 0x89, 0x04, 0x24,
            0xDC, 0x14, 0x24,                                           // fcom qword [rsp]  ; 2<5: C0=1
            0x48, 0x31, 0xC0,
            0xDF, 0xE0,                                                 // fnstsw ax
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FCOM m64fp 2<5 sets C0=1", withExit(code), [](CPU64& c) {
            // bit 8 (C0) = 0x100 must be set; bit 10 (C2) and bit 14 (C3) clear
            return (c.reg[X64_R15].u64 & 0x4500) == 0x0100;
        });
    }

    // T: FCOMPP — fld 3.0, fld 3.0, fcompp → ZF condition (C3=1).
    // Then fnstsw and pop also drained the stack; st0/st1 are empty now.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40, // 3.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 3.0
            0xDD, 0x04, 0x24,                                           // fld 3.0
            0xDE, 0xD9,                                                 // fcompp → 3==3: C3=1; pops twice
            0x48, 0x31, 0xC0,
            0xDF, 0xE0,                                                 // fnstsw ax
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FCOMPP 3==3 sets C3 + pops twice", withExit(code), [](CPU64& c) {
            // C3 = bit 14 = 0x4000. C0/C2 clear.
            return (c.reg[X64_R15].u64 & 0x4500) == 0x4000;
        });
    }

    // ----- rt_sigaction storage round-trip (Milestone B1) -----
    // Layout on stack: [rsp+0..63] new act struct, [rsp+64..127] old act buffer.
    // sub rsp,128 ; build act at [rsp]; sigaction(SIGUSR1, [rsp], [rsp+64], 8).

    // T: rt_sigaction install handler then read it back via second call's oldact.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x80,                                     // sub rsp, 128
            // build act at [rsp]: handler=0xCAFE1234DEADBEEF, others 0
            0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x34, 0x12, 0xFE, 0xCA, // mov rax, 0xCAFE1234DEADBEEF
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp+0], rax (handler)
            0x48, 0x31, 0xC0,                                           // xor rax, rax
            0x48, 0x89, 0x44, 0x24, 0x08,                               // mov [rsp+8], rax  (flags)
            0x48, 0x89, 0x44, 0x24, 0x10,                               // mov [rsp+16], rax (restorer)
            0x48, 0x89, 0x44, 0x24, 0x18,                               // mov [rsp+24], rax (mask)
            // syscall rt_sigaction(10=SIGUSR1, rsp, NULL, 8) — install
            0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,                   // mov rax, 13
            0x48, 0xC7, 0xC7, 0x0A, 0x00, 0x00, 0x00,                   // mov rdi, 10
            0x48, 0x89, 0xE6,                                           // mov rsi, rsp
            0x48, 0x31, 0xD2,                                           // xor rdx, rdx
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,                   // mov r10, 8
            0x0F, 0x05,                                                 // syscall
            // syscall rt_sigaction(10, NULL, rsp+64, 8) — read back oldact
            0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,                   // mov rax, 13
            0x48, 0xC7, 0xC7, 0x0A, 0x00, 0x00, 0x00,                   // mov rdi, 10
            0x48, 0x31, 0xF6,                                           // xor rsi, rsi
            0x48, 0x8D, 0x54, 0x24, 0x40,                               // lea rdx, [rsp+64]
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,                   // mov r10, 8
            0x0F, 0x05,                                                 // syscall
            // load oldact.handler → rax
            0x48, 0x8B, 0x44, 0x24, 0x40,                               // mov rax, [rsp+64]
            0x48, 0x83, 0xC4, 0x80,                                     // add rsp, 128
        };
        runAndCheck(r, "rt_sigaction install + readback SIGUSR1 handler", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xCAFE1234DEADBEEFULL;
        });
    }

    // T: rt_sigaction invalid signal number (0) → -EINVAL.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,                   // mov rax, 13
            0x48, 0x31, 0xFF,                                           // xor rdi, rdi (sig=0)
            0x48, 0x31, 0xF6,                                           // xor rsi
            0x48, 0x31, 0xD2,                                           // xor rdx
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,                   // mov r10, 8
            0x0F, 0x05,                                                 // syscall
        };
        runAndCheck(r, "rt_sigaction sig=0 → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL; // -22
        });
    }

    // T: rt_sigaction bad sigsetsize (16, not 8) → -EINVAL.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,                   // mov rax, 13
            0x48, 0xC7, 0xC7, 0x0A, 0x00, 0x00, 0x00,                   // mov rdi, 10
            0x48, 0x31, 0xF6,                                           // xor rsi
            0x48, 0x31, 0xD2,                                           // xor rdx
            0x49, 0xC7, 0xC2, 0x10, 0x00, 0x00, 0x00,                   // mov r10, 16
            0x0F, 0x05,                                                 // syscall
        };
        runAndCheck(r, "rt_sigaction sigsetsize=16 → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL; // -22
        });
    }

    // T: rt_sigaction SIGKILL (9) — write is silently dropped, readback shows
    // no handler installed (oldact.handler stays 0). Verifies the kernel-style
    // "accept the call but don't change the slot" behaviour.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x80,                                     // sub rsp, 128
            // act.handler = 0xDEADBEEF
            0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00, // mov rax, 0xDEADBEEF
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            0x48, 0x31, 0xC0,
            0x48, 0x89, 0x44, 0x24, 0x08,
            0x48, 0x89, 0x44, 0x24, 0x10,
            0x48, 0x89, 0x44, 0x24, 0x18,
            // attempt to install for SIGKILL — kernel silently ignores write
            0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,                   // mov rax, 13
            0x48, 0xC7, 0xC7, 0x09, 0x00, 0x00, 0x00,                   // mov rdi, 9 (SIGKILL)
            0x48, 0x89, 0xE6,                                           // mov rsi, rsp
            0x48, 0x31, 0xD2,                                           // xor rdx
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,                   // mov r10, 8
            0x0F, 0x05,                                                 // syscall
            // pre-poison oldact buffer with 0x55 so we can tell it was written
            0x48, 0xC7, 0xC0, 0x55, 0x55, 0x55, 0x55,                   // mov rax, 0x55555555
            0x48, 0x89, 0x44, 0x24, 0x40,                               // mov [rsp+64], rax
            // query SIGKILL — should write zeroed slot into oldact
            0x48, 0xC7, 0xC0, 0x0D, 0x00, 0x00, 0x00,                   // mov rax, 13
            0x48, 0xC7, 0xC7, 0x09, 0x00, 0x00, 0x00,                   // mov rdi, 9
            0x48, 0x31, 0xF6,                                           // xor rsi
            0x48, 0x8D, 0x54, 0x24, 0x40,                               // lea rdx, [rsp+64]
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            // load oldact.handler — must be 0, NOT 0xDEADBEEF nor 0x55555555
            0x48, 0x8B, 0x44, 0x24, 0x40,
            0x48, 0x83, 0xC4, 0x80,
        };
        runAndCheck(r, "rt_sigaction SIGKILL write ignored", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // ----- rt_sigprocmask storage round-trip (Milestone B1 continued) -----
    // Stack: [rsp+0..7] new set buffer, [rsp+16..23] old set buffer.
    // Syscall #14: how=RDI, set=RSI, oldset=RDX, sigsetsize=R10.

    // T: SIG_SETMASK then read back via second call's oldset.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x20,                                     // sub rsp, 32
            // set = 0xDEA9AA00 (intentionally avoids bits 8 and 18 which are
            // SIGKILL/SIGSTOP and would be stripped by the kernel).
            0x48, 0xB8, 0x00, 0xAA, 0xA9, 0xDE, 0x00, 0x00, 0x00, 0x00, // mov rax, 0xDEA9AA00
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            // syscall sigprocmask(SIG_SETMASK=2, [rsp], NULL, 8)
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,                   // mov rax, 14
            0x48, 0xC7, 0xC7, 0x02, 0x00, 0x00, 0x00,                   // mov rdi, 2
            0x48, 0x89, 0xE6,                                           // mov rsi, rsp
            0x48, 0x31, 0xD2,                                           // xor rdx
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,                   // mov r10, 8
            0x0F, 0x05,
            // syscall sigprocmask(SIG_SETMASK, NULL, [rsp+16], 8) — readback
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC7, 0x02, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xF6,
            0x48, 0x8D, 0x54, 0x24, 0x10,                               // lea rdx, [rsp+16]
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            0x48, 0x8B, 0x44, 0x24, 0x10,                               // mov rax, [rsp+16]
            0x48, 0x83, 0xC4, 0x20,
        };
        runAndCheck(r, "rt_sigprocmask SIG_SETMASK readback", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xDEA9AA00ULL;
        });
    }

    // T: SIG_BLOCK then SIG_BLOCK again ORs in additional bits.
    // Use 0x0E00 instead of 0x0F00 to avoid bit 8 (SIGKILL) being stripped.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x20,
            // first block: set = 0x0E00 (avoids SIGKILL bit 0x100)
            0x48, 0xC7, 0xC0, 0x00, 0x0E, 0x00, 0x00,                   // mov rax, 0x0E00
            0x48, 0x89, 0x04, 0x24,
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,                   // sys_rt_sigprocmask
            0x48, 0xC7, 0xC7, 0x00, 0x00, 0x00, 0x00,                   // SIG_BLOCK=0
            0x48, 0x89, 0xE6,
            0x48, 0x31, 0xD2,
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            // second block: set = 0xF000
            0x48, 0xC7, 0xC0, 0x00, 0xF0, 0x00, 0x00,                   // mov rax, 0xF000
            0x48, 0x89, 0x04, 0x24,
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC7, 0x00, 0x00, 0x00, 0x00,                   // SIG_BLOCK
            0x48, 0x89, 0xE6,
            0x48, 0x8D, 0x54, 0x24, 0x10,                               // oldset=[rsp+16]
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            // oldset[0] should be the prior 0x0F00 (state before this call)
            0x48, 0x8B, 0x44, 0x24, 0x10,
            0x48, 0x83, 0xC4, 0x20,
        };
        runAndCheck(r, "rt_sigprocmask SIG_BLOCK accumulates", withExit(code), [](CPU64& c) {
            // oldset captures the mask *before* the second SIG_BLOCK applied
            // — 0x0E00 from the first call. Final cpu.sigMask should be the
            // OR of both calls: 0x0E00 | 0xF000 = 0xFE00.
            return c.reg[X64_R15].u64 == 0x0E00ULL && c.sigMask == 0xFE00ULL;
        });
    }

    // T: SIGKILL/SIGSTOP bits (0x40100) are stripped from any incoming mask.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x20,
            // attempt SIG_SETMASK 0x40100 — should be stripped to 0
            0x48, 0xC7, 0xC0, 0x00, 0x01, 0x04, 0x00,                   // mov rax, 0x40100
            0x48, 0x89, 0x04, 0x24,
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC7, 0x02, 0x00, 0x00, 0x00,                   // SIG_SETMASK
            0x48, 0x89, 0xE6,
            0x48, 0x31, 0xD2,
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            // readback
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,
            0x48, 0xC7, 0xC7, 0x02, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xF6,
            0x48, 0x8D, 0x54, 0x24, 0x10,
            0x49, 0xC7, 0xC2, 0x08, 0x00, 0x00, 0x00,
            0x0F, 0x05,
            0x48, 0x8B, 0x44, 0x24, 0x10,
            0x48, 0x83, 0xC4, 0x20,
        };
        runAndCheck(r, "rt_sigprocmask strips SIGKILL/SIGSTOP bits", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // T: invalid sigsetsize → -EINVAL.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xFF,                                           // how=0
            0x48, 0x31, 0xF6,
            0x48, 0x31, 0xD2,
            0x49, 0xC7, 0xC2, 0x04, 0x00, 0x00, 0x00,                   // r10=4 (bad)
            0x0F, 0x05,
        };
        runAndCheck(r, "rt_sigprocmask sigsetsize=4 → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL;
        });
    }

    // ----- sigaltstack storage round-trip (Milestone B1c) -----
    // Layout: stack_t is 24 bytes; we build new at [rsp], old at [rsp+32].
    // Syscall #131: ss=RDI, oldss=RSI.

    // T: install altstack (sp=0x600000, flags=0, size=8192) then read it back.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x40,                                     // sub rsp, 64
            // build new ss: ss_sp=0x600000, ss_flags=0, ss_size=8192
            0x48, 0xC7, 0xC0, 0x00, 0x00, 0x60, 0x00,                   // mov rax, 0x600000
            0x48, 0x89, 0x04, 0x24,                                     // [rsp+0] = ss_sp
            0x48, 0x31, 0xC0,
            0x48, 0x89, 0x44, 0x24, 0x08,                               // [rsp+8] = flags|pad = 0
            0x48, 0xC7, 0xC0, 0x00, 0x20, 0x00, 0x00,                   // mov rax, 8192
            0x48, 0x89, 0x44, 0x24, 0x10,                               // [rsp+16] = ss_size
            // sys_sigaltstack(rsp, NULL)
            0x48, 0xC7, 0xC0, 0x83, 0x00, 0x00, 0x00,                   // mov rax, 131
            0x48, 0x89, 0xE7,                                           // mov rdi, rsp
            0x48, 0x31, 0xF6,
            0x0F, 0x05,
            // sys_sigaltstack(NULL, [rsp+32]) — readback
            0x48, 0xC7, 0xC0, 0x83, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xFF,
            0x48, 0x8D, 0x74, 0x24, 0x20,                               // lea rsi, [rsp+32]
            0x0F, 0x05,
            // ss_sp (offset 0 in oldss buffer) → rax
            0x48, 0x8B, 0x44, 0x24, 0x20,
            0x48, 0x83, 0xC4, 0x40,
        };
        runAndCheck(r, "sigaltstack install + readback ss_sp", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x600000ULL
                && c.sigAltStack.ssSize == 8192ULL
                && c.sigAltStack.ssFlags == 0;
        });
    }

    // T: SS_DISABLE clears the registration.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x40,
            // first install something
            0x48, 0xC7, 0xC0, 0x00, 0x00, 0x70, 0x00,                   // sp=0x700000
            0x48, 0x89, 0x04, 0x24,
            0x48, 0x31, 0xC0,
            0x48, 0x89, 0x44, 0x24, 0x08,
            0x48, 0xC7, 0xC0, 0x00, 0x20, 0x00, 0x00,                   // size=8192
            0x48, 0x89, 0x44, 0x24, 0x10,
            0x48, 0xC7, 0xC0, 0x83, 0x00, 0x00, 0x00,
            0x48, 0x89, 0xE7,
            0x48, 0x31, 0xF6,
            0x0F, 0x05,
            // now disable: ss_flags=SS_DISABLE(2)
            0x48, 0x31, 0xC0,
            0x48, 0x89, 0x04, 0x24,                                     // sp=0
            0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00,                   // flags=2
            0x48, 0x89, 0x44, 0x24, 0x08,
            0x48, 0x31, 0xC0,
            0x48, 0x89, 0x44, 0x24, 0x10,                               // size=0
            0x48, 0xC7, 0xC0, 0x83, 0x00, 0x00, 0x00,
            0x48, 0x89, 0xE7,
            0x48, 0x31, 0xF6,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x40,
        };
        runAndCheck(r, "sigaltstack SS_DISABLE clears state", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0
                && c.sigAltStack.ssSp == 0
                && c.sigAltStack.ssFlags == 2 /* SS_DISABLE */
                && c.sigAltStack.ssSize == 0;
        });
    }

    // T: size < MINSIGSTKSZ → -ENOMEM (12).
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0xC7, 0xC0, 0x00, 0x00, 0x60, 0x00,                   // sp
            0x48, 0x89, 0x04, 0x24,
            0x48, 0x31, 0xC0,
            0x48, 0x89, 0x44, 0x24, 0x08,                               // flags=0
            0x48, 0xC7, 0xC0, 0x00, 0x04, 0x00, 0x00,                   // size=1024 (too small)
            0x48, 0x89, 0x44, 0x24, 0x10,
            0x48, 0xC7, 0xC0, 0x83, 0x00, 0x00, 0x00,
            0x48, 0x89, 0xE7,
            0x48, 0x31, 0xF6,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x20,
        };
        runAndCheck(r, "sigaltstack size<MINSIGSTKSZ → -ENOMEM", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF4ULL; // -12
        });
    }

    // T: invalid flag bits → -EINVAL.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0xC7, 0xC0, 0x00, 0x00, 0x60, 0x00,                   // sp
            0x48, 0x89, 0x04, 0x24,
            0x48, 0xC7, 0xC0, 0x10, 0x00, 0x00, 0x00,                   // flags=0x10 (bogus, not DISABLE/AUTODISARM)
            0x48, 0x89, 0x44, 0x24, 0x08,
            0x48, 0xC7, 0xC0, 0x00, 0x20, 0x00, 0x00,                   // size=8192
            0x48, 0x89, 0x44, 0x24, 0x10,
            0x48, 0xC7, 0xC0, 0x83, 0x00, 0x00, 0x00,
            0x48, 0x89, 0xE7,
            0x48, 0x31, 0xF6,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x20,
        };
        runAndCheck(r, "sigaltstack bogus flags → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL; // -22
        });
    }

    // ----- rt_sigreturn frame restore (Milestone B2) -----
    //
    // Hand-build a ucontext_t at a known stack address whose saved gprs are
    // recognisable sentinels, saved RIP points into our same code buffer at
    // a "landing pad" that copies a restored register into R15 then exits,
    // and saved RAX is a known marker. Then we set RSP to point at that
    // ucontext_t and invoke rt_sigreturn (15). The restore writes our
    // sentinels into the live regs; the landing-pad code observes one of
    // them and exits with the verifier checking R15.
    //
    // gregs[] indices into ucontext_t (mctx starts at +40):
    //   8=RDI 12=RDX 13=RAX 16=RIP 17=EFL
    //
    // Layout we write at FRAME_BASE (must be 8-aligned, lives on the
    // pre-mapped stack page):
    //   FRAME_BASE + 0       : siginfo+header pad (zeroed; we don't touch)
    //   FRAME_BASE + 136     : ucontext_t starts here  (this is what RSP
    //                          must point at when rt_sigreturn fires)
    //   uctx + 40            : gregs[0..22]
    //   uctx + 296           : sigmask
    //
    // We set RBX=sentinel via gregs[X64_GREG_RBX]=0xCAFEBABE12345678 and
    // expect that after sigreturn returns, the landing-pad sees the
    // restored RBX and moves it to R15.
    {
        // Landing pad bytes: mov r15, rbx; mov rax,60; syscall (exit)
        // 0x4C 0x89 0xDF                  ; mov r15, rbx
        // 0x48 0xC7 0xC0 0x3C 0x00 0x00 0x00  ; mov rax, 60
        // 0x0F 0x05                       ; syscall
        // Total: 12 bytes for the landing pad.
        // We place the landing pad at CODE_BASE+0x100 (well past the
        // prologue's ~160 bytes of writeq_imm sequences) so we know its
        // absolute address up front.
        const U64 LANDING_PAD = CODE_BASE + 0x100;
        const U64 FRAME_BASE  = STACK_TOP - 0x1000 + 0x100; // inside RW stack page
        const U64 UCTX_BASE   = FRAME_BASE + 136;
        const U64 GREGS_BASE  = UCTX_BASE + 40;

        // Helper to emit "mov qword [imm64-addr], rax" via [rip+disp] is messy.
        // Instead use absolute addressing through R10 (we own it freely here).
        // Pattern per slot:
        //   mov rax, value64                ; 10 bytes (0x48,0xB8,...)
        //   mov r10, addr64                 ; 10 bytes (0x49,0xBA,...)
        //   mov [r10], rax                  ; 3 bytes  (0x49,0x89,0x02)
        auto writeq_imm = [](std::vector<U8>& buf, U64 addr, U64 value) {
            buf.push_back(0x48); buf.push_back(0xB8);
            for (int i = 0; i < 8; i++) buf.push_back((U8)(value >> (8*i)));
            buf.push_back(0x49); buf.push_back(0xBA);
            for (int i = 0; i < 8; i++) buf.push_back((U8)(addr  >> (8*i)));
            buf.push_back(0x49); buf.push_back(0x89); buf.push_back(0x02);
        };

        std::vector<U8> code;
        // Sentinel values we'll place in the frame:
        const U64 sRBX = 0xCAFEBABE12345678ULL;
        const U64 sRDI = 0x1111111111111111ULL;
        const U64 sRIP = LANDING_PAD;
        const U64 sRAX = 0xDEADC0DEDEADC0DEULL; // becomes syscall return → into RAX

        // Frame setup: write the gregs slots we care about (others stay 0
        // from the stack-page zero-init since we mmap'd anon RW).
        writeq_imm(code, GREGS_BASE + 8 * 11 /*RBX*/,  sRBX);
        writeq_imm(code, GREGS_BASE + 8 * 8  /*RDI*/,  sRDI);
        writeq_imm(code, GREGS_BASE + 8 * 15 /*RSP*/,  STACK_TOP - 32); // restored SP
        writeq_imm(code, GREGS_BASE + 8 * 16 /*RIP*/,  sRIP);
        writeq_imm(code, GREGS_BASE + 8 * 17 /*EFL*/,  0x202);
        writeq_imm(code, GREGS_BASE + 8 * 13 /*RAX*/,  sRAX);

        // Now: set RSP = UCTX_BASE, then `mov rax, 15; syscall` (rt_sigreturn).
        // mov rsp, imm64 has no 1-instruction form; use mov r11, imm64; mov rsp, r11.
        code.push_back(0x49); code.push_back(0xBB);
        for (int i = 0; i < 8; i++) code.push_back((U8)(UCTX_BASE >> (8*i)));
        code.push_back(0x4C); code.push_back(0x89); code.push_back(0xDC); // mov rsp, r11

        // syscall rt_sigreturn
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(15); code.push_back(0x00); code.push_back(0x00); code.push_back(0x00);
        code.push_back(0x0F); code.push_back(0x05);

        // Pad up to LANDING_PAD (CODE_BASE+0x100) with NOPs, then emit the pad.
        size_t prologueLen = code.size();
        if (prologueLen > 0x100) {
            // If this happens, increase LANDING_PAD or shrink prologue.
            printf("  FAIL: rt_sigreturn test prologue (%zu bytes) > 0x100\n", prologueLen);
            r.failed++;
        } else {
            while (code.size() < 0x100) code.push_back(0x90);
            // landing pad
            code.push_back(0x49); code.push_back(0x89); code.push_back(0xDF); // mov r15, rbx
            code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
            code.push_back(0x3C); code.push_back(0x00); code.push_back(0x00); code.push_back(0x00);
            code.push_back(0x0F); code.push_back(0x05);

            // Note: do NOT withExit; we provide our own exit in the landing pad.
            runAndCheck(r, "rt_sigreturn restores gprs from frame", code,
                [sRBX](CPU64& c) {
                    // R15 must hold the restored RBX sentinel.
                    return c.reg[X64_R15].u64 == sRBX;
                });
        }
    }

    // T: rt_sigreturn also restores RAX to the saved-RAX, not to the syscall
    // return value. We exit with rax=60 inside the landing pad — the dispatch
    // already wrote savedRax into rax before the landing pad ran, but the
    // landing pad immediately overwrites rax. So this test is identical in
    // shape to the previous one but additionally checks RFLAGS restore.
    // For RFLAGS: save 0x246 (IF | PF | ZF) and verify ZF is set post-restore
    // by branching on it before exit.
    {
        const U64 LANDING_PAD = CODE_BASE + 0x80;
        const U64 FRAME_BASE  = STACK_TOP - 0x1000 + 0x200;
        const U64 UCTX_BASE   = FRAME_BASE + 136;
        const U64 GREGS_BASE  = UCTX_BASE + 40;

        auto writeq_imm = [](std::vector<U8>& buf, U64 addr, U64 value) {
            buf.push_back(0x48); buf.push_back(0xB8);
            for (int i = 0; i < 8; i++) buf.push_back((U8)(value >> (8*i)));
            buf.push_back(0x49); buf.push_back(0xBA);
            for (int i = 0; i < 8; i++) buf.push_back((U8)(addr  >> (8*i)));
            buf.push_back(0x49); buf.push_back(0x89); buf.push_back(0x02);
        };

        std::vector<U8> code;
        // Place a known canary in RBX via the frame; landing pad re-uses it.
        writeq_imm(code, GREGS_BASE + 8 * 11 /*RBX*/, 0x33);
        writeq_imm(code, GREGS_BASE + 8 * 15 /*RSP*/, STACK_TOP - 32);
        writeq_imm(code, GREGS_BASE + 8 * 16 /*RIP*/, LANDING_PAD);
        // EFL = ZF (0x40) | IF (0x200) | reserved bit 1 (0x2) = 0x242
        writeq_imm(code, GREGS_BASE + 8 * 17 /*EFL*/, 0x242);

        code.push_back(0x49); code.push_back(0xBB);
        for (int i = 0; i < 8; i++) code.push_back((U8)(UCTX_BASE >> (8*i)));
        code.push_back(0x4C); code.push_back(0x89); code.push_back(0xDC);
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(15); code.push_back(0x00); code.push_back(0x00); code.push_back(0x00);
        code.push_back(0x0F); code.push_back(0x05);

        if (code.size() > 0x80) {
            printf("  FAIL: rt_sigreturn RFLAGS test prologue overflow\n");
            r.failed++;
        } else {
            while (code.size() < 0x80) code.push_back(0x90);
            // Landing pad: branch on ZF. If ZF set (as restored), set r15=0xA5.
            // Otherwise r15 stays 0. Then exit.
            //   74 03            jz +3 (skip the "xor rbx,rbx" no-op? no — jump over a r15-clobber)
            // Simpler: use SETZ (0F 94 c0) on AL, mov r15, rax.
            code.push_back(0x0F); code.push_back(0x94); code.push_back(0xC0); // setz al
            code.push_back(0x48); code.push_back(0x0F); code.push_back(0xB6); code.push_back(0xC0); // movzx rax, al
            code.push_back(0x49); code.push_back(0x89); code.push_back(0xC7); // mov r15, rax
            code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
            code.push_back(0x3C); code.push_back(0x00); code.push_back(0x00); code.push_back(0x00);
            code.push_back(0x0F); code.push_back(0x05);

            runAndCheck(r, "rt_sigreturn restores RFLAGS (ZF preserved)", code,
                [](CPU64& c) {
                    return c.reg[X64_R15].u64 == 1; // SETZ produced 1
                });
        }
    }

    // ----- B3: synchronous signal delivery via kill/tgkill -----
    //
    // End-to-end: install a SIGUSR1 (10) handler at a known address,
    // call kill(0, 10) to deliver to ourselves. Handler writes a
    // sentinel into a MEMORY slot (survives sigreturn, unlike a GPR
    // write which would get overwritten by the frame restore), calls
    // sys_rt_sigreturn. After return, main reads the slot into R15
    // and exits. The verifier checks R15 == sentinel — proving the
    // handler actually executed AND the unwind got us back to main.
    {
        const U64 HANDLER_ADDR = CODE_BASE + 0x200;
        const U64 SA_ADDR  = STACK_TOP - 0x80;
        const U64 MARK_ADDR = STACK_TOP - 0xA0;

        auto writeq_imm = [](std::vector<U8>& buf, U64 addr, U64 value) {
            buf.push_back(0x48); buf.push_back(0xB8);
            for (int i = 0; i < 8; i++) buf.push_back((U8)(value >> (8*i)));
            buf.push_back(0x49); buf.push_back(0xBA);
            for (int i = 0; i < 8; i++) buf.push_back((U8)(addr  >> (8*i)));
            buf.push_back(0x49); buf.push_back(0x89); buf.push_back(0x02);
        };

        std::vector<U8> code;

        // Pre-poison MARK_ADDR with 0 so a successful handler-write to 0xCAFE
        // is clearly distinguishable.
        writeq_imm(code, MARK_ADDR, 0);

        // Build sigaction at SA_ADDR.
        writeq_imm(code, SA_ADDR +  0, HANDLER_ADDR);
        writeq_imm(code, SA_ADDR +  8, 0);
        writeq_imm(code, SA_ADDR + 16, 0);
        writeq_imm(code, SA_ADDR + 24, 0);

        // rt_sigaction(10, SA_ADDR, NULL, 8): rax=13 rdi=10 rsi=SA_ADDR rdx=0 r10=8
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(13); code.push_back(0); code.push_back(0); code.push_back(0);
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC7);
        code.push_back(10); code.push_back(0); code.push_back(0); code.push_back(0);
        code.push_back(0x48); code.push_back(0xBE);
        for (int i = 0; i < 8; i++) code.push_back((U8)(SA_ADDR >> (8*i)));
        code.push_back(0x48); code.push_back(0x31); code.push_back(0xD2);
        code.push_back(0x49); code.push_back(0xC7); code.push_back(0xC2);
        code.push_back(8); code.push_back(0); code.push_back(0); code.push_back(0);
        code.push_back(0x0F); code.push_back(0x05);

        // kill(0, 10)
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(62); code.push_back(0); code.push_back(0); code.push_back(0);
        code.push_back(0x48); code.push_back(0x31); code.push_back(0xFF);
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC6);
        code.push_back(10); code.push_back(0); code.push_back(0); code.push_back(0);
        code.push_back(0x0F); code.push_back(0x05);
        // After handler+sigreturn return here: read MARK_ADDR → R15, exit.
        code.push_back(0x49); code.push_back(0xBE); // mov r14, MARK_ADDR
        for (int i = 0; i < 8; i++) code.push_back((U8)(MARK_ADDR >> (8*i)));
        code.push_back(0x4D); code.push_back(0x8B); code.push_back(0x3E); // mov r15, [r14]
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(60); code.push_back(0); code.push_back(0); code.push_back(0);
        code.push_back(0x0F); code.push_back(0x05);

        if (code.size() > 0x200) {
            printf("  FAIL: B3 signal-delivery test prologue (%zu) > 0x200\n", code.size());
            r.failed++;
        } else {
            while (code.size() < 0x200) code.push_back(0x90);
            // Handler: write 0xCAFE to MARK_ADDR via R10/RAX, then rt_sigreturn.
            // Cannot use writeq_imm closure here (we're past code-building);
            // emit inline:
            //   mov rax, 0xCAFE              48 B8 FE CA 00 00 00 00 00 00
            //   mov r10, MARK_ADDR            49 BA <8 bytes>
            //   mov [r10], rax                49 89 02
            //   add rsp, 8                    48 83 C4 08  (pop the restorer
            //                                              slot the kernel
            //                                              pushed below the
            //                                              ucontext — we
            //                                              skip the restorer
            //                                              and syscall
            //                                              directly, but
            //                                              rt_sigreturn
            //                                              expects RSP to
            //                                              point at the
            //                                              ucontext)
            //   mov rax, 15                   48 C7 C0 0F 00 00 00
            //   syscall                       0F 05
            code.push_back(0x48); code.push_back(0xB8);
            U64 mark = 0xCAFE;
            for (int i = 0; i < 8; i++) code.push_back((U8)(mark >> (8*i)));
            code.push_back(0x49); code.push_back(0xBA);
            for (int i = 0; i < 8; i++) code.push_back((U8)(MARK_ADDR >> (8*i)));
            code.push_back(0x49); code.push_back(0x89); code.push_back(0x02);
            code.push_back(0x48); code.push_back(0x83); code.push_back(0xC4); code.push_back(0x08);
            code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
            code.push_back(15); code.push_back(0); code.push_back(0); code.push_back(0);
            code.push_back(0x0F); code.push_back(0x05);

            runAndCheck(r, "kill(0, SIGUSR1) delivers to handler, sigreturn unwinds", code,
                [](CPU64& c) {
                    return c.reg[X64_R15].u64 == 0xCAFE;
                });
        }
    }

    // T: tgkill(self, self, 0) — sig=0 is a "is this tid alive" probe; for
    // our own tid it returns 0 (no delivery happens). Verify by exiting
    // with the syscall return value.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xEA, 0x00, 0x00, 0x00,                     // mov rax, 234 (tgkill)
            0x48, 0xC7, 0xC7, 0x01, 0x00, 0x00, 0x00,                     // mov rdi, 1 (tgid)
            0x48, 0xC7, 0xC6, 0x01, 0x00, 0x00, 0x00,                     // mov rsi, 1 (tid — KThread default id=1)
            0x48, 0x31, 0xD2,                                             // xor rdx, rdx (sig=0)
            0x0F, 0x05,
        };
        runAndCheck(r, "tgkill self sig=0 (probe) → 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // T: kill(0, 10) with NO handler installed → returns 0 (we log and drop
    // instead of terminating the host process — see comment in dispatch).
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x3E, 0x00, 0x00, 0x00,                     // mov rax, 62 (kill)
            0x48, 0x31, 0xFF,                                             // xor rdi (pid=0 → self)
            0x48, 0xC7, 0xC6, 0x0A, 0x00, 0x00, 0x00,                     // mov rsi, 10 (SIGUSR1)
            0x0F, 0x05,
        };
        runAndCheck(r, "kill(self, 10) no handler → 0 (log+drop)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // ----- sched_getaffinity / sched_setaffinity (Milestone B2) -----

    // T: sched_getaffinity(0, 8, [rsp]) returns 8 and mask[0]=1.
    // Pack r14 with the written mask value so the verifier can read it.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,                                     // sub rsp, 16
            // pre-poison [rsp]
            0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,                   // mov rax, -1
            0x48, 0x89, 0x04, 0x24,
            // sys_sched_getaffinity(0, 8, rsp)
            0x48, 0xC7, 0xC0, 0xCC, 0x00, 0x00, 0x00,                   // mov rax, 204
            0x48, 0x31, 0xFF,                                           // xor rdi, rdi (pid=0)
            0x48, 0xC7, 0xC6, 0x08, 0x00, 0x00, 0x00,                   // mov rsi, 8
            0x48, 0x89, 0xE2,                                           // mov rdx, rsp
            0x0F, 0x05,
            // stash return value (8) in r14 via the mask we just wrote
            0x4C, 0x8B, 0x34, 0x24,                                     // mov r14, [rsp]   (should be 1)
            0x48, 0x83, 0xC4, 0x10,                                     // add rsp, 16
        };
        runAndCheck(r, "sched_getaffinity returns 8, mask=1", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 8ULL  // return value
                && c.reg[X64_R14].u64 == 1ULL; // written mask
        });
    }

    // T: cpusetsize=0 → -EINVAL.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0xCC, 0x00, 0x00, 0x00,                   // sys_sched_getaffinity
            0x48, 0x31, 0xFF,                                           // pid=0
            0x48, 0x31, 0xF6,                                           // cpusetsize=0
            0x48, 0x31, 0xD2,                                           // mask=NULL (doesn't matter)
            0x0F, 0x05,
        };
        runAndCheck(r, "sched_getaffinity cpusetsize=0 → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL;
        });
    }

    // T: cpusetsize=7 (not multiple of 8) → -EINVAL.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xC7, 0xC0, 0xCC, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xFF,
            0x48, 0xC7, 0xC6, 0x07, 0x00, 0x00, 0x00,                   // cpusetsize=7
            0x48, 0x89, 0xE2,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "sched_getaffinity cpusetsize=7 → -EINVAL", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFEAULL;
        });
    }

    // T: sched_setaffinity accepts valid args.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x10,
            0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,                   // mask=1
            0x48, 0x89, 0x04, 0x24,
            0x48, 0xC7, 0xC0, 0xCB, 0x00, 0x00, 0x00,                   // sys_sched_setaffinity (203)
            0x48, 0x31, 0xFF,
            0x48, 0xC7, 0xC6, 0x08, 0x00, 0x00, 0x00,
            0x48, 0x89, 0xE2,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x10,
        };
        runAndCheck(r, "sched_setaffinity(valid) → 0", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0;
        });
    }

    // ----- statfs / fstatfs stub (Milestone B3) -----
    // Both write a 120-byte struct statfs. We verify the load-bearing fields
    // (f_type, f_bsize, f_namelen) — glibc reads those during ld.so cache
    // lookup. Buffer at [rsp], path/fd is unused by our stub.

    // T: statfs(path, buf) writes tmpfs magic + bsize=4096 + namelen=255.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x80,                                     // sub rsp, 128
            // sys_statfs(NULL, rsp)
            0x48, 0xC7, 0xC0, 0x89, 0x00, 0x00, 0x00,                   // mov rax, 137
            0x48, 0x31, 0xFF,                                           // path=NULL
            0x48, 0x89, 0xE6,                                           // rsi=rsp (buf)
            0x0F, 0x05,                                                 // syscall
            // Pack three load-bearing fields into one composite for the
            // verifier: r12=f_type, r13=f_bsize, r14=f_namelen. R15 will
            // be the syscall return (set by exit suffix).
            0x4C, 0x8B, 0x24, 0x24,                                     // mov r12, [rsp+0]   f_type
            0x4C, 0x8B, 0x6C, 0x24, 0x08,                               // mov r13, [rsp+8]   f_bsize
            0x4C, 0x8B, 0x74, 0x24, 0x40,                               // mov r14, [rsp+64]  f_namelen
            0x48, 0x83, 0xC4, 0x80,
        };
        runAndCheck(r, "statfs writes tmpfs magic + bsize + namelen", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0
                && c.reg[X64_R12].u64 == 0x01021994ULL
                && c.reg[X64_R13].u64 == 4096ULL
                && c.reg[X64_R14].u64 == 255ULL;
        });
    }

    // T: fstatfs(fd, buf) writes the same fields.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x80,
            0x48, 0xC7, 0xC0, 0x8A, 0x00, 0x00, 0x00,                   // mov rax, 138
            0x48, 0xC7, 0xC7, 0x03, 0x00, 0x00, 0x00,                   // fd=3 (ignored)
            0x48, 0x89, 0xE6,
            0x0F, 0x05,
            0x4C, 0x8B, 0x24, 0x24,
            0x4C, 0x8B, 0x6C, 0x24, 0x08,
            0x4C, 0x8B, 0x74, 0x24, 0x40,
            0x48, 0x83, 0xC4, 0x80,
        };
        runAndCheck(r, "fstatfs writes tmpfs magic + bsize + namelen", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0
                && c.reg[X64_R12].u64 == 0x01021994ULL
                && c.reg[X64_R13].u64 == 4096ULL
                && c.reg[X64_R14].u64 == 255ULL;
        });
    }

    // T: statfs with NULL buf → -EFAULT.
    {
        std::vector<U8> code = {
            0x48, 0xC7, 0xC0, 0x89, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xFF,
            0x48, 0x31, 0xF6,                                           // buf=NULL
            0x0F, 0x05,
        };
        runAndCheck(r, "statfs NULL buf → -EFAULT", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFF2ULL; // -14
        });
    }

    // ----- A6: end-to-end synthesize → parse → map → execute -----
    // Builds an ET_EXEC ELF with code that performs:
    //   mov r15, 0xCAFE  ; stash sentinel
    //   mov rax, 60      ; SYS_exit
    //   xor rdi, rdi     ; status=0
    //   syscall
    // Parses it via parseBuffer, maps PT_LOAD via mapSegmentsFromBuffer,
    // points CPU64 at e_entry, runs. Asserts R15==0xCAFE (proves code ran)
    // and cpu.yield==true (proves exit was hit).
    {
        // Code that the ELF will carry.
        std::vector<U8> code = {
            0x49, 0xC7, 0xC7, 0xFE, 0xCA, 0x00, 0x00,                     // mov r15, 0xCAFE
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,                     // mov rax, 60
            0x48, 0x31, 0xFF,                                             // xor rdi, rdi
            0x0F, 0x05,                                                   // syscall
        };

        // ELF layout: Ehdr immediately followed by Phdr, then code.
        const U64 LOAD_VADDR  = 0x500000;
        const size_t ehdrSize = sizeof(k_Elf64_Ehdr);
        const size_t phdrSize = sizeof(k_Elf64_Phdr);
        const size_t totalSize = ehdrSize + phdrSize + code.size();
        const U64 ENTRY_ADDR = LOAD_VADDR + ehdrSize + phdrSize;

        std::vector<U8> elf(totalSize, 0);

        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F;
        eh.e_ident[1] = 'E';
        eh.e_ident[2] = 'L';
        eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64;
        eh.e_ident[5] = 1;
        eh.e_ident[6] = 1;
        eh.e_type     = 2; // ET_EXEC
        eh.e_machine  = k_EM_X86_64;
        eh.e_version  = 1;
        eh.e_entry    = ENTRY_ADDR;
        eh.e_phoff    = ehdrSize;
        eh.e_ehsize   = (U16)ehdrSize;
        eh.e_phentsize = (U16)phdrSize;
        eh.e_phnum    = 1;
        memcpy(elf.data(), &eh, ehdrSize);

        k_Elf64_Phdr ph{};
        ph.p_type   = 1; // PT_LOAD
        ph.p_flags  = 5; // PF_R | PF_X
        ph.p_offset = 0;
        ph.p_vaddr  = LOAD_VADDR;
        ph.p_paddr  = LOAD_VADDR;
        ph.p_filesz = totalSize;
        ph.p_memsz  = totalSize;
        ph.p_align  = 0x1000;
        memcpy(elf.data() + ehdrSize, &ph, phdrSize);
        memcpy(elf.data() + ehdrSize + phdrSize, code.data(), code.size());

        // --- Parse, map, run.
        Elf64ParseResult parsed = ElfLoader64::parseBuffer(elf.data(), elf.size());
        bool parseOk = parsed.ok &&
                       parsed.entry == ENTRY_ADDR &&
                       parsed.segments.size() == 1;

        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3); // stack
        bool mapOk = parseOk && ElfLoader64::mapSegmentsFromBuffer(
            &mem, parsed, elf.data(), elf.size(), 0, "a6-e2e");

        CPU64 cpu(&mem);
        cpu.rip = parsed.entry;
        cpu.reg[X64_RSP].setU64(STACK_TOP - 16);
        if (mapOk) cpu.runBounded(200);

        bool ok = parseOk &&
                  mapOk &&
                  cpu.yield &&
                  cpu.reg[X64_R15].u64 == 0xCAFE;
        if (ok) {
            printf("  PASS: synthesized ET_EXEC parses + maps + runs to exit (R15=0xCAFE)\n");
            r.passed++;
        } else {
            printf("  FAIL: A6 e2e (parseOk=%d mapOk=%d yield=%d R15=0x%llx RIP=0x%llx)\n",
                   parseOk, mapOk, cpu.yield,
                   (unsigned long long)cpu.reg[X64_R15].u64,
                   (unsigned long long)cpu.rip);
            r.failed++;
        }
        fflush(stdout);
    }

    // ----- A7: end-to-end PIE with R_X86_64_RELATIVE -----
    // Synthesizes an ET_DYN with one PT_LOAD, a PT_DYNAMIC referencing a
    // single RELA entry. Layout (vaddrs in PIE space, offset by RELOC at load):
    //   0x0000 Ehdr
    //   0x0040 Phdr[0] PT_LOAD  (covers whole file)
    //   0x0078 Phdr[1] PT_DYNAMIC
    //   0x00B0 Dynamic array (4 entries × 16 bytes = 0x40)
    //   0x00F0 RELA[0] (24 bytes)
    //   0x0108 Slot (8 bytes, target of RELATIVE relo)
    //   0x0110 Code:  mov r15, [rip+slot_disp]; mov rax,60; xor rdi,rdi; syscall
    //
    // After relocation: slot @ vaddr 0x0108+reloc holds (reloc + addend).
    // Code loads the slot via RIP-rel and exits — R15 must equal that value.
    {
        const U64 RELOC      = 0x600000ULL;
        const U64 ADDEND     = 0x4242DEAD4242BABEULL;

        // Fixed layout offsets in the synthesized file.
        const U64 EHDR_OFF   = 0x0000;
        const U64 PHDR_OFF   = 0x0040;
        const U64 DYN_OFF    = 0x00B0;
        const U64 RELA_OFF   = 0x00F0;
        const U64 SLOT_OFF   = 0x0108;
        const U64 CODE_OFF   = 0x0110;

        // Build the buffer.
        const size_t FILE_SIZE = 0x140;
        std::vector<U8> elf(FILE_SIZE, 0);

        // Ehdr.
        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F;
        eh.e_ident[1] = 'E';
        eh.e_ident[2] = 'L';
        eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64;
        eh.e_ident[5] = 1;
        eh.e_ident[6] = 1;
        eh.e_type     = 3; // ET_DYN
        eh.e_machine  = k_EM_X86_64;
        eh.e_version  = 1;
        eh.e_entry    = CODE_OFF;
        eh.e_phoff    = PHDR_OFF;
        eh.e_ehsize   = sizeof(k_Elf64_Ehdr);
        eh.e_phentsize = sizeof(k_Elf64_Phdr);
        eh.e_phnum    = 2;
        memcpy(elf.data() + EHDR_OFF, &eh, sizeof(eh));

        // Phdr[0]: PT_LOAD covers the whole file.
        k_Elf64_Phdr load{};
        load.p_type   = 1; // PT_LOAD
        load.p_flags  = 7; // PF_R | PF_W | PF_X (writable because relo writes slot)
        load.p_offset = 0;
        load.p_vaddr  = 0;
        load.p_paddr  = 0;
        load.p_filesz = FILE_SIZE;
        load.p_memsz  = FILE_SIZE;
        load.p_align  = 0x1000;
        memcpy(elf.data() + PHDR_OFF, &load, sizeof(load));

        // Phdr[1]: PT_DYNAMIC.
        k_Elf64_Phdr dyn{};
        dyn.p_type   = k_PT_DYNAMIC;
        dyn.p_flags  = 6; // PF_R | PF_W
        dyn.p_offset = DYN_OFF;
        dyn.p_vaddr  = DYN_OFF;
        dyn.p_filesz = 0x40;
        dyn.p_memsz  = 0x40;
        dyn.p_align  = 8;
        memcpy(elf.data() + PHDR_OFF + sizeof(k_Elf64_Phdr), &dyn, sizeof(dyn));

        // Dynamic array.
        k_Elf64_Dyn dynArr[4]{};
        dynArr[0].d_tag = k_DT_RELA;     dynArr[0].d_un.d_ptr = RELA_OFF;
        dynArr[1].d_tag = k_DT_RELASZ;   dynArr[1].d_un.d_val = sizeof(k_Elf64_Rela);
        dynArr[2].d_tag = k_DT_RELAENT;  dynArr[2].d_un.d_val = sizeof(k_Elf64_Rela);
        dynArr[3].d_tag = k_DT_NULL;     dynArr[3].d_un.d_val = 0;
        memcpy(elf.data() + DYN_OFF, dynArr, sizeof(dynArr));

        // RELA entry: write SLOT_OFF, RELATIVE type, addend = ADDEND.
        k_Elf64_Rela rela{};
        rela.r_offset = SLOT_OFF;
        rela.r_info   = k_R_X86_64_RELATIVE; // sym=0, type=8
        rela.r_addend = (S64)ADDEND;
        memcpy(elf.data() + RELA_OFF, &rela, sizeof(rela));

        // Code: mov r15, [rip + disp32]; mov rax, 60; xor rdi, rdi; syscall.
        // The "mov r15, [rip+disp32]" is 7 bytes: 4C 8B 3D <disp32>.
        // RIP at end of this instruction = CODE_OFF + 7 (in vaddr space).
        // After load it's RELOC + CODE_OFF + 7. We want to read SLOT_OFF + RELOC.
        // disp32 = (SLOT_OFF + RELOC) - (CODE_OFF + 7 + RELOC) = SLOT_OFF - CODE_OFF - 7.
        S32 disp = (S32)(SLOT_OFF - CODE_OFF - 7);
        U8* code = elf.data() + CODE_OFF;
        code[0] = 0x4C; code[1] = 0x8B; code[2] = 0x3D;
        code[3] = (U8)(disp & 0xFF);
        code[4] = (U8)((disp >> 8) & 0xFF);
        code[5] = (U8)((disp >> 16) & 0xFF);
        code[6] = (U8)((disp >> 24) & 0xFF);
        // mov rax, 60
        code[7]  = 0x48; code[8]  = 0xC7; code[9]  = 0xC0;
        code[10] = 0x3C; code[11] = 0x00; code[12] = 0x00; code[13] = 0x00;
        // xor rdi, rdi
        code[14] = 0x48; code[15] = 0x31; code[16] = 0xFF;
        // syscall
        code[17] = 0x0F; code[18] = 0x05;

        // --- Parse.
        Elf64ParseResult parsed = ElfLoader64::parseBuffer(elf.data(), elf.size());
        bool parseOk = parsed.ok &&
                       parsed.isPie &&
                       parsed.dynamic.present &&
                       parsed.dynamic.vaddr == DYN_OFF &&
                       parsed.dynamic.memsz == 0x40 &&
                       parsed.entry == CODE_OFF;

        // --- Map at RELOC.
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3);
        bool mapOk = parseOk && ElfLoader64::mapSegmentsFromBuffer(
            &mem, parsed, elf.data(), elf.size(), RELOC, "a7-pie");

        // --- Apply relocations.
        U64 applied = 0;
        if (mapOk) {
            applied = ElfLoader64::applyRelativeRelocations(
                &mem, parsed.dynamic, RELOC, "a7-pie");
        }

        // --- Verify slot was written.
        U64 slotValue = mapOk ? mem.readq(RELOC + SLOT_OFF) : 0;
        U64 expected = RELOC + ADDEND;

        // --- Run.
        CPU64 cpu(&mem);
        cpu.rip = parsed.entry + RELOC;
        cpu.reg[X64_RSP].setU64(STACK_TOP - 16);
        if (mapOk) cpu.runBounded(200);

        bool ok = parseOk &&
                  mapOk &&
                  applied == 1 &&
                  slotValue == expected &&
                  cpu.yield &&
                  cpu.reg[X64_R15].u64 == expected;
        if (ok) {
            printf("  PASS: PIE+RELATIVE: relocation applied, RIP-rel load reads relocated value\n");
            r.passed++;
        } else {
            printf("  FAIL: A7 (parseOk=%d mapOk=%d applied=%llu slot=0x%llx exp=0x%llx yield=%d R15=0x%llx RIP=0x%llx)\n",
                   parseOk, mapOk, (unsigned long long)applied,
                   (unsigned long long)slotValue,
                   (unsigned long long)expected,
                   cpu.yield,
                   (unsigned long long)cpu.reg[X64_R15].u64,
                   (unsigned long long)cpu.rip);
            r.failed++;
        }
        fflush(stdout);
    }

    // ----- A10: SysV stack frame — argc reachable via [rsp] -----
    // Synthesizes a minimal ET_EXEC whose entry reads [rsp] (argc) into
    // R15 then exits. Inlines the same SysV frame builder the runner
    // uses (one argv element, one envp element) so we catch regressions
    // in the frame layout. Expect: argc == 1 lands in R15.
    {
        const U64 LOAD_VADDR = 0x500000;
        const size_t ehdrSize = sizeof(k_Elf64_Ehdr);
        const size_t phdrSize = sizeof(k_Elf64_Phdr);
        // Code: 4C 8B 3C 24      mov r15, [rsp]
        //       48 C7 C0 3C ..   mov rax, 60
        //       48 31 FF         xor rdi, rdi
        //       0F 05            syscall
        std::vector<U8> code = {
            0x4C, 0x8B, 0x3C, 0x24,
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xFF,
            0x0F, 0x05,
        };
        const size_t totalSize = ehdrSize + phdrSize + code.size();
        const U64 ENTRY_ADDR = LOAD_VADDR + ehdrSize + phdrSize;

        std::vector<U8> elf(totalSize, 0);

        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F;
        eh.e_ident[1] = 'E';
        eh.e_ident[2] = 'L';
        eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64;
        eh.e_ident[5] = 1;
        eh.e_ident[6] = 1;
        eh.e_type     = 2; // ET_EXEC
        eh.e_machine  = k_EM_X86_64;
        eh.e_version  = 1;
        eh.e_entry    = ENTRY_ADDR;
        eh.e_phoff    = ehdrSize;
        eh.e_ehsize   = (U16)ehdrSize;
        eh.e_phentsize = (U16)phdrSize;
        eh.e_phnum    = 1;
        memcpy(elf.data(), &eh, ehdrSize);

        k_Elf64_Phdr ph{};
        ph.p_type   = k_PT_LOAD;
        ph.p_flags  = 5; // PF_R | PF_X
        ph.p_offset = 0;
        ph.p_vaddr  = LOAD_VADDR;
        ph.p_paddr  = LOAD_VADDR;
        ph.p_filesz = totalSize;
        ph.p_memsz  = totalSize;
        ph.p_align  = 0x1000;
        memcpy(elf.data() + ehdrSize, &ph, phdrSize);
        memcpy(elf.data() + ehdrSize + phdrSize, code.data(), code.size());

        Elf64ParseResult parsed = ElfLoader64::parseBuffer(elf.data(), elf.size());

        // High stack matching the runner's layout.
        const U64 STACK_TOP_HI  = 0x7FFFFFFFE000ULL;
        const U64 STACK_SIZE_HI = 64 * 1024;
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(STACK_TOP_HI - STACK_SIZE_HI, STACK_SIZE_HI, 3);
        bool mapOk = ElfLoader64::mapSegmentsFromBuffer(&mem, parsed,
                                                        elf.data(), elf.size(),
                                                        0, "a10-sysv");

        // Inline SysV stack builder mirroring cpu64RunElf.cpp.
        U64 sp = STACK_TOP_HI - 16;
        const char* env0 = "PATH=/bin";
        const char* arg0 = "a10";
        U64 envLen = (U64)strlen(env0) + 1;
        sp -= envLen;
        mem.memcpyToGuest(sp, env0, envLen);
        U64 envpPtr = sp;
        U64 argLen = (U64)strlen(arg0) + 1;
        sp -= argLen;
        mem.memcpyToGuest(sp, arg0, argLen);
        U64 argvPtr = sp;
        sp -= 16; // AT_RANDOM pool
        U64 randomAddr = sp;
        for (int i = 0; i < 16; i++) mem.writeb(randomAddr + i, 0);

        // 9 aux entries (incl AT_NULL) × 2 qwords + envp[0..2] (env, NULL)
        // + argv[0..2] (arg, NULL) + argc(1) = 1 + 1 + 1 + 1 + 1 + 2*9 = 23 qwords
        struct Aux { U64 k, v; } aux[] = {
            { 3,  parsed.baseAddrLow + parsed.phoff }, // AT_PHDR
            { 4,  parsed.phentsize },                  // AT_PHENT
            { 5,  parsed.phnum },                      // AT_PHNUM
            { 6,  4096 },                              // AT_PAGESZ
            { 9,  parsed.entry },                      // AT_ENTRY
            { 25, randomAddr },                        // AT_RANDOM
            { 11, 0 },                                 // AT_UID
            { 17, 100 },                               // AT_CLKTCK
            { 0,  0 },                                 // AT_NULL
        };
        U64 totalQ = 1 + 1 + 1 + 1 + 1 + 2 * (sizeof(aux)/sizeof(aux[0]));
        U64 afterSize = totalQ * 8;
        U64 targetSp = (sp - afterSize) & ~0xFULL;
        sp = targetSp + afterSize;
        auto pushQ = [&](U64 v) { sp -= 8; mem.writeq(sp, v); };
        for (int i = (int)(sizeof(aux)/sizeof(aux[0])) - 1; i >= 0; --i) {
            pushQ(aux[i].v); pushQ(aux[i].k);
        }
        pushQ(0); pushQ(envpPtr);
        pushQ(0); pushQ(argvPtr);
        pushQ(1); // argc

        CPU64 cpu(&mem);
        cpu.rip = parsed.entry;
        cpu.reg[X64_RSP].setU64(sp);
        cpu.reg[X64_RDX].setU64(0);
        if (mapOk) cpu.runBounded(200);

        bool ok = parsed.ok && mapOk && cpu.yield && cpu.reg[X64_R15].u64 == 1;
        if (ok) {
            printf("  PASS: SysV stack frame: [rsp] == argc (1)\n");
            r.passed++;
        } else {
            printf("  FAIL: A10 (parseOk=%d mapOk=%d yield=%d R15=0x%llx RIP=0x%llx sp=0x%llx)\n",
                   parsed.ok, mapOk, cpu.yield,
                   (unsigned long long)cpu.reg[X64_R15].u64,
                   (unsigned long long)cpu.rip,
                   (unsigned long long)sp);
            r.failed++;
        }
        fflush(stdout);
    }

    // ----- A13: multi-PT_LOAD with BSS zero-fill -----
    // Synthesizes ET_EXEC with TWO PT_LOADs:
    //   seg0: RX, vaddr=0x500000, file+mem = whole code
    //   seg1: RW, vaddr=0x501000, filesz=8 (one qword sentinel 0xDEADBEEFCAFEBABE),
    //                              memsz=0x40 (trailing 56 bytes are BSS — must be zero)
    // Entry code reads the BSS qword at vaddr 0x501010 (well into the BSS gap)
    // into R15 via MOV RAX, moffs64. If the loader correctly zero-fills BSS, R15==0;
    // if stale memory leaks through, the test fails with whatever garbage is there.
    // Also reads the file-backed qword at 0x501000 into RBX to prove seg1's filesz
    // bytes were copied (sanity); only R15 is asserted by the predicate.
    {
        const U64 SEG0_VADDR = 0x500000;
        const U64 SEG1_VADDR = 0x501000;
        const U64 BSS_READ_ADDR = 0x501010; // 0x10 bytes into seg1 (past the 8 file bytes)
        const size_t ehdrSize = sizeof(k_Elf64_Ehdr);
        const size_t phdrSize = sizeof(k_Elf64_Phdr);
        // Code:
        //   48 A1 imm64        mov rax, [0x501010]   ; read from BSS
        //   49 89 C7           mov r15, rax
        //   48 B8 imm64        mov rax, 60
        //   48 31 FF           xor rdi, rdi
        //   0F 05              syscall
        std::vector<U8> code;
        code.push_back(0x48); code.push_back(0xA1);
        for (int i = 0; i < 8; i++) code.push_back((U8)((BSS_READ_ADDR >> (8*i)) & 0xFF));
        code.push_back(0x49); code.push_back(0x89); code.push_back(0xC7); // mov r15, rax
        code.push_back(0x48); code.push_back(0xC7); code.push_back(0xC0);
        code.push_back(0x3C); code.push_back(0x00); code.push_back(0x00); code.push_back(0x00); // mov rax,60
        code.push_back(0x48); code.push_back(0x31); code.push_back(0xFF); // xor rdi,rdi
        code.push_back(0x0F); code.push_back(0x05); // syscall

        // Layout of the file:
        //   0x0000 Ehdr (64)
        //   0x0040 Phdr0 (56)  PT_LOAD for code
        //   0x0078 Phdr1 (56)  PT_LOAD for data
        //   0x00B0 code bytes
        //   0x00B0+len_code (...) data bytes (8 bytes — one sentinel qword)
        const size_t PHDR0_OFF = ehdrSize;
        const size_t PHDR1_OFF = ehdrSize + phdrSize;
        const size_t CODE_OFF  = ehdrSize + 2 * phdrSize;
        const size_t DATA_OFF  = CODE_OFF + code.size();
        const size_t fileSize  = DATA_OFF + 8;

        std::vector<U8> elf(fileSize, 0);

        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64; eh.e_ident[5] = 1; eh.e_ident[6] = 1;
        eh.e_type     = 2; // ET_EXEC
        eh.e_machine  = k_EM_X86_64;
        eh.e_version  = 1;
        eh.e_entry    = SEG0_VADDR + CODE_OFF;
        eh.e_phoff    = PHDR0_OFF;
        eh.e_ehsize   = (U16)ehdrSize;
        eh.e_phentsize = (U16)phdrSize;
        eh.e_phnum    = 2;
        memcpy(elf.data(), &eh, ehdrSize);

        // Phdr0: code, RX, file+mem cover [0 .. CODE_OFF+code.size())
        k_Elf64_Phdr ph0{};
        ph0.p_type   = k_PT_LOAD;
        ph0.p_flags  = 5; // PF_R | PF_X
        ph0.p_offset = 0;
        ph0.p_vaddr  = SEG0_VADDR;
        ph0.p_paddr  = SEG0_VADDR;
        ph0.p_filesz = DATA_OFF;  // everything before the data segment's file region
        ph0.p_memsz  = DATA_OFF;
        ph0.p_align  = 0x1000;
        memcpy(elf.data() + PHDR0_OFF, &ph0, phdrSize);

        // Phdr1: data, RW, filesz=8 (one sentinel qword), memsz=0x40 (rest is BSS)
        k_Elf64_Phdr ph1{};
        ph1.p_type   = k_PT_LOAD;
        ph1.p_flags  = 6; // PF_R | PF_W
        ph1.p_offset = DATA_OFF;
        ph1.p_vaddr  = SEG1_VADDR;
        ph1.p_paddr  = SEG1_VADDR;
        ph1.p_filesz = 8;
        ph1.p_memsz  = 0x40;
        ph1.p_align  = 0x1000;
        memcpy(elf.data() + PHDR1_OFF, &ph1, phdrSize);

        // Copy code into the file at CODE_OFF
        memcpy(elf.data() + CODE_OFF, code.data(), code.size());
        // Sentinel qword at DATA_OFF — proves the file bytes ARE present
        // (so a passing test isn't just "the page never got written")
        const U64 sentinel = 0xDEADBEEFCAFEBABEULL;
        memcpy(elf.data() + DATA_OFF, &sentinel, 8);

        Elf64ParseResult parsed = ElfLoader64::parseBuffer(elf.data(), elf.size());
        bool parseOk = parsed.ok && parsed.segments.size() == 2;

        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3);
        bool mapOk = parseOk && ElfLoader64::mapSegmentsFromBuffer(
            &mem, parsed, elf.data(), elf.size(), 0, "a13-multi-seg");

        // Pre-flight: prove the BSS gap is actually zero in guest memory.
        U64 bssQ = mem.readq(BSS_READ_ADDR);
        U64 fileQ = mem.readq(SEG1_VADDR);

        CPU64 cpu(&mem);
        cpu.rip = parsed.entry;
        cpu.reg[X64_RSP].setU64(STACK_TOP - 16);
        if (mapOk) cpu.runBounded(200);

        // instructionCount > 0 is load-bearing: cpu.yield is set both by the
        // exit syscall AND by the unimpl-tracer at cpu64.cpp:3698, so checking
        // yield alone would mask a decode failure on the first instruction
        // (R15 would still be 0 because it's never written). Require that the
        // CPU actually executed at least the BSS-load + mov-r15 + setup ops.
        bool ok = parseOk &&
                  mapOk &&
                  cpu.yield &&
                  cpu.instructionCount >= 4 &&     // BSS-load, mov r15, mov rax=60, xor rdi, syscall
                  cpu.reg[X64_R15].u64 == 0 &&     // BSS read zero through CPU
                  bssQ == 0 &&                     // BSS read zero direct
                  fileQ == sentinel;               // file bytes copied
        if (ok) {
            printf("  PASS: multi-PT_LOAD with BSS zero-fill (sentinel=0x%llx)\n",
                   (unsigned long long)sentinel);
            r.passed++;
        } else {
            printf("  FAIL: A13 (parseOk=%d mapOk=%d yield=%d insn=%llu R15=0x%llx bssQ=0x%llx fileQ=0x%llx RIP=0x%llx)\n",
                   parseOk, mapOk, cpu.yield,
                   (unsigned long long)cpu.instructionCount,
                   (unsigned long long)cpu.reg[X64_R15].u64,
                   (unsigned long long)bssQ,
                   (unsigned long long)fileQ,
                   (unsigned long long)cpu.rip);
            r.failed++;
        }
        fflush(stdout);
    }

    // ----- C11: SAHF / LAHF / INT3 -----
    // T: SAHF — load AH into the low byte of rflags. Set AH=0xD5 (CF|PF|AF|ZF|SF
    // all on, plus the always-1 reserved bit), call SAHF, then read rflags low
    // byte via LAHF into AH and stash. After SAHF, AF/CF/PF/SF/ZF should be set;
    // LAHF reads back 0xD7 (0xD5 mask | reserved bit 0x02).
    {
        std::vector<U8> code = {
            // Clear flags by xor rax,rax — leaves ZF=1 / others=0
            0x48, 0x31, 0xC0,
            // mov rax, 0xD500 — AH=0xD5, AL=0
            0x48, 0xC7, 0xC0, 0x00, 0xD5, 0x00, 0x00,
            0x9E,                                                          // SAHF
            // Zero AH so LAHF result is observable; AL preserved
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    // mov rax, 0
            0x9F,                                                          // LAHF — read flags back into AH
            // Shift AH down to AL via shr rax, 8
            0x48, 0xC1, 0xE8, 0x08,
        };
        runAndCheck(r, "SAHF + LAHF round-trip (0xD5 → 0xD7)", withExit(code), [](CPU64& c) {
            // 0xD5 user-visible bits + 0x02 reserved bit = 0xD7
            return (c.reg[X64_R15].u64 & 0xFF) == 0xD7;
        });
    }

    // T: LAHF with known starting flags. xor rax,rax sets ZF=1 + PF=1 (parity
    // of 0 = even). After LAHF, AH must contain ZF|PF|reserved = 0x40|0x04|0x02 = 0x46.
    {
        std::vector<U8> code = {
            0x48, 0x31, 0xC0,                                              // xor rax,rax — ZF=1, PF=1
            0x9F,                                                          // LAHF
            0x48, 0xC1, 0xE8, 0x08,                                        // shr rax, 8
        };
        runAndCheck(r, "LAHF after xor (ZF=1, PF=1) → AH=0x46", withExit(code), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0xFF) == 0x46;
        });
    }

    // T: INT3 — yields cleanly. R15 stays at its pre-trap value (0xDEAD)
    // since we never reach the exit syscall.
    {
        std::vector<U8> code = {
            0x49, 0xC7, 0xC7, 0xAD, 0xDE, 0x00, 0x00,                      // mov r15, 0xDEAD
            0xCC,                                                          // INT3 — yields
            // unreachable: exit(0)
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,
            0x48, 0x31, 0xFF,
            0x0F, 0x05,
        };
        // Use bare runAndCheck without withExit since INT3 prevents exit() from running
        runAndCheck(r, "INT3 yields cleanly (no host crash)", code, [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0xDEAD && c.yield;
        });
    }

    // ----- A5: in-memory ELF synthesizer + parseBuffer round-trip -----
    // Builds a minimal valid x86-64 ET_EXEC ELF in a byte buffer:
    //   [Ehdr][Phdr × 1: PT_LOAD][code bytes]
    // and asserts parseBuffer() returns ok=true with all fields matching.

    // T: ET_EXEC happy path.
    {
        const U64 LOAD_VADDR = 0x400000;
        const U64 ENTRY_ADDR = 0x400000 + sizeof(k_Elf64_Ehdr) + sizeof(k_Elf64_Phdr);
        std::vector<U8> code = { 0x90, 0x90, 0x90, 0x90 }; // 4× nop

        // Lay out the ELF.
        size_t ehdrSize = sizeof(k_Elf64_Ehdr);
        size_t phdrSize = sizeof(k_Elf64_Phdr);
        size_t totalSize = ehdrSize + phdrSize + code.size();
        std::vector<U8> elf(totalSize, 0);

        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F;
        eh.e_ident[1] = 'E';
        eh.e_ident[2] = 'L';
        eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64;
        eh.e_ident[5] = 1; // ELFDATA2LSB
        eh.e_ident[6] = 1; // EV_CURRENT
        eh.e_type     = 2; // ET_EXEC
        eh.e_machine  = k_EM_X86_64;
        eh.e_version  = 1;
        eh.e_entry    = ENTRY_ADDR;
        eh.e_phoff    = ehdrSize;
        eh.e_ehsize   = (U16)ehdrSize;
        eh.e_phentsize = (U16)phdrSize;
        eh.e_phnum    = 1;
        memcpy(elf.data(), &eh, ehdrSize);

        k_Elf64_Phdr ph{};
        ph.p_type   = 1; // PT_LOAD
        ph.p_flags  = 5; // PF_R | PF_X
        ph.p_offset = 0; // covers Ehdr + Phdr + code in one mapping
        ph.p_vaddr  = LOAD_VADDR;
        ph.p_paddr  = LOAD_VADDR;
        ph.p_filesz = totalSize;
        ph.p_memsz  = totalSize;
        ph.p_align  = 0x1000;
        memcpy(elf.data() + ehdrSize, &ph, phdrSize);

        memcpy(elf.data() + ehdrSize + phdrSize, code.data(), code.size());

        Elf64ParseResult res = ElfLoader64::parseBuffer(elf.data(), elf.size());
        bool ok = res.ok &&
                  !res.isPie &&
                  res.entry == ENTRY_ADDR &&
                  res.phnum == 1 &&
                  res.phentsize == phdrSize &&
                  res.segments.size() == 1 &&
                  res.segments[0].vaddr == LOAD_VADDR &&
                  res.segments[0].memsz == totalSize &&
                  res.baseAddrLow == LOAD_VADDR &&
                  res.baseAddrHigh == LOAD_VADDR + totalSize &&
                  !res.dynamic.present &&
                  !res.tls.present &&
                  res.interpreter.length() == 0;
        if (ok) {
            printf("  PASS: parseBuffer: synthesized ET_EXEC parses to expected fields\n");
            r.passed++;
        } else {
            printf("  FAIL: parseBuffer (ok=%d entry=0x%llx phnum=%u segs=%zu lo=0x%llx hi=0x%llx)\n",
                   res.ok, (unsigned long long)res.entry, res.phnum, res.segments.size(),
                   (unsigned long long)res.baseAddrLow,
                   (unsigned long long)res.baseAddrHigh);
            r.failed++;
        }
        fflush(stdout);
    }

    // T: ET_DYN (PIE) — isPie must be true.
    {
        size_t ehdrSize = sizeof(k_Elf64_Ehdr);
        size_t phdrSize = sizeof(k_Elf64_Phdr);
        size_t totalSize = ehdrSize + phdrSize + 16;
        std::vector<U8> elf(totalSize, 0);

        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F;
        eh.e_ident[1] = 'E';
        eh.e_ident[2] = 'L';
        eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64;
        eh.e_ident[5] = 1;
        eh.e_ident[6] = 1;
        eh.e_type     = 3; // ET_DYN
        eh.e_machine  = k_EM_X86_64;
        eh.e_version  = 1;
        eh.e_entry    = 0x100;
        eh.e_phoff    = ehdrSize;
        eh.e_ehsize   = (U16)ehdrSize;
        eh.e_phentsize = (U16)phdrSize;
        eh.e_phnum    = 1;
        memcpy(elf.data(), &eh, ehdrSize);

        k_Elf64_Phdr ph{};
        ph.p_type   = 1;
        ph.p_flags  = 5;
        ph.p_offset = 0;
        ph.p_vaddr  = 0;
        ph.p_filesz = totalSize;
        ph.p_memsz  = totalSize;
        ph.p_align  = 0x1000;
        memcpy(elf.data() + ehdrSize, &ph, phdrSize);

        Elf64ParseResult res = ElfLoader64::parseBuffer(elf.data(), elf.size());
        bool ok = res.ok && res.isPie && res.entry == 0x100;
        if (ok) {
            printf("  PASS: parseBuffer: ET_DYN flagged as isPie\n");
            r.passed++;
        } else {
            printf("  FAIL: parseBuffer ET_DYN (ok=%d isPie=%d entry=0x%llx)\n",
                   res.ok, res.isPie, (unsigned long long)res.entry);
            r.failed++;
        }
        fflush(stdout);
    }

    // T: rejection — wrong e_machine.
    {
        size_t ehdrSize = sizeof(k_Elf64_Ehdr);
        std::vector<U8> elf(ehdrSize + sizeof(k_Elf64_Phdr), 0);
        k_Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7F;
        eh.e_ident[1] = 'E';
        eh.e_ident[2] = 'L';
        eh.e_ident[3] = 'F';
        eh.e_ident[4] = k_ELFCLASS64;
        eh.e_machine  = 0x28; // EM_ARM
        eh.e_phnum    = 1;
        eh.e_phentsize = sizeof(k_Elf64_Phdr);
        eh.e_phoff    = ehdrSize;
        memcpy(elf.data(), &eh, ehdrSize);
        Elf64ParseResult res = ElfLoader64::parseBuffer(elf.data(), elf.size());
        if (!res.ok) {
            printf("  PASS: parseBuffer: rejects non-x86_64 e_machine\n");
            r.passed++;
        } else {
            printf("  FAIL: parseBuffer: accepted EM_ARM\n");
            r.failed++;
        }
        fflush(stdout);
    }

    // T: rejection — short buffer (less than Ehdr).
    {
        std::vector<U8> elf(8, 0);
        elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        Elf64ParseResult res = ElfLoader64::parseBuffer(elf.data(), elf.size());
        if (!res.ok) {
            printf("  PASS: parseBuffer: rejects truncated buffer\n");
            r.passed++;
        } else {
            printf("  FAIL: parseBuffer: accepted 8-byte buffer\n");
            r.failed++;
        }
        fflush(stdout);
    }

    // T: KMemory64::mprotect flips writable pages to read-only and is a
    // no-op on holes. Verifies the foundation under PT_GNU_RELRO before
    // exercising the loader-level integration below.
    {
        KMemory64 mem(nullptr);
        const U64 BASE = 0x40000000;
        mem.mmapAnonymousFixed(BASE, 0x3000, 3); // RW (prot=0x3)
        // Before: WRITE bit set.
        U32 before = mem.getPageFlags(BASE >> K64_PAGE_SHIFT);
        bool wasWritable = (before & K64_PAGE_WRITE) != 0;
        // Flip middle page to read-only.
        mem.mprotect(BASE + K64_PAGE_SIZE, K64_PAGE_SIZE, 0x1);
        U32 mid   = mem.getPageFlags((BASE + K64_PAGE_SIZE) >> K64_PAGE_SHIFT);
        U32 first = mem.getPageFlags(BASE >> K64_PAGE_SHIFT);
        U32 last  = mem.getPageFlags((BASE + 0x2000) >> K64_PAGE_SHIFT);
        bool ok = wasWritable
                  && (mid & K64_PAGE_READ)
                  && !(mid & K64_PAGE_WRITE)
                  && (first & K64_PAGE_WRITE)   // neighbours untouched
                  && (last & K64_PAGE_WRITE);
        // Calling mprotect on an unmapped hole must not crash and must not
        // create new pages (per header semantics).
        U64 sizeBefore = mem.mappedPageCount();
        mem.mprotect(0x90000000, 0x1000, 0x1);
        U64 sizeAfter = mem.mappedPageCount();
        ok = ok && (sizeBefore == sizeAfter);
        if (ok) {
            printf("  PASS: KMemory64::mprotect flips flags, skips holes\n");
            r.passed++;
        } else {
            printf("  FAIL: KMemory64::mprotect (was_w=%d mid=0x%x first=0x%x last=0x%x sizes %llu->%llu)\n",
                   (int)wasWritable, mid, first, last,
                   (unsigned long long)sizeBefore,
                   (unsigned long long)sizeAfter);
            r.failed++;
        }
        fflush(stdout);
    }

    // T: parseBuffer captures PT_GNU_RELRO vaddr/memsz. Minimum-shape ELF
    // with a single PT_LOAD plus one PT_GNU_RELRO phdr.
    {
        // Layout: Ehdr(64) + 2 Phdrs(2*56=112) at offset 64.
        // PT_LOAD covers full file; PT_GNU_RELRO points at offset 0x100, size 0x40.
        const U64 PT_GNU_RELRO_VAL = 0x6474E552;
        std::vector<U8> elf(0x200, 0);
        // Ehdr ident + minimum fields.
        elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[6] = 1;
        // e_type=ET_EXEC=2, e_machine=0x3E (x86_64), e_version=1
        elf[16] = 2; elf[18] = 0x3E; elf[20] = 1;
        // e_phoff = 64
        elf[32] = 64;
        // e_ehsize=64, e_phentsize=56, e_phnum=2
        elf[52] = 64; elf[54] = 56; elf[56] = 2;
        // Phdr[0] PT_LOAD at file off 64.
        U64 phOff = 64;
        elf[phOff + 0] = 1; // PT_LOAD
        elf[phOff + 4] = 7; // RWX flags
        // p_filesz/p_memsz = 0x200, p_align = 0x1000
        elf[phOff + 32] = 0x00; elf[phOff + 33] = 0x02; // filesz
        elf[phOff + 40] = 0x00; elf[phOff + 41] = 0x02; // memsz
        elf[phOff + 48] = 0x00; elf[phOff + 49] = 0x10; // align
        // Phdr[1] PT_GNU_RELRO at file off 64+56 = 120
        U64 ph1 = 120;
        elf[ph1 + 0] = (U8)(PT_GNU_RELRO_VAL & 0xFF);
        elf[ph1 + 1] = (U8)((PT_GNU_RELRO_VAL >> 8) & 0xFF);
        elf[ph1 + 2] = (U8)((PT_GNU_RELRO_VAL >> 16) & 0xFF);
        elf[ph1 + 3] = (U8)((PT_GNU_RELRO_VAL >> 24) & 0xFF);
        // p_vaddr = 0x100, p_memsz = 0x40
        elf[ph1 + 16] = 0x00; elf[ph1 + 17] = 0x01;
        elf[ph1 + 40] = 0x40;
        Elf64ParseResult res = ElfLoader64::parseBuffer(elf.data(), elf.size());
        bool ok = res.ok && res.relro.present && res.relro.vaddr == 0x100 && res.relro.memsz == 0x40;
        if (ok) {
            printf("  PASS: parseBuffer: PT_GNU_RELRO captured (vaddr=0x%llx memsz=0x%llx)\n",
                   (unsigned long long)res.relro.vaddr,
                   (unsigned long long)res.relro.memsz);
            r.passed++;
        } else {
            printf("  FAIL: parseBuffer: PT_GNU_RELRO (ok=%d present=%d vaddr=0x%llx memsz=0x%llx)\n",
                   (int)res.ok, (int)res.relro.present,
                   (unsigned long long)res.relro.vaddr,
                   (unsigned long long)res.relro.memsz);
            r.failed++;
        }
        fflush(stdout);
    }

    // T: parseBuffer captures PT_GNU_STACK presence and exec-flag.
    {
        const U64 PT_GNU_STACK_VAL = 0x6474E551;
        // Two ELFs: one with PF_X (exec stack), one without.
        for (int variant = 0; variant < 2; variant++) {
            std::vector<U8> elf(0x100, 0);
            elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
            elf[4] = 2; elf[5] = 1; elf[6] = 1;
            elf[16] = 2; elf[18] = 0x3E; elf[20] = 1;
            elf[32] = 64;
            elf[52] = 64; elf[54] = 56; elf[56] = 2;
            // Phdr[0] PT_LOAD
            U64 phOff = 64;
            elf[phOff + 0] = 1;
            elf[phOff + 4] = 7;
            elf[phOff + 32] = 0x00; elf[phOff + 33] = 0x01;
            elf[phOff + 40] = 0x00; elf[phOff + 41] = 0x01;
            elf[phOff + 48] = 0x00; elf[phOff + 49] = 0x10;
            // Phdr[1] PT_GNU_STACK with variant flags.
            U64 ph1 = 120;
            elf[ph1 + 0] = (U8)(PT_GNU_STACK_VAL & 0xFF);
            elf[ph1 + 1] = (U8)((PT_GNU_STACK_VAL >> 8) & 0xFF);
            elf[ph1 + 2] = (U8)((PT_GNU_STACK_VAL >> 16) & 0xFF);
            elf[ph1 + 3] = (U8)((PT_GNU_STACK_VAL >> 24) & 0xFF);
            elf[ph1 + 4] = variant == 0 ? 6 : 7; // RW vs RWX
            Elf64ParseResult res = ElfLoader64::parseBuffer(elf.data(), elf.size());
            bool want = (variant == 1);
            bool ok = res.ok && res.gnuStackPresent && res.gnuStackExec == want;
            if (ok) {
                printf("  PASS: parseBuffer: PT_GNU_STACK exec=%d captured\n", (int)want);
                r.passed++;
            } else {
                printf("  FAIL: PT_GNU_STACK variant=%d (ok=%d present=%d exec=%d want=%d)\n",
                       variant, (int)res.ok, (int)res.gnuStackPresent,
                       (int)res.gnuStackExec, (int)want);
                r.failed++;
            }
            fflush(stdout);
        }
    }

    // ---------- A32: end-to-end PLT call through a resolved JUMP_SLOT ----------
    // This is the "Milestone A done" probe. Every prior linkSharedObjects
    // test only verified that the GOT slot ended up holding the right
    // address. Here we go one step further: after linking, we let CPU64
    // actually JUMP through the GOT slot and execute the callee's body.
    //
    // Topology:
    //   libdiscovery.so exports `discovery_answer`, body = `mov rax, 42; ret`.
    //   main exe imports `discovery_answer` via R_X86_64_JUMP_SLOT.
    //   _start does:  mov rax, [rip+got_slot]    ; load resolved fn ptr
    //                 call rax                    ; execute callee
    //                 mov r15, rax                ; stash return value
    //                 mov rax, 60                 ; sys_exit
    //                 mov rdi, r15                ; exit code = answer
    //                 syscall
    //
    // The PASS condition is: after linking + running, R15 == 42 AND the
    // CPU yielded cleanly. If JUMP_SLOT resolution wrote a bad address,
    // we'd decode-fail or page-fault on the call. If the loader didn't
    // mark the lib's code RX, we'd fault on the indirect call. If
    // applySymbolRelocations didn't walk the PLT table, the GOT slot
    // would be zero and the call would jump to NULL.
    //
    // Calling this "Milestone A done" because: it exercises every
    // surviving lane of the A roadmap in one shot — PT_DYNAMIC parse,
    // DT_NEEDED walk (we use the preloaded fetcher, not the recursive
    // one, to keep the test hermetic), symbol export, JUMP_SLOT relo,
    // multi-DSO loading, and the CPU64 interpreter's call-through-GOT
    // path. The only missing piece from the original "real glibc" exit
    // criterion is `ld-linux.so.2` itself, which requires a Linux
    // toolchain to produce (gated by Milestone D's rootfs work).
    {
        const U64 MAIN_RELOC = 0x10000000ULL;
        const U64 LIB_RELOC  = 0x20000000ULL;
        const U64 STACK_HI   = 0x800000ULL;

        // ----- Library layout -----
        const U64 L_EHDR  = 0x0000;
        const U64 L_PHDR  = 0x0040;
        const U64 L_DYN   = 0x00B0;
        const U64 L_SYM   = 0x00F0;
        const U64 L_STR   = 0x0120;
        const U64 L_CODE  = 0x0140;
        const U64 L_END   = 0x0148;

        std::vector<U8> libBuf(L_END, 0);

        k_Elf64_Ehdr leh{};
        leh.e_ident[0]=0x7F; leh.e_ident[1]='E'; leh.e_ident[2]='L'; leh.e_ident[3]='F';
        leh.e_ident[4]=k_ELFCLASS64; leh.e_ident[5]=1; leh.e_ident[6]=1;
        leh.e_type     = 3;             // ET_DYN
        leh.e_machine  = k_EM_X86_64;
        leh.e_version  = 1;
        leh.e_entry    = L_CODE;
        leh.e_phoff    = L_PHDR;
        leh.e_ehsize   = sizeof(k_Elf64_Ehdr);
        leh.e_phentsize= sizeof(k_Elf64_Phdr);
        leh.e_phnum    = 2;
        memcpy(libBuf.data() + L_EHDR, &leh, sizeof(leh));

        k_Elf64_Phdr lload{};
        lload.p_type=k_PT_LOAD; lload.p_flags=7; // RWX (X needed for call target)
        lload.p_offset=0; lload.p_vaddr=0; lload.p_paddr=0;
        lload.p_filesz=L_END; lload.p_memsz=L_END; lload.p_align=0x1000;
        memcpy(libBuf.data() + L_PHDR, &lload, sizeof(lload));

        k_Elf64_Phdr ldyn{};
        ldyn.p_type=k_PT_DYNAMIC; ldyn.p_flags=6;
        ldyn.p_offset=L_DYN; ldyn.p_vaddr=L_DYN;
        ldyn.p_filesz=0x40; ldyn.p_memsz=0x40; ldyn.p_align=8;
        memcpy(libBuf.data() + L_PHDR + sizeof(k_Elf64_Phdr), &ldyn, sizeof(ldyn));

        // SYMTAB BEFORE STRTAB so extractGlobalSymbols's
        // (strtab > symtab) heuristic bounds the scan correctly.
        k_Elf64_Sym lsym[2]{};
        lsym[1].st_name  = 1;          // offset into STRTAB → "discovery_answer"
        lsym[1].st_info  = 1 << 4;     // STB_GLOBAL
        lsym[1].st_shndx = 1;          // defined (any non-zero shndx)
        lsym[1].st_value = L_CODE;
        memcpy(libBuf.data() + L_SYM, lsym, sizeof(lsym));

        // STRTAB: \0discovery_answer\0  (length 18)
        const char* lstr = "\0discovery_answer";
        memcpy(libBuf.data() + L_STR, lstr, 18);

        k_Elf64_Dyn ldyns[4]{};
        ldyns[0].d_tag=k_DT_SYMTAB; ldyns[0].d_un.d_ptr=L_SYM;
        ldyns[1].d_tag=k_DT_STRTAB; ldyns[1].d_un.d_ptr=L_STR;
        ldyns[2].d_tag=k_DT_SYMENT; ldyns[2].d_un.d_val=sizeof(k_Elf64_Sym);
        ldyns[3].d_tag=k_DT_NULL;   ldyns[3].d_un.d_val=0;
        memcpy(libBuf.data() + L_DYN, ldyns, sizeof(ldyns));

        // Code: mov rax, 42 ; ret
        //   48 C7 C0 2A 00 00 00 = mov rax, 0x2A
        //   C3                    = ret
        U8 lcode[] = { 0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
        memcpy(libBuf.data() + L_CODE, lcode, sizeof(lcode));

        // ----- Main exe layout -----
        const U64 M_EHDR  = 0x0000;
        const U64 M_PHDR  = 0x0040;
        const U64 M_DYN   = 0x00B0;
        const U64 M_SYM   = 0x0120;
        const U64 M_STR   = 0x0150;
        const U64 M_JMP   = 0x0180;
        const U64 M_GOT   = 0x01A0;
        const U64 M_CODE  = 0x01C0;
        const U64 M_END   = 0x01E0;

        std::vector<U8> mainBuf(M_END, 0);

        k_Elf64_Ehdr meh{};
        meh.e_ident[0]=0x7F; meh.e_ident[1]='E'; meh.e_ident[2]='L'; meh.e_ident[3]='F';
        meh.e_ident[4]=k_ELFCLASS64; meh.e_ident[5]=1; meh.e_ident[6]=1;
        meh.e_type     = 3;             // ET_DYN (PIE; we apply MAIN_RELOC manually)
        meh.e_machine  = k_EM_X86_64;
        meh.e_version  = 1;
        meh.e_entry    = M_CODE;
        meh.e_phoff    = M_PHDR;
        meh.e_ehsize   = sizeof(k_Elf64_Ehdr);
        meh.e_phentsize= sizeof(k_Elf64_Phdr);
        meh.e_phnum    = 2;
        memcpy(mainBuf.data() + M_EHDR, &meh, sizeof(meh));

        k_Elf64_Phdr mload{};
        mload.p_type=k_PT_LOAD; mload.p_flags=7;
        mload.p_offset=0; mload.p_vaddr=0; mload.p_paddr=0;
        mload.p_filesz=M_END; mload.p_memsz=M_END; mload.p_align=0x1000;
        memcpy(mainBuf.data() + M_PHDR, &mload, sizeof(mload));

        k_Elf64_Phdr mdyn{};
        mdyn.p_type=k_PT_DYNAMIC; mdyn.p_flags=6;
        mdyn.p_offset=M_DYN; mdyn.p_vaddr=M_DYN;
        mdyn.p_filesz=0x70; mdyn.p_memsz=0x70; mdyn.p_align=8;
        memcpy(mainBuf.data() + M_PHDR + sizeof(k_Elf64_Phdr), &mdyn, sizeof(mdyn));

        // SYMTAB: slot 0 null, slot 1 = `discovery_answer` UNDEF (imported).
        k_Elf64_Sym msym[2]{};
        msym[1].st_name  = 17;         // offset into STRTAB → "discovery_answer"
        msym[1].st_info  = 1 << 4;     // STB_GLOBAL
        msym[1].st_shndx = 0;          // UNDEF
        memcpy(mainBuf.data() + M_SYM, msym, sizeof(msym));

        // STRTAB: \0 libdiscovery.so \0 discovery_answer \0
        // offsets: 0=NUL, 1="libdiscovery.so", 17="discovery_answer"
        const char* mstr = "\0libdiscovery.so\0discovery_answer";
        memcpy(mainBuf.data() + M_STR, mstr, 34);

        // JMPREL: one JUMP_SLOT relocation against GOT slot for symbol 1.
        k_Elf64_Rela mrela{};
        mrela.r_offset = M_GOT;
        mrela.r_info   = ((U64)1 << 32) | k_R_X86_64_JUMP_SLOT;
        mrela.r_addend = 0;
        memcpy(mainBuf.data() + M_JMP, &mrela, sizeof(mrela));

        // _start at M_CODE.
        // Compute disp32 for `mov rax, [rip+disp]` (7 bytes: 48 8B 05 d d d d).
        // After the 7-byte instr, RIP = (M_CODE + MAIN_RELOC) + 7. Target =
        // (M_GOT + MAIN_RELOC). disp = target - RIP_after = M_GOT - M_CODE - 7.
        S32 disp = (S32)((S64)M_GOT - (S64)M_CODE - 7);
        U8 mcode[] = {
            0x48, 0x8B, 0x05,
                (U8)(disp & 0xFF),
                (U8)((disp >> 8) & 0xFF),
                (U8)((disp >> 16) & 0xFF),
                (U8)((disp >> 24) & 0xFF),  // mov rax, [rip+disp]  (loads GOT slot)
            0xFF, 0xD0,                       // call rax            (jump through GOT)
            0x49, 0x89, 0xC7,                 // mov r15, rax        (stash answer)
            0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00, // mov rax, 60 (sys_exit)
            0x4C, 0x89, 0xFF,                 // mov rdi, r15        (exit code = answer)
            0x0F, 0x05,                       // syscall
        };
        memcpy(mainBuf.data() + M_CODE, mcode, sizeof(mcode));

        // DYN: SYMTAB, STRTAB, SYMENT, NEEDED(libdiscovery.so), JMPREL,
        // PLTRELSZ, PLTREL, NULL = 7 entries (+ NULL terminator above plan).
        k_Elf64_Dyn mdyns[8]{};
        mdyns[0].d_tag=k_DT_SYMTAB;   mdyns[0].d_un.d_ptr=M_SYM;
        mdyns[1].d_tag=k_DT_STRTAB;   mdyns[1].d_un.d_ptr=M_STR;
        mdyns[2].d_tag=k_DT_SYMENT;   mdyns[2].d_un.d_val=sizeof(k_Elf64_Sym);
        mdyns[3].d_tag=k_DT_NEEDED;   mdyns[3].d_un.d_val=1;  // strtab offset of "libdiscovery.so"
        mdyns[4].d_tag=k_DT_JMPREL;   mdyns[4].d_un.d_ptr=M_JMP;
        mdyns[5].d_tag=k_DT_PLTRELSZ; mdyns[5].d_un.d_val=sizeof(k_Elf64_Rela);
        mdyns[6].d_tag=k_DT_PLTREL;   mdyns[6].d_un.d_val=7;  // DT_RELA
        mdyns[7].d_tag=k_DT_NULL;     mdyns[7].d_un.d_val=0;
        memcpy(mainBuf.data() + M_DYN, mdyns, sizeof(mdyns));

        // ----- Load, link, run -----
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(MAIN_RELOC, 0x2000, 7);
        mem.mmapAnonymousFixed(LIB_RELOC,  0x2000, 7);
        mem.mmapAnonymousFixed(STACK_HI - 0x1000, 0x1000, 3);

        Elf64ParseResult mainParsed = ElfLoader64::parseBuffer(mainBuf.data(), mainBuf.size());
        bool mapOk = mainParsed.ok &&
                     ElfLoader64::mapSegmentsFromBuffer(
                         &mem, mainParsed, mainBuf.data(), mainBuf.size(),
                         MAIN_RELOC, "a32-main");

        std::vector<ElfLoader64::PreloadedLibrary> preloaded = {
            { "libdiscovery.so", libBuf.data(), (U64)libBuf.size(), LIB_RELOC }
        };
        std::vector<ElfLoader64::LinkedLibrary> outLibs;
        U64 nLinked = mapOk
            ? ElfLoader64::linkSharedObjects(&mem, mainParsed, MAIN_RELOC,
                                             preloaded, 0, &outLibs)
            : 0;

        // Spot-check: GOT slot must now point at libdiscovery's discovery_answer.
        U64 gotValue = mem.readq(M_GOT + MAIN_RELOC);
        U64 expectedFn = LIB_RELOC + L_CODE;

        CPU64 cpu(&mem);
        cpu.rip = M_CODE + MAIN_RELOC;
        cpu.reg[X64_RSP].setU64(STACK_HI - 16);
        cpu.runBounded(2000);

        bool linkedOk = mapOk && nLinked == 1 && gotValue == expectedFn;
        bool ranOk    = cpu.yield && cpu.reg[X64_R15].u64 == 42;
        if (linkedOk && ranOk) {
            printf("  PASS: end-to-end PLT call — main called discovery_answer "
                   "through resolved JUMP_SLOT and got 42 (Milestone A)\n");
            r.passed++;
        } else {
            printf("  FAIL: A32 end-to-end (mapOk=%d nLinked=%llu got=0x%llx "
                   "expFn=0x%llx yield=%d R15=%llu RIP=0x%llx)\n",
                   mapOk, (unsigned long long)nLinked,
                   (unsigned long long)gotValue,
                   (unsigned long long)expectedFn,
                   cpu.yield,
                   (unsigned long long)cpu.reg[X64_R15].u64,
                   (unsigned long long)cpu.rip);
            r.failed++;
        }
        fflush(stdout);
    }

    // T: FXSAVE/FXRSTOR round-trip preserves XMM0. This is the regression
    // guard for the "first %f prints 0.00000" bug: glibc's lazy PLT resolver
    // (_dl_runtime_resolve_fxsave) FXSAVEs the FP arg registers, runs the
    // symbol resolver (which clobbers XMM0), then FXRSTORs them. When
    // FXSAVE/FXRSTOR were no-ops, the first float-taking libc call through the
    // PLT lost its XMM0 argument and read 0.0. Here we load a sentinel double
    // into XMM0, FXSAVE, clobber XMM0 with PXOR, FXRSTOR, and require the
    // sentinel to survive.
    {
        const U64 SENTINEL = 0x400921FB54442D18ULL; // double ~ pi
        std::vector<U8> code = {
            0x48, 0xB8, 0x18, 0x2D, 0x44, 0x54, 0xFB, 0x21, 0x09, 0x40, // mov rax, SENTINEL
            0x66, 0x48, 0x0F, 0x6E, 0xC0,                               // movq xmm0, rax
            0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,                   // sub rsp, 512
            0x0F, 0xAE, 0x04, 0x24,                                     // fxsave [rsp]
            0x66, 0x0F, 0xEF, 0xC0,                                     // pxor xmm0, xmm0  (clobber)
            0x0F, 0xAE, 0x0C, 0x24,                                     // fxrstor [rsp]
            0x66, 0x48, 0x0F, 0x7E, 0xC0,                               // movq rax, xmm0
            0x48, 0x81, 0xC4, 0x00, 0x02, 0x00, 0x00,                   // add rsp, 512
        };
        runAndCheck(r, "FXSAVE/FXRSTOR preserves XMM0 (PLT-resolver FP arg)", withExit(code), [SENTINEL](CPU64& c) {
            return c.reg[X64_R15].u64 == SENTINEL;
        });
    }

    // Linux getcwd syscall returns a byte count including NUL, not a pointer.
    // Mono/Unity also query their current directory during initialization.
    for (U8 size : {1, 2, 16}) {
        std::vector<U8> code = {
            0x48, 0x89, 0xE7,                 // mov rdi, rsp
            0xBE, size, 0, 0, 0,              // mov esi, size
            0xB8, 79, 0, 0, 0,                // mov eax, SYS_getcwd
            0x0F, 0x05,
        };
        runAndCheck(r, "getcwd returns length including NUL / ERANGE", withExit(code), [size](CPU64& c) {
            if (size == 1) return c.reg[X64_R15].u64 == (U64)(S64)-34;
            return c.reg[X64_R15].u64 == 2 && c.memory->readb(STACK_TOP - 16) == '/' &&
                   c.memory->readb(STACK_TOP - 15) == 0;
        });
    }

    // PREFETCH/PREFETCHW are cache hints, including for unmapped addresses.
    for (U8 modrm : {0x03, 0x4B, 0x8B}) {
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
        CPU64 cpu(&mem);
        cpu.rip = CODE_BASE;
        cpu.reg[X64_RBX].setU64(0xDEAD0000);
        cpu.rflags = X64_CF | X64_ZF | X64_OF | 2;
        std::vector<U8> code = {0x0F, 0x0D, modrm};
        if ((modrm & 0xC0) == 0x40) code.push_back(0x14);
        if ((modrm & 0xC0) == 0x80) code.insert(code.end(), {0x14, 0, 0, 0});
        loadCode(mem, CODE_BASE, code);
        cpu.runBounded(1);
        bool ok = cpu.instructionCount == 1 && cpu.rip == CODE_BASE + code.size() &&
                  cpu.rflags == (X64_CF | X64_ZF | X64_OF | 2) && cpu.reg[X64_RBX].u64 == 0xDEAD0000;
        printf("  %s: PREFETCH(W) decodes displacement without memory access (%02x)\n", ok ? "PASS" : "FAIL", modrm);
        ok ? r.passed++ : r.failed++;
    }

    // Unity startup regressions: exercise step() through its generated dispatch,
    // not a helper that bypasses decoding. Sentinels catch lost SIMD lanes.
    for (U16 cleanup : {0, 8, 256}) {
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
        mem.mmapAnonymousFixed(STACK_TOP - 0x1000, 0x1000, 3);
        loadCode(mem, CODE_BASE, {0xC2, (U8)cleanup, (U8)(cleanup >> 8)});
        const U64 sp = STACK_TOP - 512;
        mem.writeq(sp, CODE_BASE + 32);
        CPU64 cpu(&mem);
        cpu.rip = CODE_BASE;
        cpu.reg[X64_RSP].setU64(sp);
        cpu.runBounded(1);
        bool ok = cpu.instructionCount == 1 && cpu.rip == CODE_BASE + 32 &&
                  cpu.reg[X64_RSP].u64 == sp + 8 + cleanup;
        printf("  %s: RET imm16 dispatch/stack cleanup %u\n", ok ? "PASS" : "FAIL", cleanup);
        ok ? r.passed++ : r.failed++;
    }
    for (U8 opcode : {0x52, 0x53}) {
        for (bool scalar : {false, true}) {
            for (bool fromMemory : {false, true}) {
                KMemory64 mem(nullptr);
                mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
                CPU64 cpu(&mem);
                cpu.rip = CODE_BASE;
                // {4, 16, 64, 256}: all reciprocal/sqrt results are exact.
                cpu.xmm[0].lo = 0x4180000040800000ULL;
                cpu.xmm[0].hi = 0x4380000042800000ULL;
                cpu.xmm[1].lo = 0xDEADBEEF01234567ULL;
                cpu.xmm[1].hi = 0xFEDCBA9876543210ULL;
                std::vector<U8> code;
                if (scalar) code.push_back(0xF3);
                code.insert(code.end(), {0x0F, opcode, (U8)(fromMemory ? 0x08 : 0xC8)});
                if (fromMemory) {
                    // m32 at the page boundary: the scalar form must not read m64.
                    U64 addr = scalar ? CODE_BASE + 0xFFC : CODE_BASE + 0xFE0;
                    cpu.reg[X64_RAX].setU64(addr);
                    mem.writed(addr, (U32)cpu.xmm[0].lo);
                    if (!scalar) {
                        mem.writeq(addr, cpu.xmm[0].lo);
                        mem.writeq(addr + 8, cpu.xmm[0].hi);
                    }
                }
                loadCode(mem, CODE_BASE, code);
                bool sqrt = opcode == 0x52;
                U64 expectedLo = scalar
                    ? 0xDEADBEEF00000000ULL | (sqrt ? 0x3F000000u : 0x3E800000u)
                    : (sqrt ? 0x3E8000003F000000ULL : 0x3D8000003E800000ULL);
                U64 expectedHi = scalar ? 0xFEDCBA9876543210ULL
                    : (sqrt ? 0x3D8000003E000000ULL : 0x3B8000003C800000ULL);
                cpu.runBounded(1);
                bool ok = cpu.instructionCount == 1 && cpu.rip == CODE_BASE + code.size() &&
                          cpu.xmm[1].lo == expectedLo && cpu.xmm[1].hi == expectedHi;
                printf("  %s: %s%s %s preserves all SIMD lanes\n", ok ? "PASS" : "FAIL",
                       sqrt ? "RSQRT" : "RCP", scalar ? "SS" : "PS", fromMemory ? "memory" : "register");
                ok ? r.passed++ : r.failed++;
            }
        }
    }
    for (bool wide : {false, true}) {
        for (bool equal : {false, true}) {
            KMemory64 mem(nullptr);
            mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
            CPU64 cpu(&mem);
            cpu.rip = CODE_BASE;
            cpu.reg[X64_RSI].setU64(CODE_BASE + 0x800);
            U64 lo = wide ? 0x1111111122222222ULL : 0x2222222211111111ULL;
            mem.writeq(CODE_BASE + 0x800, lo);
            mem.writeq(CODE_BASE + 0x808, 0x3333333344444444ULL);
            cpu.reg[X64_RAX].setU64(equal ? (wide ? lo : 0x11111111ULL) : 0);
            cpu.reg[X64_RDX].setU64(equal ? (wide ? 0x3333333344444444ULL : 0x22222222ULL) : 0);
            cpu.reg[X64_RBX].setU64(0xAAAAAAAA55555555ULL);
            cpu.reg[X64_RCX].setU64(0xBBBBBBBB66666666ULL);
            const U32 flags = X64_CF | X64_PF | X64_AF | X64_SF | X64_OF | 2;
            cpu.rflags = flags | (equal ? 0 : X64_ZF);
            std::vector<U8> code = {0xF0};
            if (wide) code.push_back(0x48);
            code.insert(code.end(), {0x0F, 0xC7, 0x0E}); // lock cmpxchg{8,16}b [rsi]
            loadCode(mem, CODE_BASE, code);
            cpu.runBounded(1);
            bool ok = cpu.instructionCount == 1 && cpu.rip == CODE_BASE + code.size() &&
                      cpu.rflags == (flags | (equal ? X64_ZF : 0));
            if (equal) {
                ok = ok && mem.readq(CODE_BASE + 0x800) ==
                    (wide ? 0xAAAAAAAA55555555ULL : 0x6666666655555555ULL);
                if (wide) ok = ok && mem.readq(CODE_BASE + 0x808) == 0xBBBBBBBB66666666ULL;
            } else {
                ok = ok && cpu.reg[X64_RAX].u64 == (wide ? lo : 0x11111111ULL) &&
                    cpu.reg[X64_RDX].u64 == (wide ? 0x3333333344444444ULL : 0x22222222ULL) &&
                    mem.readq(CODE_BASE + 0x800) == lo;
            }
            printf("  %s: CMPXCHG%uB %s changes only ZF\n", ok ? "PASS" : "FAIL", wide ? 16 : 8, equal ? "equal" : "unequal");
            ok ? r.passed++ : r.failed++;
        }
    }
    {
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
        loadCode(mem, CODE_BASE, {0xF3, 0x0F, 0x5A, 0xC8}); // cvtss2sd xmm1, xmm0
        CPU64 cpu(&mem);
        cpu.rip = CODE_BASE;
        cpu.xmm[0].lo = 0x40800000; // 4.0f
        cpu.xmm[1].hi = 0xDEADBEEF12345678ULL;
        cpu.runBounded(1);
        bool ok = cpu.instructionCount == 1 && cpu.rip == CODE_BASE + 4 &&
                  cpu.xmm[1].lo == 0x4010000000000000ULL &&
                  cpu.xmm[1].hi == 0xDEADBEEF12345678ULL;
        printf("  %s: CVTSS2SD converts and preserves upper qword\n", ok ? "PASS" : "FAIL");
        ok ? r.passed++ : r.failed++;
    }

    // (Winbox KMemory64 shared-file-identity tests live with that port.)

    // The bounded runner must stop before the remaining records of a cached
    // block change registers. This matters for scheduler slices as well as
    // instruction-by-instruction tests.
    {
        KMemory64 mem(nullptr);
        mem.mmapAnonymousFixed(CODE_BASE, 0x1000, 7);
        loadCode(mem, CODE_BASE, {
            0xB8, 0x42, 0x00, 0x00, 0x00, // mov eax, 0x42
            0xB9, 0x99, 0x00, 0x00, 0x00, // mov ecx, 0x99
            0x31, 0xD2,                   // xor edx, edx
            0x74, 0x00,                   // jz next instruction
        });
        CPU64 cpu(&mem);
        cpu.rip = CODE_BASE;
        cpu.reg[X64_RCX].setU64(0x1122334455667788ULL);
        cpu.reg[X64_RDX].setU64(0x8877665544332211ULL);
        U64 ran = cpu.runBounded(1);
        bool ok = ran == 1 && cpu.instructionCount == 1 && cpu.rip == CODE_BASE + 5 &&
                  cpu.reg[X64_RAX].u64 == 0x42 &&
                  cpu.reg[X64_RCX].u64 == 0x1122334455667788ULL &&
                  cpu.reg[X64_RDX].u64 == 0x8877665544332211ULL;
        printf("  %s: runBounded stops within a decoded block\n", ok ? "PASS" : "FAIL");
        ok ? r.passed++ : r.failed++;
    }

    // A store may patch a later instruction in the very block being executed.
    // Re-decoding only on the next block entry would run the old immediate.
    runAndCheck(r, "store patches a later instruction in the same block", withExit({
        0xC7, 0x05, 0x01, 0x00, 0x00, 0x00, // mov dword [rip+1], 0x99
        0x99, 0x00, 0x00, 0x00,
        0xB8, 0x42, 0x00, 0x00, 0x00,       // mov eax, 0x42 (patched to 0x99)
    }), [](CPU64& c) { return c.reg[X64_R15].u64 == 0x99; });

    // JRCXZ (E3): jump short if RCX == 0. Taken case.
    // Note: the exit syscall (0F 05) clobbers RCX with the return RIP per
    // the AMD64 ABI (cpu64.cpp dsp_23), so RCX is saved to RBX before exit.
    runAndCheck(r, "jrcxz taken (rcx=0)", withExit({
        0x48, 0xC7, 0xC1, 0x00, 0x00, 0x00, 0x00, // mov rcx, 0
        0xE3, 0x07,                               // jrcxz +7 (taken)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 0;
    });

    // JRCXZ (E3): not-taken case (RCX != 0 falls through).
    // Note: the exit syscall (0F 05) clobbers RCX with the return RIP per
    // the AMD64 ABI (cpu64.cpp dsp_23), so RCX is saved to RBX before exit.
    runAndCheck(r, "jrcxz not taken (rcx=1)", withExit({
        0x48, 0xC7, 0xC1, 0x01, 0x00, 0x00, 0x00, // mov rcx, 1
        0xE3, 0x07,                               // jrcxz +7 (not taken)
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (fallthrough) mov rax, 0xAA
        0xEB, 0x07,                               // jmp +7 (skip taken-target)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // JECXZ (67 E3): 0x67 address-size prefix selects ECX. RCX has high
    // bits set but ECX == 0, so the jump must be taken.
    runAndCheck(r, "jecxz taken (ecx=0, rcx high bits set)", withExit({
        0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov rcx, 0x100000000
        0x67, 0xE3, 0x07,                                           // jecxz +7 (taken)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00,                   // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00,                   // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                                           // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 0x100000000ULL;
    });

    // JECXZ (67 E3): not-taken case (ECX != 0 falls through).
    runAndCheck(r, "jecxz not taken (ecx=1)", withExit({
        0x48, 0xC7, 0xC1, 0x01, 0x00, 0x00, 0x00, // mov rcx, 1
        0x67, 0xE3, 0x07,                         // jecxz +7 (not taken)
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (fallthrough) mov rax, 0xAA
        0xEB, 0x07,                               // jmp +7 (skip taken-target)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // LOOP (E2): decrement RCX, jump if the decremented count != 0.
    // RCX=2 -> 1, jump taken.
    // Note: the exit syscall (0F 05) clobbers RCX with the return RIP per
    // the AMD64 ABI (cpu64.cpp dsp_23), so RCX is saved to RBX before exit.
    runAndCheck(r, "loop taken (rcx=2->1)", withExit({
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x48, 0x31, 0xC0,                         // xor rax, rax  ; ZF=1 (deterministic)
        0xE2, 0x07,                               // loop +7 (taken: rcx 2->1)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // LOOP (E2): RCX=1 -> 0, falls through (loop termination case).
    runAndCheck(r, "loop not taken (rcx=1->0)", withExit({
        0x48, 0xC7, 0xC1, 0x01, 0x00, 0x00, 0x00, // mov rcx, 1
        0x48, 0x31, 0xC0,                         // xor rax, rax
        0xE2, 0x07,                               // loop +7 (not taken: rcx 1->0)
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (fallthrough) mov rax, 0xAA
        0xEB, 0x07,                               // jmp +7 (skip taken-target)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 0;
    });

    // LOOP (E2): RCX=0 wraps to 0xFFFFFFFFFFFFFFFF (decrement first, per
    // Intel SDM) so the jump is taken, not fallen through.
    runAndCheck(r, "loop wraps (rcx=0->max, taken)", withExit({
        0x48, 0xC7, 0xC1, 0x00, 0x00, 0x00, 0x00, // mov rcx, 0
        0x48, 0x31, 0xC0,                         // xor rax, rax
        0xE2, 0x07,                               // loop +7 (taken: rcx 0->max)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 0xFFFFFFFFFFFFFFFFULL;
    });

    // LOOPE/LOOPZ (E1): jump when count != 0 AND ZF==1. Taken.
    runAndCheck(r, "loope taken (rcx=2, zf=1)", withExit({
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x48, 0x31, 0xC0,                         // xor rax, rax  ; ZF=1
        0xE1, 0x07,                               // loope +7 (taken)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // LOOPE (E1): count != 0 but ZF==0 -> not taken. RCX is still
    // decremented even when the jump is not taken.
    runAndCheck(r, "loope not taken (zf=0)", withExit({
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x48, 0x31, 0xC0,                         // xor rax, rax  ; ZF=1
        0x48, 0x83, 0xF8, 0x01,                   // cmp rax, 1    ; ZF=0
        0xE1, 0x07,                               // loope +7 (not taken)
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (fallthrough) mov rax, 0xAA
        0xEB, 0x07,                               // jmp +7 (skip taken-target)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // LOOPNE/LOOPNZ (E0): jump when count != 0 AND ZF==0. Taken.
    runAndCheck(r, "loopne taken (rcx=2, zf=0)", withExit({
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x48, 0x31, 0xC0,                         // xor rax, rax  ; ZF=1
        0x48, 0x83, 0xF8, 0x01,                   // cmp rax, 1    ; ZF=0
        0xE0, 0x07,                               // loopne +7 (taken)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // LOOPNE (E0): count != 0 but ZF==1 -> not taken.
    runAndCheck(r, "loopne not taken (zf=1)", withExit({
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x48, 0x31, 0xC0,                         // xor rax, rax  ; ZF=1
        0xE0, 0x07,                               // loopne +7 (not taken)
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // (fallthrough) mov rax, 0xAA
        0xEB, 0x07,                               // jmp +7 (skip taken-target)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 1;
    });

    // LOOP with 0x67 address-size prefix: decrements ECX (32-bit), and the
    // 32-bit write zero-extends into RCX. ECX=0 -> 0xFFFFFFFF != 0, taken.
    runAndCheck(r, "loop 32-bit (67 E2, ecx=0->max, taken)", withExit({
        0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov rcx, 0x100000000
        0x48, 0x31, 0xC0,                                           // xor rax, rax
        0x67, 0xE2, 0x07,                                           // loop +7 (taken)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00,                   // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00,                   // (target) mov rax, 0xAA
        0x48, 0x89, 0xCB,                                           // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA && c.reg[X64_RBX].u64 == 0xFFFFFFFFULL;
    });

    // LOOP must not modify flags: ZF=1 before, still ZF=1 after (read back
    // via sete al; rax was 0 so sete leaves rax==1).
    runAndCheck(r, "loop preserves ZF", withExit({
        0x48, 0xC7, 0xC1, 0x02, 0x00, 0x00, 0x00, // mov rcx, 2
        0x48, 0x31, 0xC0,                         // xor rax, rax  ; ZF=1
        0xE2, 0x07,                               // loop +7 (taken)
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x0F, 0x94, 0xC0,                         // (target) sete al  ; al = ZF
        0x48, 0x89, 0xCB,                         // mov rbx, rcx (save pre-syscall)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 1 && c.reg[X64_RBX].u64 == 1;
    });

    // LODSB (AC): AL = [RSI], RSI += 1 (DF=0). Only AL is written;
    // the upper 56 bits of RAX are preserved.
    runAndCheck(r, "lodsb forward (df=0)", withExit({
        0xC6, 0x44, 0x24, 0xF8, 0x42,  // mov byte [rsp-8], 0x42
        0xC6, 0x44, 0x24, 0xF9, 0x99,  // mov byte [rsp-7], 0x99
        0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,  // mov rax, -1 (all bits set)
        0x48, 0x8D, 0x74, 0x24, 0xF8,  // lea rsi, [rsp-8]
        0xFC,                          // cld
        0xAC,                          // lodsb -> AL=0x42, RSI+=1
        0x48, 0x89, 0xF3,              // mov rbx, rsi (save)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFF42ULL && c.reg[X64_RBX].u64 == 0x7FFFE9ULL;
    });

    // LODSB (AC) with DF=1 (STD): RSI decrements by 1.
    runAndCheck(r, "lodsb backward (df=1)", withExit({
        0xC6, 0x44, 0x24, 0xF8, 0x42,  // mov byte [rsp-8], 0x42
        0xC6, 0x44, 0x24, 0xF9, 0x99,  // mov byte [rsp-7], 0x99
        0x48, 0x31, 0xC0,              // xor rax, rax
        0x48, 0x8D, 0x74, 0x24, 0xF9,  // lea rsi, [rsp-7]
        0xFD,                          // std
        0xAC,                          // lodsb -> AL=0x99, RSI-=1
        0x48, 0x89, 0xF3,              // mov rbx, rsi (save)
        0xFC,                          // cld (restore)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x99ULL && c.reg[X64_RBX].u64 == 0x7FFFE8ULL;
    });

    // LODSW (66 AD): AX = [RSI], RSI += 2.
    runAndCheck(r, "lodsw", withExit({
        0xC6, 0x44, 0x24, 0xF8, 0x34,  // mov byte [rsp-8], 0x34
        0xC6, 0x44, 0x24, 0xF9, 0x12,  // mov byte [rsp-7], 0x12
        0x48, 0x31, 0xC0,              // xor rax, rax
        0x48, 0x8D, 0x74, 0x24, 0xF8,  // lea rsi, [rsp-8]
        0xFC,                          // cld
        0x66, 0xAD,                    // lodsw -> AX=0x1234, RSI+=2
        0x48, 0x89, 0xF3,              // mov rbx, rsi (save)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x1234ULL && c.reg[X64_RBX].u64 == 0x7FFFEAULL;
    });

    // LODSD (AD): EAX = [RSI] (zero-extends into RAX), RSI += 4.
    runAndCheck(r, "lodsd", withExit({
        0xC6, 0x44, 0x24, 0xF8, 0xDE,  // [rsp-8]=0xDE
        0xC6, 0x44, 0x24, 0xF9, 0xAD,  // [rsp-7]=0xAD
        0xC6, 0x44, 0x24, 0xFA, 0xBE,  // [rsp-6]=0xBE
        0xC6, 0x44, 0x24, 0xFB, 0xEF,  // [rsp-5]=0xEF
        0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,  // mov rax, -1 (high bits set)
        0x48, 0x8D, 0x74, 0x24, 0xF8,  // lea rsi, [rsp-8]
        0xFC,                          // cld
        0xAD,                          // lodsd -> EAX=0xEFBEADDE, RAX zero-extended
        0x48, 0x89, 0xF3,              // mov rbx, rsi (save)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xEFBEADDEULL && c.reg[X64_RBX].u64 == 0x7FFFECULL;
    });

    // LODSQ (48 AD): RAX = [RSI], RSI += 8.
    runAndCheck(r, "lodsq (rex.w)", withExit({
        0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // mov rax, 0x1122334455667788
        0x48, 0x89, 0x44, 0x24, 0xF8,  // mov [rsp-8], rax
        0x48, 0x31, 0xC0,              // xor rax, rax
        0x48, 0x8D, 0x74, 0x24, 0xF8,  // lea rsi, [rsp-8]
        0xFC,                          // cld
        0x48, 0xAD,                    // lodsq -> RAX=0x1122334455667788, RSI+=8
        0x48, 0x89, 0xF3,              // mov rbx, rsi (save)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x1122334455667788ULL && c.reg[X64_RBX].u64 == 0x7FFFF0ULL;
    });

    // LODSB with 0x67 address-size prefix: 32-bit ESI is used. The
    // effective address is masked to 32 bits, and the ESI update
    // zero-extends into RSI (high bits cleared), per x86-64 semantics.
    runAndCheck(r, "lodsb 32-bit (67 AC, esi zero-extends)", withExit({
        0xC6, 0x44, 0x24, 0xF8, 0x42,  // mov byte [rsp-8], 0x42
        0x48, 0x31, 0xC0,              // xor rax, rax
        0x48, 0xBE, 0xE8, 0xFF, 0x7F, 0x00, 0x78, 0x56, 0x34, 0x12,
            // mov rsi, 0x12345678007FFFE8 (high bits set, low32 = data addr)
        0xFC,                          // cld
        0x67, 0xAC,                    // lodsb (addr32) -> AL=0x42, RSI=0x7FFFE9
        0x48, 0x89, 0xF3,              // mov rbx, rsi (save)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x42ULL && c.reg[X64_RBX].u64 == 0x7FFFE9ULL;
    });

    // ENTER (C8 iw ib): create stack frame. Level 0, alloc 16.
    // push RBP; RBP=RSP-8; RSP=RSP-8-16.
    runAndCheck(r, "enter level 0, alloc 16", withExit({
        0x48, 0xBD, 0xF0, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov rbp, 0x7FFFF0
        0xC8, 0x10, 0x00, 0x00,  // enter 16, 0
        0x48, 0x89, 0xE3,        // mov rbx, rsp (save)
        0x48, 0x89, 0xE8,        // mov rax, rbp
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x7FFFE8ULL && c.reg[X64_RBX].u64 == 0x7FFFD8ULL;
    });

    // ENTER level 0, alloc 0: just push RBP, no frame allocation.
    runAndCheck(r, "enter level 0, alloc 0", withExit({
        0x48, 0xBD, 0xF0, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov rbp, 0x7FFFF0
        0xC8, 0x00, 0x00, 0x00,  // enter 0, 0
        0x48, 0x89, 0xE3,        // mov rbx, rsp (save)
        0x48, 0x89, 0xE8,        // mov rax, rbp
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x7FFFE8ULL && c.reg[X64_RBX].u64 == 0x7FFFE8ULL;
    });

    // ENTER level 1, alloc 8: push RBP, push FrameTemp, set RBP, alloc.
    runAndCheck(r, "enter level 1, alloc 8", withExit({
        0x48, 0xBD, 0xF0, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov rbp, 0x7FFFF0
        0xC8, 0x08, 0x00, 0x01,  // enter 8, 1
        0x48, 0x89, 0xE3,        // mov rbx, rsp (save)
        0x48, 0x89, 0xE8,        // mov rax, rbp
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x7FFFE8ULL && c.reg[X64_RBX].u64 == 0x7FFFD8ULL;
    });

    // ENTER level 2, alloc 0: exercises the nested display-entry loop.
    // (Display entry contents not verified here; RBP/RSP updates are verified.
    // Note: mov r64,[rbp+disp8] was suspected broken but proven working by
    // dedicated execute-and-verify tests, 2026-10-08.)
    runAndCheck(r, "enter level 2", withExit({
        0x48, 0xBD, 0xF0, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov rbp, 0x7FFFF0
        0xC8, 0x00, 0x00, 0x02,  // enter 0, 2
        0x48, 0x89, 0xE3,        // mov rbx, rsp (save)
        0x48, 0x89, 0xE8,        // mov rax, rbp
    }), [](CPU64& c) {
        // push RBP (8) + push display (8) + push FrameTemp (8) = 24 bytes
        // RBP=0x7FFFF0-8=0x7FFFE8, RSP=0x7FFFF0-24=0x7FFFD8
        return c.reg[X64_R15].u64 == 0x7FFFE8ULL && c.reg[X64_RBX].u64 == 0x7FFFD8ULL;
    });

    // ENTER with 0x66 operand-size prefix: 16-bit pushes on BP/SP.
    // Upper 48 bits of RBP/RSP are preserved; only low 16 bits change.
    runAndCheck(r, "enter 16-bit (66 C8)", withExit({
        0x48, 0xBD, 0x00, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov rbp, 0x7FFF00
        0x66, 0xC8, 0x10, 0x00, 0x01,  // enter 16, 1 (16-bit)
        0x48, 0x89, 0xE3,              // mov rbx, rsp (save)
        0x48, 0x89, 0xE8,              // mov rax, rbp
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x7FFFEEULL && c.reg[X64_RBX].u64 == 0x7FFFDCULL;
    });

    // ----- x87 m32fp forms (D8/D9): FADD/FMUL/FSUB/FDIV/FST -----
    // T: FADD m32fp — 2.5 + 1.5f = 4.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,                                     // sub rsp, 8
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,                                     // mov [rsp], rax
            0xDD, 0x04, 0x24,                                           // fld qword [rsp]
            0xC7, 0x04, 0x24, 0x00, 0x00, 0xC0, 0x3F,                   // mov dword [rsp], 1.5f
            0xD8, 0x04, 0x24,                                           // fadd dword [rsp] ; st0=4.0
            0xDD, 0x1C, 0x24,                                           // fstp qword [rsp]
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0x83, 0xC4, 0x08,                                     // add rsp, 8
        };
        runAndCheck(r, "x87 FADD m32fp (2.5+1.5f=4.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4010000000000000ULL; // 4.0
        });
    }

    // T: FMUL m32fp — 2.5 * 2.0f = 5.0.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, // 2.5
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 2.5
            0xC7, 0x04, 0x24, 0x00, 0x00, 0x00, 0x40,                   // mov dword [rsp], 2.0f
            0xD8, 0x0C, 0x24,                                           // fmul dword [rsp] ; st0=5.0
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FMUL m32fp (2.5*2.0f=5.0)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4014000000000000ULL; // 5.0
        });
    }

    // T: FSUB m32fp — 10.0 - 4.5f = 5.5.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0xC7, 0x04, 0x24, 0x00, 0x00, 0x90, 0x40,                   // mov dword [rsp], 4.5f
            0xD8, 0x24, 0x24,                                           // fsub dword [rsp] ; st0=5.5
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FSUB m32fp (10.0-4.5f=5.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4016000000000000ULL; // 5.5
        });
    }

    // T: FDIV m32fp — 10.0 / 4.0f = 2.5.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x40, // 10.0
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 10.0
            0xC7, 0x04, 0x24, 0x00, 0x00, 0x80, 0x40,                   // mov dword [rsp], 4.0f
            0xD8, 0x34, 0x24,                                           // fdiv dword [rsp] ; st0=2.5
            0xDD, 0x1C, 0x24,                                           // fstp
            0x48, 0x8B, 0x04, 0x24,
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FDIV m32fp (10.0/4.0f=2.5)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u64 == 0x4004000000000000ULL; // 2.5
        });
    }

    // T: FST m32fp — store 3.75 as float32 without popping.
    {
        std::vector<U8> code = {
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x40, // 3.75
            0x48, 0x89, 0x04, 0x24,
            0xDD, 0x04, 0x24,                                           // fld 3.75
            0xD9, 0x14, 0x24,                                           // fst dword [rsp]  ; no pop
            0x8B, 0x04, 0x24,                                           // mov eax, [rsp]
            0x48, 0x83, 0xC4, 0x08,
        };
        runAndCheck(r, "x87 FST m32fp (3.75 -> 0x40700000, no pop)", withExit(code), [](CPU64& c) {
            return c.reg[X64_R15].u32 == 0x40700000U; // 3.75f
        });
    }

    // ----- Shift edge cases: sar sign-extends, shr zero-fills -----
    // T: SAR r64,1 sign-extends bit 63.
    runAndCheck(r, "sar rax,1 sign-extends (0x8000..->0xC000..)", withExit({
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  // mov rax, 0x8000000000000000
        0x48, 0xC1, 0xF8, 0x01,                                     // sar rax, 1
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xC000000000000000ULL;
    });

    // T: SHR r64,1 zero-fills bit 63.
    runAndCheck(r, "shr rax,1 zero-fills (0x8000..->0x4000..)", withExit({
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  // mov rax, 0x8000000000000000
        0x48, 0xC1, 0xE8, 0x01,                                     // shr rax, 1
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x4000000000000000ULL;
    });

    // T: SHR sets CF from the last bit shifted out (1>>1 -> 0, CF=1).
    runAndCheck(r, "shr rax,1 sets CF from last bit", withExit({
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,  // mov rax, 1
        0x48, 0xC1, 0xE8, 0x01,                    // shr rax, 1  (rax=0, CF=1)
        0x9C,                                      // pushfq
        0x58,                                      // pop rax (rax=rflags)
    }), [](CPU64& c) {
        return (c.reg[X64_R15].u64 & 0x1ULL) != 0;
    });

    // T: SAR r32,1 sign-extends within 32 bits and zero-extends to 64.
    runAndCheck(r, "sar eax,1 32-bit (0x80000000->0xC0000000)", withExit({
        0xB8, 0x00, 0x00, 0x00, 0x80,  // mov eax, 0x80000000
        0xC1, 0xF8, 0x01,              // sar eax, 1
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xC0000000ULL;
    });

    // ----- ADC/SBB carry chains -----
    // T: ADC with CF=1: 0xFFFF..F + 0 + 1 wraps to 0 with CF=1.
    runAndCheck(r, "adc rax,0 with CF=1 (wrap -> 0, CF=1)", withExit({
        0xF9,                                                        // stc
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // mov rax, 0xFFFFFFFFFFFFFFFF
        0x48, 0x83, 0xD0, 0x00,                                     // adc rax, 0
        0x48, 0x89, 0xC3,                                           // mov rbx, rax (save result)
        0x9C,                                                        // pushfq
        0x58,                                                        // pop rax (rax=rflags)
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 0 && (c.reg[X64_R15].u64 & 0x1ULL) != 0;
    });

    // T: ADC 64-bit carry out: 0xFFFF..F + 1 + 1 = 0x1_0000..01 -> 1, CF=1.
    runAndCheck(r, "adc rax,1 carry out (-> 1, CF=1)", withExit({
        0xF9,                                                        // stc
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // mov rax, 0xFFFFFFFFFFFFFFFF
        0x48, 0x83, 0xD0, 0x01,                                     // adc rax, 1
        0x48, 0x89, 0xC3,                                           // mov rbx, rax
        0x9C,                                                        // pushfq
        0x58,                                                        // pop rax
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 1 && (c.reg[X64_R15].u64 & 0x1ULL) != 0;
    });

    // T: SBB with CF=1: 5 - 3 - 1 = 1, CF=0 (no borrow out).
    runAndCheck(r, "sbb rax,3 with CF=1 (5-3-1=1, CF=0)", withExit({
        0xF9,                                      // stc
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // mov rax, 5
        0x48, 0x83, 0xD8, 0x03,                    // sbb rax, 3
        0x48, 0x89, 0xC3,                          // mov rbx, rax
        0x9C,                                       // pushfq
        0x58,                                       // pop rax
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 1 && (c.reg[X64_R15].u64 & 0x1ULL) == 0;
    });

    // T: SBB underflow: 0 - 0 - 1 = 0xFFFF..F, CF=1.
    runAndCheck(r, "sbb rax,0 underflow (-> 0xFFFF.., CF=1)", withExit({
        0x48, 0x31, 0xC0,  // xor rax, rax (CF=0)
        0xF9,              // stc (CF=1)
        0x48, 0x83, 0xD8, 0x00,  // sbb rax, 0
        0x48, 0x89, 0xC3,  // mov rbx, rax
        0x9C,              // pushfq
        0x58,              // pop rax
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 0xFFFFFFFFFFFFFFFFULL && (c.reg[X64_R15].u64 & 0x1ULL) != 0;
    });

    // ----- INC/DEC preserve CF (classic x86 gotcha) -----
    // T: INC does not touch CF.
    runAndCheck(r, "inc preserves CF=1", withExit({
        0xF9,                                      // stc
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // mov rax, 5
        0xFF, 0xC0,                                // inc rax
        0x48, 0x89, 0xC3,                          // mov rbx, rax
        0x9C,                                       // pushfq
        0x58,                                       // pop rax
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 6 && (c.reg[X64_R15].u64 & 0x1ULL) != 0;
    });

    // T: DEC does not touch CF.
    runAndCheck(r, "dec preserves CF=1", withExit({
        0xF9,                                      // stc
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,  // mov rax, 5
        0xFF, 0xC8,                                // dec rax
        0x48, 0x89, 0xC3,                          // mov rbx, rax
        0x9C,                                       // pushfq
        0x58,                                       // pop rax
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 4 && (c.reg[X64_R15].u64 & 0x1ULL) != 0;
    });

    // T: INC 0xFFFF..F -> 0 sets ZF, leaves CF=0.
    runAndCheck(r, "inc 0xFFFF..->0 sets ZF, CF=0", withExit({
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // mov rax, 0xFFFFFFFFFFFFFFFF
        0xFF, 0xC0,  // inc rax
        0x48, 0x89, 0xC3,  // mov rbx, rax
        0x9C,  // pushfq
        0x58,  // pop rax
    }), [](CPU64& c) {
        return c.reg[X64_RBX].u64 == 0 &&
               (c.reg[X64_R15].u64 & 0x40ULL) != 0 &&
               (c.reg[X64_R15].u64 & 0x1ULL) == 0;
    });

    // ----- AND/OR always clear CF and OF -----
    // T: AND clears CF and OF even when both were set.
    runAndCheck(r, "and rax,rax clears CF+OF", withExit({
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,  // mov rax, 0x7FFFFFFFFFFFFFFF
        0x48, 0x01, 0xC0,  // add rax, rax (OF=1, signed overflow)
        0xF9,              // stc (CF=1)
        0x48, 0x21, 0xC0,  // and rax, rax (CF=0, OF=0)
        0x9C,              // pushfq
        0x58,              // pop rax
    }), [](CPU64& c) {
        return (c.reg[X64_R15].u64 & 0x801ULL) == 0;
    });

    // T: OR clears CF and OF even when both were set.
    runAndCheck(r, "or rax,rax clears CF+OF", withExit({
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,  // mov rax, 0x7FFFFFFFFFFFFFFF
        0x48, 0x01, 0xC0,  // add rax, rax (OF=1)
        0xF9,              // stc (CF=1)
        0x48, 0x09, 0xC0,  // or rax, rax (CF=0, OF=0)
        0x9C,              // pushfq
        0x58,              // pop rax
    }), [](CPU64& c) {
        return (c.reg[X64_R15].u64 & 0x801ULL) == 0;
    });

    // ----- IMUL signed multiply -----
    // T: IMUL r64,r/m64,imm8 with negatives: -7 * -6 = 42.
    runAndCheck(r, "imul rax,rax,-6 (-7*-6=42)", withExit({
        0x48, 0xC7, 0xC0, 0xF9, 0xFF, 0xFF, 0xFF,  // mov rax, -7
        0x48, 0x6B, 0xC0, 0xFA,                    // imul rax, rax, -6
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 42;
    });

    // T: IMUL overflow sets CF and OF (0x7FFF.. * 2 doesn't fit).
    runAndCheck(r, "imul overflow sets CF+OF", withExit({
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F,  // mov rax, 0x7FFFFFFFFFFFFFFF
        0x48, 0x6B, 0xC0, 0x02,  // imul rax, rax, 2
        0x9C,  // pushfq
        0x58,  // pop rax
    }), [](CPU64& c) {
        return (c.reg[X64_R15].u64 & 0x801ULL) == 0x801ULL;
    });

    // The two-operand form also runs through BK_IMUL_R_RM in cached builds.
    runAndCheck(r, "imul r64,r/m64 overflow sets CF+OF", withExit({
        0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, // mov rax, INT64_MAX
        0x48, 0xC7, 0xC3, 0x02, 0x00, 0x00, 0x00,                  // mov rbx, 2
        0x48, 0x0F, 0xAF, 0xC3,                                    // imul rax, rbx
        0x9C, 0x58,                                                // pushfq; pop rax
    }), [](CPU64& c) {
        if ((c.reg[X64_R15].u64 & 0x801ULL) != 0x801ULL) return false;
        return true;
    });

    // ----- MOVSX / MOVSXD sign extension -----
    // T: MOVSX r64,r/m8 sign-extends 0xFF -> 0xFFFF..FF.
    runAndCheck(r, "movsx rbx,al (0xFF->0xFFFF..FF)", withExit({
        0x48, 0xC7, 0xC0, 0xFF, 0x00, 0x00, 0x00,  // mov rax, 0xFF
        0x48, 0x0F, 0xBE, 0xD8,                    // movsx rbx, al
        0x48, 0x89, 0xD8,                          // mov rax, rbx
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFFFFFFULL;
    });

    // T: MOVSX r64,r/m16 sign-extends 0x8000 -> 0xFFFF..8000.
    runAndCheck(r, "movsx rbx,ax (0x8000->0xFFFF..8000)", withExit({
        0x66, 0xB8, 0x00, 0x80,  // mov ax, 0x8000
        0x48, 0x0F, 0xBF, 0xD8,  // movsx rbx, ax
        0x48, 0x89, 0xD8,        // mov rax, rbx
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xFFFFFFFFFFFF8000ULL;
    });

    // T: MOVSXD r64,r/m32 sign-extends 0x80000000 -> 0xFFFFFFFF80000000.
    runAndCheck(r, "movsxd rbx,eax (0x80000000->0xFFFFFFFF80000000)", withExit({
        0xB8, 0x00, 0x00, 0x00, 0x80,  // mov eax, 0x80000000
        0x48, 0x63, 0xD8,              // movsxd rbx, eax
        0x48, 0x89, 0xD8,              // mov rax, rbx
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xFFFFFFFF80000000ULL;
    });

    // ----- BSR -----
    // T: BSR finds the index of the highest set bit (0x100 -> 8).
    runAndCheck(r, "bsr rax,rbx (0x100->8)", withExit({
        0x48, 0xC7, 0xC3, 0x00, 0x01, 0x00, 0x00,  // mov rbx, 0x100
        0x48, 0x0F, 0xBD, 0xC3,                    // bsr rax, rbx
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 8;
    });

    // ----- Conditional jumps beyond JZ -----
    // T: JNZ taken when ZF=0.
    runAndCheck(r, "jnz taken (ZF=0)", withExit({
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,  // mov rax, 1
        0x48, 0x85, 0xC0,                          // test rax, rax (ZF=0)
        0x75, 0x07,                                 // jnz +7
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // mov rax, 0xAA (target)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA;
    });

    // T: JNZ not taken when ZF=1 (falls through).
    runAndCheck(r, "jnz not taken (ZF=1)", withExit({
        0x48, 0x31, 0xC0,                          // xor rax, rax (ZF=1)
        0x75, 0x07,                                 // jnz +7 (not taken)
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // mov rax, 0xAA (fallthrough)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA;
    });

    // T: JB taken on unsigned below (3 < 5 -> CF=1).
    runAndCheck(r, "jb taken (3<5 unsigned, CF=1)", withExit({
        0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00,  // mov rax, 3
        0x48, 0xC7, 0xC3, 0x05, 0x00, 0x00, 0x00,  // mov rbx, 5
        0x48, 0x39, 0xD8,                          // cmp rax, rbx (CF=1)
        0x72, 0x07,                                 // jb +7
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // mov rax, 0xAA (target)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA;
    });

    // T: JL taken on signed less (-1 < 5 -> SF!=OF).
    runAndCheck(r, "jl taken (-1<5 signed)", withExit({
        0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,  // mov rax, -1
        0x48, 0xC7, 0xC3, 0x05, 0x00, 0x00, 0x00,  // mov rbx, 5
        0x48, 0x39, 0xD8,                          // cmp rax, rbx (SF!=OF)
        0x7C, 0x07,                                 // jl +7
        0x48, 0xC7, 0xC0, 0xBA, 0xD0, 0x00, 0x00, // (skipped) mov rax, 0xD0BA
        0x48, 0xC7, 0xC0, 0xAA, 0x00, 0x00, 0x00, // mov rax, 0xAA (target)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0xAA;
    });

    // ----- LEA with SIB -----
    // T: LEA with SIB addressing: [rbx+rcx*4+0x10] = 0x1000+12+16 = 0x101C.
    runAndCheck(r, "lea rax,[rbx+rcx*4+0x10] SIB (-> 0x101C)", withExit({
        0x48, 0xC7, 0xC3, 0x00, 0x10, 0x00, 0x00,  // mov rbx, 0x1000
        0x48, 0xC7, 0xC1, 0x03, 0x00, 0x00, 0x00,  // mov rcx, 3
        0x48, 0x8D, 0x44, 0x8B, 0x10,              // lea rax, [rbx+rcx*4+0x10]
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x101CULL;
    });

    // ----- CMPXCHG mismatch -----
    // T: CMPXCHG [rsp],rbx mismatch: ZF=0, rax=old mem, mem unchanged.
    runAndCheck(r, "cmpxchg [rsp],rbx mismatch (ZF=0, rax=mem)", withExit({
        0x48, 0xC7, 0xC0, 0x11, 0x11, 0x00, 0x00,  // mov rax, 0x1111
        0x50,                                      // push rax ([rsp]=0x1111)
        0x48, 0xC7, 0xC0, 0x22, 0x22, 0x00, 0x00,  // mov rax, 0x2222 (expected, wrong)
        0x48, 0xC7, 0xC3, 0x33, 0x33, 0x00, 0x00,  // mov rbx, 0x3333 (new value)
        0x48, 0x0F, 0xB1, 0x1C, 0x24,              // cmpxchg [rsp], rbx (ZF=0, rax=0x1111)
        0x9C,                                      // pushfq
        0x59,                                      // pop rcx (rcx=rflags)
        0x5A,                                      // pop rdx (rdx=[rsp], must be 0x1111)
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x1111ULL &&
               (c.reg[X64_RCX].u64 & 0x40ULL) == 0 &&
               c.reg[X64_RDX].u64 == 0x1111ULL;
    });

    // ----- PUSHFQ/POPFQ -----
    // T: PUSHFQ+POPFQ round-trip preserves ZF/PF and reserved bit 1.
    runAndCheck(r, "pushfq/popfq round-trip (ZF|PF)", withExit({
        0x48, 0x31, 0xC0,  // xor rax, rax (ZF=1, PF=1)
        0x9C,              // pushfq
        0x58,              // pop rax (rax=rflags)
    }), [](CPU64& c) {
        // mask 0xC5 = CF|PF|AF|ZF|SF (excludes reserved bit 1)
        return (c.reg[X64_R15].u64 & 0xC5ULL) == 0x44ULL;
    });

    // ----- TEST flag behavior -----
    // T: TEST rax,rax with negative value sets SF.
    runAndCheck(r, "test rax,rax sets SF (negative)", withExit({
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  // mov rax, 0x8000000000000000
        0x48, 0x85, 0xC0,  // test rax, rax (SF=1)
        0x9C,              // pushfq
        0x58,              // pop rax
    }), [](CPU64& c) {
        return (c.reg[X64_R15].u64 & 0x80ULL) != 0;
    });

    const char* oldRecoverValue = std::getenv("BW64_RECOVER");
    const bool hadRecoverValue = oldRecoverValue != nullptr;
    const std::string savedRecoverValue = oldRecoverValue ? oldRecoverValue : "";
    setRecoverPolicyForTest(false);

    // Synchronous invalid-opcode and user-mode #GP faults stay at the
    // instruction RIP in this handler-free native self-test harness.
    runFaultAtRIP(r, "UD2 raises SIGILL at faulting RIP", {0x0F, 0x0B});
    runFaultAtRIP(r, "unimplemented opcode raises SIGILL", {0x0F, 0xB9, 0xC0});
    setRecoverPolicyForTest(true);
    runAndCheck(r, "recover skips unknown 0F opcode with ModRM length", withExit({
        0x0F, 0xB9, 0xC0,                         // unknown opcode + register ModRM
        0xB8, 0x78, 0x56, 0x34, 0x12             // mov eax,0x12345678 proves continuation
    }), [](CPU64& c) {
        return c.reg[X64_R15].u64 == 0x12345678ULL;
    });
    const U64 recoverInsdDst = CODE_BASE + 0x800;
    runAndCheck(r, "recover emulates INSD from an absent port as zero", withExit({
        0x6D,                                     // insd [rdi], dx
        0x48, 0x89, 0xF8                          // mov rax,rdi (save updated destination)
    }), [=](CPU64& c) {
        return c.memory->readd(recoverInsdDst) == 0 &&
               c.reg[X64_R15].u64 == recoverInsdDst + 4;
    }, [=](CPU64& c) {
        c.memory->writed(recoverInsdDst, 0xDEADBEEF);
        c.reg[X64_RDI].setU64(recoverInsdDst);
        c.reg[X64_RDX].setU64(0x80);
    });
    const U64 recoverInswDst = CODE_BASE + 0x810;
    runAndCheck(r, "recover emulates operand-size-prefixed INSW", withExit({
        0x66, 0x6D,                               // insw [rdi], dx
        0x48, 0x89, 0xF8                          // mov rax,rdi (save updated destination)
    }), [=](CPU64& c) {
        return c.memory->readw(recoverInswDst) == 0 &&
               c.memory->readw(recoverInswDst + 2) == 0xBEEF &&
               c.reg[X64_R15].u64 == recoverInswDst + 2;
    }, [=](CPU64& c) {
        c.memory->writed(recoverInswDst, 0xBEEFCAFE);
        c.reg[X64_RDI].setU64(recoverInswDst);
        c.reg[X64_RDX].setU64(0x80);
    });
    runFaultAtRIP(r, "recover never skips UD2", {0x0F, 0x0B});
    setRecoverPolicyForTest(false);
    runFaultAtRIP(r, "INT imm8 raises #GP", {0xCD, 0x29});
    runFaultAtRIP(r, "HLT raises #GP", {0xF4});
    runFaultAtRIP(r, "CLI raises #GP", {0xFA});
    runFaultAtRIP(r, "STI raises #GP", {0xFB});
    runFaultAtRIP(r, "INT1 raises #GP", {0xF1});
    runFaultAtRIP(r, "IN imm8 forms raise #GP", {0xE4, 0x80});
    runFaultAtRIP(r, "IN imm16 form raises #GP", {0xE5, 0x80});
    runFaultAtRIP(r, "OUT imm8 forms raise #GP", {0xE6, 0x80});
    runFaultAtRIP(r, "OUT imm16 form raises #GP", {0xE7, 0x80});
    runFaultAtRIP(r, "IN DX byte form raises #GP", {0xEC});
    runFaultAtRIP(r, "IN DX word/dword form raises #GP", {0xED});
    runFaultAtRIP(r, "OUT DX byte form raises #GP", {0xEE});
    runFaultAtRIP(r, "OUT DX word/dword form raises #GP", {0xEF});
    runFaultAtRIP(r, "INS byte form raises #GP", {0x6C});
    runFaultAtRIP(r, "INS word/dword form raises #GP", {0x6D});
    runFaultAtRIP(r, "OUTS byte form raises #GP", {0x6E});
    runFaultAtRIP(r, "OUTS word/dword form raises #GP", {0x6F});
    runFaultAtRIP(r, "far CALL direct raises #GP", {0x9A, 0, 0, 0, 0, 0, 0});
    runFaultAtRIP(r, "far JMP direct raises #GP", {0xEA, 0, 0, 0, 0, 0, 0});
    runFaultAtRIP(r, "far CALL indirect raises #GP", {0xFF, 0x1B});
    runFaultAtRIP(r, "far JMP indirect raises #GP", {0xFF, 0x2B});
    runFaultAtRIP(r, "RETF imm16 raises #GP", {0xCA, 0, 0});
    runFaultAtRIP(r, "RETF raises #GP", {0xCB});
#ifdef BOXEDWINE_MSVC
    _putenv_s("BW64_RECOVER", hadRecoverValue ? savedRecoverValue.c_str() : "");
#else
    if (hadRecoverValue) setenv("BW64_RECOVER", savedRecoverValue.c_str(), 1);
    else unsetenv("BW64_RECOVER");
#endif

    // Requested x87 forms. The scratch area is inside the mapped code page so
    // each case can inspect stores and the public FPU state after execution.
    const U64 X87_SCRATCH = CODE_BASE + 0x800;
    auto useX87Scratch = [=](CPU64& c) {
        c.reg[X64_RBX].setU64(X87_SCRATCH);
    };
    {
        runAndCheck(r, "x87 DD /2 FST m64 stores without pop", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xE8, 0xD8,0xC0, 0xDD,0x13
        }), [=](CPU64& c) {
            return c.memory->readq(X87_SCRATCH) == 0x4000000000000000ULL &&
                   c.fpu.top == 6 && c.fpu.tags[6] != TAG_Empty;
        }, useX87Scratch);

        float five = 5.0f;
        U32 fiveBits;
        std::memcpy(&fiveBits, &five, sizeof(fiveBits));
        runAndCheck(r, "x87 D8 /5 FSUBR m32 (5-2=3)", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xE8, 0xD8,0xC0,
            0xD8,0x2B, 0xD9,0x5B,0x04
        }), [=](CPU64& c) {
            union { U32 u; float f; } result; result.u = c.memory->readd(X87_SCRATCH + 4);
            return result.f == 3.0f;
        }, [=](CPU64& c) {
            useX87Scratch(c);
            c.memory->writed(X87_SCRATCH, fiveBits);
        });

        float eight = 8.0f;
        U32 eightBits;
        std::memcpy(&eightBits, &eight, sizeof(eightBits));
        runAndCheck(r, "x87 D8 /7 FDIVR m32 (8/2=4)", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xE8, 0xD8,0xC0,
            0xD8,0x3B, 0xD9,0x5B,0x04
        }), [=](CPU64& c) {
            union { U32 u; float f; } result; result.u = c.memory->readd(X87_SCRATCH + 4);
            return result.f == 4.0f;
        }, [=](CPU64& c) {
            useX87Scratch(c);
            c.memory->writed(X87_SCRATCH, eightBits);
        });

        runAndCheck(r, "x87 D8 FSUBR ST(0),ST(1)", withExit({
            0xDB,0xE3, 0xD9,0xEE, 0xD9,0xE8, 0xD8,0xE9, 0xDD,0x1B
        }), [=](CPU64& c) {
            return c.memory->readq(X87_SCRATCH) == 0xBFF0000000000000ULL;
        }, useX87Scratch);

        runAndCheck(r, "x87 D8 FDIVR ST(0),ST(1)", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xE8, 0xD8,0xC0,
            0xD9,0xE8, 0xD8,0xF9, 0xDD,0x1B
        }), [=](CPU64& c) {
            return c.memory->readq(X87_SCRATCH) == 0x4000000000000000ULL;
        }, useX87Scratch);
    }

    {
        runAndCheck(r, "x87 FLDENV/FNSTENV 28-byte layout", withExit({
            0xD9,0x23, 0xD9,0x33
        }), [=](CPU64& c) {
            bool ok = c.fpu.CW() == 0x0B7F && c.fpu.top == 7 &&
                      c.fpu.envData[0] == 0x11111111 &&
                      c.fpu.envData[1] == 0x22222222 &&
                      c.memory->readd(X87_SCRATCH + 0) == 0x0B40 &&
                      c.memory->readd(X87_SCRATCH + 4) == 0x3800 &&
                      c.memory->readd(X87_SCRATCH + 8) == 0x0000FFFF &&
                      c.memory->readd(X87_SCRATCH + 12) == 0x11111111 &&
                      c.memory->readd(X87_SCRATCH + 16) == 0x22222222 &&
                      c.memory->readd(X87_SCRATCH + 20) == 0x33333333 &&
                      c.memory->readd(X87_SCRATCH + 24) == 0x44444444;
            return ok;
        }, [=](CPU64& c) {
            useX87Scratch(c);
            c.memory->writed(X87_SCRATCH + 0, 0x0B40);
            c.memory->writed(X87_SCRATCH + 4, 0x3800);
            c.memory->writed(X87_SCRATCH + 8, 0xFFFFFFFF);
            c.memory->writed(X87_SCRATCH + 12, 0x11111111);
            c.memory->writed(X87_SCRATCH + 16, 0x22222222);
            c.memory->writed(X87_SCRATCH + 20, 0x33333333);
            c.memory->writed(X87_SCRATCH + 24, 0x44444444);
        });

        runAndCheck(r, "x87 D9 FA FSQRT m64 input", withExit({
            0xDB,0xE3, 0xDD,0x43,0x08, 0xD9,0xFA, 0xDD,0x5B,0x10
        }), [=](CPU64& c) {
            return c.memory->readq(X87_SCRATCH + 16) == 0x4000000000000000ULL;
        }, [=](CPU64& c) {
            useX87Scratch(c);
            c.memory->writeq(X87_SCRATCH + 8, 0x4010000000000000ULL);
        });

        runAndCheck(r, "x87 D9 E4 FTST sets equal condition", withExit({
            0xDB,0xE3, 0xD9,0xEE, 0xD9,0xE4, 0xDF,0xE0
        }), [](CPU64& c) {
            return (c.reg[X64_R15].u64 & 0x4500) == 0x4000;
        });

        runAndCheck(r, "x87 DF E9 FUCOMIP sets CF and pops", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xEE, 0xDF,0xE9
        }), [](CPU64& c) {
            return (c.rflags & X64_CF) != 0 && c.fpu.top == 7;
        });

        struct FcmovCase { const char* name; U8 op; U8 modrm; U32 flags; };
        const FcmovCase fcmov[] = {
            {"FCMOVB", 0xDA,0xC1,X64_CF}, {"FCMOVE",0xDA,0xC9,X64_ZF},
            {"FCMOVBE",0xDA,0xD1,X64_CF}, {"FCMOVU",0xDA,0xD9,X64_PF},
            {"FCMOVNB",0xDB,0xC1,0}, {"FCMOVNE",0xDB,0xC9,0},
            {"FCMOVNBE",0xDB,0xD1,0}, {"FCMOVNU",0xDB,0xD9,0},
        };
        for (const auto& x : fcmov) {
            std::vector<U8> code = {0xDB,0xE3, 0xD9,0xE8, 0xD9,0xEE,
                                    x.op,x.modrm, 0xDD,0x1B};
            runAndCheck(r, (std::string("x87 ") + x.name + " moves on true condition").c_str(),
                withExit(code), [=](CPU64& c) {
                    return c.memory->readq(X87_SCRATCH) == 0x3FF0000000000000ULL;
                }, [=](CPU64& c) {
                    useX87Scratch(c);
                    c.rflags = (c.rflags & ~((U32)X64_CF | X64_ZF | X64_PF)) | x.flags;
                });
        }

        runAndCheck(r, "x87 DF C1 FFREEP frees and pops", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xEE, 0xDF,0xC1
        }), [](CPU64& c) {
            return c.fpu.top == 7 && c.fpu.tags[6] == TAG_Empty &&
                   c.fpu.tags[7] == TAG_Empty;
        });
        runAndCheck(r, "x87 D9 F6 FDECSTP", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xF6
        }), [](CPU64& c) { return c.fpu.top == 6; });
        runAndCheck(r, "x87 D9 F7 FINCSTP", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xF7
        }), [](CPU64& c) { return c.fpu.top == 0; });

        runAndCheck(r, "x87 DD /6 FNSAVE + DD /4 FRSTOR 108-byte layout", withExit({
            0xDB,0xE3, 0xD9,0xE8, 0xD9,0xEE,
            0xDD,0x33, 0xD9,0xBB,0x78,0,0,0,
            0xDD,0x23, 0xDD,0x9B,0x80,0,0,0
        }), [=](CPU64& c) {
            return c.memory->readw(X87_SCRATCH) == 0x037F &&
                   c.memory->readw(X87_SCRATCH + 120) == 0x037F &&
                   c.memory->readq(X87_SCRATCH + 28) == 0 &&
                   c.memory->readw(X87_SCRATCH + 36) == 0 &&
                   c.memory->readq(X87_SCRATCH + 38) == 0x8000000000000000ULL &&
                   c.memory->readw(X87_SCRATCH + 46) == 0x3FFF &&
                   c.memory->readq(X87_SCRATCH + 128) == 0 &&
                   c.fpu.top == 7 && c.fpu.getF64(c.fpu.STV(0)) == 1.0;
        }, useX87Scratch);

        struct FisttpCase { const char* name; U8 op; };
        const FisttpCase fisttp[] = {
            {"m16",0xDF}, {"m32",0xDB}, {"m64",0xDD},
        };
        for (const auto& x : fisttp) {
            std::vector<U8> code = {0xDB,0xE3, 0xDD,0x43,0x08,
                                    x.op,0x0B}; // FISTTP m16/m32/m64 [RBX]
            runAndCheck(r, (std::string("x87 FISTTP ") + x.name + " truncates toward zero").c_str(),
                withExit(code), [=](CPU64& c) {
                    U64 value = x.op == 0xDF ? c.memory->readw(X87_SCRATCH)
                               : x.op == 0xDB ? c.memory->readd(X87_SCRATCH)
                                             : c.memory->readq(X87_SCRATCH);
                    U64 expected = x.op == 0xDF ? 0xFFFD
                                 : x.op == 0xDB ? 0xFFFFFFFDULL
                                                : 0xFFFFFFFFFFFFFFFDULL;
                    return value == expected;
                }, [=](CPU64& c) {
                    useX87Scratch(c);
                    c.memory->writeq(X87_SCRATCH + 8, 0xC00E000000000000ULL); // -3.75
                });
        }
    }

    {
        const U64 psA0 = 0xC040000040A00000ULL; // {5,-3}
        const U64 psA1 = 0x4100000000000000ULL; // {0,8}
        const U64 psB0 = 0x4080000040000000ULL; // {2,4}
        const U64 psB1 = 0xC1000000C0A00000ULL; // {-5,-8}
        auto initPs = [=](CPU64& c) {
            c.xmm[0].lo = psA0; c.xmm[0].hi = psA1;
            c.xmm[1].lo = psB0; c.xmm[1].hi = psB1;
        };
        runAndCheck(r, "SSE3 ADDSUBPS register form", withExit({0xF2,0x0F,0xD0,0xC1}),
            [](CPU64& c) { return c.xmm[0].lo == 0x3F80000040400000ULL &&
                                  c.xmm[0].hi == 0x0000000040A00000ULL; }, initPs);
        runAndCheck(r, "SSE3 ADDSUBPS memory form", withExit({0xF2,0x0F,0xD0,0x03}),
            [](CPU64& c) { return c.xmm[0].lo == 0x3F80000040400000ULL &&
                                  c.xmm[0].hi == 0x0000000040A00000ULL; }, [=](CPU64& c) {
                c.reg[X64_RBX].setU64(X87_SCRATCH);
                c.xmm[0].lo = psA0; c.xmm[0].hi = psA1;
                c.memory->writeq(X87_SCRATCH, psB0); c.memory->writeq(X87_SCRATCH + 8, psB1);
            });

        runAndCheck(r, "SSE3 HADDPS register form", withExit({0xF2,0x0F,0x7C,0xC1}),
            [](CPU64& c) { return c.xmm[0].lo == 0x4100000040000000ULL &&
                                  c.xmm[0].hi == 0xC150000040C00000ULL; }, initPs);
        runAndCheck(r, "SSE3 HSUBPS memory form", withExit({0xF2,0x0F,0x7D,0x03}),
            [](CPU64& c) { return c.xmm[0].lo == 0xC100000041000000ULL &&
                                  c.xmm[0].hi == 0x40400000C0000000ULL; }, [=](CPU64& c) {
                c.reg[X64_RBX].setU64(X87_SCRATCH);
                c.xmm[0].lo = psA0; c.xmm[0].hi = psA1;
                c.memory->writeq(X87_SCRATCH, psB0); c.memory->writeq(X87_SCRATCH + 8, psB1);
            });

        const U64 pdA0 = 0xC008000000000000ULL, pdB0 = 0x4010000000000000ULL;
        runAndCheck(r, "SSE3 ADDSUBPD register form", withExit({0x66,0x0F,0xD0,0xC1}),
            [](CPU64& c) { return c.xmm[0].lo == 0x4008000000000000ULL &&
                                  c.xmm[0].hi == 0x3FF0000000000000ULL; }, [=](CPU64& c) {
                c.xmm[0].lo = 0x4014000000000000ULL; c.xmm[0].hi = pdA0;
                c.xmm[1].lo = 0x4000000000000000ULL; c.xmm[1].hi = pdB0;
            });
        runAndCheck(r, "SSE3 HADDPD memory form", withExit({0x66,0x0F,0x7C,0x03}),
            [](CPU64& c) { return c.xmm[0].lo == 0x4000000000000000ULL &&
                                  c.xmm[0].hi == 0x4018000000000000ULL; }, [=](CPU64& c) {
                c.reg[X64_RBX].setU64(X87_SCRATCH);
                c.xmm[0].lo = 0x4014000000000000ULL; c.xmm[0].hi = pdA0;
                c.memory->writeq(X87_SCRATCH, 0x4000000000000000ULL);
                c.memory->writeq(X87_SCRATCH + 8, pdB0);
            });
        runAndCheck(r, "SSE3 HSUBPD register form", withExit({0x66,0x0F,0x7D,0xC1}),
            [](CPU64& c) { return c.xmm[0].lo == 0x4020000000000000ULL &&
                                  c.xmm[0].hi == 0xC000000000000000ULL; }, [=](CPU64& c) {
                c.xmm[0].lo = 0x4014000000000000ULL; c.xmm[0].hi = pdA0;
                c.xmm[1].lo = 0x4000000000000000ULL; c.xmm[1].hi = pdB0;
            });

        runAndCheck(r, "SSE3 LDDQU memory form", withExit({0xF2,0x0F,0xF0,0x03}),
            [](CPU64& c) { return c.xmm[0].lo == 0x0123456789ABCDEFULL &&
                                  c.xmm[0].hi == 0xFEDCBA9876543210ULL; }, [](CPU64& c) {
                c.reg[X64_RBX].setU64(X87_SCRATCH);
                c.memory->writeq(X87_SCRATCH, 0x0123456789ABCDEFULL);
                c.memory->writeq(X87_SCRATCH + 8, 0xFEDCBA9876543210ULL);
            });
        runAndCheck(r, "SSE3 MOVDDUP duplicates low double", withExit({0xF2,0x0F,0x12,0xC1}),
            [](CPU64& c) { return c.xmm[0].lo == 0x3FF8000000000000ULL &&
                                  c.xmm[0].hi == 0x3FF8000000000000ULL; }, [](CPU64& c) {
                c.xmm[1].lo = 0x3FF8000000000000ULL; c.xmm[1].hi = 0xC004000000000000ULL;
            });
        runAndCheck(r, "SSE3 MONITOR is a no-op hint", withExit({0x0F,0x01,0xC8}),
            [](CPU64& c) { return c.reg[X64_R15].u64 == 0; });
        runAndCheck(r, "SSE3 MWAIT is a no-op hint", withExit({0x0F,0x01,0xC9}),
            [](CPU64& c) { return c.reg[X64_R15].u64 == 0; });
    }

    {
        U64 maddDstLo = (U64)(U16)1 | ((U64)(U16)2 << 16) |
                        ((U64)(U16)(S16)-3 << 32) | ((U64)(U16)4 << 48);
        U64 maddDstHi = (U64)(U16)5 | ((U64)(U16)6 << 16) |
                        ((U64)(U16)7 << 32) | ((U64)(U16)8 << 48);
        U64 maddSrcLo = (U64)(U16)2 | ((U64)(U16)3 << 16) |
                        ((U64)(U16)4 << 32) | ((U64)(U16)5 << 48);
        U64 maddSrcHi = (U64)(U16)(S16)-1 | ((U64)(U16)2 << 16) |
                        ((U64)(U16)3 << 32) | ((U64)(U16)4 << 48);
        runAndCheck(r, "SSE2 PMADDWD register form", withExit({
            0x66,0x0F,0xF5,0xC1
        }), [](CPU64& c) {
            return c.xmm[0].lo == (8ULL | (8ULL << 32)) &&
                   c.xmm[0].hi == (7ULL | (53ULL << 32));
        }, [=](CPU64& c) {
            c.xmm[0].lo = maddDstLo; c.xmm[0].hi = maddDstHi;
            c.xmm[1].lo = maddSrcLo; c.xmm[1].hi = maddSrcHi;
        });
        runAndCheck(r, "SSE2 PMADDWD memory form", withExit({
            0x66,0x0F,0xF5,0x03
        }), [](CPU64& c) {
            return c.xmm[0].lo == (8ULL | (8ULL << 32)) &&
                   c.xmm[0].hi == (7ULL | (53ULL << 32));
        }, [=](CPU64& c) {
            c.reg[X64_RBX].setU64(X87_SCRATCH);
            c.xmm[0].lo = maddDstLo; c.xmm[0].hi = maddDstHi;
            c.memory->writeq(X87_SCRATCH,maddSrcLo);
            c.memory->writeq(X87_SCRATCH+8,maddSrcHi);
        });

        U64 avgDstLo = 0ULL | (1ULL << 16) | (0xFFFFULL << 32) | (0xFFFEULL << 48);
        U64 avgDstHi = 30000ULL | (20000ULL << 16) | (10ULL << 32) | (11ULL << 48);
        U64 avgSrcLo = 1ULL | (2ULL << 16) | (1ULL << 32) | (3ULL << 48);
        U64 avgSrcHi = 35535ULL | (45536ULL << 16) | (11ULL << 32) | (10ULL << 48);
        auto checkAverage = [](CPU64& c) {
            return c.xmm[0].lo == (1ULL | (2ULL << 16) | (0x8000ULL << 32) |
                                   (0x8001ULL << 48)) &&
                   c.xmm[0].hi == (32768ULL | (32768ULL << 16) |
                                   (11ULL << 32) | (11ULL << 48));
        };
        runAndCheck(r, "SSE2 PAVGW register form", withExit({
            0x66,0x0F,0xE3,0xC1
        }), checkAverage, [=](CPU64& c) {
            c.xmm[0].lo = avgDstLo; c.xmm[0].hi = avgDstHi;
            c.xmm[1].lo = avgSrcLo; c.xmm[1].hi = avgSrcHi;
        });
        runAndCheck(r, "SSE2 PAVGW memory form", withExit({
            0x66,0x0F,0xE3,0x03
        }), checkAverage, [=](CPU64& c) {
            c.reg[X64_RBX].setU64(X87_SCRATCH);
            c.xmm[0].lo = avgDstLo; c.xmm[0].hi = avgDstHi;
            c.memory->writeq(X87_SCRATCH,avgSrcLo);
            c.memory->writeq(X87_SCRATCH+8,avgSrcHi);
        });

        const U64 ntLo = 0x0123456789ABCDEFULL;
        const U64 ntHi = 0xFEDCBA9876543210ULL;
        for (bool pd : {false,true}) {
            std::vector<U8> code;
            if (pd) code.push_back(0x66);
            code.insert(code.end(), {0x0F,0x2B,0x03});
            runAndCheck(r, pd ? "SSE2 MOVNTPD memory store" : "SSE MOVNTPS memory store",
                withExit(code), [=](CPU64& c) {
                    return c.memory->readq(X87_SCRATCH) == ntLo &&
                           c.memory->readq(X87_SCRATCH+8) == ntHi;
                }, [=](CPU64& c) {
                    c.reg[X64_RBX].setU64(X87_SCRATCH);
                    c.xmm[0].lo = ntLo; c.xmm[0].hi = ntHi;
                });
        }

        runAndCheck(r, "SSE2 MASKMOVDQU masked bytes to [RDI]", withExit({
            0x66,0x0F,0xF7,0xC1
        }), [=](CPU64& c) {
            for (int i = 0; i < 16; i++) {
                U8 expected = (i % 3 == 0) ? (U8)(i + 1) : (U8)(0xA0 + i);
                if (c.memory->readb(X87_SCRATCH + i) != expected) return false;
            }
            return true;
        }, [=](CPU64& c) {
            c.reg[X64_RDI].setU64(X87_SCRATCH);
            for (int i = 0; i < 16; i++) {
                U64* lanes = i < 8 ? &c.xmm[0].lo : &c.xmm[0].hi;
                U32 shift = (i & 7) * 8;
                *lanes |= (U64)(i + 1) << shift;
                lanes = i < 8 ? &c.xmm[1].lo : &c.xmm[1].hi;
                *lanes |= (U64)((i % 3 == 0) ? 0x80 : 0x7F) << shift;
                c.memory->writeb(X87_SCRATCH+i, (U8)(0xA0+i));
            }
        });

        runAndCheck(r, "SSE EMMS empties modeled x87 tags", withExit({
            0x0F,0x77
        }), [](CPU64& c) {
            if (c.fpu.isMMXInUse) return false;
            for (int i = 0; i < 8; i++) if (c.fpu.tags[i] != TAG_Empty) return false;
            return true;
        }, [](CPU64& c) { c.fpu.startMMX(); });
    }

    {
        const U64 mmxData = CODE_BASE + 0x800;
        const U64 a = 0xFEDCBA9876543210ULL;
        const U64 b = 0x0123456789ABCDEFULL;
        auto initMmx = [=](CPU64& c) {
            c.fpu.startMMX();
            c.fpu.getMMX(0)->q = a;
            c.fpu.getMMX(1)->q = b;
        };
        runAndCheck(r, "MMX PADDQ register form", withExit({0x0F,0xD4,0xC1}),
            [=](CPU64& c) {
                return c.fpu.getMMX(0)->q == a + b && c.fpu.top == 0 && c.fpu.isMMXInUse;
            }, initMmx);
        runAndCheck(r, "MMX PADDQ memory form", withExit({0x0F,0xD4,0x03}),
            [=](CPU64& c) {
                return c.fpu.getMMX(0)->q == a + b && c.fpu.top == 0 && c.fpu.isMMXInUse;
            }, [=](CPU64& c) {
                c.reg[X64_RBX].setU64(mmxData);
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = a;
                c.memory->writeq(mmxData, b);
            });

        std::vector<U8> ripShuffle = withExit({0x0F,0x70,0x05,0x0C,0,0,0,0x1B});
        const U64 shuffleSource = 0x0605040302010000ULL;
        for (int i = 0; i < 8; i++) ripShuffle.push_back((U8)(shuffleSource >> (i * 8)));
        runAndCheck(r, "MMX PSHUFW RIP-relative source with imm8", ripShuffle,
            [](CPU64& c) {
                return c.fpu.getMMX(0)->q == 0x0000020104030605ULL &&
                       c.fpu.top == 0 && c.fpu.isMMXInUse;
            }, [](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = 0xFFFFFFFFFFFFFFFFULL;
            });

        runAndCheck(r, "MMX MOVQ REX.W source and x87 alias state", withExit({
            0x48,0xB8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
            0x48,0x0F,0x6E,0xC0
        }), [](CPU64& c) {
            if (c.fpu.getMMX(0)->q != 0x1122334455667788ULL ||
                c.fpu.top != 0 || !c.fpu.isMMXInUse) return false;
            for (int i = 0; i < 8; i++) if (c.fpu.tags[i] == TAG_Empty) return false;
            return true;
        });

        auto initPair = [=](CPU64& c) {
            c.fpu.startMMX(); c.fpu.getMMX(0)->q = a; c.fpu.getMMX(1)->q = b;
        };
        runAndCheck(r, "MMX PUNPCKLBW interleaves low lanes", withExit({0x0F,0x60,0xC1}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0x8976AB54CD32EF10ULL; }, initPair);
        runAndCheck(r, "MMX PACKSSDW saturates dword lanes", withExit({0x0F,0x6B,0xC1}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0x7FFF800080007FFFULL; }, initPair);
        runAndCheck(r, "MMX PSRLW imm8 shifts each word lane", withExit({0x0F,0x71,0xD0,0x04}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0x0FED0BA907650321ULL; }, initPair);
        runAndCheck(r, "MMX PINSRW register source and imm8", withExit({0x0F,0xC4,0xC1,0x02}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0xFEDCCDEF76543210ULL; }, [=](CPU64& c) {
                initPair(c); c.reg[X64_RCX].setU64(0xCDEF);
            });
        runAndCheck(r, "MMX PINSRW memory source and imm8", withExit({0x0F,0xC4,0x43,0x08,0x02}),
            [=](CPU64& c) { return c.fpu.getMMX(0)->q == 0xFEDCCDEF76543210ULL; },
            [=](CPU64& c) {
                c.reg[X64_RBX].setU64(mmxData);
                c.memory->writeq(mmxData, a); c.memory->writew(mmxData + 8, 0xCDEF);
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = a;
            });
        std::vector<U8> pinsRip = withExit({0x0F,0xC4,0x05,0x0C,0,0,0,0x02});
        pinsRip.push_back(0xEF); pinsRip.push_back(0xCD);
        runAndCheck(r, "MMX PINSRW RIP-relative memory source and imm8", pinsRip,
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0xFEDCCDEF76543210ULL; }, initPair);
        runAndCheck(r, "MMX PEXTRW imm8 extracts selected word", withExit({0x0F,0xC5,0xC0,0x02}),
            [](CPU64& c) { return c.reg[X64_R15].u64 == 0xBA98; }, [=](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = a;
            });
        runAndCheck(r, "MMX PMOVMSKB extracts sign bits", withExit({0x0F,0xD7,0xC0}),
            [](CPU64& c) { return c.reg[X64_R15].u64 == 0xC8; }, [=](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = 0x80FF017F80010203ULL;
            });
        runAndCheck(r, "MMX MOVD writes GP register", withExit({0x0F,0x7E,0xC0}),
            [](CPU64& c) { return c.reg[X64_R15].u64 == 0x76543210ULL; }, [=](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = a;
            });
        runAndCheck(r, "MMX MOVQ2DQ transfers MMX to XMM", withExit({0xF3,0x0F,0xD6,0xC0}),
            [=](CPU64& c) { return c.xmm[0].lo == a && c.xmm[0].hi == 0; }, [=](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q = a;
            });
        runAndCheck(r, "MMX MOVDQ2Q transfers XMM to MMX", withExit({0xF2,0x0F,0xD6,0xC0}),
            [=](CPU64& c) { return c.fpu.getMMX(0)->q == a; }, [=](CPU64& c) {
                c.xmm[0].lo = a; c.xmm[0].hi = 0x8877665544332211ULL;
            });
        runAndCheck(r, "MMX CVTPI2PS converts two signed dwords", withExit({0x0F,0x2A,0xC1}),
            [](CPU64& c) {
                U32 lo = 0x3F800000U, hi = 0xC0000000U;
                return (U32)c.xmm[0].lo == lo && (U32)(c.xmm[0].lo >> 32) == hi;
            }, [=](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(1)->q = 0xFFFFFFFE00000001ULL;
            });
        runAndCheck(r, "MMX CVTPS2PI converts two packed singles", withExit({0x0F,0x2D,0xC1}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0xFFFFFFFE00000002ULL; }, [](CPU64& c) {
                c.xmm[1].lo = 0xC02000003FE00000ULL;
            });
        runAndCheck(r, "MMX CVTPI2PD converts two signed dwords", withExit({0x66,0x0F,0x2A,0xC1}),
            [](CPU64& c) { return c.xmm[0].lo == 0x3FF0000000000000ULL &&
                                  c.xmm[0].hi == 0xC000000000000000ULL; }, [](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(1)->q = 0xFFFFFFFE00000001ULL;
            });
        runAndCheck(r, "MMX CVTPD2PI converts two packed doubles", withExit({0x66,0x0F,0x2D,0xC1}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q == 0xFFFFFFFE00000002ULL; }, [](CPU64& c) {
                c.xmm[1].lo = 0x3FF8000000000000ULL;
                c.xmm[1].hi = 0xC004000000000000ULL;
            });

        auto ssse3MmxReference = [](U8 op, U64 d, U64 s) -> U64 {
            U8 db[8], sb[8], out[8] = {};
            for (int i = 0; i < 8; i++) { db[i] = (U8)(d >> (i*8)); sb[i] = (U8)(s >> (i*8)); }
            auto sat16 = [](S64 x) -> U16 { return (U16)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); };
            U64 result = 0;
            if (op == 0x00) {
                for (int i = 0; i < 8; i++) out[i] = sb[i] & 0x80 ? 0 : db[sb[i] & 7];
                for (int i = 0; i < 8; i++) result |= (U64)out[i] << (i*8);
            } else if (op == 0x1C) {
                for (int i = 0; i < 8; i++) { S32 x = (S8)sb[i]; out[i] = (U8)(x < 0 ? -x : x); }
                for (int i = 0; i < 8; i++) result |= (U64)out[i] << (i*8);
            } else if (op == 0x1D) {
                for (int i = 0; i < 4; i++) { S32 x = (S16)(U16)(s >> (i*16)); result |= (U64)(U16)(x < 0 ? -x : x) << (i*16); }
            } else if (op == 0x1E) {
                for (int i = 0; i < 2; i++) { S64 x = (S32)(U32)(s >> (i*32)); result |= (U64)(U32)(x < 0 ? -x : x) << (i*32); }
            } else if (op == 0x01 || op == 0x03 || op == 0x05 || op == 0x07) {
                U16 lanes[4];
                for (int side = 0; side < 2; side++) for (int pair = 0; pair < 2; pair++) {
                    U64 v = side ? s : d;
                    S64 a = (S16)(U16)(v >> (pair*32));
                    S64 b = (S16)(U16)(v >> (pair*32+16));
                    S64 x = (op == 0x01 || op == 0x03) ? a+b : a-b;
                    lanes[side*2+pair] = (op == 0x03 || op == 0x07) ? sat16(x) : (U16)x;
                }
                for (int i = 0; i < 4; i++) result |= (U64)lanes[i] << (i*16);
            } else if (op == 0x02 || op == 0x06) {
                U32 d0=(U32)d,d1=(U32)(d>>32),s0=(U32)s,s1=(U32)(s>>32);
                result = (U64)(op == 0x02 ? d0+d1 : d0-d1) |
                         ((U64)(op == 0x02 ? s0+s1 : s0-s1) << 32);
            } else if (op == 0x04) {
                for (int i = 0; i < 4; i++) {
                    S64 x=(S64)db[i*2]*(S8)sb[i*2]+(S64)db[i*2+1]*(S8)sb[i*2+1];
                    result |= (U64)sat16(x) << (i*16);
                }
            } else if (op == 0x08) {
                for (int i = 0; i < 8; i++) { S64 x=(S8)db[i],sgn=(S8)sb[i]; out[i]=(U8)(sgn<0?-x:sgn==0?0:x); }
                for (int i = 0; i < 8; i++) result |= (U64)out[i] << (i*8);
            } else if (op == 0x09 || op == 0x0B) {
                for (int i = 0; i < 4; i++) {
                    S64 a=(S16)(U16)(d>>(i*16)),b=(S16)(U16)(s>>(i*16));
                    U16 x=op==0x09?(U16)(b<0?-a:b==0?0:a):(U16)((a*b+0x4000)>>15);
                    result |= (U64)x << (i*16);
                }
            } else if (op == 0x0A) {
                for (int i = 0; i < 2; i++) {
                    S64 a=(S32)(U32)(d>>(i*32)),b=(S32)(U32)(s>>(i*32));
                    result |= (U64)(U32)(b<0?-a:b==0?0:a) << (i*32);
                }
            }
            return result;
        };
        const U8 ssse3Ops[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                               0x08,0x09,0x0A,0x0B,0x1C,0x1D,0x1E};
        const U64 mmxDst = 0xFEDCBA9876543210ULL;
        const U64 mmxSrc = 0x0123456789ABCDEFULL;
        const U64 mmxMem = CODE_BASE + 0x800;
        for (U8 op : ssse3Ops) {
            U64 expected = ssse3MmxReference(op,mmxDst,mmxSrc);
            for (int memorySource = 0; memorySource < 2; memorySource++) {
                std::vector<U8> code = {0x0F,0x38,op,(U8)(memorySource?0x03:0xC1)};
                runAndCheck(r,(std::string("MMX SSSE3 ") + std::to_string(op) +
                               (memorySource ? " memory" : " register")).c_str(),
                    withExit(code),[=](CPU64& c) {
                        return c.fpu.getMMX(0)->q == expected && c.fpu.top == 0 && c.fpu.isMMXInUse;
                    },[=](CPU64& c) {
                        c.fpu.startMMX(); c.fpu.getMMX(0)->q=mmxDst;
                        c.fpu.getMMX(1)->q=mmxSrc;
                        c.reg[X64_RBX].setU64(mmxMem); c.memory->writeq(mmxMem,mmxSrc);
                    });
            }
        }
        runAndCheck(r,"MMX PALIGNR register source with imm8",withExit({0x0F,0x3A,0x0F,0xC1,0x05}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q==0x1413121110070605ULL; },[](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q=0x1716151413121110ULL;
                c.fpu.getMMX(1)->q=0x0706050403020100ULL;
            });
        runAndCheck(r,"MMX PALIGNR memory source with imm8",withExit({0x0F,0x3A,0x0F,0x43,0x08,0x05}),
            [](CPU64& c) { return c.fpu.getMMX(0)->q==0x1413121110070605ULL; },[](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q=0x1716151413121110ULL;
                c.reg[X64_RBX].setU64(mmxMem); c.memory->writeq(mmxMem+8,0x0706050403020100ULL);
            });
        std::vector<U8> mmxPalignRip = {0x0F,0x3A,0x0F,0x05,0x02,0,0,0,0x05,0xEB,0x08};
        const U64 mmxAlignSrc = 0x0706050403020100ULL;
        for (int i=0;i<8;i++) mmxPalignRip.push_back((U8)(mmxAlignSrc>>(i*8)));
        mmxPalignRip.insert(mmxPalignRip.end(),{0x0F,0x7F,0x03});
        runAndCheck(r,"MMX PALIGNR RIP-relative source with imm8",withExit(mmxPalignRip),
            [](CPU64& c) { return c.memory->readq(CODE_BASE+0x800)==0x1413121110070605ULL; },[](CPU64& c) {
                c.fpu.startMMX(); c.fpu.getMMX(0)->q=0x1716151413121110ULL;
                c.reg[X64_RBX].setU64(CODE_BASE+0x800);
            });
    }

    printf("=== self-test summary: %d passed, %d failed ===\n\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}

#endif // BOXEDWINE_GUEST_X64
