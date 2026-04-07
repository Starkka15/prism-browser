# Builds Cairo 1.18 for ARM32 Windows (MSVC)
# Cairo 1.18 is meson-only; we generate a custom CMakeLists.txt.
# Run AFTER build-zlib, build-libpng, build-freetype, build-pixman
# Output: arm32\lib\cairo.lib, arm32\include\cairo\

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

foreach ($req in @("$outDir\lib\zlib.lib","$outDir\lib\libpng.lib","$outDir\lib\freetype.lib","$outDir\lib\pixman-1.lib")) {
    if (!(Test-Path $req)) { Write-Error "Missing $req"; exit 1 }
}

$srcDir = "$depsDir\src\cairo"
$bldDir = "$depsDir\build\cairo-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning Cairo 1.18.2..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch 1.18.2 https://gitlab.freedesktop.org/cairo/cairo.git $srcDir
} else { Write-Host "Cairo source already present." }

# Generate cairo-features.h — normally produced by meson
$featuresH = @'
/* cairo-features.h — generated for ARM32 MSVC static build */
#ifndef CAIRO_FEATURES_H
#define CAIRO_FEATURES_H

#define CAIRO_HAS_IMAGE_SURFACE      1
#define CAIRO_HAS_USER_FONT          1
#define CAIRO_HAS_MIME_SURFACE       1
#define CAIRO_HAS_OBSERVER_SURFACE   1
#define CAIRO_HAS_RECORDING_SURFACE  1
#define CAIRO_HAS_TEE_SURFACE        1
#define CAIRO_HAS_FT_FONT            1
#define CAIRO_HAS_PNG_FUNCTIONS      1
#define CAIRO_HAS_WIN32_SURFACE      1
#define CAIRO_HAS_WIN32_FONT         1
#define CAIRO_HAS_SVG_SURFACE        1
#define CAIRO_HAS_PS_SURFACE         1
#define CAIRO_HAS_PDF_SURFACE        1
#define CAIRO_HAS_SCRIPT_SURFACE     1
#define CAIRO_HAS_FONT_SUBSET        1
#define CAIRO_HAS_PDF_OPERATORS      1
#define CAIRO_HAS_DEFLATE_STREAM     1

/* Not enabled */
#define CAIRO_HAS_PTHREAD            0
#define CAIRO_HAS_FC_FONT            0
#define CAIRO_HAS_DWRITE_FONT        0
#define CAIRO_HAS_HIDDEN_SYMBOLS     0
#define CAIRO_HAS_QUARTZ_SURFACE     0
#define CAIRO_HAS_QUARTZ_FONT        0
#define CAIRO_HAS_QUARTZ_IMAGE_SURFACE 0
#define CAIRO_HAS_XCB_SURFACE        0
#define CAIRO_HAS_XCB_SHM_FUNCTIONS  0
#define CAIRO_HAS_XLIB_SURFACE       0
#define CAIRO_HAS_XLIB_XCB_FUNCTIONS 0
#define CAIRO_HAS_XLIB_XRENDER_SURFACE 0
#define CAIRO_HAS_TEST_PAGINATED_SURFACE 0

#endif /* CAIRO_FEATURES_H */
'@

# Write cairo-features.h into src/ so relative includes inside src/ find it
[System.IO.File]::WriteAllText("$srcDir\src\cairo-features.h", $featuresH)
Write-Host "  Wrote cairo-features.h"

# Generate config.h — normally produced by meson's compiler feature detection
$configH = @'
/* config.h — generated for ARM32 MSVC static build */
#ifndef CAIRO_CONFIG_H
#define CAIRO_CONFIG_H

/* Basic integer types */
#define HAVE_STDINT_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_INTSAFE_H   1
#define HAVE_UINT64_T    1

/* Pointer size: ARM32 = 4 bytes */
#define SIZEOF_VOID_P    4

/* Little-endian ARM32 — do NOT define WORDS_BIGENDIAN */

/* No POSIX string extras in MSVC */
/* #undef HAVE_STRNDUP */

/* No POSIX time functions */
/* #undef HAVE_CLOCK_GETTIME */
/* #undef HAVE_CTIME_R */
/* #undef HAVE_GMTIME_R */
/* #undef HAVE_LOCALTIME_R */

/* No POSIX/GLibC-specific headers */
/* #undef HAVE_BYTESWAP_H */
/* #undef HAVE_ALLOCA_H */
/* #undef HAVE_UNISTD_H */
/* #undef HAVE_FCNTL_H */
/* #undef HAVE_XLOCALE_H */
/* #undef HAVE_SYS_INT_TYPES_H */
/* #undef HAVE_NEWLOCALE */
/* #undef HAVE_STRTOD_L */

/* No 128-bit integers in MSVC */
/* #undef HAVE_UINT128_T */
/* #undef HAVE___UINT128_T */

/* Atomic ops: handled by Win32 Interlocked path in cairo-atomic-private.h */
/* #undef HAVE_C11_ATOMIC_PRIMITIVES */
/* #undef HAVE_CXX11_ATOMIC_PRIMITIVES */
/* #undef HAVE_GCC_LEGACY_ATOMICS */
/* #undef HAVE_LIB_ATOMIC_OPS */
/* #undef HAVE_OS_ATOMIC_OPS */

/* No pthreads on Windows MSVC */
/* #undef HAVE_PTHREAD */

/* No debug/profiling tools */
/* #undef HAVE_VALGRIND */
/* #undef HAVE_LOCKDEP */
/* #undef HAVE_MEMFAULT */

/* FreeType 2.13 features */
#define HAVE_FT_COLR_V1      1
#define HAVE_FT_LOAD_NO_SVG  1
#define HAVE_FT_SVG_DOCUMENT 1

