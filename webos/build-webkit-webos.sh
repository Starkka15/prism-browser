#!/usr/bin/env bash
# Build WebKit for HP TouchPad (ARMv7, webOS 3.0.5, SDL 1.2 + Cairo backend)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WEBKIT_SRC="$REPO_ROOT/webos/deps/webkit"
BUILD_DIR="$REPO_ROOT/webos/build/webkit"
INSTALL_DIR="$REPO_ROOT/webos/out"
TOOLCHAIN="$SCRIPT_DIR/toolchain.cmake"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
PDK="/opt/PalmPDK"

export PATH="$GCC10:$PATH"

mkdir -p "$BUILD_DIR" "$INSTALL_DIR"

if [ ! -d "$WEBKIT_SRC" ]; then
    echo "ERROR: WebKit source not found at $WEBKIT_SRC"
    echo "Clone it first:"
    echo "  git clone https://github.com/WebKit/WebKit.git $WEBKIT_SRC --depth=1"
    exit 1
fi

cd "$BUILD_DIR"

cmake "$WEBKIT_SRC" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    \
    -DPORT=WebKitGTK \
    -DENABLE_WEBKIT=ON \
    -DENABLE_WEBKIT2=ON \
    -DENABLE_WEBDRIVER=OFF \
    -DENABLE_TOOLS=OFF \
    -DENABLE_MINIBROWSER=OFF \
    \
    -DUSE_GTK4=OFF \
    -DUSE_GTK3=OFF \
    -DUSE_SDL=ON \
    \
    -DENABLE_GRAPHICS_CONTEXT_3D=OFF \
    -DENABLE_WEBGL=OFF \
    -DENABLE_ACCELERATED_2D_CANVAS=OFF \
    -DUSE_OPENGL=OFF \
    -DUSE_EGL=OFF \
    \
    -DENABLE_GEOLOCATION=OFF \
    -DENABLE_NOTIFICATIONS=OFF \
    -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_MEDIA_SOURCE=OFF \
    -DENABLE_WEB_RTC=OFF \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DENABLE_GAMEPAD=OFF \
    \
    -DUSE_LIBHYPHEN=OFF \
    -DUSE_WOFF2=OFF \
    -DUSE_AVIF=OFF \
    -DUSE_JPEGXL=OFF \
    -DUSE_LIBWEBP=OFF \
    \
    -DENABLE_JIT=ON \
    -DENABLE_C_LOOP=OFF \
    \
    -DCMAKE_C_FLAGS="-I${PDK}/include -I${PDK}/include/SDL" \
    -DCMAKE_CXX_FLAGS="-I${PDK}/include -I${PDK}/include/SDL" \
    -DCMAKE_EXE_LINKER_FLAGS="-L${PDK}/device/lib -lSDL" \
    2>&1 | tee "$BUILD_DIR/cmake.log"

echo ""
echo "CMake done. Starting build..."
ninja -C "$BUILD_DIR" -j$(nproc) 2>&1 | tee "$BUILD_DIR/build.log"
