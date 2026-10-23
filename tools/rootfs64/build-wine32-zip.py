#!/usr/bin/env python3
"""build-wine32-zip.py -- assemble wine32.zip (M22): the i386 half of Debian
bookworm's multiarch wine 8.0~repack-4, i.e. the SAME wine build as the
wine64.zip overlay, so 32-bit wine processes speak the exact wineserver
protocol of the session's 64-bit wineserver (single-session WoW64, the way
Debian's wine32 + wine64 packages work together).

No Docker and no apt: packages come straight from deb.debian.org, are
verified against the signed-index SHA256, and unpacked with ar/tar. Only what
the 32-bit side actually loads is shipped:

  * the wine32 + libwine packages' loader and i386-unix / i386-windows trees
  * the i386 ELF DT_NEEDED closure of the loader + every i386-unix module,
    resolved soname -> package through Debian's Contents-i386 index
  * the libraries winex11/wine dlopen() at runtime (never in DT_NEEDED),
    mirroring tools/rootfs64/build-wine64-zip.sh's list

Shared data (/usr/share/wine nls/fonts) is NOT duplicated: wine64.zip already
ships it and both halves read the same tree.

Usage: tools/rootfs64/build-wine32-zip.py [--out tools/rootfs64/dist/wine32.zip]
Cache: tools/rootfs64/work/deb32/ (indexes + .debs, reused across runs)
"""
import argparse, gzip, hashlib, io, lzma, os, shutil, struct, subprocess, sys, tarfile, urllib.request, zipfile

MIRROR = "https://deb.debian.org/debian"
SUITE = "bookworm"
WINE_VERSION = "8.0~repack-4"
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "work", "deb32")
STAGE = os.path.join(CACHE, "stage")

ROOT_PACKAGES = ["wine32", "libwine"]
# dlopen()ed at runtime, so no DT_NEEDED edge leads to them (same list and
# reasons as build-wine64-zip.sh: X11 extension libs winex11 probes, glibc's
# lazily loaded unwinder, font + sound stacks).
DLOPEN_SONAMES = [
    "libXrender.so.1", "libXcursor.so.1", "libXfixes.so.3", "libXcomposite.so.1",
    "libXi.so.6", "libXinerama.so.1", "libXxf86vm.so.1", "libXrandr.so.2",
    "libgcc_s.so.1", "libfreetype.so.6", "libfontconfig.so.1", "libasound.so.2",
]
LIB_DIRS = ["lib/i386-linux-gnu", "usr/lib/i386-linux-gnu", "lib", "usr/lib",
            "usr/lib/i386-linux-gnu/pulseaudio"]
# Contents lists every package shipping a basename; a few first matches are
# wrong for us (apulse ships a compat libpulse.so.0 in a private dir).
SONAME_OWNER_OVERRIDES = {"libpulse.so.0": "libpulse0"}
# wine's own unix-side modules (resolved via the loader's rpath, not LIB_DIRS)
WINE_INTERNAL_SONAMES = {"ntdll.so", "win32u.so"}


def fetch(url, dest):
    if os.path.exists(dest):
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    os.replace(tmp, dest)
    return dest


def load_packages(path):
    pk = {}
    for stanza in lzma.open(path, "rt", encoding="utf-8").read().split("\n\n"):
        d = {}
        for line in stanza.split("\n"):
            if line and not line.startswith(" ") and ":" in line:
                k, v = line.split(":", 1)
                d[k] = v.strip()
        if "Package" in d:
            pk[d["Package"]] = d
    return pk


def load_contents(path):
    """basename of every i386 shared library -> owning package (first wins)."""
    owner = {}
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").rsplit(None, 1)
            if len(parts) != 2:
                continue
            fpath, pkgs = parts
            if "i386-linux-gnu/" not in fpath or ".so" not in fpath:
                continue
            base = fpath.rsplit("/", 1)[-1]
            pkg = pkgs.split(",")[0].split("/")[-1]
            owner.setdefault(base, pkg)
    return owner


def deb_unpack(deb, dest):
    """Extract a .deb's data.tar.* into dest (ar container parsed in Python)."""
    data = open(deb, "rb").read()
    assert data[:8] == b"!<arch>\n", deb
    off = 8
    while off < len(data):
        name = data[off:off + 16].decode().strip().rstrip("/")
        size = int(data[off + 48:off + 58].decode().strip())
        body = data[off + 60:off + 60 + size]
        if name.startswith("data.tar"):
            mode = "r:xz" if name.endswith(".xz") else "r:gz" if name.endswith(".gz") else "r:zst"
            if mode == "r:zst":
                body = subprocess.run(["zstd", "-dc"], input=body, capture_output=True, check=True).stdout
                mode = "r:"
            with tarfile.open(fileobj=io.BytesIO(body), mode=mode) as t:
                t.extractall(dest, filter="tar")
            return
        off += 60 + size + (size & 1)
    raise RuntimeError("no data.tar in " + deb)


