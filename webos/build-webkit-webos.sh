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
EXTRA_CFLAGS="-I$PREFIX/include -I$SYSROOT/usr/include -D_GNU_SOURCE -D_DEFAULT_SOURCE -D__STDC_FORMAT_MACROS"
EXTRA_CFLAGS="$EXTRA_CFLAGS -Wno-error=format-overflow -Wno-error=restrict"
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
    -DENABLE_VIDEO=OFF \
    -DENABLE_WEB_AUDIO=OFF \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_MEDIA_SOURCE=OFF \
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
