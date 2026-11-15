#!/usr/bin/env bash
# Build a desktop (Linux) qemu-system-arm with the htc-leo machine for debugging.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_SRC="${QEMU_SRC:-$ROOT/build/qemu-src}"
BUILD="$QEMU_SRC/build"
if [ ! -f "$BUILD/build.ninja" ]; then
    mkdir -p "$BUILD" && cd "$BUILD"
    ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
        --disable-kvm --disable-xen --disable-vnc --disable-sdl --disable-gtk \
        --disable-opengl --disable-tools --disable-guest-agent \
        --enable-debug-info
fi
ninja -C "$BUILD" qemu-system-arm
ls -la "$BUILD/qemu-system-arm"
