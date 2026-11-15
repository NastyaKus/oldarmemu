#!/usr/bin/env python3
"""Inspect / build htc-leo NAND images (raw 2112-byte pages, bit-inverted).

  nandtool.py dump IMG PAGE [COUNT]      hexdump logical pages (data+oob16)
  nandtool.py ptable [NAND_MB] [SYS_MB] [CACHE_MB]
"""
import sys

CW, USER, CWS, PAGE = 528, 516, 4, 2048
RAW = CW * CWS
BBM = PAGE - CW * (CWS - 1)        # 464
SPARE = USER + 10                   # 526

def read_page(f, page):
    """HD2 layout: per codeword 512 data, 10 ecc, 4 spare (oob), 2 bbm."""
    f.seek(page * RAW)
    raw = bytes(255 - b for b in f.read(RAW))
    data, oob = bytearray(), bytearray()
    for cw in range(CWS):
        c = bytearray(raw[cw * CW:(cw + 1) * CW])
        if cw == CWS - 1:
            c[BBM:BBM + 2] = c[CW - 2:CW]
        data += c[:512]
        oob += c[522:526]
    return bytes(data), bytes(oob)

def ptable(nand_mb=512, sys_mb=150, cache_mb=44):
    nblocks = nand_mb * 8
    start, end = 0x219, nblocks - 1 - 186 - 4
    out = []
    for name, mb in (("misc", 1), ("recovery", 5), ("boot", 5),
                     ("system", sys_mb), ("cache", cache_mb), ("userdata", 0)):
        ln = mb * 8 if mb else end - start
        out.append((name, start, ln))
        start += ln
    return out

if __name__ == "__main__":
    op = sys.argv[1]
    if op == "dump":
        with open(sys.argv[2], "rb") as f:
            p0 = int(sys.argv[3], 0)
            for p in range(p0, p0 + int(sys.argv[4]) if len(sys.argv) > 4 else p0 + 1):
                d, o = read_page(f, p)
                print("page %d oob %s data %s" % (p, o.hex(), d[:48].hex()))
    elif op == "ptable":
        for n, s, l in ptable(*[int(x) for x in sys.argv[2:]]):
            print("%-9s start %5d len %5d  (page %d)" % (n, s, l, s * 64))
