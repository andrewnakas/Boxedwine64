# Boxedwine64

**Boxedwine64** is a fork of [Boxedwine](https://github.com/danoon2/Boxedwine) that adds x86\_64 guest support so it can run 64-bit Wine (`wine64`) and 64-bit Linux ELF binaries. Boxedwine itself is a userland emulator: it implements an x86 CPU and a fake Linux kernel, then runs Wine on top of that so Windows applications execute without a real Linux host.

This fork is a **work in progress**, but a substantial one: real Debian `wine64` now boots a Windows program (`notepad.exe`) all the way to a **visible, rendered GUI window** on macOS arm64, driving real `wineserver64`, `winex11`, FreeType/fontconfig text, and an in-process X11 wire server. The 32-bit code path remains fully functional and unchanged. The 64-bit code path is gated behind `BOXEDWINE_GUEST_X64` and was built out entirely by running real binaries and implementing each opcode/syscall they touch.

![wine64 notepad.exe rendering a real GUI window in Boxedwine64 on macOS](docs/images/notepad-gui.png)

*Real `wine64 notepad.exe` running under Boxedwine64 on macOS arm64 — full menu bar, FreeType-rendered text, scrollbar, status bar, and a live caret, painted through the in-process X11 wire server.*

> Boxedwine is released under the GNU General Public License v2 (GPL). Original upstream by danoon2 — see [github.com/danoon2/Boxedwine](https://github.com/danoon2/Boxedwine).

---

## Current state (June 2026)

**`wine64 notepad.exe` now runs as a real, interactive GUI app on macOS arm64 —
including File ▸ Save As all the way to a file on disk.** The full Windows-PE +
X11 path is up: real Debian `wine64` boots the entire `wineboot → services.exe →
winex11` chain, connects to an in-process X11 wire server, and **paints a live
notepad window** (hundreds of `PutImage`/GDI draw requests per frame,
FreeType-rendered text, an animated caret). The `wine64`↔`wineserver` IPC
handshake, the NT-syscall dispatch, real PE image loading, and the X11 wire
protocol all work end to end. **Keyboard and mouse input work**: you can type
into notepad, click to place the caret, open the menus (composited as separate
top-level windows), and the host shows wine's own cursor (I-beam over text,
arrow elsewhere) aligned to the pointer.

**The common-dialog round trip works**: opening Save As pops the modal dialog as
its own top-level window, you can type a filename, **click its buttons** (pointer
events hit-test to the topmost window under the cursor, so clicks land on the
dialog and not the window behind it), and the file is written through wine's
`C:` drive to the host filesystem (`tools/rootfs64/root/home/username/`).

Boots now reach a painted window in **~25 s** rather than appearing to hang for
minutes: a per-CPU instruction-fetch page cache removed a per-byte
mutex+hashmap lookup that was pinning a core inside wineserver's name-table
scans. Pick any bundled Windows program to launch with the
**`tools/run_wine64_gui.sh`** picker (notepad, winecfg, regedit, taskmgr, clock,
write, …) — the 64-bit equivalent of the 32-bit "run a program" UI.

Headless, the same stack runs `wine64 wineboot --init` through **~4000 syscalls
across the full process tree** (client, `wineserver64`, `services.exe`,
`winex11`, …) with **zero unimplemented syscalls and no heap corruption**,
populating a real win64 prefix.

The 64-bit guest path can:

- Decode and execute a large fraction of the x86\_64 user-mode ISA (general-purpose, SSE2 packed + scalar FP, most of x87, CMOV, multi-byte NOP, XGETBV, RDTSCP, BT family, REP string ops, PSHUFB, PALIGNR, segment-register MOV, atomic RMW: XCHG/CMPXCHG/XADD/LOCK-prefixed ALU)
- Load and run **real dynamically-linked glibc 2.36 ELF64 binaries** from a 64-bit rootfs — full PT\_DYNAMIC walk, versioned-symbol (Verneed) resolution across multiple DSOs, lazy PLT/GOT resolution, IFUNC, TLS
- Run **real x86\_64 threads**: `clone`/`clone3`, real `futex` WAIT/WAKE, per-thread CPU state, `pthread_create`/`pthread_join` — each guest thread on its own host thread, with sharded per-address atomic locks so glibc mutexes are correct under contention
- **Real `fork()`** (non-thread `clone` → new process with a deep-copied address space) + **64-bit `execve`** + **`wait4`** reaping — the basis for `wine64` spawning `wineserver`
- A full **AF\_UNIX socket + epoll IPC surface**: `socket`/`socketpair`/`bind`/`connect`/`listen`/`accept`/`shutdown`/`setsockopt`, `epoll_create`/`ctl`/`wait`, `pipe`/`pipe2`, and **`sendmsg`/`recvmsg` with full 64-bit `msghdr`/`iovec`/`cmsghdr` marshaling and `SCM_RIGHTS` fd-passing** (the wineserver request/reply protocol)
- Dispatch 90+ syscalls total (the above plus write/read/open/openat/close/stat/fstat/newfstatat/mmap/mprotect/munmap/brk/dup/fcntl/chdir/fchdir/mkdir/symlink/unlink/pread64/pwrite64/ftruncate/getdents64/rt\_sig\*/sigaltstack/sched\_get\|setaffinity/exit\_group/arch\_prctl/uname/getrandom/prlimit64/set\_tid\_address/umask/setsid/…)
- Run the 64-bit self-test harness — **234/234 PASS**

### What works end-to-end

- **`wine64 notepad.exe`** → boots the full `wineboot → services.exe → winex11`
  chain and **renders a visible notepad window** on macOS: real PE image
  loading, NT-syscall dispatch, an in-process X11 wire server, FreeType +
  fontconfig text, and the GDI draw path (`CreateGC`/`PolyFillRectangle`/
  `CopyArea`/`PutImage`). Run **without** `-novideo` to get the host window.
- **`wine64 --version`** → `wine-8.0 (Debian 8.0~repack-4)`, exit 0, headless
- **`wine64 wineboot --init`** → forks `wineserver64`; wineserver binds/listens
  its socket, accepts the client, loads NLS locales, creates and **populates** a
  real win64 registry/prefix, runs its epoll main loop; all processes exit
  cleanly with no heap corruption.
- **Dynamic glibc programs from a 64-bit rootfs**: `hello_glibc`, busybox `ls -la /` (dynamic `getdents64`), GNU `ls` across 3 versioned DSOs
- **Threading probes**: `clone`+futex join, a 4-thread atomic/mutex probe (`mt_probe`), `pthread_join` wakeup — all deterministic PASS
- The static-PIE smoke suite (`tools/x64test/run-static-elf-suite.sh`) — **7/7 PASS** on `zig cc`-built musl binaries (hello, sum, sieve, fib25, qsort, strops, hash)
- The in-tree end-to-end PLT self-test: loads a separate shared library, resolves `R_X86_64_JUMP_SLOT` against an exported function, calls through the GOT to return 42
- **Interactive GUI input**: type into notepad, click to place/move the caret,
  open and track the menus, and see wine's own cursor aligned to the pointer —
  SDL key/mouse events are translated to X11 input events, pointer/focus/grab
  requests are answered, popup menus are composited as overlay windows, and
  click coordinates are translated through the window's root origin
- **Common-dialog Save As**: type a filename, click the dialog's buttons, and
  write the file out to the host. Backed by SysV shared memory
  (`shmget`/`shmat`/`shmctl`/`shmdt` for wine's view-backing), MAP_FIXED-correct
  anonymous `mmap` placement (a bare hint relocates instead of stomping a wine
  view → no more `create_view` abort), and multi-window pointer hit-testing so a
  modal dialog over the main window still receives its clicks
- **`tools/run_wine64_gui.sh`** picker: launch any bundled GUI program (notepad,
  winecfg, regedit, taskmgr, clock, write, explorer, …) in a real window, by
  menu or by name — the 64-bit analogue of the 32-bit "run a program" UI

### What does not work yet

- **No 64-bit JIT** — interpreter only (by design; v1 ships interpreter-only)
- **No WASM `MEMORY64=2` build target yet**

---

## Roadmap to a working Boxedwine64

This project drives the [`docs/PLAN_64BIT.md`](docs/PLAN_64BIT.md) §3.7–§3.10 roadmap toward "wine64 notepad.exe works on desktop and Chrome". The progression is iterative — every commit reflects one concrete opcode, syscall, or relocation discovered by running a real binary through `--x64-run-elf` and watching the decoder fail at the next unsupported byte sequence.

| Milestone | Goal | Status |
|---|---|---|
| **A — Dynamic linking** | PT\_DYNAMIC walk, R\_X86\_64\_\* relocations, DT\_NEEDED recursion, versioned symbols, PT\_TLS | ✅ Complete — real glibc + multi-DSO programs run from a 64-bit rootfs |
| **B — Threading + signals** | `clone`/`clone3`, real futex, `rt_sigaction`/`rt_sigreturn`, signal frames | ✅ Complete — `pthread_create`/`join`, per-thread CPU, sharded atomics |
| **C — Scalar FP + ISA gaps** | SSE2 scalar FP, x87 subset, BT family, XGETBV, RDTSCP, SSSE3, atomic RMW | ✅ Complete (229/229 selftest PASS) |
| **D — Rootfs + Wine64 build** | Build a real `wine64` rootfs (Docker), run it headless | ✅ Complete — `wine64 --version` → `wine-8.0`, headless |
| **E — fork/exec + wineserver IPC** | real `fork`/`execve`/`wait4`, AF\_UNIX sockets, epoll, `sendmsg`/`recvmsg` + SCM\_RIGHTS | ✅ Complete — `wineboot --init` drives the full wine64↔wineserver handshake |
| **F — Windows PE + GUI** | populate the prefix with Windows PE files, then the X server / GUI path; interactive input + a working common dialog | ✅ Complete — notepad paints, takes keyboard+mouse, and Save As writes a file to the host |
| **G — App breadth + X completeness** | get a spread of bundled apps (winecfg, regedit, clock, write, …) usable, fill the remaining X core opcodes they need | ⏳ Next |
| **H — Interpreter throughput** | close the gap to interactive speed: block/trace caching, faster hot-path decode, fewer per-op map lookups | ⏳ Not started |
| **I — WASM memory64 + v1 polish** | Emscripten `MEMORY64=2`, slim wine64 package, lazy DLL fetch, browser tests | ⏳ Not started |

The commit log (`git log --oneline`) is the canonical, blow-by-blow record of the
bring-up — each commit names the opcode or syscall and the real binary that
uncovered it.

### Concrete next steps

The most useful work, in rough priority order:

1. **App breadth (Milestone G).** Run each program in `tools/run_wine64_gui.sh`
   and fix the first thing each one needs. `winecfg`, `regedit`, and `clock` are
   the highest-value targets (tabbed dialogs, tree controls, timers). Expect
   missing X **core-font/text opcodes** — the dialog text path uses
   `OpenFont`(45)/`QueryFont`(47)/`PolyText8`(65)/`ImageText8`(76); they
   currently fall through `default:` in `source/x11wire/xwireconnection.cpp`
   (use `BW64_XWIREDUMP=1` to hex-dump unhandled requests). Implement a builtin
   fixed font + glyph blit into the window pixmap so dialog labels render.
2. **A real Save/Open into the Mac home.** Point the guest's
   `Documents`/`Desktop` `.link` files at a host-visible folder (or add a
   `-drive` mapping) so files land somewhere natural instead of inside the repo
   rootfs. Today everything under `C:` lives in `tools/rootfs64/root/`.
3. **Interpreter throughput (Milestone H).** The fetch cache helped, but the
   `step()` decoder still re-decodes every instruction. A per-page decoded-block
   cache (keyed by guest RIP, invalidated on unmap/exec) would speed up the hot
   loops that dominate wineserver and GDI; this is the single biggest lever on
   "feels native."
4. **Reliability: kill the residual boot wedge.** Boots are fast now but still
   occasionally stall in wineserver's O(n) name-table scan. Either a wineserver-
   side fast path for the UTF-16 case-fold compare (disasm at `wineserver64`
   file-offset `0x663e0`/`0x302a0`) or the block cache above should clear it.
   Use `BW64_RIPSAMPLE=1` to confirm the spinner.
5. **Regression coverage.** Add a headless smoke test that boots `wineboot
   --init` + a non-interactive PE to a known exit, so the memory/mmap changes
   here can't silently regress. Keep `--x64-selftest` at 234/234.

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

Run a dynamically-linked glibc ELF64 from the 64-bit rootfs:

```sh
BW=~/Library/Developer/Xcode/DerivedData/Boxedwine-*/Build/Products/Debug/Boxedwine.app/Contents/MacOS/Boxedwine
"$BW" -novideo -root tools/rootfs64/root /bin/hello_glibc
```

Run real `wine64` headless (the rootfs zips are built by
`tools/rootfs64/build-wine64-zip.sh`, which needs Docker for the Debian amd64
image):

```sh
D=tools/rootfs64/dist
# version check
"$BW" -novideo -env WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine \
      -zip "$D/glibc-rootfs64.zip" -zip "$D/wine64.zip" \
      /usr/lib/wine/wine64 --version            # -> wine-8.0

# full prefix bring-up + wineserver handshake (set BW64_SYSTRACE=1 to trace syscalls)
"$BW" -novideo -env HOME=/winePrefix -env WINEPREFIX=/winePrefix/.wine \
      -env WINESERVER=/usr/lib/wine/wineserver64 \
      -zip "$D/glibc-rootfs64.zip" -zip "$D/wine64.zip" \
      /usr/lib/wine/wine64 wineboot --init
```

(Launch the real `wineserver64` ELF via `WINESERVER` — the guest VFS won't exec
the `wineserver` wrapper shell-script.)

Run a static-PIE x86\_64 ELF (cross-compile with `zig cc -target x86_64-linux-musl -static -O2 hello.c -o hello`) or the full smoke suite:

```sh
"$BW" --x64-run-elf /tmp/hello
tools/x64test/run-static-elf-suite.sh          # requires zig
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
6. Build, run selftest (must stay at 229/229), run the binary again, commit with the opcode bytes and the binary that uncovered them in the commit message

The commit log is the canonical record of what musl/glibc actually touches during startup — every commit there has the form "cpu64: \<opcode\> — \<what discovered it\>".
