#!/usr/bin/env bash
# Build the Prism Browser shell for HP TouchPad (ARMv7, webOS 3.0.5)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/prism-browser"
BUILD="$SCRIPT_DIR/build/prism-browser"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
PDK="/opt/PalmPDK"
TOOLCHAIN="$SCRIPT_DIR/toolchain.cmake"
GCC10_ARM_LIB="$HOME/webos-touchpad-modernize/toolchain/gcc10/arm-none-linux-gnueabi/lib"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

mkdir -p "$BUILD" && cd "$BUILD"

cmake "$SRC" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX;$SYSROOT;$PDK;$PDK/device" \
    -DCMAKE_C_FLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -I$PREFIX/include --sysroot=$SYSROOT -D_GNU_SOURCE" \
    -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib -L$GCC10_ARM_LIB -lrt -Wl,--allow-shlib-undefined -Wl,-rpath-link,$PREFIX/lib -Wl,-rpath-link,$SYSROOT/usr/lib"

make -j$(nproc) && make install
echo "prism-browser done."