/* X11/XCB/XRender — not used on Windows */
/* #undef HAVE_X11_EXTENSIONS_SHMPROTO_H */
/* #undef HAVE_X11_EXTENSIONS_SHMSTR_H */
/* #undef HAVE_X11_EXTENSIONS_XSHM_H */
/* #undef HAVE_XRENDERCREATECONICALGRADIENT */
/* #undef HAVE_XRENDERCREATELINEARGRADIENT */
/* #undef HAVE_XRENDERCREATERADIALGRADIENT */
/* #undef HAVE_XRENDERCREATESOLIDFILL */

#endif /* CAIRO_CONFIG_H */
'@

[System.IO.File]::WriteAllText("$srcDir\src\config.h", $configH)
Write-Host "  Wrote config.h"

# Write CMakeLists.txt for Cairo (no official CMake support in 1.18)
# Use single-quoted here-string so ${CMAKE_...} tokens are NOT expanded by PowerShell
$cmakeContent = @'
cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 OLD)
project(cairo C)

# ---- Source collection ----
# Core sources: all .c in src/, excluding platform-specific and test files
file(GLOB CAIRO_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(FILTER CAIRO_CORE_SOURCES EXCLUDE REGEX ".*/test-.*\\.c$")
list(FILTER CAIRO_CORE_SOURCES EXCLUDE REGEX ".*/cairo-xcb.*\\.c$")
list(FILTER CAIRO_CORE_SOURCES EXCLUDE REGEX ".*/cairo-xlib.*\\.c$")
list(FILTER CAIRO_CORE_SOURCES EXCLUDE REGEX ".*/cairo-quartz.*\\.c$")
# cairo-image-mask-compositor.c is #include'd by cairo-image-compositor.c, not compiled directly
list(FILTER CAIRO_CORE_SOURCES EXCLUDE REGEX ".*/cairo-image-mask-compositor\\.c$")

# Win32 backend: all .c in src/win32/ (skip .cpp DWrite file)
file(GLOB CAIRO_WIN32_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/*.c")

set(CAIRO_SOURCES ${CAIRO_CORE_SOURCES} ${CAIRO_WIN32_SOURCES})

add_library(cairo STATIC ${CAIRO_SOURCES})

target_compile_options(cairo PRIVATE
    /std:c11
    /wd4005
    /wd4996
    /wd4244
    /wd4267
    /wd4305
    /wd4146
    /wd4018
    /wd4090
    /wd4133
    /wd4028
)

target_compile_definitions(cairo PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    PACKAGE="cairo"
    PACKAGE_VERSION="1.18.2"
    CAIRO_WIN32_STATIC_BUILD
    PIXMAN_API=
    PIXMAN_EXPORT=
)

target_include_directories(cairo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win32
        ${PIXMAN_INCLUDE_DIR}
        ${FT_INCLUDE_DIR}
        ${FT_INCLUDE_DIR}/freetype
        ${ZLIB_INCLUDE_DIR}
        ${PNG_INCLUDE_DIR}
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(cairo PRIVATE
    ${PIXMAN_LIB}
    ${FT_LIB}
    ${PNG_LIB}
    ${ZLIB_LIB}
    gdi32 msimg32 user32
)

install(TARGETS cairo ARCHIVE DESTINATION lib)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
        DESTINATION include/cairo
        FILES_MATCHING PATTERN "cairo*.h"
        PATTERN "cairo-*-private.h" EXCLUDE
        PATTERN "cairo-*-inline.h"  EXCLUDE
        PATTERN "cairoint.h"        EXCLUDE)
'@

[System.IO.File]::WriteAllText("$srcDir\CMakeLists.txt", $cmakeContent)
Write-Host "  Wrote CMakeLists.txt"

if (Test-Path $bldDir) { Remove-Item $bldDir -Recurse -Force }
New-Item $bldDir -ItemType Directory -Force | Out-Null
Push-Location $bldDir
try {
    & $cmake -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_SYSTEM_NAME=Windows `
        -DCMAKE_SYSTEM_PROCESSOR=ARM `
        -DCMAKE_C_COMPILER="$clExe" `
        -DCMAKE_CXX_COMPILER="$clExe" `
        -DBUILD_SHARED_LIBS=OFF `
        "-DPIXMAN_INCLUDE_DIR=$outDir\include\pixman-1" `
        "-DPIXMAN_LIB=$outDir\lib\pixman-1.lib" `
        "-DFT_INCLUDE_DIR=$outDir\include\freetype2" `
        "-DFT_LIB=$outDir\lib\freetype.lib" `
        "-DZLIB_INCLUDE_DIR=$outDir\include" `
        "-DZLIB_LIB=$outDir\lib\zlib.lib" `
        "-DPNG_INCLUDE_DIR=$outDir\include" `
        "-DPNG_LIB=$outDir\lib\libpng.lib" `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja cairo
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"           -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\cairo" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "cairo*.lib" -Recurse | Select-Object -First 1
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\cairo.lib" -Force; Write-Host "  $($lib.Name) -> cairo.lib" }
else { Write-Warning "cairo.lib not found in $bldDir" }

# Copy public headers from src/
Get-ChildItem "$srcDir\src\cairo*.h" | Where-Object {
    $_.Name -notmatch '-private\.h$' -and $_.Name -notmatch '-inline\.h$' -and $_.Name -ne 'cairoint.h'
} | ForEach-Object { Copy-Item $_.FullName "$outDir\include\cairo\" -Force }

Write-Host ""
Write-Host "Done. Cairo ARM32: $outDir\lib\cairo.lib"
