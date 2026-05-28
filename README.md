# Boxedwine64

**Boxedwine64** is a fork of [Boxedwine](https://github.com/danoon2/Boxedwine) that adds x86\_64 guest support so it can run 64-bit Wine (`wine64`) and 64-bit Linux ELF binaries. Boxedwine itself is a userland emulator: it implements an x86 CPU and a fake Linux kernel, then runs Wine on top of that so Windows applications execute without a real Linux host.

This fork is a **work in progress**. The 32-bit code path remains fully functional and unchanged. The 64-bit code path is gated behind `BOXEDWINE_GUEST_X64` and is iteratively being built out via real ELF execution.

> Boxedwine is released under the GNU General Public License v2 (GPL). Original upstream by danoon2 — see [github.com/danoon2/Boxedwine](https://github.com/danoon2/Boxedwine).

---

## Current state (May 2026)

The 64-bit guest path can:

- Decode and execute a large fraction of x86\_64 user-mode ISA (general-purpose, SSE2 packed + scalar FP, most of x87, CMOV, multi-byte NOP, XGETBV, RDTSCP, BT family, REP string ops, PSHUFB, PALIGNR)
- Load static-PIE ELFs produced by `zig cc -target x86_64-linux-musl -static` and step them through to `exit_group` end-to-end
- Apply R\_X86\_64\_JUMP\_SLOT relocations eagerly across a synthesized DT\_NEEDED graph (verified via the in-tree end-to-end PLT self-test)
- Dispatch 50+ syscalls (write/read/open/openat/close/stat/fstat/mmap/mprotect/munmap/brk/dup/fcntl/sigaltstack/futex(real bookkeeping)/sched\_yield/clock\_getres/clock\_nanosleep/rt\_sigaction/rt\_sigreturn/rt\_sigprocmask/rt\_sigtimedwait/rt\_sigsuspend/rt\_sigpending/pause/wait4/getitimer/statfs/sched\_getaffinity/exit\_group/arch\_prctl/uname/getrandom/prlimit64/set\_tid\_address/…)
- Surface Wine64 rootfs zips in the UI launcher (label suffix `(Wine64)`) when `x86_64-linux-gnu/`, `x86_64-unix/`, `x86_64-windows/`, or `/lib64/` paths are detected in the central directory
- Run the 64-bit self-test harness — **210/210 PASS** at last measurement

### What works end-to-end

- Static x86\_64 musl hello-world (`write(1,"hi\n",3); exit(0)`) executes and tees output to host stdout
- Static musl `sum-loops` (arithmetic loop returning a constant) runs to completion
- Static musl `printf("%d %s\n", ...)` runs through string/integer formatting paths
- `malloc → strcpy → snprintf → strlen → strcat` chain executes end-to-end
- The end-to-end PLT call test loads a synthesized shared library, resolves `R_X86_64_JUMP_SLOT` against an exported function, and the main executable correctly calls through the GOT to return 42

### What does not work yet

- **No 64-bit JIT** — interpreter only (per design; v1 ships interpreter-only)
- **No real wine64 execution** — blocked on Linux-only build artifacts (cross-compiled rootfs, `wine64` build) which cannot be produced on the current macOS arm64 build host without Docker
- **No `clone(56)` implementation** — single-threaded only for now; needs KThread64 infrastructure
- **No PT\_DYNAMIC parsing on the file-loading path** — synthesized in-memory DT\_NEEDED works; loading `libc.so.6` from a real rootfs is blocked on (a) above
- **No WASM `MEMORY64=2` build target yet** — Milestone F

---

## Roadmap to a working Boxedwine64

This project drives the [`docs/PLAN_64BIT.md`](docs/PLAN_64BIT.md) §3.7–§3.10 roadmap toward "wine64 notepad.exe works on desktop and Chrome". The progression is iterative — every commit reflects one concrete opcode, syscall, or relocation discovered by running a real binary through `--x64-run-elf` and watching the decoder fail at the next unsupported byte sequence.

| Milestone | Goal | Status |
|---|---|---|
| **A — Dynamic linking** | PT\_DYNAMIC walk, R\_X86\_64\_\* relocations, DT\_NEEDED recursion, PT\_TLS | In-memory variant ✅ (selftest), file-loading variant ⏳ (blocked on rootfs) |
| **B — Threading + signals** | `clone(56)`, real futex, real `rt_sigaction`/`rt_sigreturn`, signal frame builder | Signals + futex bookkeeping ✅, `clone(56)` ⏳ (needs KThread64) |
| **C — Scalar FP + ISA gaps** | SSE2 scalar FP, x87 FPU minimal subset, BT family, XGETBV, RDTSCP, SSSE3 | ✅ Complete (210/210 selftest PASS) |
| **D — Rootfs + Wine64 build** | `fszip` x86\_64 layout detect, `TinyCore16x64WineBase.zip`, `tools/buildWine/buildWine64.sh`, launcher hook | Detection + UI ✅, rootfs + Wine build ⏳ (Linux-host blocked) |
| **E — Wine64 GUI + X server** | 64-bit pointer audit in `source/x11/*`, ioctl64 surface, MIT-SHM | ⏳ Not started |
| **F — WASM memory64 + v1 polish** | Emscripten `MEMORY64=2`, slim wine64 package, lazy DLL fetch, browser tests | ⏳ Not started |

See [`docs/MILESTONE_D_STATUS.md`](docs/MILESTONE_D_STATUS.md) for the detailed status of the Linux-host-blocked items.

---

## How to build (macOS arm64, dev path)

```sh
cd project/mac-xcode/Boxedwine
xcodebuild -project Boxedwine.xcodeproj -scheme Boxedwine \
           -configuration Debug -arch arm64 CODE_SIGNING_ALLOWED=NO
```

The Debug config defines `BOXEDWINE_GUEST_X64=1`, which enables the 64-bit interpreter, syscall64 dispatch, ELF64 loader, and the `--x64-selftest` / `--x64-run-elf` harnesses.

Run the self-test:

```sh
~/Library/Developer/Xcode/DerivedData/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine --x64-selftest
```

Run a static-PIE x86\_64 ELF (cross-compile with `zig cc -target x86_64-linux-musl -static -O2 hello.c -o hello`):

```sh
~/Library/Developer/Xcode/DerivedData/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine --x64-run-elf /tmp/hello
```

For 32-bit builds and the original Wine flow, see the upstream [How-To-Build-Boxedwine.md](docs/How-To-Build-Boxedwine.md).

---

## Original Boxedwine features (32-bit, fully working)

- Runs 16/32-bit Windows programs
- Works on Windows, macOS, Linux, and Web (Emscripten/WASM)
- Can run multiple versions of Wine, from 3.1 to 11.0
- Apps and games using OpenGL, Direct3D and Vulkan are supported

### Original 32-bit performance (from upstream)

#### Cinebench 11.5 Multi-Core

- **10.02** Windows 11 i7-14700 x64
- **4.71** macOS Mac Mini M4 Arm64
- **4.14** Windows 11 Snapdragon X X126100 Arm64
- **3.90** Windows 11 i7-14700 x86
- **2.67** Asahi Linux Mac Mini M1 Arm64

#### Quake 2 +timedemo 1 +map demo1.dm2

- **88.9 fps** macOS Mac Mini M4 Arm64
- **72.7 fps** Windows 11 i7-14700 x64
- **65.7 fps** Asahi Linux Mac Mini M1 Arm64
- **57.4 fps** Windows 11 i7-14700 x86

---

## Documentation

- [PLAN\_64BIT.md](docs/PLAN_64BIT.md) — the full 64-bit roadmap
- [MILESTONE\_D\_STATUS.md](docs/MILESTONE_D_STATUS.md) — what's done vs. Linux-blocked
- [Upcoming Features](docs/Roadmap-Features.md) (upstream)
- [Troubleshooting Games/Apps](docs/Troubleshooting-Games-Apps.md) (upstream)
- [Developer Debugging](docs/Developer-Debugging.md) (upstream)
- [How To Build Boxedwine](docs/How-To-Build-Boxedwine.md) (upstream)
- [CPU Emulation](docs/CPUemulation.md) (upstream)

---

## Contributing

The fastest way to move Boxedwine64 forward is the **real-binary discovery loop**:

1. Cross-compile a static x86\_64 binary with `zig cc -target x86_64-linux-musl -static -O2`
2. Run it through `--x64-run-elf`
3. When the tracer prints `unimpl opcode at RIP=… bytes=…`, look up the opcode in the Intel SDM and add a handler to `source/emulation/cpu/cpu64.cpp`
4. When a syscall stub returns `-ENOSYS`, port the 32-bit implementation from `source/kernel/syscall.cpp` into `source/kernel/syscall64.cpp`
5. Add a self-test entry in `source/emulation/cpu/cpu64SelfTest.cpp`
6. Build, run selftest (must stay at 210/210), run the binary again, commit with the opcode bytes and the binary that uncovered them in the commit message

The commit log is the canonical record of what musl/glibc actually touches during startup — every commit there has the form "cpu64: \<opcode\> — \<what discovered it\>".
