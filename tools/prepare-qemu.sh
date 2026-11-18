#!/usr/bin/env bash
# Fetch upstream QEMU, overlay our HTC Leo sources and apply patches.
#   QEMU_SRC  - where the QEMU tree lives (default: build/qemu-src)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_TAG="${QEMU_TAG:-v10.1.5}"
QEMU_SRC="${QEMU_SRC:-$ROOT/build/qemu-src}"
QEMU_URL="${QEMU_URL:-https://gitlab.com/qemu-project/qemu.git}"

if [ ! -d "$QEMU_SRC/.git" ]; then
    git clone --depth 1 --branch "$QEMU_TAG" "$QEMU_URL" "$QEMU_SRC"
fi
cd "$QEMU_SRC"
git checkout -q -- . 
git clean -fdq -e build -e build-android
for p in "$ROOT"/emu/patches/*.patch; do
    git apply --whitespace=nowarn "$p"
done
# live symlink so edits in emu/ are picked up without re-running this
ln -sfn "$ROOT/emu/qemu-overlay/hw/arm/htcleo" hw/arm/htcleo
ln -sfn "$ROOT/emu/qemu-overlay/target/arm/leojit" target/arm/leojit
echo "QEMU prepared in $QEMU_SRC"
