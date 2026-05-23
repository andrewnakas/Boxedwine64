# Boxedwine64: x86-64 Guest Support — Implementation Plan

**Status:** Draft v1, May 2026
**Author:** Andrew Nakas + Claude (Opus 4.7) reconnaissance
**Scope:** Add x86-64 guest emulation to Boxedwine while preserving 100% of current 32-bit functionality and all current platform targets (Windows, macOS, Linux, Android, iOS, Web/WASM).

---

## 1. Goals and non-goals

### Goals
- Run **64-bit x86 Linux ELF binaries** in the guest, on all current host platforms.
- Run **64-bit Wine (wine64)** inside the guest, capable of executing 64-bit Windows `.exe` files.
- Preserve the existing 32-bit guest path bit-for-bit. No regressions in 32-bit support.
- Ship a **two-tier WASM build**:
  - **Slim** (~target 80–120 MB): minimal wine64 + a curated DLL set + interpreter only.
  - **Full** (~300–500 MB): full wine64 install, all DLLs, mono, gecko, larger libs.
- Use **WebAssembly memory64** for the web build to give the guest >4 GB of addressable virtual space when needed. Fall back to 32-bit-guest-only mode on browsers that lack memory64 (notably Safari).

### Non-goals (v1)
- A 64-bit JIT-to-WASM backend. Browser builds stay interpreter-only initially.
- Running mixed 32/64-bit processes in one address space (Linux WoW64-style). One guest = one bitness per v1.
- Booting a 64-bit Linux kernel. Boxedwine fakes the kernel; we don't run a real one.
- Hardware-virt features (VT-x exits, etc.). Userspace emulation only, like today.

---

## 2. Constraints we have to design around

### 2.1 WASM platform constraints (browser target)

