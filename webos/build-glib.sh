#!/usr/bin/env bash
# Build GLib 2.56.4 for HP TouchPad (ARMv7, webOS 3.0.5, glibc 2.8)
# GLib 2.56 uses autotools — easier to cross-compile than meson-based versions
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/deps/src/glib-2.58.3"
TARBALL="$SCRIPT_DIR/deps/src/glib-2.58.3.tar.xz"
BUILD="$SCRIPT_DIR/build/glib"
PREFIX="$SCRIPT_DIR/out"            # install into webos/out/
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"

export PATH="$GCC10:$PATH"
export CC="${CROSS}-gcc"
export CXX="${CROSS}-g++"
export AR="${CROSS}-ar"
export RANLIB="${CROSS}-ranlib"
export STRIP="${CROSS}-strip"

CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 -pipe -Wno-error=format-overflow -Wno-error=restrict"
CFLAGS="$CFLAGS --sysroot=$SYSROOT -I$SYSROOT/usr/include -I$PREFIX/include"
LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib -L$SYSROOT/usr/lib -L$SYSROOT/lib"
LDFLAGS="$LDFLAGS -Wl,-rpath-link,$PREFIX/lib -Wl,-rpath-link,$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/lib"

# ── Extract ──────────────────────────────────────────────────────────────────
if [ ! -d "$SRC" ]; then
    echo "Extracting GLib..."
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
fi

mkdir -p "$BUILD"
cd "$BUILD"

# ── Configure ────────────────────────────────────────────────────────────────
if [ ! -f Makefile ]; then
    echo "Configuring GLib 2.56.4..."
    "$SRC/configure" \
        --host="$CROSS" \
        --build=x86_64-linux-gnu \
        --prefix="$PREFIX" \
        --sysconfdir="$PREFIX/etc" \
        --localstatedir="$PREFIX/var" \
        --enable-shared \
        --disable-static \
        \
        --with-pcre=internal \
        --disable-libelf \
        --disable-gtk-doc \
        --disable-gtk-doc-html \
        --disable-man \
        --disable-dtrace \
        --disable-systemtap \
        --disable-coverage \
        --disable-Bsymbolic \
        --disable-fam \
        --disable-xattr \
        --disable-selinux \
        --disable-libmount \
        --disable-installed-tests \
        --disable-always-build-tests \
        \
        CFLAGS="$CFLAGS" \
        LDFLAGS="$LDFLAGS" \
        glib_cv_stack_grows=no \
        glib_cv_uscore=no \
        ac_cv_func_posix_getpwuid_r=yes \
        ac_cv_func_posix_getgrgid_r=yes \
        ac_cv_path_PYTHON=/usr/bin/python3 \
        2>&1 | tee "$BUILD/configure.log"
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "Building GLib..."
make -j$(nproc) 2>&1 | tee "$BUILD/build.log"

# ── Install ───────────────────────────────────────────────────────────────────
echo "Installing GLib to $PREFIX..."
make install 2>&1 | tee "$BUILD/install.log"

echo ""
echo "Done. GLib installed to $PREFIX"
echo "Libraries: $PREFIX/lib/libglib-2.0.so*"
