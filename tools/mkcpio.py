#!/usr/bin/env python3
"""Minimal newc cpio (+gzip) writer: mkcpio.py out.gz file:/init[:mode] ..."""
import gzip, os, sys
def entry(name, data, mode):
    hdr = "070701" + "".join("%08X" % v for v in (
        0, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0))
    out = hdr.encode() + name.encode() + b"\0"
    out += b"\0" * (-len(out) % 4) + data + b"\0" * (-len(data) % 4)
    return out
blob = b""
for spec in sys.argv[2:]:
    src, dst, *m = spec.split(":")
    blob += entry(dst.lstrip("/"), open(src, "rb").read(),
                  int(m[0], 8) if m else 0o100755)
blob += entry("TRAILER!!!", b"", 0)
with gzip.open(sys.argv[1], "wb") as f:
    f.write(blob)
