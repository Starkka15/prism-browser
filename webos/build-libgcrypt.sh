#!/usr/bin/env bash
# Build libgcrypt 1.10.2 for HP TouchPad (ARMv7)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/libgcrypt-1.10.2"
TARBALL="$SCRIPT_DIR/deps/src/libgcrypt-1.10.2.tar.bz2"
BUILD="$SCRIPT_DIR/build/libgcrypt"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export CC="${CROSS}-gcc"
export CXX="${CROSS}-g++"
export AR="${CROSS}-ar"
export RANLIB="${CROSS}-ranlib"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT -I$PREFIX/include"
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
        --with-gpg-error-prefix="$PREFIX" \
        --disable-doc \
        --disable-tests \
        --disable-asm \
        ac_cv_sys_symbol_underscore=no \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$LDFLAGS"
fi

make -j$(nproc) && make install
echo "libgcrypt done."
