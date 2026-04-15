#!/usr/bin/env bash
# Cross-compile FFmpeg 6.1 for HP TouchPad (ARMv7 softfp)
# Provides libavcodec/libavformat/libswresample for gst-libav
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/gstreamer/ffmpeg-6.1"
BUILD_DIR="$SCRIPT_DIR/build/ffmpeg"
PREFIX="$SCRIPT_DIR/out"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

"$SRC/configure" \
    --prefix="$PREFIX" \
    --cross-prefix="$CROSS-" \
    --arch=arm \
    --cpu=cortex-a9 \
    --target-os=linux \
    --enable-cross-compile \
    --enable-shared \
    --disable-static \
    --disable-debug \
    --disable-doc \
    --disable-programs \
    --disable-ffmpeg \
    --disable-ffplay \
    --disable-ffprobe \
    \
    --enable-pic \
    --enable-neon \
    --extra-cflags="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fPIC" \
    --extra-ldflags="-L$PREFIX/lib" \
    \
    --disable-everything \
    \
    --enable-decoder=aac \
    --enable-decoder=aac_latm \
    --enable-decoder=mp3 \
    --enable-decoder=mp3float \
    --enable-decoder=vorbis \
    --enable-decoder=opus \
    --enable-decoder=flac \
    --enable-decoder=h264 \
    --enable-decoder=mpeg4 \
    --enable-decoder=h263 \
    --enable-decoder=hevc \
    --enable-decoder=vp8 \
    --enable-decoder=vp9 \
    \
    --enable-parser=aac \
    --enable-parser=aac_latm \
    --enable-parser=h264 \
    --enable-parser=hevc \
    --enable-parser=mpeg4video \
    --enable-parser=mpegaudio \
    --enable-parser=vorbis \
    --enable-parser=opus \
    \
    --enable-demuxer=aac \
    --enable-demuxer=mov \
    --enable-demuxer=matroska \
    --enable-demuxer=mp3 \
    --enable-demuxer=ogg \
    \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    \
    2>&1 | tee "$BUILD_DIR/configure.log"

make -j4
make install
echo "FFmpeg 6.1 done."
