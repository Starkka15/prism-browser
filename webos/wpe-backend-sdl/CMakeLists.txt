cmake_minimum_required(VERSION 3.5)
project(wpe-backend-sdl VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB   REQUIRED glib-2.0)
pkg_check_modules(CAIRO  REQUIRED cairo)

# Sources
add_library(WPEBackend-sdl SHARED
    src/loader.c
    src/backend-egl.c
    src/renderer-host.c
    src/view-backend.c
)

# Hide all symbols by default; only _wpe_loader_interface is exported
set_target_properties(WPEBackend-sdl PROPERTIES
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME "WPEBackend-sdl"
    VERSION     0.1.0
    SOVERSION   0
)

# Include paths
target_include_directories(WPEBackend-sdl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${WPE_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${SDL_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    /opt/PalmPDK/include
)

# Link libraries — EGL and SDL come from PDK device libs
target_link_libraries(WPEBackend-sdl PRIVATE
    ${WPE_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${CAIRO_LIBRARIES}
    EGL
    SDL
    m
    -L/opt/PalmPDK/device/lib -lGLESv2
    -L/opt/PalmPDK/device/lib -lpdl
)

install(TARGETS WPEBackend-sdl
    LIBRARY DESTINATION lib
)

# ── LD_PRELOAD shim: FBO redirect + pre-swap pixel readback ──────────────────
add_library(swap-hook SHARED
    src/swap-hook.c
)
# All symbols visible so the dynamic linker can interpose them
set_target_properties(swap-hook PROPERTIES
    C_VISIBILITY_PRESET default
    OUTPUT_NAME "swap-hook"
)
target_include_directories(swap-hook PRIVATE
    /opt/PalmPDK/include
)
target_link_libraries(swap-hook PRIVATE
    -ldl
    EGL
    -L/opt/PalmPDK/device/lib -lGLESv2
)
install(TARGETS swap-hook
    LIBRARY DESTINATION lib
)
