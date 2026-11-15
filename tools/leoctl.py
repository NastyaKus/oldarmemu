#!/usr/bin/env python3
"""Drive a running htc-leo QEMU through QMP.

  leoctl.py SOCK key <name> [hold_ms]     press an HD2 button
  leoctl.py SOCK tap X Y                  tap the screen (480x800 coords)
  leoctl.py SOCK swipe X1 Y1 X2 Y2        drag a finger
  leoctl.py SOCK wake                     press power only if the screen is off
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

def swipe(q, x1, y1, x2, y2, steps=12, ms=400):
    def ev(x, y, down):
        return [{"type": "abs", "data": {"axis": "x", "value": int(x * 0x7fff / 479)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7fff / 799)}},
                {"type": "btn", "data": {"down": down, "button": "left"}}]
    for i in range(steps + 1):
        x = x1 + (x2 - x1) * i / steps
        y = y1 + (y2 - y1) * i / steps
        q.cmd("input-send-event", events=ev(x, y, True))
        time.sleep(ms / 1000 / steps)
    q.cmd("input-send-event", events=ev(x2, y2, False))
    time.sleep(0.25)

def screen_on(q):
    """True if the LCDC (MDP 0xAA2E0000) is scanning out."""
    r = q.hmp("xp /1wx 0xaa2e0000")
    return r.strip().endswith("0x00000001")

def wake(q):
    if not screen_on(q):
        key(q, "power", 200)
        for _ in range(30):
            time.sleep(0.5)
            if screen_on(q):
                break
    time.sleep(2)

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
    elif op == "wake":
        wake(q)
    elif op == "screen":
        print("on" if screen_on(q) else "off")
    elif op == "swipe":
        swipe(q, *[int(v) for v in argv[1:5]])
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
