#!/usr/bin/env bash
# Run a host build configured like the Android one (build-hostlike) from an
# empty directory with the exact arguments the app uses, so missing data
# files or boards show up before an APK ships.
#   smoke-test.sh RECOVERY.img [SDCARD.img]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/qemu-src/build-hostlike/qemu-system-arm"
D="$(mktemp -d)"
mkdir -p "$D/qemu-data/keymaps"
cp "$BIN" "$D/libqemu-leo.so"
cp "$ROOT/android/app/src/main/assets/qemu/keymaps/en-us" "$D/qemu-data/keymaps/"
touch "$D/nand.img"
SD="${2:-}"
[ -n "$SD" ] || { truncate -s 1G "$D/sd.img"; SD="$D/sd.img"; }
cd "$D"
TMPDIR="$D" timeout 40 ./libqemu-leo.so -L "$D/qemu-data" \
    -M htc-leo,nand-size=512 -accel tcg,tb-size=128 -nodefaults \
    -drive if=mtd,format=raw,file="$D/nand.img" -drive if=sd,format=raw,file="$SD" \
    -display vnc=unix:"$D/vnc.sock" -qmp unix:"$D/qmp.sock",server=on,wait=off \
    -serial file:"$D/console.log" -monitor none -kernel "$1" > qemu.log 2>&1 &
sleep 20
if [ -S "$D/vnc.sock" ] && grep -q 'Freeing init memory' "$D/console.log"; then
    echo "smoke test OK ($D)"
else
    echo "smoke test FAILED:"; cat qemu.log; tail -5 console.log 2>/dev/null; exit 1
fi
kill %1 2>/dev/null || true
