cmake_minimum_required(VERSION 3.5)
project(prism-browser VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB     REQUIRED glib-2.0)
pkg_check_modules(WPEWEBKIT REQUIRED wpe-webkit-1.0)
pkg_check_modules(WPE      REQUIRED wpe-1.0)

add_executable(prism-browser main.c)

target_include_directories(prism-browser PRIVATE
    ${GLIB_INCLUDE_DIRS}
    ${WPEWEBKIT_INCLUDE_DIRS}
    ${WPE_INCLUDE_DIRS}
    /opt/PalmPDK/include
    /opt/PalmPDK/include/SDL
)

target_compile_options(prism-browser PRIVATE
    ${GLIB_CFLAGS_OTHER}
    ${WPEWEBKIT_CFLAGS_OTHER}
)

target_link_libraries(prism-browser PRIVATE
    ${GLIB_LIBRARIES}
    ${WPEWEBKIT_LIBRARIES}
    ${WPE_LIBRARIES}
    dl
    -L/opt/PalmPDK/device/lib -lpdl
)

install(TARGETS prism-browser
    RUNTIME DESTINATION bin
)
