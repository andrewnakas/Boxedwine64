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

BLOCKER discovered (precise): the Boxedwine64 Xcode target compiles the FULL
kernel — kscheduler.cpp builds (413 .o files, including kprocess/fs/x11). The
link failure for a headless `--x64-run-root` was NOT a missing file. It's that
this target defines **`BOXEDWINE_MULTI_THREADED`**, and `runSlice()` only
exists in the single-threaded `#else` branch of kscheduler.cpp (line 213,
inside the `#ifdef BOXEDWINE_MULTI_THREADED ... #else ... #endif`). In MT mode
there is no `runSlice()`: each guest thread runs on its own HOST thread via
`KThread::runThreadSlice`, orchestrated by `sdl/multiThreaded/threadedMainloop.cpp`.

NEXT STEP (do deliberately; keep the green 224/27 baseline):
  (a) RECOMMENDED — run via the normal app path: `Boxedwine -root
      tools/rootfs64/root /bin/hello_glibc`. The exec path is already wired
      for 64-bit (loader.cpp:265 → loadProgram64, which loads PT_INTERP via
      openGuestPath and builds the SysV stack). This boots SDL/UI though, so
      it needs either a display or a headless-friendly video option
      (startupArgs.videoOption) — try the existing `-nozip`/console options.
  (b) Headless MT driver: a `--x64-run-root` that, after startProcess, drives
      the *multi-threaded* mainloop (sdl/multiThreaded/threadedMainloop.cpp)
      rather than `runSlice()`. Must use the MT scheduling entry, not the ST
      one. Heavier; only if (a) proves unworkable headless.
  Verify first that `loadProgram64`'s openGuestPath finds
  /lib64/ld-linux-x86-64.so.2 against this root/ tree.

## Rebuilding the artifacts

Requires Docker Desktop running (`open -a Docker`). From repo root:

```sh
docker run --rm --platform linux/amd64 -v "$PWD/tools/rootfs64/work:/w" \
  gcc:13 bash -c 'cd /w && gcc -O2 -o hello_glibc hello_glibc.c && \
                  gcc -O2 -static -o hello_static hello_glibc.c && \
                  gcc -O2 -o dirprobe dirprobe.c && \
                  cp -L /lib64/ld-linux-x86-64.so.2 . && \
                  cp -L /lib/x86_64-linux-gnu/libc.so.6 .'
```

`dirprobe.c` is a libc-only directory lister (opendir/readdir/qsort) — the
coreutils-`ls` essence, exercising `getdents64` through real dynamic glibc.

### usefoo / libfoo — the versioned multi-DSO test

`usefoo.c` + `libfoo.c` (+ `libfoo.map`) are a 2-DSO test where `libfoo.so`
exports a *versioned* symbol (`FOO_1.0`) and `usefoo` binds to it. This forces
the guest ld.so to process a Verneed record for a SECOND shared library — the
path that was broken by the duplicate mmap bump allocators (an anonymous map
and a file-backed map could be handed the same base, and `mmapAnonymousFixed`
zero-fills on map, clobbering one DSO's version records → guest ld.so reported
`unsupported version 0 of Verneed record`). Fixed by unifying the allocator on
`CPU64::mmapNext`. Build + install:

```sh
docker run --rm --platform linux/amd64 -v "$PWD/tools/rootfs64/work:/w" \
  gcc:13 bash -c 'cd /w && \
    gcc -O2 -fPIC -shared -Wl,--version-script=libfoo.map \
        -Wl,-soname,libfoo.so -o libfoo.so libfoo.c && \
    gcc -O2 -o usefoo usefoo.c -L. -lfoo -Wl,-rpath,/lib'
cp tools/rootfs64/work/usefoo     tools/rootfs64/root/bin/usefoo
cp tools/rootfs64/work/libfoo.so  tools/rootfs64/root/lib/libfoo.so
tools/run_x64_root.sh /bin/usefoo   # -> "foo_value()=42", exit 0
```

Dynamic GNU `ls` (3 versioned DSOs: libselinux + libpcre2 + libc) is the
"real" version of this test. Its binaries are large redistributables, so they
are gitignored, not committed; reproduce on demand:

