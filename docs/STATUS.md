# Status

| Block | State |
|---|---|
| Scorpion CPU (MIDR 510f00f2, impl-defined cp15) | done |
| VIC, SIRC | done |
| GPT/DGT timers, A2M doorbells (CSR) | done |
| GPIO (165 pins, interrupts) | done |
| Fake ARM9: SMEM TOC, DEX, proc_comm | basic |
| ram_console mirror to serial | done |
| I2C controller, touchscreen (type 2A), MicroP, regfile PMIC/compass | done |
| Data mover (ADM), NAND (raw 2112-byte pages, cLK partition ATAG) | done |
| MDP 3.1 + LCDC display (480x800, RGB565/XRGB8888) | done |
| SDCC (microSD, PIO + data mover box mode) | done |
| Hardware buttons (keypad matrix + power) | done |
| Android boot.img as -kernel (HLE cLK) | done |
| cLK boot from NAND | todo |
| Android app | todo |

Reference kernel: tytung htcleo 2.6.32 completes all driver init in ~2 s and
stops at mounting the root fs (no ROM attached yet).

Verified: unmodified CWM 5.0.2.6 for leo (recovery_leo.img) boots to its menu.
Verified: CWM installs NexusHD2-Gingerbread V3.2a from the emulated SD card into NAND.
