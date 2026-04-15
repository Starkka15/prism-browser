#!/usr/bin/env bash
# Build gst-plugins-good 1.18.6 for HP TouchPad (ARMv7 softfp, webOS 3.0.5)
# We need: soup (HTTP streaming source), isomp4 (MP4 demux), matroska (WebM/MKV),
#          vpx (VP8 decode), audiofx, autodetect
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/gstreamer/gst-plugins-good-1.18.6"
BUILD_DIR="$SCRIPT_DIR/build/gst-plugins-good"
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
    -Dtests=disabled \
    -Dexamples=disabled \
    -Dnls=disabled \
    -Dorc=disabled \
    -Dsoup=enabled \
    -Disomp4=enabled \
    -Dmatroska=enabled \
    -Dvpx=disabled \
    -Daalib=disabled \
    -Dcairo=disabled \
    -Ddv=disabled \
    -Dflac=disabled \
    -Dgdk-pixbuf=disabled \
    -Dgtk3=disabled \
    -Djack=disabled \
    -Djpeg=disabled \
    -Dlame=disabled \
    -Dlibcaca=disabled \
    -Dmpg123=disabled \
    -Dpng=disabled \
    -Dpulse=disabled \
    -Dqt5=disabled \
    -Dshout2=disabled \
    -Dspeex=disabled \
    -Dtaglib=disabled \
    -Dtwolame=disabled \
    -Dwavpack=disabled \
    -Dximagesrc=disabled \
    -Dv4l2=disabled \
    -Doss=disabled \
    -Doss4=disabled \
    -Dautodetect=enabled \
    -Daudioparsers=enabled \
    -Drtp=enabled \
    -Drtpmanager=enabled \
    -Dudp=enabled \
    -Drtsp=enabled \
    -Ddeinterlace=enabled \
    -Dvideofilter=enabled \
    -Dvideobox=enabled \
    -Dvideocrop=enabled \
    -Dvideomixer=enabled \
    2>&1 | tee "$BUILD_DIR/meson.log"

ninja -j4
ninja install
echo "gst-plugins-good-1.18.6 done."
