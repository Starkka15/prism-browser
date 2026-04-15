#!/usr/bin/env bash
# Build GStreamer 1.x palmvideodec plugin for HP TouchPad
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/gst-palm-video-decoder"
BUILD_DIR="$SCRIPT_DIR/build/gst-palm-video-decoder"
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
    --libdir=lib \
    2>&1 | tee "$BUILD_DIR/meson.log"

ninja -j4
ninja install
echo "gst-palm-video-decoder done."
