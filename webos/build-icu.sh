#!/usr/bin/env bash
# Build ICU 67.1 for HP TouchPad (ARMv7)
# Two-stage: build host tools first, then cross-compile ARM target.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARBALL="$SCRIPT_DIR/deps/src/icu4c-67_1-src.tgz"
SRC="$SCRIPT_DIR/deps/src/icu"
BUILD_HOST="$SCRIPT_DIR/build/icu-host"
BUILD_ARM="$SCRIPT_DIR/build/icu-arm"
PREFIX="$SCRIPT_DIR/out"
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
CROSS="arm-none-linux-gnueabi"
NCPU=$(nproc)

# ── Extract ──────────────────────────────────────────────────────────────────
if [ ! -d "$SRC/source" ]; then
    echo "Extracting ICU 67.1..."
    mkdir -p "$(dirname "$SRC")"
    tar -xf "$TARBALL" -C "$(dirname "$SRC")"
    # tarball extracts to 'icu/source'
fi

# ── Stage 1: build for host (x86_64) ─────────────────────────────────────────
echo "=== Stage 1: ICU host build (x86_64) ==="
mkdir -p "$BUILD_HOST" && cd "$BUILD_HOST"

if [ ! -f Makefile ]; then
    "$SRC/source/configure" \
        --prefix="$BUILD_HOST/install" \
        --enable-shared \
        --disable-static \
        --disable-tests \
        --disable-samples
fi

make -j$NCPU
make install

# ── Stage 2: cross-compile for ARM ───────────────────────────────────────────
echo "=== Stage 2: ICU ARM cross-compile ==="
export PATH="$GCC10:$PATH"

mkdir -p "$BUILD_ARM" && cd "$BUILD_ARM"

if [ ! -f Makefile ]; then
    CC="${CROSS}-gcc" \
    CXX="${CROSS}-g++" \
    AR="${CROSS}-ar" \
    RANLIB="${CROSS}-ranlib" \
    CFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT" \
    CXXFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -O2 --sysroot=$SYSROOT" \
    LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib" \
    "$SRC/source/configure" \
        --host="$CROSS" \
        --build=x86_64-linux-gnu \
        --prefix="$PREFIX" \
        --with-cross-build="$BUILD_HOST" \
        --enable-shared \
        --disable-static \
        --disable-tests \
        --disable-samples \
        --with-data-packaging=library
fi

make -j$NCPU
make install

echo ""
echo "ICU 67.1 installed to $PREFIX"
echo "Libraries: $PREFIX/lib/libicuuc.so*"
