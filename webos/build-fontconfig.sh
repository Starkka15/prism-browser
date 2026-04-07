#!/usr/bin/env bash
# Build fontconfig 2.14.2 for HP TouchPad (ARMv7)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/fontconfig-2.14.2"
TARBALL="$SCRIPT_DIR/deps/src/fontconfig-2.14.2.tar.xz"
BUILD="$SCRIPT_DIR/build/fontconfig"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT -I$PREFIX/include"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib"

if [ ! -d "$SRC" ]; then
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD"

cat > "$BUILD/cross.ini" << EOF
[binaries]
c = '${GCC10}/${CROSS}-gcc'
cpp = '${GCC10}/${CROSS}-g++'
ar = '${GCC10}/${CROSS}-ar'
strip = '${GCC10}/${CROSS}-strip'
pkg-config = 'pkg-config'

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
    --cross-file "$BUILD/cross.ini" \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Ddoc=disabled \
    -Dtests=disabled \
    -Dtools=disabled \
    -Dcache-build=disabled \
    -Dnls=disabled

ninja -C "$BUILD" -j$(nproc) && ninja -C "$BUILD" install
echo "fontconfig done."