```sh
docker run --rm --platform linux/amd64 -v "$PWD/tools/rootfs64/work:/w" \
  debian:bookworm-slim bash -c 'mkdir -p /w/lsbundle/lib && cp -L /bin/ls /w/lsbundle/ls && \
    for so in $(ldd /bin/ls | grep -oE "/[^ ]+\.so[^ ]*"); do cp -L "$so" /w/lsbundle/lib/; done'
cp tools/rootfs64/work/lsbundle/ls                   tools/rootfs64/root/bin/ls_dyn
cp tools/rootfs64/work/lsbundle/lib/libselinux.so.1  tools/rootfs64/root/lib/x86_64-linux-gnu/
cp tools/rootfs64/work/lsbundle/lib/libpcre2-8.so.0  tools/rootfs64/root/lib/x86_64-linux-gnu/
tools/run_x64_root.sh /bin/ls_dyn /   # -> directory listing, exit 0
```

`busybox` (in `root/bin/`) is a static x86-64 build pulled from Debian
bookworm; it runs `busybox ls -la /`, `echo`, `pwd`, `cat`, etc. through the
full 64-bit kernel. Refresh it (and the dynamic coreutils `ls` + its libs)
with:

```sh
docker run --rm --platform linux/amd64 -v "$PWD/tools/rootfs64/work/extract:/out" \
  debian:bookworm-slim bash -c \
  'apt-get update -qq && apt-get install -y -qq busybox-static && \
   cp /bin/busybox /out/ && cp /bin/ls /out/ && \
   cp /lib/x86_64-linux-gnu/libselinux.so.1 /lib/x86_64-linux-gnu/libpcre2-8.so.0 /out/'
```

The dynamic GNU `ls` additionally needs `libselinux.so.1` + `libpcre2-8.so.0`;
those currently trip the guest ld.so on a `DT_RELR` relocation our loader
doesn't yet emit correctly, so only static busybox `ls` and `dirprobe` are
committed as working fixtures. (`work/extract/` is gitignored.)

## What runs today (64-bit rootfs, full kernel)

```
tools/run_x64_root.sh /bin/busybox ls -la /     # real coreutils, long format
tools/run_x64_root.sh /bin/dirprobe /bin         # dynamic glibc getdents64
tools/run_x64_root.sh /bin/hello_glibc           # exit 42
tools/run_x64_root.sh /bin/probe2                # %f + malloc + qsort
```

## Threading (KThread64)

`clone(2)`/`clone3(2)` create real threads (per-thread `CPU64` sharing one
`KMemory64`), driven by the multi-threaded host-thread scheduler
(`platformThread`), and `futex(2)` truly blocks/wakes across them. The x86-64
atomic RMW instructions glibc's mutexes rely on (XCHG/CMPXCHG/XADD and
`lock`-prefixed ALU) are serialized so locking is correct under real host
threads.

Threading probes live in `work/` (source only — build + drop in `root/bin/`
to run):

```sh
# raw clone + shared counter + futex WAIT/WAKE join (no libc threading layer)
gcc -O2 -static -nostartfiles -ffreestanding clone_ok.c -o clone_ok   # -> PASS
# simplest: child sets a shared flag, parent observes it
gcc -O2 -static -nostartfiles -ffreestanding clone_flag.c -o clone_flag # -> flag-set
# real glibc pthreads: pthread_create + mutex-protected counter + join
gcc -O2 -pthread pthread_probe.c -o pthread_probe
```

Build them under `docker run --rm --platform linux/amd64 debian:bookworm`
(the `--platform` flag is required on Apple Silicon), then
`tools/run_x64_root.sh /bin/clone_ok` etc.

KNOWN LIMITS: the raw probes pass deterministically. Real glibc `pthread_probe`
runs both threads through real mutexes but is slow (the atomic-RMW serialization
is coarse) and full `pthread_join` completion is not yet confirmed end-to-end.
See the project memory note for the two root-cause fixes and the remaining
join/perf work.
