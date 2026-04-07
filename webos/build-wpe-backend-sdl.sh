#!/usr/bin/env bash
set -e
SCRIPT_DIR="/home/starkka15/prism-browser/webos"
SRC="$SCRIPT_DIR/wpe-backend-sdl"
BUILD="$SCRIPT_DIR/build/wpe-backend-sdl"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
PDK="/opt/PalmPDK"
TOOLCHAIN="$SCRIPT_DIR/toolchain.cmake"

rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"

cmake "$SRC" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX;$SYSROOT;$PDK;$PDK/device" \
    -DWPE_INCLUDE_DIRS="$PREFIX/include/wpe-1.0" \
    -DWPE_LIBRARIES="$PREFIX/lib/libwpe-1.0.so" \
    -DEGL_INCLUDE_DIRS="$PDK/include" \
    -DEGL_LIBRARIES="$PDK/device/lib/libEGL.so" \
    -DSDL_INCLUDE_DIRS="$PDK/include/SDL" \
    -DSDL_LIBRARIES="$PDK/device/lib/libSDL.so" \
    -DCMAKE_C_FLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -I$PDK/include -I$PDK/include/SDL -I$PREFIX/include/wpe-1.0 --sysroot=$SYSROOT" \
    -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib -L$PDK/device/lib" \
    -DCMAKE_SHARED_LINKER_FLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib -L$PDK/device/lib"

make -j$(nproc) && make install
echo "wpe-backend-sdl done."
