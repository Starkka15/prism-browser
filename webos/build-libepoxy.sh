#!/usr/bin/env bash
# Build libepoxy 1.5.10 for HP TouchPad (OpenGL function loader, uses PDK EGL)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/libepoxy-1.5.10"
TARBALL="$SCRIPT_DIR/deps/src/libepoxy-1.5.10.tar.gz"
BUILD="$SCRIPT_DIR/build/libepoxy"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
PDK="/opt/PalmPDK"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

KHRONOS="$SCRIPT_DIR/deps/khronos"
CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT -I$KHRONOS -I$PDK/include"
LDFLAGS="--sysroot=$SYSROOT -L$PDK/device/lib -L$SYSROOT/usr/lib"

if [ ! -d "$SRC" ]; then
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD" && cd "$BUILD"

cat > cross.ini << EOF
[binaries]
c = '${GCC10}/${CROSS}-gcc'
cpp = '${GCC10}/${CROSS}-g++'
ar = '${GCC10}/${CROSS}-ar'
strip = '${GCC10}/${CROSS}-strip'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['$(echo $CFLAGS | sed "s/ /', '/g")']
c_link_args = ['$(echo $LDFLAGS | sed "s/ /', '/g")']

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF

meson setup "$BUILD" "$SRC" \
    --cross-file cross.ini \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Degl=yes \
    -Dglx=no \
    -Dx11=false \
    -Dtests=false \
    -Ddocs=false

ninja -j$(nproc) && ninja install
echo "libepoxy done."
