#!/usr/bin/env bash
# Build GStreamer 1.18.6 core for HP TouchPad (ARMv7 softfp, webOS 3.0.5)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/gstreamer/gstreamer-1.18.6"
BUILD_DIR="$SCRIPT_DIR/build/gstreamer"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mthumb -mthumb-interwork -mfpu=neon -mfloat-abi=softfp"
CFLAGS="$CFLAGS -I$PREFIX/include -I$SYSROOT/usr/include -D_GNU_SOURCE"
LDFLAGS="-L$PREFIX/lib -L$SYSROOT/usr/lib -Wl,-rpath-link,$PREFIX/lib -Wl,-rpath-link,$SYSROOT/usr/lib"
GCC10_ARM_LIB="$HOME/webos-touchpad-modernize/toolchain/gcc10/arm-none-linux-gnueabi/lib"
LDFLAGS="$LDFLAGS -L$GCC10_ARM_LIB -Wl,-rpath-link,$GCC10_ARM_LIB -lrt"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# GStreamer 1.18.6 uses Meson
meson setup "$SRC" \
    --cross-file "$SCRIPT_DIR/meson-cross-webos.ini" \
    --prefix="$PREFIX" \
    --buildtype=release \
    --default-library=shared \
    -Dintrospection=disabled \
    -Ddbghelp=disabled \
    -Dlibunwind=disabled \
    -Dlibdw=disabled \
    -Dcheck=disabled \
    -Dtests=disabled \
    -Dexamples=disabled \
    -Dbenchmarks=disabled \
    -Dtools=enabled \
    -Dptp-helper-permissions=none \
    2>&1 | tee "$BUILD_DIR/meson.log"

ninja -j4
ninja install
echo "gstreamer-1.18.6 done."
