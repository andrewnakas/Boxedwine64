# Milestone D status — rootfs + Wine64 build pipeline

This document tracks what's done, what's blocked, and what a Linux
contributor needs to do to finish Milestone D of the 64-bit roadmap
(see `docs/PLAN_64BIT.md` §3.9 and the production roadmap in commit
history under "Milestone D").

## Done (in-tree, no external deps)

- **`fszip.cpp` x86_64 layout detection** — commit `26e5e10f`.
  `FsZip::guestIs64` is true when any archive entry path contains
  `x86_64-linux-gnu/`, `x86_64-unix/`, `x86_64-windows/`, or
  `/lib64/`. Verified false against the existing 32-bit zips
  (TinyCore16, Wine7.0, Wine9.0).

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

4. **UI launcher hook** — `source/ui/data/globalSettings.cpp` and
   `source/ui/controls/installView.cpp` currently auto-pick the
   32-bit container for any installed Wine zip. Add a branch: if
   the FsZip's `guestIs64` flag is true, set the container's
   `is64Bit` flag (and surface a "Wine64" badge in the install
   view). The detection is already in place (D1); only the consumer
   side is missing.

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
