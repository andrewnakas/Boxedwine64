# tools/rootfs64 — x86_64 glibc rootfs for Boxedwine64

A real glibc 2.36 runtime, extracted from Debian bookworm (amd64) via Docker,
laid out at canonical Linux paths so a dynamically-linked guest binary's
`PT_INTERP` (`/lib64/ld-linux-x86-64.so.2`) and ld.so's `openat()` of
`libc.so.6` resolve against real files.

## Layout

```
work/                              raw extracted artifacts (not a rootfs)
  ld-linux-x86-64.so.2             glibc 2.36 dynamic linker
  libc.so.6                        glibc 2.36 C library (2782 exported symbols)
  hello_glibc                      gcc -O2, dynamically linked
  hello_static                     gcc -O2 -static (no ld.so needed)
  hello_glibc.c                    source

root/                              mountable guest rootfs ("/" tree)
  lib64/ld-linux-x86-64.so.2
  lib/x86_64-linux-gnu/libc.so.6
  bin/hello_glibc
  bin/hello_static
  etc/ld.so.cache                  (empty; ld.so probes resolve cleanly)
```

## Status (what runs today)

- **Static glibc**: `hello_static` runs end-to-end via the bare standalone
  runner — `Boxedwine --x64-run-elf tools/testdata/hello_glibc_static.elf`
  → prints "hello from glibc", exits 42. Regression-guarded as smoke probe
  `glibcStatic`. This is the proven Milestone D foundation.

- **Dynamic glibc**: the dynamic linker EXECUTES correctly (bootstrap +
  self-relocation) but cannot load `libc.so.6` through the bare runner,
  because `--x64-run-elf` has **no filesystem layer** (openat → ENOSYS).

## What's needed to run the dynamic path

Dynamic linking needs a real guest filesystem, which lives in the FULL
kernel (`Fs::initFileSystem` + `KProcess::startProcess` + `runSlice`), not
the bare `--x64-run-elf` runner.

BLOCKER discovered: the **Boxedwine64 Xcode target is a minimal diagnostic
build**. Its synchronized `source/` group compiles cpu64/syscall64/loader64/
kprocess, but `kscheduler.cpp` (and X11 / full audio / full FS) are NOT
linked into this target — a headless `--x64-run-root` that calls `runSlice()`
fails to link (`Undefined symbols: runSlice()`).

NEXT STEP (build-system task, do deliberately to avoid regressing the green
224/27 baseline): either
  (a) expand the Boxedwine64 Xcode target to compile the full kernel
      (kscheduler + FS + minimal X11 stubs), then add a headless
      `--x64-run-root <hostRootDir> <guestPath>` entry that mounts this
      `root/` tree and drives `runSlice()` until the process exits; or
  (b) run via the existing full Boxedwine app build (the non-minimal target)
      with `-root tools/rootfs64/root /bin/hello_glibc`, which already routes
      64-bit ELFs through `loadProgram64` (loader.cpp:265) — that path is
      wired, it just needs the GUI build + a way to run headless.

## Rebuilding the artifacts

Requires Docker Desktop running (`open -a Docker`). From repo root:

```sh
docker run --rm --platform linux/amd64 -v "$PWD/tools/rootfs64/work:/w" \
  gcc:13 bash -c 'cd /w && gcc -O2 -o hello_glibc hello_glibc.c && \
                  gcc -O2 -static -o hello_static hello_glibc.c && \
                  cp -L /lib64/ld-linux-x86-64.so.2 . && \
                  cp -L /lib/x86_64-linux-gnu/libc.so.6 .'
```
