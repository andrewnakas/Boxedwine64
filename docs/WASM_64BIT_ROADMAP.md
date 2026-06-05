# 64-bit WASM Roadmap — running wine64 in the browser

**Status:** Milestone I in progress. The 64-bit guest core runs in WASM headless
(both `wasm32` and `-sMEMORY64`/wasm64), and the multi-threaded **browser** build
now **boots real `wine64` in a tab** — `wine64 --version` → `wine-8.0`, clean
`exit_group(0)`, verified in headless Chrome. Remaining work is the GL backend
(WebGL), lazy/streamable rootfs, the in-browser wineserver IPC shim, and CI.

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
| `wasm64-mt` | `make wasm64-mt` | 64-bit core + browser threading (Workers + SharedArrayBuffer) + **real `wine64` boots in a tab** | serve + open the wine64 launcher (below) → console prints `wine-8.0 (Debian 8.0~repack-4)`, clean `exit_group(0)` |

### Running real wine64 in a browser tab  ✅

The `wasm64-mt` build ships a dedicated launcher — `wine64.html` +
`wine64-launcher.js` — that loads the two layered 64-bit rootfs zips into the
WASM VFS and passes the same argv the native headless command uses. It is
separate from the generic 32-bit `boxedwine-shell.js`/`shell.html` (single
`boxedwine.zip`, many query-param quirks).

```sh
cd project/emscripten
# the launcher fetches the zips relative to wine64.html; symlink them into the
# build dir (cheap — no 200MB copy):
ln -sf ../../../../tools/rootfs64/dist/glibc-rootfs64.zip Build/Wasm64Mt/glibc-rootfs64.zip
ln -sf ../../../../tools/rootfs64/dist/wine64.zip          Build/Wasm64Mt/wine64.zip
node server.mjs 8000   # sets COOP/COEP so SharedArrayBuffer / crossOriginIsolated works
# open http://127.0.0.1:8000/Build/Wasm64Mt/wine64.html
```

Launcher query params (all optional):

- `?p=--version` — program + args for wine64 (default `--version`; URL-encode
  spaces as `%20`).
- `?boot=1` — run `wineboot --init` instead (full prefix bring-up; sets
  `HOME`/`WINEPREFIX`/`WINESERVER`).
- `?novideo=1` — headless (no SDL window); fastest for the `--version` smoke.
- `?base=<url>` — where the zips are fetched from (default `./`).

**Verified (two depths):**

- `wine64.html?novideo=1` → the browser console (and the on-page `#output`
  textarea) prints `wine-8.0 (Debian 8.0~repack-4)` then
  `CPU64: exit_group syscall, status=0`. Real x86_64 `wine64` executing in WASM.
- `wine64.html?novideo=1&boot=1` → the **full `wineboot --init` boot stack runs
  in the tab**: `wineserver64` forks and stays resident, the in-process X11 wire
  server (`XWireServer`) accepts client connections and completes handshakes
  (`vendor=Boxedwine root=0x260 visual=0x21`), the process tree forks/execs/exits
  cleanly across ~7 guest pids, and **`XWire: first window mapped`** — a window
  is mapped (present sink=headless under `?novideo=1`). 13.5k lines of boot
  activity, **no OOM / no abort**. (Capture ends at the test's 180s cap, not a
  crash; SHAPE/MIT-SHM/FreeType warnings are the slim-rootfs / headless-sink
  gaps.) A trimmed reference trace is saved at
  [`../tools/x64test/wasm/wineboot-browser-boot.log`](../tools/x64test/wasm/wineboot-browser-boot.log).

Two things made the deep boot work and are baked in: the launcher pre-creates a
writable `WINEPREFIX` (`/winePrefix/.wine`) in MEMFS before boot (wine64 chdir's
into it before creating it), and `wasm64-mt` links with
`-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB` (the multi-process boot OOMs a
fixed 512 MB heap).

**Headless test harness:** `cdp-attach.mjs` attaches to an already-running
Chrome's DevTools endpoint (`--remote-debugging-port=9222 --no-sandbox
--enable-features=SharedArrayBuffer`), opens the page, and streams console +
`#output` until a match string appears or it times out — no npm deps (raw CDP
over a built-in-only WebSocket client). Note: a fresh headless Chrome needs
`--no-sandbox` on macOS or it dies with a Mach-port permission error, and
`crossOriginIsolated` is only true with COOP/COEP **and** `SharedArrayBuffer`
enabled.

```sh
node cdp-attach.mjs "http://127.0.0.1:8000/Build/Wasm64Mt/wine64.html?novideo=1" 180000 "wine-8.0"
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

## Remaining work

### 1. Boot a 64-bit rootfs in `wasm64-mt`  ✅ DONE

`wine64.html` + `wine64-launcher.js` load `glibc-rootfs64.zip` +
`wine64.zip` as layered `-zip` mounts and pass the native-equivalent argv.
`wine64 --version` prints `wine-8.0` and exits cleanly; **`?boot=1` drives the
full `wineboot --init` through wineserver64 + the X11 wire server to a mapped
window in the tab** (see "Running real wine64 in a browser tab" above). The
writable-WINEPREFIX + `ALLOW_MEMORY_GROWTH` fixes that unlocked the deep boot are
in the launcher / makefile.

The two zips still load 205 MB up front into the WASM VFS — fine for the
correctness milestone, addressed by step 3 (streaming) for a shippable demo.

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
- Browser shell (32-bit, generic): `project/emscripten/{shell.html,boxedwine-shell.js,boxedwine.css,server.mjs}`.
- **wine64 browser launcher: `project/emscripten/{wine64.html,wine64-launcher.js}`** —
  loads the two layered 64-bit zips + passes the wine64 argv.
- Headless test driver (no deps, raw CDP): `project/emscripten/cdp-attach.mjs`
  (attach to a running Chrome) / `cdp-test.mjs` (launch one).
- 64-bit core: `source/emulation/cpu/cpu64*.cpp`, `source/kernel/kmemory64.cpp`,
  `source/kernel/syscall64.cpp`, `source/kernel/loader/loader64.cpp`.
- GL bridge: `source/opengl/gl64bridge.cpp` (+ `.h`, `_abi.h`).
- Pointer-width / platform gating: `include/platformBoxedwine.h`.
- Rootfs tooling + zips: `tools/rootfs64/` (`build-wine64-zip.sh`, `dist/*.zip`).
- Static test ELF: `tools/x64test/wasm/hello_static`.
- Build docs: `project/emscripten/buildInstructions.md`.
