# 64-bit WASM Roadmap — running wine64 in the browser

**Status:** Milestone I in progress. The 64-bit guest core runs in WASM headless
(both `wasm32` and `-sMEMORY64`/wasm64) and the multi-threaded **browser** build
loads in a tab. Remaining work is the rootfs, the GL backend, the wineserver IPC
shim, and a browser test harness.

This file is the actionable hand-off for finishing "wine64 in a browser tab." A
fresh session should be able to read this top-to-bottom and continue. See also
the narrative in [`../README.md`](../README.md) ("WebAssembly and the browser")
and the original design in [`PLAN_64BIT.md`](PLAN_64BIT.md).

---

## Toolchain prerequisites

- **Emscripten** (emsdk). `git clone …/emsdk ~/emsdk && ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest && source ~/emsdk/emsdk_env.sh`. Verified with emcc 6.0.0.
- **Node v24+** is REQUIRED to *run* the `-sMEMORY64` output (Emscripten's wasm64
  loader refuses older Node). `nvm install 24`. wasm32 targets run on Node 18+.
- **zig** (optional) to cross-compile static x86_64 test ELFs:
  `zig cc -target x86_64-linux-musl -static -O2 hello.c -o hello`.

All make targets below are in `project/emscripten/` (`cd project/emscripten`).

---

## What already works (DONE)

| Target | Command | What it proves | Verify |
|--------|---------|----------------|--------|
| `wasm64-selftest` | `make wasm64-selftest` | 64-bit core builds on wasm32 host | `node Build/Wasm64SelfTest/boxedwine64-selftest.js --x64-selftest` → `234 passed, 0 failed` |
| `wasm64-runelf` | `make wasm64-runelf` | a real static x86_64 ELF loads + runs (loader64 + SysV stack + syscall64) | `node Build/Wasm64RunElf/boxedwine64-runelf.js --x64-run-elf tools/x64test/wasm/hello_static` → prints message, clean `exit_group` |
| `wasm64-selftest-mem64` | `make wasm64-selftest-mem64` | true 64-bit host pointers (`-sMEMORY64`) | **Node 24:** `node Build/Wasm64SelfTestMem64/boxedwine64-selftest-mem64.js --x64-selftest` → 234/234 |
| `wasm64-mt` | `make wasm64-mt` | 64-bit core + browser threading (Workers + SharedArrayBuffer) loads in a tab | serve + open (below); WASM instantiates, 64-Worker pthread pool up, canvas live, waits on `boxedwine.zip` |

Serving the browser build (needs COOP/COEP for `SharedArrayBuffer`):

```sh
cd project/emscripten
node server.mjs 8000
# open http://127.0.0.1:8000/Build/Wasm64Mt/boxedwine64.html
```

### Key facts learned (don't re-discover these)

- The x64 interpreter core (`cpu64`/`kmemory64`/`loader64`/`syscall64`) is
  **pointer-width clean**: `KMemory64` is a software page table keyed on `U64`
  guest addresses, so guest pointer width is independent of host pointer width.
  That's why the 64-bit guest runs on a plain wasm32 host.
- The ONLY change `-sMEMORY64` needed was in `include/platformBoxedwine.h`:
  `BOXEDWINE_64` must track real pointer width (`__SIZEOF_POINTER__ == 8` /
  `__wasm64__`), **not** `__WORDSIZE` — Emscripten wasm64 keeps `__WORDSIZE == 32`
  while pointers are 8 bytes. The breakage was in the 32-bit softmmu
  (`soft_ram.cpp` `RAM_TYPE`), not the x64 core.
- The wasm64 targets reuse the full release `SOURCES` list (the link closure is
  hard to prune). Two link/compile fixes were needed: add `source/x11wire/*.cpp`
  to `SOURCES`, and guard a `boxedWineCriticalSection.unlock()` in
  `kunixsocket.cpp` under `#ifdef BOXEDWINE_MULTI_THREADED`.

---

## Remaining work to a first "wine64 in a tab" demo

Ordered by leverage. Steps 1 and 2 are the minimum for a *visible* boot; step 3
is what makes it *fast/shippable*.

### 1. Boot a 64-bit rootfs in `wasm64-mt`  ← highest leverage

The page currently 404s on `boxedwine.zip` and stops. The 64-bit rootfs zips
already exist:

- `tools/rootfs64/dist/glibc-rootfs64.zip` (~9.8 MB)
- `tools/rootfs64/dist/wine64.zip` (~205 MB)

(Rebuild with `tools/rootfs64/build-wine64-zip.sh`; needs Docker for the Debian
amd64 image. See its header comment.)

