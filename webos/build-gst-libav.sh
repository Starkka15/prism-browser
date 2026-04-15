#!/usr/bin/env bash
# Cross-compile gst-libav 1.18.6 for HP TouchPad (ARMv7 softfp)
# Requires FFmpeg already built in out/
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/gstreamer/gst-libav-1.18.6"
BUILD_DIR="$SCRIPT_DIR/build/gst-libav"
PREFIX="$SCRIPT_DIR/out"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

meson setup "$SRC" \
    --cross-file "$SCRIPT_DIR/meson-cross-webos.ini" \
    --prefix="$PREFIX" \
    --buildtype=release \
    --default-library=shared \
    -Ddoc=disabled \
    2>&1 | tee "$BUILD_DIR/meson.log"

ninja -j4
ninja install
echo "gst-libav 1.18.6 done."
