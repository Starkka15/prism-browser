#!/usr/bin/env bash
# Build GMP 6.3.0 for HP TouchPad (ARMv7, webOS 3.0.5)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/gmp-6.3.0"
TARBALL="$SCRIPT_DIR/deps/src/gmp-6.3.0.tar.xz"
BUILD="$SCRIPT_DIR/build/gmp"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export CC="${CROSS}-gcc"
export CXX="${CROSS}-g++"
export AR="${CROSS}-ar"
export RANLIB="${CROSS}-ranlib"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 -pipe"
CFLAGS="$CFLAGS --sysroot=$SYSROOT -I$SYSROOT/usr/include -I$PREFIX/include"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib -L$SYSROOT/lib"
LDFLAGS="$LDFLAGS -Wl,-rpath-link,$PREFIX/lib -Wl,-rpath-link,$SYSROOT/usr/lib"

if [ ! -d "$SRC" ]; then
    echo "Extracting GMP..."
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f Makefile ]; then
    echo "Configuring GMP 6.3.0..."
    "$SRC/configure" \
        --host="${CROSS}" \
        --build=x86_64-linux-gnu \
        --prefix="$PREFIX" \
        --enable-shared \
        --disable-static \
        --disable-assembly \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$LDFLAGS"
fi

echo "Building GMP..."
make -j$(nproc)
make install

echo "GMP done."
