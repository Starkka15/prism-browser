#!/usr/bin/env bash
# Build Nettle 3.9.1 for HP TouchPad (ARMv7, webOS 3.0.5)
# Nettle provides both nettle (symmetric crypto) and hogweed (asymmetric crypto).
# Both are required by GnuTLS.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/nettle-3.9.1"
TARBALL="$SCRIPT_DIR/deps/src/nettle-3.9.1.tar.gz"
BUILD="$SCRIPT_DIR/build/nettle"
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
    echo "Extracting Nettle..."
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f Makefile ]; then
    echo "Configuring Nettle 3.9.1..."
    "$SRC/configure" \
        --host="${CROSS}" \
        --build=x86_64-linux-gnu \
        --prefix="$PREFIX" \
        --enable-shared \
        --disable-static \
        --disable-assembler \
        --with-include-path="$PREFIX/include" \
        --with-lib-path="$PREFIX/lib" \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$LDFLAGS"
fi

echo "Building Nettle..."
make -j$(nproc)
make install

echo "Nettle done."
