# Status

| Block | State |
|---|---|
| Scorpion CPU (MIDR 510f00f2, impl-defined cp15) | done |
| VIC, SIRC | done |
| GPT/DGT timers, A2M doorbells (CSR) | done |
| GPIO (165 pins, interrupts) | done |
| Fake ARM9: SMEM TOC, DEX, proc_comm, SMSM/DEM sleep handshake, deep-sleep wake | done |
| SMD channels + DAL server (q6audio DSP, ACDB, ADIE), ONCRPC router (PMIC, PM, USB, HTC) | basic |
| Adreno 200 GPU (KGSL) | todo: ROMs need tools/mkcompat.py (software GL) |
| Wi-Fi, radio, audio output, camera | todo |
| ram_console mirror to serial | done |
| I2C controller, touchscreen (type 2A), MicroP, regfile PMIC/compass | done |
| Data mover (ADM), NAND (raw 2112-byte pages, cLK partition ATAG) | done |
| MDP 3.1 + LCDC display (480x800, RGB565/XRGB8888) | done |
| SDCC (microSD, PIO + data mover box mode) | done |
| Hardware buttons (keypad matrix + power) | done |
| Android boot.img as -kernel (HLE cLK) | done |
| Boot from NAND (raw boot.img or yaffs2 zImage+initrd.gz) | done |
| Android app | todo |

Reference kernel: tytung htcleo 2.6.32 completes all driver init in ~2 s and
stops at mounting the root fs (no ROM attached yet).

Verified: unmodified CWM 5.0.2.6 for leo (recovery_leo.img) boots to its menu.
Verified: CWM installs NexusHD2-Gingerbread V3.2a from the emulated SD card into NAND.
Verified: NexusHD2 GB kernel boots from emulated NAND, Android init starts; stops at q6audio (needs SMD/DAL DSP emulation).

Milestone: NexusHD2-Gingerbread V3.2a boots to the Android 2.3.7 setup wizard
from emulated NAND; touch, keys, power-key wake from deep sleep work.

Kernel quirks applied at run time (hw/arm/htcleo/leo_quirks.c):
- INT_A9_M2A_4 marked as an SMSM wake-up irq (allows idle power collapse)
- the q6audio SMD channel is declared as a modem channel and renamed in the
  kernel to DSP_DAL_AQ_AUD so it is serviced from the modem SMD interrupt
