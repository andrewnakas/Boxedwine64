#!/usr/bin/env python3
"""fetch-liberation-fonts.py -- M26: metric-compatible core fonts for the prefix.

Downloads Debian bookworm's fonts-liberation2 (SIL Open Font License) from
deb.debian.org, verifies it against the index SHA256, and extracts its TTFs
into tools/rootfs64/work/fonts/. build-prefix64.sh copies them into
C:\\windows\\Fonts and registers wine font replacements so Arial / Times New
Roman / Courier New requests get Liberation Sans / Serif / Mono, which share
their glyph metrics (text lays out at the same widths as on Windows).
"""
import hashlib, io, lzma, os, tarfile, urllib.request

MIRROR = "https://deb.debian.org/debian"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "work", "fonts")
PACKAGES = ["fonts-liberation2"]


def main():
    idx = urllib.request.urlopen(f"{MIRROR}/dists/bookworm/main/binary-all/Packages.xz").read()
    pk = {}
    for stanza in lzma.decompress(idx).decode("utf-8").split("\n\n"):
        d = dict(l.split(":", 1) for l in stanza.split("\n") if ":" in l and not l.startswith(" "))
        if "Package" in d:
            pk[d["Package"].strip()] = {k: v.strip() for k, v in d.items()}
    os.makedirs(OUT, exist_ok=True)
    for name in PACKAGES:
        d = pk[name]
        deb = urllib.request.urlopen(f"{MIRROR}/{d['Filename']}").read()
        if hashlib.sha256(deb).hexdigest() != d["SHA256"]:
            raise SystemExit(f"SHA256 mismatch for {name}")
        off = 8
        while off < len(deb):
            member = deb[off:off + 16].decode().strip().rstrip("/")
            size = int(deb[off + 48:off + 58].decode().strip())
            body = deb[off + 60:off + 60 + size]
            if member.startswith("data.tar"):
                with tarfile.open(fileobj=io.BytesIO(body), mode="r:xz" if member.endswith(".xz") else "r:*") as t:
                    for m in t.getmembers():
                        if m.isfile() and m.name.lower().endswith(".ttf"):
                            data = t.extractfile(m).read()
                            open(os.path.join(OUT, os.path.basename(m.name)), "wb").write(data)
                            print(f"  + {os.path.basename(m.name)} ({len(data)} bytes)")
            off += 60 + size + (size & 1)
        print(f"{name} {d['Version']} -> {OUT}")


if __name__ == "__main__":
    main()
