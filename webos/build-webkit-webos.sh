#!/usr/bin/env bash
# Build WPE WebKit 2.38 for HP TouchPad (ARMv7, webOS 3.0.5)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WEBKIT_SRC="$SCRIPT_DIR/deps/webkit"
BUILD_DIR="$SCRIPT_DIR/build/webkit"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
PDK="/opt/PalmPDK"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
TOOLCHAIN="$SCRIPT_DIR/toolchain.cmake"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

mkdir -p "$BUILD_DIR"

# Extra include/link flags forwarded to all targets
# ARM flags must be here (not just toolchain.cmake) because -DCMAKE_CXX_FLAGS overrides CMAKE_CXX_FLAGS_INIT
EXTRA_CFLAGS="-march=armv7-a -mtune=cortex-a9 -mthumb -mthumb-interwork -mfpu=neon -mfloat-abi=softfp"
# PREFIX/include must come before PDK/include so our cross-compiled headers
# (zlib 1.3.1 with z_const, etc.) take priority over PDK's older versions.
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$PREFIX/include -I$SYSROOT/usr/include"
EXTRA_CFLAGS="$EXTRA_CFLAGS -I$PDK/include -I$PDK/include/SDL"
EXTRA_CFLAGS="$EXTRA_CFLAGS -D_GNU_SOURCE -D_DEFAULT_SOURCE -D__STDC_FORMAT_MACROS"
EXTRA_CFLAGS="$EXTRA_CFLAGS -Wno-error=format-overflow -Wno-error=restrict"
# glibc 2.8 sysroot lacks kernel 3.4+ madvise constants (needed by bmalloc)
EXTRA_CFLAGS="$EXTRA_CFLAGS -DMADV_DONTDUMP=16 -DMADV_DODUMP=17"
EXTRA_LDFLAGS="-L$PREFIX/lib -L$SYSROOT/usr/lib -Wl,-rpath-link,$PREFIX/lib"
EXTRA_LDFLAGS="$EXTRA_LDFLAGS -Wl,-rpath-link,$SYSROOT/usr/lib"
EXTRA_LDFLAGS="$EXTRA_LDFLAGS -lrt"
GCC10_ARM_LIB="$HOME/webos-touchpad-modernize/toolchain/gcc10/arm-none-linux-gnueabi/lib"
EXTRA_LDFLAGS="$EXTRA_LDFLAGS -L$GCC10_ARM_LIB -Wl,-rpath-link,$GCC10_ARM_LIB"

cd "$BUILD_DIR"

cmake "$WEBKIT_SRC" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX;$SYSROOT;/opt/PalmPDK;/opt/PalmPDK/device" \
    \
    -DPORT=WPE \
    \
    -DENABLE_JIT=ON \
    -DENABLE_DFG_JIT=ON \
    -DENABLE_C_LOOP=OFF \
    \
    -DUSE_SOUP2=ON \
    -DENABLE_INTROSPECTION=OFF \
    -DENABLE_DOCUMENTATION=OFF \
    -DENABLE_JOURNALD_LOG=OFF \
    -DENABLE_ACCESSIBILITY=OFF \
    -DENABLE_GAMEPAD=OFF \
    -DENABLE_WEBDRIVER=OFF \
    -DENABLE_MINIBROWSER=OFF \
    -DENABLE_API_TESTS=OFF \
    -DENABLE_COG=OFF \
    -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    \
    -DENABLE_VIDEO=ON \
    -DUSE_GSTREAMER_GL=OFF \
    -DENABLE_WEB_AUDIO=ON \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_MEDIA_SOURCE=ON \
    -DENABLE_WEB_RTC=OFF \
    -DENABLE_ENCRYPTED_MEDIA=OFF \
    -DENABLE_WEB_CRYPTO=OFF \
    -DENABLE_XSLT=OFF \
    \
    -DUSE_AVIF=OFF \
    -DUSE_JPEGXL=OFF \
    -DUSE_LCMS=OFF \
    -DUSE_OPENJPEG=OFF \
    -DUSE_WOFF2=OFF \
    -DUSE_LIBHYPHEN=OFF \
    \
    -DENABLE_GEOLOCATION=OFF \
    -DENABLE_NOTIFICATIONS=OFF \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    \
    -DCMAKE_C_FLAGS="$EXTRA_CFLAGS" \
    -DCMAKE_CXX_FLAGS="$EXTRA_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$EXTRA_LDFLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$EXTRA_LDFLAGS" \
    2>&1 | tee "$BUILD_DIR/cmake.log"
