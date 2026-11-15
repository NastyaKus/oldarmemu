#!/usr/bin/env python3
"""Recover Android logcat from a guest RAM dump (QMP pmemsave).

  logcat-from-ram.py ram.bin [filter-regex]

Scans for 2.6/3.0-era struct logger_entry records
  u16 len; u16 pad; s32 pid, tid, sec, nsec; payload = prio, tag\\0, msg\\0
and prints them sorted by timestamp.
"""
import re, struct, sys

data = open(sys.argv[1], "rb").read()
flt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
PRIO = "??VDIWEF"
# guest wall clock: the emulated HD2 starts in 1970 unless it was set
MAX_SEC = int(sys.argv[3]) if len(sys.argv) > 3 else 4 * 10**9
out = {}
# payload starts with a priority byte 2..7 followed by a printable tag
for m in re.finditer(rb"[\x02-\x07][A-Za-z][\x20-\x7e]{0,40}\x00", data):
    p = m.start()
    if p < 20:
        continue
    ln, pad, pid, tid, sec, nsec = struct.unpack_from("<HHiiii", data, p - 20)
    if pad != 0 or not (3 <= ln <= 4076) or not (0 < pid < 65536) \
            or not (0 <= sec < MAX_SEC) or not (0 <= nsec < 1000000000):
        continue
    payload = data[p:p + ln]
    try:
        tag, msg = payload[1:].split(b"\x00", 1)
    except ValueError:
        continue
    msg = msg.rstrip(b"\x00")
    if len(tag) < 2 or b"\x00" in msg or \
            sum(32 <= c < 127 or c in (9, 10) for c in msg) < len(msg) * 0.9:
        continue
    msg = msg.decode("utf-8", "replace")
    line = "%d.%03d %5d %c/%s: %s" % (sec, nsec // 1000000, pid, PRIO[payload[0]],
                                      tag.decode("latin1"), msg)
    if flt and not flt.search(line):
        continue
    out[(sec, nsec, pid, line)] = line
for k in sorted(out):
    print(out[k])
