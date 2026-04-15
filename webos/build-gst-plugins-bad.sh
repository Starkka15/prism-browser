#!/usr/bin/env bash
# Build gst-plugins-bad 1.18.6 (minimal) for HP TouchPad
# We need: scaletempo (time-stretching for playback rate), webrtcdsp (optional),
#          webvtt (subtitle encoder for WebKit)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/gstreamer/gst-plugins-bad-1.18.6"
BUILD_DIR="$SCRIPT_DIR/build/gst-plugins-bad"
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
    -Dtests=disabled \
    -Dexamples=disabled \
    -Dnls=disabled \
    -Dorc=disabled \
    -Dintrospection=disabled \
    -Daom=disabled \
    -Davtp=disabled \
    -Dbluez=disabled \
    -Dbs2b=disabled \
    -Dbz2=disabled \
    -Dchromaprint=disabled \
    -Dclosedcaption=disabled \
    -Dcolormanagement=disabled \
    -Dcurl=disabled \
    -Dd3dvideosink=disabled \
    -Ddc1394=disabled \
    -Ddirectfb=disabled \
    -Ddts=disabled \
    -Ddvb=disabled \
    -Dfaac=disabled \
    -Dfaad=disabled \
    -Dfdkaac=disabled \
    -Dflite=disabled \
    -Dgme=disabled \
    -Dgsm=disabled \
    -Dipcpipeline=disabled \
    -Dkate=disabled \
    -Dlv2=disabled \
    -Dmicrodns=disabled \
    -Dmodplug=disabled \
    -Dmpeg2enc=disabled \
    -Dmplex=disabled \
    -Dmsdk=disabled \
    -Dmusepack=disabled \
    -Dnvcodec=disabled \
    -Dofa=disabled \
    -Dopenexr=disabled \
    -Dopenni2=disabled \
    -Dopensles=disabled \
    -Dopus=disabled \
    -Dresindvd=disabled \
    -Drsvg=disabled \
    -Drtmp=disabled \
    -Dsbc=disabled \
    -Dsctp=disabled \
    -Dsndfile=disabled \
    -Dsoundtouch=disabled \
    -Dspandsp=disabled \
    -Dsrtp=disabled \
    -Dsvthevcenc=disabled \
    -Dteletext=disabled \
    -Dtinyalsa=disabled \
    -Dttml=disabled \
    -Duvch264=disabled \
    -Dva=disabled \
    -Dvoaacenc=disabled \
    -Dvoamrwbenc=disabled \
    -Dvulkan=disabled \
    -Dwasapi=disabled \
    -Dwasapi2=disabled \
    -Dwebp=disabled \
    -Dwebrtc=disabled \
    -Dwebrtcdsp=disabled \
    -Dwildmidi=disabled \
    -Dwinks=disabled \
    -Dwpe=disabled \
    -Dx265=disabled \
    -Dzbar=disabled \
    -Dzxing=disabled \
    2>&1 | tee "$BUILD_DIR/meson.log"

ninja -j4
ninja install
echo "gst-plugins-bad-1.18.6 done."