Tasks:
- Get these zips served as the build's root FS. The shell (`boxedwine-shell.js` +
  `shell.html`) loads a single `boxedwine.zip` by default and can take `?app=`/`?p=`
  query params (see `docs/How-To-Create-FileSystem-For-Web.md`). Decide whether to
  (a) concatenate/repackage into one `boxedwine.zip`, or (b) extend the shell to
  mount multiple layered zips (the native path already mounts both via `-zip`).
- The launcher must treat the guest as 64-bit: `FsZip::guestIs64` trips on paths
  like `x86_64-linux-gnu/`, `/lib64/`, `x86_64-unix/`, `x86_64-windows/` (already
  true for these zips).
- The headless command line that works natively is the reference for the args the
  shell must pass (see README "How to build", the wineboot/`--version` invocations):
  `WINEDLLPATH`, `WINEPREFIX`, `WINESERVER=/usr/lib/wine/wineserver64`, etc.
- 205 MB up front is too big for a real demo but fine as a first **correctness**
  milestone — get *something* (e.g. `wine64 --version`, then `wineboot --init`)
  to run in the tab before optimizing size. Slim the package and/or do step 3
  afterward.

Verify: open the page (served as above); the guest should reach the same point it
does headlessly (`wine-8.0` version print → wineserver handshake). Use the
browser-test agent / headless Chrome to capture console.

### 2. WebGL GL backend (`source/opengl/gl64bridge.cpp`)  ← needed for glcube

`gl64bridge.cpp` currently asks SDL for a desktop **compatibility-profile** GL
context (`SDL_GL_CONTEXT_PROFILE_COMPATIBILITY`), which **WebGL2 (GLES3-like) does
not provide**. This is real porting work, not a flag flip.

Tasks:
- Under Emscripten, request a GLES3/WebGL2 context (`SDL_GL_CONTEXT_PROFILE_ES`,
  version 3.0) instead of compatibility profile — gate on `__EMSCRIPTEN__`.
- Audit the GL command stream that `gl64bridge` translates (the same translation
  layer that targets host OpenGL today) for fixed-function / compatibility-only
  calls that WebGL2 rejects; map or stub them.
- Keep `SwapBuffers` → `requestAnimationFrame` (Emscripten's SDL_GL_SwapWindow
  already does this under the main-loop).
- Target binary: `glcube.exe` (in `tools/rootfs64/root/home/username/`). The README
  "graphics deep-dive" documents the native GL bring-up this mirrors.

Verify: `glcube` draws a spinning cube on the canvas in a tab.

### 3. Lazy / streamable rootfs

So the page starts fast instead of downloading a full prefix. Range-fetch DLL/zip
contents on demand (pull `kernel32`/`opengl32`/etc. when first opened) rather than
downloading all 200+ MB up front. The 32-bit web build's storage mode
(`INDEXED_DB`, visible in the shell logs) is the persistence half; the streaming
half is new. Consider an HTTP Range-backed FsZip.

### 4. wineserver IPC in the browser

The desktop build talks to `wineserver64` over unix-domain sockets. In the browser
there are no real fds. Provide an in-memory socketpair/epoll shim for the
wineserver IPC. Much of the unix-socket machinery is in
`source/kernel/kunixsocket.cpp` + `source/x11wire/` (X-over-socket). The
multi-threaded model (already up in `wasm64-mt`) is the substrate — wineserver and
clients are guest threads/processes sharing `SharedArrayBuffer` memory.

### 5. Browser test harness + v1 polish

Headless-Chrome smoke tests: boot → window-map → first frame. A slimmed wine64
package. The demo page. Wire into CI (`Jenkinsfile` builds the other web targets).

---

## File map (where things live)

- Build targets: `project/emscripten/makefile` (per-target env blocks + recursive
  `$(MAKE)`; the `wasm64-*` targets define `BOXEDWINE_GUEST_X64`).
- Browser shell: `project/emscripten/{shell.html,boxedwine-shell.js,boxedwine.css,server.mjs}`.
- 64-bit core: `source/emulation/cpu/cpu64*.cpp`, `source/kernel/kmemory64.cpp`,
  `source/kernel/syscall64.cpp`, `source/kernel/loader/loader64.cpp`.
- GL bridge: `source/opengl/gl64bridge.cpp` (+ `.h`, `_abi.h`).
- Pointer-width / platform gating: `include/platformBoxedwine.h`.
- Rootfs tooling + zips: `tools/rootfs64/` (`build-wine64-zip.sh`, `dist/*.zip`).
- Static test ELF: `tools/x64test/wasm/hello_static`.
- Build docs: `project/emscripten/buildInstructions.md`.
