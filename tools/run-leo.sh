#!/usr/bin/env bash
# Boot a kernel on the htc-leo machine headless; ram_console goes to stdout.
#   run-leo.sh <zImage|boot.img|nand> [seconds] [extra qemu args...]
#   "nand" boots the NAND boot partition like cLK (BOOTMODE=recovery for recovery)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="${QEMU:-$ROOT/build/qemu-src/build/qemu-system-arm}"
K="$1"; T="${2:-30}"; shift; shift || true
LOG="${UNIMP_LOG:-$ROOT/build/leo-unimp.log}"
if [ "$K" = nand ]; then KARGS=(-M "htc-leo,boot-mode=${BOOTMODE:-boot}");
else KARGS=(-M htc-leo -kernel "$K" -append "${CMDLINE:-console=ram}"); fi
exec timeout "$T" "$Q" "${KARGS[@]}" \
    -display none -serial stdio -monitor "${MONITOR:-none}" \
    ${QMP:+-qmp unix:$QMP,server,nowait} \
    -d unimp,guest_errors -D "$LOG" "$@"
