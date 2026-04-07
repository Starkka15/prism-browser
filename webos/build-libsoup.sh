#!/usr/bin/env bash
# Build libsoup 2.74.3 for HP TouchPad (WPE WebKit HTTP stack, USE_SOUP2=ON)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/libsoup-2.74.3"
TARBALL="$SCRIPT_DIR/deps/src/libsoup-2.74.3.tar.xz"
BUILD="$SCRIPT_DIR/build/libsoup"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT -I$PREFIX/include -D_DEFAULT_SOURCE -D_GNU_SOURCE"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib"

if [ ! -d "$SRC" ]; then
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD" && cd "$BUILD"

# libsoup 2.74 uses meson
cat > cross.ini << EOF
[binaries]
c = '${GCC10}/${CROSS}-gcc'
cpp = '${GCC10}/${CROSS}-g++'
ar = '${GCC10}/${CROSS}-ar'
strip = '${GCC10}/${CROSS}-strip'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['$(echo $CFLAGS | sed "s/ /', '/g")']
cpp_args = ['$(echo $CFLAGS | sed "s/ /', '/g")']
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
    -Dgssapi=disabled \
    -Dkrb5_config='' \
    -Dntlm=disabled \
    -Dtests=false \
    -Dvapi=disabled \
    -Dintrospection=disabled \
    -Dgtk_doc=false \
    -Dtls_check=false \
    -Dgnome=false \
    -Dsysprof=disabled

ninja -j$(nproc) && ninja install
echo "libsoup done."
