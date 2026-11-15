# Status

| Block | State |
|---|---|
| Scorpion CPU (MIDR 510f00f2, impl-defined cp15) | done |
| VIC, SIRC | done |
| GPT/DGT timers, A2M doorbells (CSR) | done |
| GPIO (165 pins, interrupts) | done |
| Fake ARM9: SMEM TOC, DEX, proc_comm | basic |
| ram_console mirror to serial | done |
| I2C, touchscreen, MicroP | todo |
| Data mover (ADM), NAND | todo |
| MDP/LCDC display | todo |
| SDCC (microSD) | todo |
| cLK boot from NAND | todo |
| Android app | todo |

Reference kernel: tytung htcleo 2.6.32 boots to driver probing
(framebuffer 480x800, KGSL, NAND init).
