#!/usr/bin/env bash
# Cross-compile QEMU's dependencies (libffi, pcre2, glib, pixman, libslirp)
# for Android.
#   ABI=arm64-v8a|x86_64  NDK=/path/to/ndk  build-deps.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ABI="${ABI:-arm64-v8a}"
API="${API:-28}"
NDK="${NDK:?set NDK to the Android NDK path}"
case "$ABI" in
    arm64-v8a) TRIPLE=aarch64-linux-android; CPU=aarch64 ;;
    x86_64)    TRIPLE=x86_64-linux-android; CPU=x86_64 ;;
    *) echo "unsupported ABI $ABI"; exit 1 ;;
esac
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
PREFIX="$ROOT/build/android/$ABI/prefix"
SRC="$ROOT/build/android/src"
WORK="$ROOT/build/android/$ABI/work"
mkdir -p "$PREFIX" "$SRC" "$WORK"

export CC="$TC/bin/$TRIPLE$API-clang" CXX="$TC/bin/$TRIPLE$API-clang++"
export AR="$TC/bin/llvm-ar" RANLIB="$TC/bin/llvm-ranlib" STRIP="$TC/bin/llvm-strip"
export NM="$TC/bin/llvm-nm" LD="$TC/bin/ld.lld"
export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""

fetch() {   # url
    local f="$SRC/$(basename "$1")"
    [ -f "$f" ] || curl -sSL -o "$f" "$1"
    echo "$f"
}

CROSS="$WORK/cross.txt"
cat > "$CROSS" <<EOT
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
ranlib = '$RANLIB'
nm = '$NM'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-O2', '-fPIC']
cpp_args = ['-O2', '-fPIC']
[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'
[host_machine]
system = 'android'
cpu_family = '$CPU'
cpu = '$CPU'
endian = 'little'
EOT

build_autotools() {   # tarball dir args...
    local tb="$1" dir="$2"; shift 2
    [ -f "$PREFIX/.done-$dir" ] && return
    rm -rf "$WORK/$dir" && tar -C "$WORK" -xf "$tb"
    (cd "$WORK/$dir" && ./configure --host="$TRIPLE" --prefix="$PREFIX" \
        --disable-shared --enable-static "$@" > configure.log && \
        make -j"$(nproc)" > make.log && make install > install.log)
    touch "$PREFIX/.done-$dir"
}

build_meson() {   # tarball dir args...
    local tb="$1" dir="$2"; shift 2
    [ -f "$PREFIX/.done-$dir" ] && return
    rm -rf "$WORK/$dir" && tar -C "$WORK" -xf "$tb"
    [ -f "$ROOT/tools/android/patches/$dir.patch" ] && \
        patch -d "$WORK/$dir" -p1 < "$ROOT/tools/android/patches/$dir.patch"
    (cd "$WORK/$dir" && meson setup _build --cross-file "$CROSS" \
        --prefix="$PREFIX" --libdir=lib -Ddefault_library=static \
        --buildtype=release "$@" > setup.log && \
        ninja -C _build > build.log && ninja -C _build install > install.log)
    touch "$PREFIX/.done-$dir"
}

build_autotools "$(fetch https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz)" \
    libffi-3.4.6 --disable-docs
build_autotools "$(fetch https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz)" \
    pcre2-10.44
build_meson "$(fetch https://download.gnome.org/sources/glib/2.82/glib-2.82.5.tar.xz)" \
    glib-2.82.5 -Dtests=false -Dintrospection=disabled -Dnls=disabled \
    -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dsysprof=disabled \
    -Dman-pages=disabled -Ddocumentation=false -Dglib_debug=disabled \
    -Dinstalled_tests=false
build_meson "$(fetch https://cairographics.org/releases/pixman-0.44.2.tar.gz)" \
    pixman-0.44.2 -Dtests=disabled -Dgtk=disabled -Dlibpng=disabled -Ddemos=disabled
build_meson "$(fetch https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.8.0/libslirp-v4.8.0.tar.gz)" \
    libslirp-v4.8.0
echo "deps for $ABI installed in $PREFIX"
