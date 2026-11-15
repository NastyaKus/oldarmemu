# oldarmemu — HTC HD2 (Leo) emulator

Goal: an Android app that emulates the HTC HD2 (Qualcomm QSD8250) closely
enough to run the custom Android ROMs, recoveries (CWM/TWRP) and the cLK
bootloader made for it.

The core is a QEMU 10.1 fork: our machine lives in `emu/qemu-overlay`
(symlinked into the QEMU tree) plus small patches in `emu/patches`.

## Layout
- `emu/qemu-overlay/hw/arm/htcleo/` — `htc-leo` machine and QSD8250 devices
  (VIC, SIRC, GPIO, CSR timers, fake ARM9 modem, stubs)
- `emu/patches/` — patches against upstream QEMU (Scorpion CPU, build glue)
- `tools/` — build/run helpers

## Build & run (Linux host)
    ./tools/prepare-qemu.sh       # clone QEMU v10.1.5, apply patches
    ./tools/build-qemu-host.sh    # build qemu-system-arm
    ./tools/run-leo.sh zImage 60  # boot an HD2 kernel, ram_console on stdout

## Status
See `docs/STATUS.md`.
