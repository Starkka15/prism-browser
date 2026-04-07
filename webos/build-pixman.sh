#!/usr/bin/env bash
# Build pixman 0.42.2 for HP TouchPad (Cairo dependency)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/pixman-0.42.2"
TARBALL="$SCRIPT_DIR/deps/src/pixman-0.42.2.tar.gz"
BUILD="$SCRIPT_DIR/build/pixman"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT"
LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib"

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

PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig" \
meson setup "$BUILD" "$SRC" \
    --cross-file cross.ini \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Dtests=disabled \
    -Dgnuplot=false \
    -Dgtk=disabled

ninja -j$(nproc) && ninja install
echo "pixman done."
