#!/bin/sh
# Wrapper for running ARM cross-compiled binaries on x86 host via QEMU.
# Used by cmake's CMAKE_CROSSCOMPILING_EMULATOR during WebKit build so that
# host-side code-generator tools (LLIntSettingsExtractor, etc.) can execute.
SYSROOT="$HOME/webos-touchpad-modernize/sysroot"
ARMLIBS="$HOME/prism-browser/webos/out/lib"
GCC10LIBS="$HOME/webos-touchpad-modernize/toolchain/gcc10/arm-none-linux-gnueabi/lib"
# Stub ICU data lib must come FIRST — the real libicudata.so.67 has misaligned
# ELF LOAD segments (built with 64KB page size) that QEMU can't mmap.
STUBS="$HOME/prism-browser/webos/host-tools/stubs"

exec qemu-arm \
    -L "$SYSROOT" \
    -E "LD_LIBRARY_PATH=$STUBS:$ARMLIBS:$GCC10LIBS:$SYSROOT/lib:$SYSROOT/usr/lib" \
    "$@"