- **memory64 is Wasm 3.0, Chrome ships, Firefox flagged, Safari has no implementation** (Safari dropped objection late 2025 but has not committed). Source: [Can I Use](https://caniuse.com/wf-wasm-memory64).
- Browser cap on memory64 is ~16 GB regardless of spec.
- memory64 has a measurable perf cost vs memory32; only use it when needed.
- Strategy: **build two WASM variants** — `boxedwine-mem32.wasm` (32-bit guest only, all browsers) and `boxedwine-mem64.wasm` (64-bit guest, modern browsers).

### 2.2 Source-code blast radius (concrete numbers from recon)

| Area | Files / coordinates | Cost-of-change estimate |
|---|---|---|
| `Reg` union | `include/reg.h:22–36`, `cpu.h:194` | Small file, but ~866 call sites touch `.u32`/`.u16`/`.u8` |
| Instruction enum | `source/emulation/cpu/decoder.h:58–1511` | 1454 entries; need 64-bit variants for arithmetic, move, push/pop, etc. (~+400 entries) |
| Decoder dispatch | `source/emulation/cpu/decoder.cpp:26+`, plus `cpu_init.h` (20k lines) | Per-opcode registration must learn 64-bit variants |
| Lazy flags | `source/emulation/cpu/common/lazyFlags.h:42–95` | Add `FLAGS_ADD64`, `FLAGS_SUB64`, etc. (~15 new entries) |
| ModR/M `eaa` macros | `decoder.h:22–24` | New `eaa64` for 64-bit displacement + RIP-relative + SIB.X8 |
| Memory model | `include/kmemory.h:26–30, 108–117`, `softmmu/kmemory_soft.h:58` | Page table is `MMU mmu[K_NUMBER_OF_PAGES]` (1M entries = 4 GB only) |
| `getRamPtr` | `source/kernel/kmemory.cpp:896–908` | All addresses are `U32`; needs `U64` variant |
| Syscall dispatcher | `source/kernel/syscall.cpp:2255–2306` | i386 `int 0x80` convention only |
| Syscall table | `source/kernel/syscall.cpp:1809–2254` | 440 entries, indexed by i386 numbers |
| ELF loader | `source/kernel/loader/kelf.h:32–78`, `loader.cpp:40–51, 233–301` | ELF32 only; rejects `e_ident[4]==2` |
| Auxv | `source/kernel/kprocess.cpp:340–401` | Uses `cpu->push32`; needs `push64` |
| JIT x86/x64 backend | `source/emulation/cpu/x32/jitX86CodeGen.cpp` (5,748 LOC) | Emits code for 32-bit guest regs (`R32(...)`, `offsetofReg32`) |
| JIT ARMv8 backend | `source/emulation/cpu/armv8/jitArmV8CodeGen.cpp` (8,624 LOC) | Same |
| Wine build | `tools/buildWine/buildAll.sh:139` | Hardcoded `--without` flags + `-march=pentium4`, no `--enable-win64` |

### 2.3 Wine itself

- We will use **upstream Wine 9.x+ in "new WoW64" mode** (`--enable-win64`) since modern Wine no longer requires the multilib hack. Wine 11.0 (Jan 2026) merged the 32/64 command split.
- Wine internal architecture: a 64-bit Wine install can run 64-bit Windows apps natively; 32-bit Windows apps are handled via a thunking layer (`wow64cpu.dll`) that flips the CPU to 32-bit mode. Boxedwine's emulator must support both modes per-process.
- For v1 we punt on WoW64 thunks — build separate wine32 and wine64 containers. v2 can add WoW64.

---

## 3. Architecture decisions

### 3.1 Bitness as a per-process property

A `KProcess` will gain a `bool is64Bit` field (default false). The CPU struct gains a `bool cpuMode64` mirror for fast access. All decoder, syscall, ELF, and memory paths branch on this at well-defined seams. **We do not template/instantiate twice** — one binary, runtime branch, because the cost of an extra `if (is64)` is dwarfed by per-instruction dispatch overhead in the interpreter.

JIT is different: a JIT compilation block is fixed to one mode at compile time, so blocks compiled for 32-bit guest stay 32-bit. Crossing modes invalidates the block cache (rare event — only happens at WoW64 transitions, which v1 doesn't support).

### 3.2 Register file: extend, don't replace

Edit `include/reg.h` so `Reg` becomes:

```c
struct Reg {
    union {
        U64 u64;
        U32 u32;       // = low 32 bits of u64
        union {
            U16 u16;
            struct { U8 u8; U8 h8; };
        };
        U16 h16;
    };
};
```

`u32` aliases the low 32 bits of `u64`, exactly as RAX/EAX aliasing in real x86-64. Writing `cpu->reg[0].u32 = x` in 64-bit mode must **zero the upper 32 bits** to match x86-64 semantics — this is the one footgun. We'll add `cpu->reg[0].setU32(x)` setter for the 64-bit path. The interpreter's 32-bit handlers in `normal/*` stay unchanged because they read/write `.u32` directly and we're in 32-bit mode (upper bits don't exist / are ignored).

`reg[]` array grows from 9 to 17 entries (R8–R15 + zero reg). Padding bits 8–15 are zero in 32-bit mode. **Memory cost: 9 × 8 = 72 bytes per CPU added; negligible.**

`eip` becomes `Reg rip` (still `Reg eip` accessor as a `#define` for legacy code). RIP-relative addressing needs decoder support — see §3.4.

### 3.3 Memory model: two-tier MMU

The flat `mmu[K_NUMBER_OF_PAGES]` array (1M entries = 4 GB) cannot scale to a 48-bit guest address space (2^36 entries × 16 bytes each = a terabyte of MMU descriptors). We need a real multi-level page table for 64-bit, but want to keep the fast inline path for 32-bit.

Design:
- Keep current `KMemoryData::mmu[K_NUMBER_OF_PAGES]` exactly as-is for 32-bit processes. Zero change to perf.
- Add `KMemoryData64` (new file `source/emulation/softmmu/kmemory_soft64.{h,cpp}`) with a **two-level page table**: top level is a `std::unordered_map<U32 topIdx, MMU*>` (each top-level entry covers a 4 GB chunk), each populated entry points to a `MMU page[K_NUMBER_OF_PAGES]` array (1M × 16 B = 16 MB). We allocate top-level chunks lazily as the guest touches them. Typical 64-bit Wine app uses <8 chunks (≤128 MB MMU overhead) — acceptable.
- All `KMemory::read*/write*` methods get U64 overloads. The 32-bit overload calls into the existing fast path; the 64-bit overload dispatches by process bitness.
- `getRamPtr(U32, ...)` keeps its current signature for compatibility; a new `getRamPtr64(U64, ...)` is added.

For the **WASM memory64 build**, the host-side allocator that backs guest RAM pages must use `memory.grow` against a 64-bit linear memory. Emscripten exposes this via `-sMEMORY64=2` (the "BigInt at JS boundary" mode); the C side sees `size_t` as 64-bit. Most of our code already uses `size_t` for buffer sizes, so this should be largely automatic — the audit is making sure no host-side pointer is truncated to `U32`.

### 3.4 Decoder: REX prefix + new instruction variants

x86-64 adds:
1. **REX prefix** (0x40–0x4F when in long mode). In 32-bit mode these bytes are 1-byte `INC`/`DEC` of EAX-EDI; in 64-bit mode they're prefixes.
2. **64-bit operand size** when REX.W=1, or as the default for some instructions (PUSH/POP, near branches).
3. **8 more GP registers** addressed via REX.R, REX.X, REX.B bits.
4. **RIP-relative addressing** in ModR/M (`mod=00, rm=101`).
5. **Wider immediates and displacements** for some instructions.
6. **Removed instructions**: AAA/AAS/DAA/DAS/BOUND/PUSHA/POPA — fault in 64-bit mode.

Approach:
- Add a `bool decode64` flag to the decoder state. In `cpu->decoder.cpp`, the top-level byte-dispatch loop checks `if (decode64 && byte >= 0x40 && byte <= 0x4F)` and parses REX. The 4 REX bits get stashed on `DecodedOp` (need 4 new fields: `rex_w`, `rex_r`, `rex_x`, `rex_b`).
- ModR/M decoding consults REX.R for reg field, REX.B for rm field, REX.X for SIB index. This is the same `eaa` machinery; we add `eaa64`.
- The instruction enum (`decoder.h:58`) gets new entries for 64-bit variants where they don't already exist: `AddR64E64, AddE64R64, AddR64R64, AddR64I32` (note: 64-bit ADD takes 32-bit sign-extended imm, not 64-bit imm — this is a real x86-64 quirk), etc. Estimated +400 entries to cover the 64-bit ALU + MOV + PUSH/POP + branch instruction set. We'll generate these programmatically (see §3.10).
- New `InstructionInfo` rows for each new entry, with width=64 and updated flag-bits.
- `Lazy flag` enum gains 64-bit variants: `FLAGS_ADD64, FLAGS_SUB64, FLAGS_OR64, FLAGS_AND64, FLAGS_XOR64, FLAGS_CMP64, FLAGS_TEST64, FLAGS_INC64, FLAGS_DEC64, FLAGS_SHL64, FLAGS_SHR64, FLAGS_SAR64, FLAGS_NEG64`. Each needs a `LazyFlags*` subclass implementing getCF/SF/ZF/OF/AF/PF for 64-bit.

### 3.5 Interpreter handlers

For each new 64-bit instruction enum entry, add a handler in `source/emulation/cpu/normal/normal_*.h`. These are mostly trivial: existing 32-bit handlers like `add_r32_r32(cpu, op)` map directly to `add_r64_r64(cpu, op)` with `u32 → u64` substitution. Plan to **script the generation** of these handlers from a small DSL (see §3.10).

Dispatcher (`normal/normalCPU.cpp:245–265`) needs the new entries wired through `cpu_init.h`. The MUSTTAIL pattern keeps perf the same.

### 3.6 Syscalls

x86-64 Linux uses the `syscall` instruction (opcode `0F 05`) and:
- Number in RAX
- Args in RDI, RSI, RDX, R10, R8, R9
- Return in RAX, R11 clobbered, RCX = caller RIP
- **Different number table than i386** (e.g., `read` is 0 on x86-64 but 3 on i386)

Plan:
- Decoder: recognize `0F 05` (currently unsupported) → new instruction `Syscall64`.
- Handler: new file `source/kernel/syscall64.cpp` mirroring `syscall.cpp` but with:
  - `ARG1..ARG6` macros over RDI/RSI/RDX/R10/R8/R9.
  - A separate `syscallFunc64[]` table indexed by x86-64 syscall numbers (≈350 entries).
  - Handlers that share kernel-internal logic with the i386 versions — most syscalls don't care about bitness once args are unpacked. We refactor each i386 handler `syscall_X(args...)` so the bitness-specific shim is just the arg-extraction wrapper.
- Struct layout differences (stat, dirent, sigaction, etc.) must be emitted in their 64-bit form. `ksystem.cpp:488–547` already has an `is64` switch for `writeStat`; extend the pattern to all struct-writing syscalls.
- `mmap2` is i386-only; x86-64 uses `mmap` with byte offset. The handler must accept a U64 offset.
- The `int 0x80` opcode in 64-bit mode invokes the *32-bit* syscall ABI as a compat shim; Wine doesn't use it, so we can punt (raise SIGSYS) in v1.

### 3.7 ELF loader

New file `source/kernel/loader/kelf64.h` with `k_Elf64_Ehdr`, `k_Elf64_Phdr`, `k_Elf64_Dyn`, etc. All address fields are U64.

`loader.cpp:40–51` (`isValidElf`) updated:
- Accept `e_ident[4] == 2` (ELFCLASS64).
- Check `e_machine == 0x3E` (EM_X86_64) for 64-bit; `e_machine == 0x03` (EM_386) for 32-bit.
- Set `KProcess::is64Bit` flag based on the class byte.

`loader.cpp:233–301` (`loadProgram`) refactored into a template or two parallel functions over header type. The control flow is identical; only field widths differ.

Auxv (`kprocess.cpp:340–401`) becomes bitness-aware. Each push uses `cpu->push32` or `cpu->push64`. `push64` already needs to exist for 64-bit `PUSH r64` instructions, so this is consistent.

### 3.8 JIT backends — defer the hard parts

For v1, **the JIT only handles 32-bit guests.** 64-bit guests run on the interpreter path only. Rationale:
- Implementing a 64-bit guest JIT for x86_64 host is the most code-intensive part of the project (~5,000+ lines of new emitter code in `jitX86CodeGen.cpp`).
- Interpreter-only 64-bit Wine on a modern host CPU should still be usable for non-game workloads (think: running 64-bit MS Office, calc.exe, simple installers).
- JIT support can land in v2 once correctness is established on the interpreter.

The dispatch logic in `KProcess::startProcess` checks `is64Bit` and forces the CPU to interpreter mode if true. Existing `BOXEDWINE_DIRECT_NORMAL_DISPATCH` path is used unchanged.

**Long-term JIT plan** (out of scope for v1): The x32 backend emits via AsmJit `x86::r32`/`r64` — we'd add a parallel set of emitters keyed on guest mode. ARMv8 backend similarly. WASM build will likely never have a JIT (WASM-from-WASM codegen is its own research project).

### 3.9 Wine build

New script `tools/buildWine/buildWine64.sh`:
- Configure: `./configure --enable-win64 --without-cups --without-pulse --without-dbus --without-sane --without-alsa --without-x11 (no, we need x11) ... --prefix=/opt/wine64 --disable-tests`
- CFLAGS: `-O2 -mtune=generic` (no `-march=pentium4`; that's i386-only).
- Build environment: Debian 11 amd64 chroot (replaces the i386 chroot used for wine32).
- Output: `Wine64-{VERSION}.zip` with `/opt/wine64/...` tree.

New base rootfs `TinyCore16x64WineBase.zip`:
- Tiny Core Pure64 (the 64-bit variant) — ~5 MB stripped.
- Includes `ld-linux-x86-64.so.2`, 64-bit glibc, 64-bit X libs.
- Built per `How-To-Build-Tiny-Core-Base.md` with the Pure64 base.

Filesystem loader `source/io/fszip.cpp:157` updated to recognize both `i386-unix` and `x86_64-unix` layouts.

### 3.10 Two-tier WASM builds

**Build matrix:**

| Target | Guest bitness | Memory model | Wine package | Approx size |
|---|---|---|---|---|
| `boxedwine.wasm` (legacy) | 32 only | memory32 | wine32 slim | 50 MB |
| `boxedwine64-slim.wasm` | 32 + 64 | memory64 | wine64 slim | 100–150 MB |
| `boxedwine64-full.wasm` | 32 + 64 | memory64 | wine64 full | 300–500 MB |

**Slim wine64 package:**
- No `mono`, no `gecko` (apps that need .NET or HTML rendering get a friendly error or fall back to a stub).
- Only the DLLs in the curated "core" set: `kernel32, ntdll, user32, gdi32, advapi32, ole32, oleaut32, comctl32, comdlg32, msvcrt, shell32, ws2_32, wininet, winmm`.
- Stripped binaries (`strip --strip-all` post-build).
- Excluded translation `.mo` files for non-English locales.

**Full wine64 package:**
- Full upstream `make install` output.
- Bundled mono + gecko packages from Wine's distribution.

**Lazy DLL fetch:**
- New code path in `fszip.cpp`: a `FsZipLazy` variant that, on `open()` of a file matching `/opt/wine64/**/*.dll`, issues an `emscripten_fetch` if the file isn't yet present locally. Cache hit on subsequent opens. This lets us ship the slim wasm with only ~20 MB of core DLLs preloaded and stream the rest on first use.
- Off by default. Enabled via URL query param `?lazy=1` so initial deployments stay simple.

### 3.11 Build flag taxonomy

New flags:
- `BOXEDWINE_GUEST_X64` — enables 64-bit guest decoder/syscall/ELF paths. Implies `BOXEDWINE_64` (host must be 64-bit).
- `BOXEDWINE_WASM_MEMORY64` — Emscripten target uses `-sMEMORY64=2`.
- `BOXEDWINE_WINE64` — link the wine64 filesystem loader at startup (otherwise we look for wine32 only).

Existing `BOXEDWINE_64` (host is 64-bit) is **necessary but not sufficient** for `BOXEDWINE_GUEST_X64`. Document this clearly in `buildFlags.txt`.

---

## 4. Phased execution

### Phase 0 — Scaffolding (1 week)
- Add `BOXEDWINE_GUEST_X64` flag, gated everywhere with empty branches.
- Add `KProcess::is64Bit` field, defaulting to false.
- Add `Reg::u64` field via union extension; verify zero regressions in the 32-bit test suite.
- Add `LazyFlags` 64-bit subclass stubs (return UB if invoked).
- Exit criteria: all existing tests pass; new flag compiles in/out cleanly on all platforms.

### Phase 1 — ELF64 loader (1–2 weeks)
- Add `kelf64.h` structs.
- Extend `isValidElf` and `loadProgram` for ELFCLASS64.
- Extend `pushThreadStack` / auxv builder.
- Add `cpu->push64` / `cpu->pop64` helpers (interpreter only).
- Test: load a static x86-64 hello-world ELF; verify entry point, stack layout, argv/envp/auxv via a debugger trap at entry.
- Exit criteria: a `_start` symbol is reached with correct RSP/RIP/argv state. No instructions need to execute yet.

### Phase 2 — 64-bit decoder + interpreter for "core" instruction set (4–8 weeks)
- REX prefix handling in decoder.
- New instruction enum entries (auto-generated where possible).
- 64-bit `eaa` (RIP-relative + SIB).
- Interpreter handlers for: MOV, ADD, SUB, AND, OR, XOR, CMP, TEST, INC, DEC, NEG, NOT, SHL, SHR, SAR, ROL, ROR, PUSH, POP, CALL (rel/abs), RET, JMP (rel/abs), Jcc (all conditions), LEAVE, ENTER, XCHG, CMPXCHG, LEA, MOVSX, MOVZX (incl MOVSXD), CDQE, CQO.
- 64-bit lazy flags.
- Exit criteria: a hello-world C program compiled with `gcc -O2 -static` for x86-64 prints to stdout via `write` syscall in the guest.

### Phase 3 — 64-bit syscall layer (3–6 weeks)
- `syscall` opcode (`0F 05`).
- `syscall64.cpp` with `syscallFunc64[]` table.
- Refactor every syscall handler to share core logic between i386 and x86-64 entry points.
- 64-bit `stat`, `mmap`, `mmap2 → mmap`, `fcntl`, `ioctl` struct layouts.
- Exit criteria: a dynamically-linked x86-64 hello-world (using `ld-linux-x86-64.so.2` + libc) runs to completion.

### Phase 4 — SSE2/SSE3/SSSE3/SSE4 audit and 64-bit instruction tail (4–6 weeks)
- All remaining x86-64 instructions: BSWAP, MOVQ between GP and XMM, PUSHFQ/POPFQ, IMUL r64, DIV r64, MUL r64, IDIV r64, string ops (REP MOVSQ, REP STOSQ), CPUID (advertise x86-64 features), RDTSC.
- Verify XMM/MMX paths work with REX.R/REX.B (XMM8-XMM15).
- Exit criteria: a 64-bit `coreutils`-style binary suite runs (ls, cat, grep, etc.).

### Phase 5 — Wine64 build pipeline (2–3 weeks)
- `buildWine64.sh` script.
- Tiny Core Pure64 base rootfs.
- Test: `wine64 notepad.exe` launches in the guest, displays an X11 window via the existing X server emulation.
- Exit criteria: Notepad runs end-to-end on a desktop host (Linux/macOS/Windows). UI input works.

### Phase 6 — WASM port with memory64 (3–5 weeks)
- Emscripten makefile additions: `MEMORY64=2`, audit all host-side `U32`/`size_t` boundaries.
- Build the slim `boxedwine64-slim.wasm`.
- Browser test: Chrome (works), Firefox (works with flag), Safari (graceful fallback to mem32 build).
- Lazy DLL fetch implementation (optional, behind query param).
- Exit criteria: notepad.exe (64-bit) runs in Chrome at usable speed (target: >5 Hz UI redraw).

### Phase 7 — Test suite expansion (continuous, 4+ weeks of focused work)
- Add `source/test/cpu64/` with per-instruction tests for every new 64-bit handler.
- CI matrix: Linux x86_64, macOS arm64, Windows x86_64, Emscripten (Chrome headless).
- Exit criteria: 95%+ test coverage of new 64-bit instruction handlers, all matrices green.

### Phase 8 — Hardening + v1 release (2–4 weeks)
- Stress-test with real 64-bit Windows apps: 7-Zip, Notepad++, foobar2000, Paint.NET.
- Performance tuning of the 64-bit interpreter path (likely 0.3–0.6× the speed of 32-bit JIT — set expectations in release notes).
- Documentation: update README, add `How-To-Run-64bit-Wine.md`.
- Release tag `v0.40-64bit-preview`.

**Total estimate: 6–10 calendar months for one full-time engineer.** Less if some phases parallelize, more if Wine64-in-emulator surfaces unforeseen issues (likely).

---

## 5. Risks and unknowns

### High risk
- **Wine64 + threading + signals**: 64-bit Wine relies heavily on `pthread_*`, signal masks, and TLS in ways that may expose latent bugs in Boxedwine's threading layer. v1 may need targeted fixes to `kthread.cpp` and friends.
- **64-bit interpreter performance**: doubling the bit-width and adding REX-prefix overhead to every dispatch may push the interpreter from "usable for simple apps" to "frustrating for any app." Mitigation: profile early, consider a thin tier-0 JIT for hot blocks before v1 ships.
- **Address-space sparsity in Wine64**: Wine64 allocates address space in a *highly sparse* pattern (zone allocator, NT heap, mmap holes). Our two-level page table assumes a few dense 4 GB chunks; if Wine sprays across the full 47-bit user address space, MMU overhead could explode. May need to fall back to a hashed page table.

### Medium risk
- **Memory64 in Emscripten** is relatively new (stable since ~Emscripten 3.1.50). May hit toolchain bugs. Mitigation: pin to a known-good emcc version.
- **Safari**: no memory64 means Safari users get 32-bit only. Acceptable as long as the slim mem32 build still ships and works.
- **Build size**: targets in §3.10 are optimistic. Real wine64 stripped is ~80 MB; with rootfs we're at ~120 MB minimum for "slim." User has said this is OK but we should track it.

### Low risk
- **Decoder enum explosion**: 400+ new entries is large but each is mechanical. Code-generation script (Python or similar) drafted in Phase 2 keeps it maintainable.
- **FPU/XMM**: already 64/128-bit-clean, just needs REX.R/REX.B awareness.

---

## 6. Open questions

1. **Code generation for instruction handlers**: invest in a real DSL/generator now (Python script emitting `.cpp`), or hand-write? Recommendation: generator. ROI breakeven at ~50 instructions; we have 400+.
2. **WoW64 support**: defer to v2 confirmed, but should the v1 process struct already carry the necessary fields (compatible mode register, etc.) to avoid breaking changes later? Recommendation: yes, add the fields but don't wire them.
3. **JIT for 64-bit guest**: do we commit to v2 having it on x86_64-host (~3 months) and ARMv8-host (~3 months) backends, or stay interpreter-only indefinitely on the assumption that "good enough for the apps people actually want to run"? Need user input.
4. **Wine version pin**: ship with Wine 10.0 (stable Jan 2025) or Wine 11.0 (stable Jan 2026)? 11.0 is the "no more 32/64 split" release which simplifies our build but is newer.
5. **What's the "compelling demo app" for the v1 release?** Helps focus the test suite. Suggestions: Notepad++, foobar2000, 7-Zip GUI.

---

## 7. What changes file-by-file (quick reference for Phase 0/1 start)

| File | Change |
|---|---|
| `buildFlags.txt` | Document `BOXEDWINE_GUEST_X64`, `BOXEDWINE_WASM_MEMORY64`, `BOXEDWINE_WINE64` |
| `include/reg.h` | Extend `Reg` union with `U64 u64` |
| `include/kprocess.h` | Add `bool is64Bit` field; add `U64` overloads for entry/brk/loaderBaseAddress |
| `include/kmemory.h` | Add `U64`-address overloads for `readb/w/d/q/writeb/w/d/q/getRamPtr` |
| `source/emulation/cpu/common/cpu.h` | Add `bool cpuMode64`; extend `Reg eip` to `Reg rip`; add R8–R15 (expand `reg[9]` to `reg[17]`); add `push64`/`pop64` |
| `source/emulation/cpu/decoder.h` | Add new instruction enum entries (REX-prefixed variants); add `eaa64` macro; add `rex_w/r/x/b` fields to `DecodedOp` |
| `source/emulation/cpu/decoder.cpp` | REX prefix parsing in main decode loop; new `InstructionInfo` rows |
| `source/emulation/cpu/common/lazyFlags.h` | Add `FLAGS_*64` enum entries |
| `source/emulation/cpu/common/lazyFlagsXXX64.cpp` | New: 64-bit lazy flag implementations |
| `source/emulation/cpu/normal/normal_*.h` | New 64-bit handlers (generated) |
| `source/emulation/cpu/common/cpu_init.h` | Wire new handlers into dispatch table |
| `source/emulation/softmmu/kmemory_soft64.{h,cpp}` | New: two-level page table for 64-bit guest |
| `source/kernel/loader/kelf64.h` | New: ELF64 struct definitions |
| `source/kernel/loader/loader.cpp` | Accept ELFCLASS64; branch on bitness |
| `source/kernel/syscall64.cpp` | New: x86-64 syscall table + dispatcher |
| `source/kernel/syscall.cpp` | Refactor handlers into bitness-shared cores |
| `source/kernel/ksystem.cpp` | Extend `writeStat` pattern to all struct emitters |
| `source/kernel/kprocess.cpp` | Bitness-aware `pushThreadStack`/auxv |
| `source/io/fszip.cpp` | Detect `x86_64-unix` layout |
| `tools/buildWine/buildWine64.sh` | New: wine64 build script |
| `tools/buildWine/buildAll.sh` | Add `64` mode |
| `project/linux/makefile` | Add `wine64-slim` and `wine64-full` build targets |
| `project/mac-xcode/...` | New build configuration `Release64BitGuest` |
| `project/msvc/.../BoxedWine.vcxproj` | New build config |
| `project/emscripten/makefile` | Add `MEMORY64=2`; new targets `slim64`, `full64` |
| `docs/How-To-Run-64bit-Wine.md` | New user-facing doc |
| `docs/PLAN_64BIT.md` | This file (kept up to date as scope changes) |
| `source/test/cpu64/` | New test directory mirroring `source/test/cpu/` |

---

## 8. Acceptance criteria for v1 ship

1. All 32-bit guest tests pass on all current host platforms (no regression).
2. The 64-bit guest test suite (Phase 7) reaches 95%+ pass rate.
3. `wine64 notepad.exe` works on Linux, macOS, Windows desktop builds.
4. `wine64 notepad.exe` works in Chrome via `boxedwine64-slim.wasm` with memory64.
5. Slim wasm bundle initial download is documented (no hard size limit per user request).
6. Documentation explains how to choose between 32-bit and 64-bit Wine in the launcher UI.

---

*This plan is a starting contract, not a commitment. We revise it after Phase 2 (decoder POC) once we know how the interpreter performance actually shapes up.*
