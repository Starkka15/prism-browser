cmake_minimum_required(VERSION 3.16)

# Cross-compilation: ARMv7 Linux (HP TouchPad, webOS 3.0.5, glibc 2.8)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(TOOLCHAIN_PREFIX arm-none-linux-gnueabi)
set(TOOLCHAIN_BIN    $ENV{HOME}/webos-touchpad-modernize/toolchain/gcc10/bin)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_AR           ${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_RANLIB       ${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}-ranlib)
set(CMAKE_STRIP        ${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}-strip)

# Sysroot: glibc + webOS system libs (CRT, libc, ld-linux live here)
set(PDK /opt/PalmPDK)
set(WEBOS_SYSROOT $ENV{HOME}/webos-touchpad-modernize/sysroot)

# Use the real glibc sysroot so the linker finds crt1.o, ld-linux.so.3 etc.
set(CMAKE_SYSROOT ${WEBOS_SYSROOT})

# Also search PDK for EGL/GLES/SDL libraries, and our built prefix
set(WEBOS_PREFIX $ENV{WEBOS_PREFIX})
if(NOT WEBOS_PREFIX)
    set(WEBOS_PREFIX "$ENV{HOME}/prism-browser/webos/out")
endif()

set(CMAKE_FIND_ROOT_PATH
    ${WEBOS_PREFIX}
    ${WEBOS_SYSROOT}
    ${PDK}
    ${PDK}/device
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Cross-compile emulator: allows ARM host tools (LLIntSettingsExtractor, etc.)
# to run on x86_64 during the build.  Uses a wrapper that sets LD_LIBRARY_PATH
# and substitutes a stub libicudata (the real one has misaligned ELF segments
# built with 64 KB page size that QEMU/binfmt cannot mmap).
set(CMAKE_CROSSCOMPILING_EMULATOR "$ENV{HOME}/prism-browser/webos/qemu-arm-wrapper.sh")
# BOTH: search re-rooted paths AND the absolute HINTS from pkg-config
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# ARMv7-A Thumb-2, Cortex-A9 (Snapdragon APQ8060)
# -mthumb: emit Thumb-2 instructions; defines __thumb2__ which WebKit's cmake
#   detects to enable the ARM JIT (ARM_THUMB2_DETECTED).  Without this flag
#   the check fails and WebKit falls back to the slow CLoop interpreter.
# -mthumb-interwork: allow safe calls between Thumb-2 and ARM code in the
#   same process (system libs, PDK, EGL are all legacy ARM32).
set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mtune=cortex-a9 -mthumb -mthumb-interwork -mfpu=neon -mfloat-abi=softfp -I${PDK}/include -I${PDK}/include/SDL")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mtune=cortex-a9 -mthumb -mthumb-interwork -mfpu=neon -mfloat-abi=softfp -I${PDK}/include -I${PDK}/include/SDL")

link_directories(
    ${PDK}/device/lib
)
