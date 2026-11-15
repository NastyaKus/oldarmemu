#!/usr/bin/env python3
"""Build a recovery-flashable "emulator compatibility" zip for a ROM.

  mkcompat.py ROM.zip OUT.zip

Until the Adreno 200 GPU is emulated, ROMs must use Android's software
OpenGL ES renderer: the zip rewrites /system/lib/egl/egl.cfg to list only
"android".  The edify interpreter (update-binary) is taken from the ROM
zip itself, so nothing third-party is redistributed.
"""
import sys, zipfile

SCRIPT = '''ui_print("HD2 emulator compatibility");
mount("yaffs2", "MTD", "system", "/system");
package_extract_file("egl.cfg", "/system/lib/egl/egl.cfg");
set_perm(0, 0, 0644, "/system/lib/egl/egl.cfg");
unmount("/system");
ui_print("software OpenGL ES enabled");
'''

def main(rom, out):
    with zipfile.ZipFile(rom) as z:
        binary = z.read("META-INF/com/google/android/update-binary")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as o:
        info = zipfile.ZipInfo("META-INF/com/google/android/update-binary")
        info.external_attr = 0o755 << 16
        o.writestr(info, binary)
        o.writestr("META-INF/com/google/android/updater-script", SCRIPT)
        o.writestr("egl.cfg", "0 0 android\n")
    print("created", out)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
