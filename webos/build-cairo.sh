#!/usr/bin/env bash
# Build Cairo 1.16.0 for HP TouchPad (ARMv7)
# Backend: image (software rasterizer) + EGL/GLES2 surface
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/cairo-1.16.0"
TARBALL="$SCRIPT_DIR/deps/src/cairo-1.16.0.tar.xz"
BUILD="$SCRIPT_DIR/build/cairo"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export CC="${CROSS}-gcc"
export CXX="${CROSS}-g++"
export AR="${CROSS}-ar"
export RANLIB="${CROSS}-ranlib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT -I$PREFIX/include -I$SYSROOT/usr/include -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib"

if [ ! -d "$SRC" ]; then
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD" && cd "$BUILD"

if [ ! -f Makefile ]; then
    "$SRC/configure" \
        --host="$CROSS" \
        --build=x86_64-linux-gnu \
        --prefix="$PREFIX" \
        --enable-shared \
        --disable-static \
        --enable-ft=yes \
        --enable-fc=yes \
        --enable-png=yes \
        --disable-svg \
        --disable-pdf \
        --disable-ps \
        --disable-script \
        --disable-gobject \
        --disable-trace \
        --disable-interpreter \
        --without-x \
        ax_cv_c_float_words_bigendian=no \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$LDFLAGS"
fi

make -j$(nproc) -C src && make -C src install
echo "cairo done."
