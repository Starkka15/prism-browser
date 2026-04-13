#!/usr/bin/env bash
# Build glib-networking 2.58.0 for HP TouchPad (ARMv7, webOS 3.0.5)
# Provides libgiognutls.so — the GIO TLS backend used by libsoup/WebKit for HTTPS.
# Install path: out/lib/gio/modules/  (matches GIO_MODULE_DIR in the app wrapper)
# glib-networking 2.58.0 uses Meson (not autotools).
# Requires: glib 2.58, GnuTLS — build those first.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/glib-networking-2.58.0"
TARBALL="$SCRIPT_DIR/deps/src/glib-networking-2.58.0.tar.xz"
BUILD="$SCRIPT_DIR/build/glib-networking"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

if [ ! -d "$SRC" ]; then
    echo "Extracting glib-networking..."
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

# Write Meson cross-file
mkdir -p "$SCRIPT_DIR/build"
CROSS_FILE="$SCRIPT_DIR/build/arm-webos.cross"
cat > "$CROSS_FILE" << EOF
[binaries]
c         = '${GCC10}/${CROSS}-gcc'
cpp       = '${GCC10}/${CROSS}-g++'
ar        = '${GCC10}/${CROSS}-ar'
strip     = '${GCC10}/${CROSS}-strip'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['-march=armv7-a', '-mtune=cortex-a9', '-mfpu=neon', '-mfloat-abi=softfp', '-O2', '-pipe', '--sysroot=${SYSROOT}', '-I${SYSROOT}/usr/include', '-I${PREFIX}/include']
c_link_args = ['--sysroot=${SYSROOT}', '-L${PREFIX}/lib', '-L${SYSROOT}/usr/lib', '-L${SYSROOT}/lib', '-Wl,-rpath-link,${PREFIX}/lib', '-Wl,-rpath-link,${SYSROOT}/usr/lib']

[host_machine]
system     = 'linux'
cpu_family = 'arm'
cpu        = 'cortex-a9'
endian     = 'little'
EOF

echo "Cross-file written: $CROSS_FILE"

# Clean and configure (always reconfigure — cross-file may have changed)
rm -rf "$BUILD"
mkdir -p "$BUILD"

echo "Configuring glib-networking 2.58.0 (Meson)..."
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
meson setup "$BUILD" "$SRC" \
    --cross-file "$CROSS_FILE" \
    --prefix "$PREFIX" \
    --libdir "lib" \
    --buildtype plain \
    -Dgnome_proxy_support=false \
    -Dlibproxy_support=false \
    -Dpkcs11_support=false \
    -Dinstalled_tests=false \
    -Dstatic_modules=false

echo "Building glib-networking..."
ninja -C "$BUILD" -j$(nproc)
ninja -C "$BUILD" install

# The GIO module lands in $PREFIX/lib/gio/modules/libgiognutls.so
echo ""
echo "glib-networking done."
echo "GIO TLS module: $PREFIX/lib/gio/modules/libgiognutls.so"
