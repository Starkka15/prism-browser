#!/usr/bin/env bash
# Build libjpeg-turbo 2.1.5.1 for HP TouchPad (ARMv7, NEON)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/libjpeg-turbo-2.1.5.1"
TARBALL="$SCRIPT_DIR/deps/src/libjpeg-turbo-2.1.5.1.tar.gz"
BUILD="$SCRIPT_DIR/build/libjpeg-turbo"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"
TOOLCHAIN="$SCRIPT_DIR/toolchain.cmake"

export PATH="$GCC10:$PATH"

if [ ! -d "$SRC" ]; then
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD" && cd "$BUILD"

cmake "$SRC" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX;$SYSROOT;/opt/PalmPDK" \
    -DENABLE_SHARED=ON \
    -DENABLE_STATIC=OFF \
    -DWITH_SIMD=ON \
    -DWITH_JPEG8=ON

make -j$(nproc) && make install
echo "libjpeg-turbo done."
