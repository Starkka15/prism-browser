#!/usr/bin/env bash
# Build gst-plugins-base 1.18.6 for HP TouchPad (ARMv7 softfp, webOS 3.0.5)
# Required by WebKit: app, pbutils, tag, video elements
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/gstreamer/gst-plugins-base-1.18.6"
BUILD_DIR="$SCRIPT_DIR/build/gst-plugins-base"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

meson setup "$SRC" \
    --cross-file "$SCRIPT_DIR/meson-cross-webos.ini" \
    --prefix="$PREFIX" \
    --buildtype=release \
    --default-library=shared \
    -Dintrospection=disabled \
    -Dtests=disabled \
    -Dexamples=disabled \
    -Dnls=disabled \
    -Dorc=disabled \
    -Dalsa=disabled \
    -Dogg=disabled \
    -Dopus=disabled \
    -Dpango=disabled \
    -Dtheora=disabled \
    -Dvorbis=disabled \
    -Dgl=disabled \
    -Dgl-graphene=disabled \
    -Dxvideo=disabled \
    -Dx11=disabled \
    -Dcdparanoia=disabled \
    -Dlibvisual=disabled \
    2>&1 | tee "$BUILD_DIR/meson.log"

ninja -j4
ninja install
echo "gst-plugins-base-1.18.6 done."
