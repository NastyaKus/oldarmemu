#!/usr/bin/env python3
"""Make an AROMA ROM zip installable without its touch UI (for tests).

  aroma-preset.py ROM.zip OUT.zip [file.prop:key=value ...]

The copy runs the ROM's real edify interpreter (update-binary-installer)
and starts by writing the answers AROMA would have saved under
/tmp/aroma.  Defaults suit the NexusHD2 ROMs: NAND install, cLK/MAGLDR
kernel to NAND, RMNET, softkeys on, wipe data, no optional extras.
"""
import shutil
import sys
import zipfile

DEFAULTS = [
    "installtype.prop:selected.1=1",
    "kernel.prop:NAND=yes",
    "mods.prop:selected.1=1",
    "mods.prop:selected.2=1",
    "mods.prop:selected.3=2",
    "customize.prop:item.0.1=0",
]
META = "META-INF/com/google/android/"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    answers = {}
    for a in DEFAULTS + sys.argv[3:]:
        f, kv = a.split(":", 1)
        answers.setdefault(f, {})
        k, v = kv.split("=", 1)
        answers[f][k] = v
    lines = ["mkdir -p /tmp/aroma"]
    for f, kv in answers.items():
        body = "\\n".join(f"{k}={v}" for k, v in kv.items())
        lines.append(f"printf '{body}\\n' > /tmp/aroma/{f}")
    pre = ('ui_print("aroma-preset: answers written");\n'
           'run_program("/sbin/sh", "-c", "' + "; ".join(lines) + '");\n')

    with zipfile.ZipFile(src) as zin, \
            zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
        names = set(zin.namelist())
        if META + "update-binary-installer" not in names:
            sys.exit("not an AROMA zip: no update-binary-installer")
        for info in zin.infolist():
            n = info.filename
            if n.startswith(META + "aroma") or n in (
                    "META-INF/MANIFEST.MF", "META-INF/CERT.SF",
                    "META-INF/CERT.RSA", META + "update-binary",
                    META + "updater-script"):
                continue
            if n == META + "update-binary-installer":
                zout.writestr(META + "update-binary", zin.read(n))
                continue
            with zin.open(info) as fi, zout.open(info, "w") as fo:
                shutil.copyfileobj(fi, fo, 1 << 20)
        script = zin.read(META + "updater-script").decode("utf-8", "replace")
        zout.writestr(META + "updater-script", pre + script)
    print("wrote", out)


if __name__ == "__main__":
    main()
