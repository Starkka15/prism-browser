cmake_minimum_required(VERSION 3.5)
project(wpe-backend-sdl VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)

# Sources
add_library(WPEBackend-sdl SHARED
    src/loader.c
    src/backend-egl.c
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
)

# Link libraries — EGL and SDL come from PDK device libs
target_link_libraries(WPEBackend-sdl PRIVATE
    ${WPE_LIBRARIES}
    EGL
    SDL
)

install(TARGETS WPEBackend-sdl
    LIBRARY DESTINATION lib
)
