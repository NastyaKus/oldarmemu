#!/usr/bin/env python3
"""Drive a running htc-leo QEMU through QMP.

  leoctl.py SOCK key <name> [hold_ms]     press an HD2 button
  leoctl.py SOCK tap X Y                  tap the screen (480x800 coords)
  leoctl.py SOCK shot out.png             screenshot
  leoctl.py SOCK hmp "<command>"          raw HMP command
  leoctl.py SOCK script FILE              run lines of the above (no SOCK)

Buttons: volup voldown windows call end back home power
"""
import json, socket, sys, time, os, tempfile

KEYS = {"volup": "pgup", "voldown": "pgdn", "windows": "f1", "call": "f2",
        "end": "f3", "back": "esc", "home": "home", "power": "f4"}

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        r = self._read()
        if "error" in r:
            raise RuntimeError(r["error"])
        return r.get("return")

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

def key(q, name, hold=120):
    k = KEYS.get(name, name)
    q.cmd("input-send-event", events=[{"type": "key", "data": {
        "down": True, "key": {"type": "qcode", "data": k}}}])
    time.sleep(hold / 1000)
    q.cmd("input-send-event", events=[{"type": "key", "data": {
        "down": False, "key": {"type": "qcode", "data": k}}}])
    time.sleep(0.25)

def tap(q, x, y, hold=120):
    ax = int(x * 0x7fff / 479)
    ay = int(y * 0x7fff / 799)
    ev = lambda down: [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
        {"type": "btn", "data": {"down": down, "button": "left"}}]
    q.cmd("input-send-event", events=ev(True))
    time.sleep(hold / 1000)
    q.cmd("input-send-event", events=ev(False))
    time.sleep(0.25)

def shot(q, out):
    tmp = tempfile.mktemp(suffix=".ppm")
    q.cmd("screendump", filename=tmp)
    try:
        from PIL import Image
        Image.open(tmp).save(out)
        os.unlink(tmp)
    except ImportError:
        os.rename(tmp, out)

def run(q, argv):
    op = argv[0]
    if op == "key":
        key(q, argv[1], int(argv[2]) if len(argv) > 2 else 120)
    elif op == "tap":
        tap(q, int(argv[1]), int(argv[2]))
    elif op == "shot":
        shot(q, argv[1])
    elif op == "hmp":
        print(q.hmp(argv[1]))
    elif op == "sleep":
        time.sleep(float(argv[1]))
    else:
        raise SystemExit("unknown op " + op)

if __name__ == "__main__":
    q = QMP(sys.argv[1])
    if sys.argv[2] == "script":
        for line in open(sys.argv[3]):
            line = line.split("#")[0].strip()
            if line:
                run(q, line.split())
    else:
        run(q, sys.argv[2:])
