#!/usr/bin/env bash
# Cross-compile qemu-system-arm (htc-leo) for Android as a PIE executable.
# The result is shipped in the APK as lib/<abi>/libqemu-leo.so.
#   ABI=arm64-v8a|x86_64  NDK=/path/to/ndk  build-qemu.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ABI="${ABI:-arm64-v8a}"
API="${API:-28}"
NDK="${NDK:?set NDK to the Android NDK path}"
case "$ABI" in
    arm64-v8a) TRIPLE=aarch64-linux-android; CPU=aarch64 ;;
    x86_64)    TRIPLE=x86_64-linux-android; CPU=x86_64 ;;
esac
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
PREFIX="$ROOT/build/android/$ABI/prefix"
QEMU_SRC="${QEMU_SRC:-$ROOT/build/qemu-src}"
BUILD="$QEMU_SRC/build-android-$ABI"
OUT="$ROOT/android/app/src/main/jniLibs/$ABI"

export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH="" PKG_CONFIG=pkg-config
mkdir -p "$BUILD" "$OUT"
cd "$BUILD"
if [ ! -f build.ninja ]; then
    ../configure --target-list=arm-softmmu \
        --cross-prefix="$TC/bin/llvm-" \
        --cc="$TC/bin/$TRIPLE$API-clang" --cxx="$TC/bin/$TRIPLE$API-clang++" \
        --host-cc=cc --cpu="$CPU" \
        --extra-cflags="-I$PREFIX/include -O2" --extra-ldflags="-L$PREFIX/lib" \
        --disable-docs --disable-werror --disable-tools --disable-guest-agent \
        --disable-kvm --disable-xen --disable-sdl --disable-gtk --disable-opengl \
        --disable-spice --disable-curses --disable-cocoa --disable-pa \
        --disable-alsa --disable-oss --disable-jack --disable-pipewire --disable-sndio \
        --disable-libusb --disable-usb-redir --disable-smartcard --disable-vhost-user \
        --disable-vhost-net --disable-vhost-kernel --disable-vhost-vdpa \
        --disable-linux-aio --disable-linux-io-uring --disable-seccomp \
        --disable-cap-ng --disable-attr --disable-libudev --disable-mpath \
        --disable-vnc-jpeg --disable-png --disable-vnc-sasl --enable-vnc \
        --enable-slirp --disable-plugins --enable-fdt=internal --disable-zstd \
        --disable-bzip2 --disable-lzo --disable-snappy --disable-gnutls \
        --disable-nettle --disable-gcrypt --disable-crypto-afalg --disable-libssh \
        --disable-curl --disable-rbd --disable-glusterfs --disable-libiscsi \
        --disable-libnfs --disable-virglrenderer --disable-u2f --disable-tpm \
        --disable-membarrier --disable-libdw --disable-qom-cast-debug \
        --without-default-devices --audio-drv-list=
fi
ninja qemu-system-arm
# --without-default-devices silently drops boards: make sure ours is in
if [ "$(strings qemu-system-arm | grep -cx 'htc-leo-machine')" = 0 ]; then
    echo "error: htc-leo machine missing from the Android build" >&2
    exit 1
fi
cp qemu-system-arm "$OUT/libqemu-leo.so"
# data files QEMU loads at run time (-L): the VNC keyboard layout
ASSETS="$ROOT/android/app/src/main/assets/qemu/keymaps"
mkdir -p "$ASSETS"
cp "$QEMU_SRC/pc-bios/keymaps/en-us" "$ASSETS/"
"$TC/bin/llvm-strip" "$OUT/libqemu-leo.so"
ls -la "$OUT/libqemu-leo.so"
