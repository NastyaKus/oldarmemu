#!/usr/bin/env python3
"""Send HMP commands to a QEMU monitor unix socket: qmon.py SOCK 'cmd' ..."""
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(1.0)
def rd():
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
            if out.endswith(b"(qemu) "): break
    except socket.timeout: pass
    return out.decode(errors="replace")
rd()
for c in sys.argv[2:]:
    s.sendall((c + "\n").encode()); print(rd().replace("(qemu) ", ""))