def elf32_needed(path):
    """DT_NEEDED sonames of an ELF32 little-endian file ([] if not one)."""
    try:
        b = open(path, "rb").read()
    except OSError:
        return []
    if b[:4] != b"\x7fELF" or b[4] != 1:
        return []
    e_phoff, = struct.unpack_from("<I", b, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", b, 42)
    loads, dyn = [], None
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", b, e_phoff + i * e_phentsize)
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dyn = (p_offset, p_filesz)
    if not dyn:
        return []
    def v2o(v):
        for va, off, sz in loads:
            if va <= v < va + sz:
                return off + (v - va)
        return None
    needed, strtab = [], None
    for i in range(0, dyn[1], 8):
        tag, val = struct.unpack_from("<iI", b, dyn[0] + i)
        if tag == 0:
            break
        if tag == 1:
            needed.append(val)
        elif tag == 5:
            strtab = v2o(val)
    if strtab is None:
        return []
    return [b[strtab + n:b.index(b"\0", strtab + n)].decode() for n in needed]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "dist", "wine32.zip"))
    args = ap.parse_args()

    print("=== indexes ===")
    pk = load_packages(fetch(f"{MIRROR}/dists/{SUITE}/main/binary-i386/Packages.xz",
                             os.path.join(CACHE, "Packages-i386.xz")))
    # Arch:all packages (libz-mingw-w64 carries the PE zlib1.dll Debian's wine
    # links user32/etc. against instead of a bundled copy)
    for name, d in load_packages(fetch(f"{MIRROR}/dists/{SUITE}/main/binary-all/Packages.xz",
                                       os.path.join(CACHE, "Packages-all.xz"))).items():
        pk.setdefault(name, d)
    owner = load_contents(fetch(f"{MIRROR}/dists/{SUITE}/main/Contents-i386.gz",
                                os.path.join(CACHE, "Contents-i386.gz")))
    for p in ROOT_PACKAGES:
        if pk[p]["Version"] != WINE_VERSION:
            sys.exit(f"{p} is {pk[p]['Version']}, expected {WINE_VERSION} (must match wine64.zip)")

    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    unpacked = set()

    def install(pkg):
        if pkg in unpacked:
            return
        d = pk[pkg]
        deb = fetch(f"{MIRROR}/{d['Filename']}", os.path.join(CACHE, "debs", os.path.basename(d["Filename"])))
        if hashlib.sha256(open(deb, "rb").read()).hexdigest() != d["SHA256"]:
            os.remove(deb)
            sys.exit(f"SHA256 mismatch for {pkg}")
        deb_unpack(deb, STAGE)
        unpacked.add(pkg)
        print(f"  + {pkg} {d['Version']}")

    print("=== wine32 + libwine ===")
    for p in ROOT_PACKAGES:
        install(p)

    def find_lib(soname):
        for d in LIB_DIRS:
            p = os.path.join(STAGE, d, soname)
            if os.path.lexists(p):
                return p
        return None

    # PE-side system library: wine64.zip carries x86_64-windows/zlib1.dll the
    # same way (Debian resolves it from /usr/<triplet>/lib at build time).
    install("libz-mingw-w64")
    pe_zlib = os.path.join(STAGE, "usr/i686-w64-mingw32/lib/zlib1.dll")
    shutil.copy2(pe_zlib, os.path.join(STAGE, "usr/lib/i386-linux-gnu/wine/i386-windows/zlib1.dll"))

    print("=== i386 shared-library closure ===")
    seeds = []
    for dirpath, _, files in os.walk(STAGE):
        for f in files:
            full = os.path.join(dirpath, f)
            if "/i386-unix/" in full or full.endswith(("/usr/lib/wine/wine", "/usr/lib/wine/wine-preloader")):
                seeds.append(full)
    keep_libs, todo, seen = set(), [], set()
    for s in seeds:
        todo += elf32_needed(s)
    todo += DLOPEN_SONAMES
    while todo:
        so = todo.pop()
        if so in seen or so in WINE_INTERNAL_SONAMES:
            continue
        seen.add(so)
        path = find_lib(so)
        if not path:
            pkg = SONAME_OWNER_OVERRIDES.get(so) or owner.get(so)
            if not pkg or pkg not in pk:
                print(f"  ! no i386 package provides {so}")
                continue
            install(pkg)
            path = find_lib(so)
            if not path:
                print(f"  ! {pkg} did not provide {so}")
                continue
        keep_libs.add(path)
        todo += elf32_needed(os.path.realpath(path))

    print("=== zip ===")
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    count = 0
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        def add(full):
            nonlocal count
            rel = os.path.relpath(full, STAGE)
            real = os.path.realpath(full)
            if os.path.isdir(real):
                return
            z.write(real, rel)          # symlinks dereferenced, like cp -aL
            count += 1
        for dirpath, _, files in os.walk(STAGE):
            for f in files:
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, STAGE)
                if rel.startswith(("usr/lib/wine/", "usr/lib/i386-linux-gnu/wine/")) or rel == "usr/bin/wine":
                    add(full)
        for lib in sorted(keep_libs):
            add(lib)
        # the i386 dynamic loader under every name the PT_INTERP / ldd use
        ld = find_lib("ld-linux.so.2")
        if ld:
            z.write(os.path.realpath(ld), "lib/ld-linux.so.2")
            count += 1
    print(f"wrote {args.out}: {count} files, {os.path.getsize(args.out) / 1e6:.1f} MB, "
          f"{len(unpacked)} packages, {len(keep_libs)} libraries")


if __name__ == "__main__":
    main()
