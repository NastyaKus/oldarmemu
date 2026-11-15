#!/usr/bin/env bash
# Boot a kernel on the htc-leo machine headless; ram_console goes to stdout.
#   run-leo.sh <zImage> [seconds] [extra qemu args...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="${QEMU:-$ROOT/build/qemu-src/build/qemu-system-arm}"
K="$1"; T="${2:-30}"; shift; shift || true
LOG="${UNIMP_LOG:-$ROOT/build/leo-unimp.log}"
exec timeout "$T" "$Q" -M htc-leo -kernel "$K" \
    -append "${CMDLINE:-console=ram}" \
    -display none -serial stdio -monitor "${MONITOR:-none}" \
    -d unimp,guest_errors -D "$LOG" "$@"
