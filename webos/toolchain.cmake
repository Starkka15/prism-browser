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

# Sysroot: use PDK device libs + our modernized libs
set(PDK /opt/PalmPDK)
set(CMAKE_SYSROOT ${PDK})
set(CMAKE_FIND_ROOT_PATH
    ${PDK}
    ${PDK}/device
    $ENV{HOME}/webos-touchpad-modernize/sysroot
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# ARMv7-A, Cortex-A9 (Snapdragon APQ8060)
set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp")

# SDL 1.2 + PDK headers
include_directories(
    ${PDK}/include
    ${PDK}/include/SDL
)
link_directories(
    ${PDK}/device/lib
)
