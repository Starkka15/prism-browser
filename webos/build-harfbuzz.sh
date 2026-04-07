#!/usr/bin/env bash
# Build HarfBuzz 5.3.1 for HP TouchPad (ARMv7)
# Enabled: freetype backend, ICU unicode functions
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/harfbuzz-5.3.1"
TARBALL="$SCRIPT_DIR/deps/src/harfbuzz-5.3.1.tar.xz"
BUILD="$SCRIPT_DIR/build/harfbuzz"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT -I$PREFIX/include"
CXXFLAGS="$CFLAGS"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib"

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
cpp_args = ['$(echo $CXXFLAGS | sed "s/ /', '/g")']
c_link_args = ['$(echo $LDFLAGS | sed "s/ /', '/g")']
cpp_link_args = ['$(echo $LDFLAGS | sed "s/ /', '/g")']

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
    -Dfreetype=enabled \
    -Dicu=enabled \
    -Dglib=disabled \
    -Dgobject=disabled \
    -Dcairo=disabled \
    -Ddocs=disabled \
    -Dtests=disabled \
    -Dbenchmark=disabled

ninja -j$(nproc) && ninja install
echo "harfbuzz done."
