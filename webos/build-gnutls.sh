#!/usr/bin/env bash
# Build GnuTLS 3.8.4 for HP TouchPad (ARMv7, webOS 3.0.5)
# Requires: GMP, Nettle (+ hogweed), libtasn1 — build those first.
# p11-kit, libunistring, libidn2 are disabled to keep the dep chain short.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/gnutls-3.8.4"
TARBALL="$SCRIPT_DIR/deps/src/gnutls-3.8.4.tar.xz"
BUILD="$SCRIPT_DIR/build/gnutls"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export CC="${CROSS}-gcc"
export CXX="${CROSS}-g++"
export AR="${CROSS}-ar"
export RANLIB="${CROSS}-ranlib"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 -pipe"
CFLAGS="$CFLAGS --sysroot=$SYSROOT -I$SYSROOT/usr/include -I$PREFIX/include"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib -L$SYSROOT/lib"
LDFLAGS="$LDFLAGS -Wl,-rpath-link,$PREFIX/lib -Wl,-rpath-link,$SYSROOT/usr/lib"

# pkg-config pointing at our out/ prefix for GMP/Nettle/libtasn1
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG="pkg-config --define-variable=prefix=$PREFIX"

if [ ! -d "$SRC" ]; then
    echo "Extracting GnuTLS..."
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f Makefile ]; then
    echo "Configuring GnuTLS 3.8.4..."
    "$SRC/configure" \
        --host="${CROSS}" \
        --build=x86_64-linux-gnu \
        --prefix="$PREFIX" \
        --enable-shared \
        --disable-static \
        \
        --with-included-libtasn1=no \
        --with-included-unistring \
        --without-p11-kit \
        --without-idn \
        --without-zlib \
        --without-brotli \
        --without-zstd \
        --disable-doc \
        --disable-manpages \
        --disable-tools \
        --disable-tests \
        --disable-nls \
        --disable-rpath \
        --disable-openssl-compatibility \
        --disable-hardware-acceleration \
        --with-default-trust-store-file=/tmp/prism-ca.crt \
        \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$LDFLAGS" \
        GMP_CFLAGS="-I$PREFIX/include" \
        GMP_LIBS="-L$PREFIX/lib -lgmp" \
        NETTLE_CFLAGS="-I$PREFIX/include" \
        NETTLE_LIBS="-L$PREFIX/lib -lnettle" \
        HOGWEED_CFLAGS="-I$PREFIX/include" \
        HOGWEED_LIBS="-L$PREFIX/lib -lhogweed" \
        LIBTASN1_CFLAGS="-I$PREFIX/include" \
        LIBTASN1_LIBS="-L$PREFIX/lib -ltasn1"
fi

echo "Building GnuTLS..."
make -j$(nproc)
make install

echo "GnuTLS done."
