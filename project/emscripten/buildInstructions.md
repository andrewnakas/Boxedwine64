# Emscripten Build

There are 2 builds for the Web
- Single Threaded. Target Release
- Multi-Threaded. Target multiThreaded

Plus a headless 64-bit core target for Node:
- 64-bit guest self-test. Target wasm64-selftest (see below)


## Build

From `project/emscripten`:

type make and the name of the target. See contents of ./Build/<Target> folder for output


## Running Single Threaded Build
- runs in main browser thread

python3 -m http.server <port number>


## Running Multi-Threaded Build
- runs using Emscripten -pthread -sPROXY_TO_PTHREAD=1

browser cross-origin isolation headers required for `SharedArrayBuffer`

node server.mjs <port number>

alternatively make sure your web server returns COEP, COOP headers


## 64-bit guest core in WASM (`wasm64-selftest`)

First step of the 64-bit / browser milestone (see the repo README, "WebAssembly
and the browser"): build the `BOXEDWINE_GUEST_X64` interpreter core under
Emscripten and run the in-process CPU64/KMemory64/loader64 self-test headless in
Node. This is a `wasm32` host build — `KMemory64` is a software page table keyed
on `U64` guest addresses, so guest pointer width is independent of host pointer
width (`-sMEMORY64` is a later step, only needed for the full wine64 prefix).

From `project/emscripten`:

    make wasm64-selftest

Output is `Build/Wasm64SelfTest/boxedwine64-selftest.js` (+ `.wasm`), a plain Node
target (no SDL canvas / shell HTML). Run the self-test:

    node Build/Wasm64SelfTest/boxedwine64-selftest.js --x64-selftest

It prints `=== self-test summary: 234 passed, 0 failed ===` and exits 0; any
failure makes it exit non-zero (`runX64SelfTest()` returns the failed count).


## Run a real x86_64 ELF in WASM (`wasm64-runelf`)

Second step: load and execute a real x86_64 Linux ELF (loader64 + the SysV init
stack + syscall64), still headless in Node. Same 64-bit core as above, but linked
with `-sNODERAWFS=1` so the guest loader's `fopen()` reads straight off the host
filesystem.

From `project/emscripten`:

    make wasm64-runelf

Run a static binary (build one with e.g.
`zig cc -target x86_64-linux-musl -static -O2 hello.c -o hello`):

    node Build/Wasm64RunElf/boxedwine64-runelf.js --x64-run-elf /path/to/hello

It loads the segments, builds the argv/envp/auxv frame, and interprets from
`_start` through the guest's `write`/`exit_group`. With no path argument it runs
an embedded hand-built hello-world ELF instead. A prebuilt static test binary
lives at `tools/x64test/wasm/hello_static`.


## Memory64 (wasm64) build (`wasm64-selftest-mem64`)

Same 64-bit core, but the WASM module itself uses 64-bit linear-memory addressing
(`-sMEMORY64=1`), so the *host* `void*` is 64-bit too. This is the build that
proves no 32-bit-host-pointer assumption is hiding in the path. (`KMemory64` is
unaffected — its guest pointers were already `U64` — but the shared softmmu /
kernel code now indexes host RAM with full-width pointers; `BOXEDWINE_64` is
auto-enabled from the pointer width, see `include/platformBoxedwine.h`.)

From `project/emscripten`:

    make wasm64-selftest-mem64

**Requires a Memory64-capable engine: Node v24+** (Emscripten's wasm64 output
refuses to run on older Node). Run it:

    node Build/Wasm64SelfTestMem64/boxedwine64-selftest-mem64.js --x64-selftest

Same `234 passed, 0 failed` result as the wasm32 self-test.


## 64-bit guest in the browser (`wasm64-mt`)

Wires the 64-bit core into the same multi-threaded browser machinery the 32-bit
web build already uses: pthreads → Web Workers, `PROXY_TO_PTHREAD`, and
`SharedArrayBuffer` for the guest `clone`/futex threading model, rendering to a
`<canvas>` via `shell.html`. This is the front-end foundation for running wine64
in a tab (full GL via a WebGL backend and lazy rootfs streaming build on top).

From `project/emscripten`:

    make wasm64-mt

Output is `Build/Wasm64Mt/boxedwine64.html` (+ `.js`/`.wasm`). Like the 32-bit
multi-threaded build it needs cross-origin isolation (COOP/COEP) for
`SharedArrayBuffer`, so serve it with the bundled server:

    node server.mjs 8000
    # then open http://127.0.0.1:8000/Build/Wasm64Mt/boxedwine64.html

The page loads, the WASM instantiates, and the pthread Worker pool spins up; the
emulator then waits for a root filesystem (`boxedwine.zip`). Bundling a 64-bit
rootfs zip (lazy/streamed) is the next step — see the README roadmap.


