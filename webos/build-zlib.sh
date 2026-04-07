#!/usr/bin/env bash
# Build zlib 1.3.1 for HP TouchPad (ARMv7)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$HOME/webos-touchpad-modernize/sources/zlib-1.3.1"
BUILD="$SCRIPT_DIR/build/zlib"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export CC="${CROSS}-gcc"
export AR="${CROSS}-ar"
export RANLIB="${CROSS}-ranlib"
export STRIP="${CROSS}-strip"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT"

mkdir -p "$BUILD"
cp -a "$SRC"/. "$BUILD/"
cd "$BUILD"

CFLAGS="$CFLAGS" ./configure \
    --prefix="$PREFIX" \
    --shared

make -j$(nproc)
make install

echo "zlib done. Installed to $PREFIX"
