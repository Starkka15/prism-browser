#!/usr/bin/env bash
# Build libwpe for HP TouchPad (ARMv7, WPE WebKit backend interface library)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/libwpe"
BUILD="$SCRIPT_DIR/build/libwpe"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"
KHRONOS="$SCRIPT_DIR/deps/khronos"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

mkdir -p "$BUILD" && cd "$BUILD"

cmake "$SRC" \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-I$PREFIX/include -I$KHRONOS --sysroot=$SYSROOT" \
    -DCMAKE_CXX_FLAGS="-I$PREFIX/include -I$KHRONOS --sysroot=$SYSROOT" \
    -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib" \
    -DCMAKE_SHARED_LINKER_FLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib" \
    -DBUILD_DOCS=OFF \
    -DWPE_ENABLE_XKB=OFF

make -j$(nproc) && make install
echo "libwpe done."
