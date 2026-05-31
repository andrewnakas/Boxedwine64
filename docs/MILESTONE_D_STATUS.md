# Milestone D status — rootfs + Wine64 build pipeline

This document tracks what's done, what's blocked, and what a Linux
contributor needs to do to finish Milestone D of the 64-bit roadmap
(see `docs/PLAN_64BIT.md` §3.9 and the production roadmap in commit
history under "Milestone D").

## UPDATE: docker block lifted; real coreutils now run

The "docker unavailable" hard block below is **stale**. Docker works on the
dev Mac (`docker run --platform linux/amd64 ...`). As of commit `ee512d8f`,
real x86-64 programs run from a 64-bit rootfs through the full kernel:

```
tools/run_x64_root.sh /bin/busybox ls -la /   # static coreutils, long format
tools/run_x64_root.sh /bin/dirprobe /bin       # dynamic glibc getdents64
```

This reaches the *engine* half of D's first exit criterion (run a coreutils
program from a 64-bit rootfs). Required: real guest argv/envp (was hardcoded
to 1 element), a real `getdents64`, and `setuid/setgid/prctl/time` — all in
`ee512d8f`. busybox (static, from debian:bookworm) lives in
`tools/rootfs64/root/bin/`; `dirprobe.c` is a libc-only ls.

**Remaining engine blocker before wine64:** multi-DSO **symbol versioning**.
Dynamic GNU `ls` (libselinux+libpcre2) and any 2-DSO test fail in the guest
ld.so with `unsupported version 0 of Verneed record`. Narrowed to an l_addr /
link_map issue for the 2nd+ DSO (the Verneed bytes are provably correct in
guest memory), NOT segment-copy corruption. See the
`project_boxedwine64_coreutils` memory for the exact diagnosis and where to
look next (`sys_mmap64_file` reservation-vs-FIXED base agreement).

## Done (in-tree, no external deps)

- **`fszip.cpp` x86_64 layout detection** — commit `26e5e10f`.
  `FsZip::guestIs64` is true when any archive entry path contains
  `x86_64-linux-gnu/`, `x86_64-unix/`, `x86_64-windows/`, or
  `/lib64/`. Verified false against the existing 32-bit zips
  (TinyCore16, Wine7.0, Wine9.0).

- **UI launcher hook** (D4). New `FsZip::detectGuestIs64(zipFile)`
  static helper runs the same sniff against the central directory
  without constructing a full FsZip; `GlobalSettings::lookForFileSystems`
  calls it for each `.zip` it discovers and sets
  `FileSystemZip::guestIs64`. The version dropdown
  (`BaseView::createFileSystemVersionCombobox`) appends a
  ` (Wine64)` suffix when the flag is set, and
  `BoxedContainer::isWine64()` exposes the same bit to the launch
  path. The 32-bit selftest still reports 209/209 PASS on macOS.

## Blocked on Linux build environment

The remaining D items each require a Linux box with a working x86_64
toolchain (gcc-x86-64-linux-gnu, glibc 2.35+ devel headers, a
distro with multiarch enabled). The development machine for this
roadmap is macOS arm64 which lacks all of:
- a Linux ELF linker (Apple `ld` rejects `--target=linux`)
- `docker` (not installed; pulling Linux base images not possible
  without engaging it)
- `x86_64-linux-gnu-gcc` cross-toolchain (no Homebrew formula)
- `lld` with cross-targeting enabled

This is a hard block — none of the items below can be tested on
macOS, and shipping them sight-unseen would be irresponsible.

### Blocked items

1. **`TinyCore16x64WineBase.zip`** — Pure64 (~5 MB) + glibc + minimal
   X libs (libX11.so.6, libXext.so.6, libxcb.so.1, libxkbcommon.so.0).
   Process:
   - download TinyCore Pure64 ISO from `http://www.tinycorelinux.net/14.x/x86_64/release/`
   - loopmount, extract `core.gz`, decompress to a staging dir
   - strip dev/doc/locale/man, retain `/lib64`, `/usr/lib64`,
     `/usr/lib/x86_64-linux-gnu`, `/usr/bin/{ls,sh}`,
     `/lib64/ld-linux-x86-64.so.2`, `/lib/x86_64-linux-gnu/libc.so.6`
   - repackage as a zip with `mount=/` so `FsZip::init` sees the
     paths as `/lib64/...` etc. (which trips `guestIs64`)
   - target unzipped size: < 30 MB
   - mirrors the existing `How-To-Build-Tiny-Core-Base.md` recipe
     for the 32-bit base — read that first.

2. **`tools/buildWine/buildWine64.sh`** — new script (sibling of the
   existing 32-bit `buildWine.sh`). Invocation pattern:
   ```sh
   ./configure --enable-win64 \
               --prefix=/opt/wine64 \
               --disable-tests \
               --without-cups --without-pulse --without-dbus \
               --without-sane --without-alsa --without-oss
   make -j$(nproc) && make install DESTDIR=$BUILD
   strip $BUILD/opt/wine64/bin/wine64
   strip --strip-unneeded $BUILD/opt/wine64/lib64/wine/x86_64-{unix,windows}/*.so
   cd $BUILD && zip -r9 ../Wine64-$VERSION.zip opt
   ```
   Must run inside a Debian/Ubuntu amd64 container or VM (Wine's
   configure script will reject the build if the host is not x86_64
   Linux). Output is a single `Wine64-$VERSION.zip` file usable
   directly as the second FsZip mount, in the same way Wine9.0.zip
   layers on top of TinyCore16.zip today.

3. **Release build config with `BOXEDWINE_GUEST_X64=1`** — currently
   only Xcode Debug has this preprocessor flag set. Linux contributor
   should add it to the equivalent Linux Release config (CMake
   `add_compile_definitions(BOXEDWINE_GUEST_X64)` in the relevant
   target). Without it, the 64-bit interpreter and `--x64-selftest`
   harness are excluded at compile time.

4. ~~**UI launcher hook**~~ — done (see "Done" section above).

## Exit criterion (per roadmap)

> Milestone D done when: `Boxedwine --zip TinyCore16x64WineBase.zip /bin/ls /`
> runs `ls` from the 64-bit rootfs and lists files. `Boxedwine
> --zip Wine64-9.0.zip wine64 /opt/wine64/share/wine/programs/notepad/notepad.exe.so`
> launches notepad's stub.

Both depend on a real x86_64 rootfs being available, so neither
can be verified on this build host.

## Why ship D1 (detection) without the rest

The detection code is harmless on its own (a single bool that
nothing reads yet), and shipping it now means a Linux contributor
who builds the rootfs doesn't also need to remember to wire the
detection. It also gives us a place to hang regression tests
against synthetic zips that contain the right path markers, even
without a real rootfs to exec.
