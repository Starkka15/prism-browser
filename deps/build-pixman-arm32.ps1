# Builds pixman for ARM32 Windows (MSVC)
# pixman has no official CMake; we compile its C sources directly.
# Output: arm32\lib\pixman-1.lib, arm32\include\pixman-1\pixman.h

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

$srcDir = "$depsDir\src\pixman"
$bldDir = "$depsDir\build\pixman-arm32"

if (!(Test-Path "$srcDir\pixman\pixman.c")) {
    Write-Host "Downloading pixman 0.43.4..."
    $url = "https://cairographics.org/releases/pixman-0.43.4.tar.gz"
    $tgz = "$depsDir\pixman-0.43.4.tar.gz"
    Invoke-WebRequest $url -OutFile $tgz
    New-Item $srcDir -ItemType Directory -Force | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xzf $tgz -C $srcDir --strip-components=1
    Remove-Item $tgz -Force -ErrorAction SilentlyContinue
} else { Write-Host "pixman source already present." }

# Write a minimal CMakeLists.txt for pixman (no official CMake support)
$cmakeContent = @'
cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 OLD)
project(pixman C)

# Glob all C sources from pixman/ subdir (avoids hardcoding version-specific file lists)
file(GLOB PIXMAN_SOURCES pixman/*.c)
# Exclude SIMD/MMX/SSE/VMX/Neon files - generic fallback only for ARM32 MSVC
list(FILTER PIXMAN_SOURCES EXCLUDE REGEX ".*-(mmx|sse2|ssse3|vmx|neon|mips|loongson).*\\.c$")
# pixman-region.c is a template included by pixman-region16.c and pixman-region32.c
list(FILTER PIXMAN_SOURCES EXCLUDE REGEX ".*/pixman-region\\.c$")

add_library(pixman-1 STATIC ${PIXMAN_SOURCES})

target_compile_options(pixman-1 PRIVATE /std:c11)

target_compile_definitions(pixman-1 PRIVATE
    PACKAGE="pixman-1"
    PACKAGE_VERSION="0.43.4"
    PIXMAN_NO_TLS
    PIXMAN_API=
    PIXMAN_EXPORT=
    MAYBE_UNUSED=
    "FUNC=__FUNCTION__"
    _USE_MATH_DEFINES
)

# Generate pixman-version.h
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pixman/pixman-version.h"
"#define PIXMAN_VERSION_MAJOR 0\n"
"#define PIXMAN_VERSION_MINOR 43\n"
"#define PIXMAN_VERSION_MICRO 4\n"
"#define PIXMAN_VERSION_STRING \"0.43.4\"\n"
"#define PIXMAN_VERSION_ENCODE(mj,mn,mc) ((mj)*1000000+(mn)*1000+(mc))\n"
"#define PIXMAN_VERSION PIXMAN_VERSION_ENCODE(PIXMAN_VERSION_MAJOR,PIXMAN_VERSION_MINOR,PIXMAN_VERSION_MICRO)\n"
)

target_include_directories(pixman-1
    PRIVATE  ${CMAKE_CURRENT_SOURCE_DIR}/pixman
             ${CMAKE_CURRENT_BINARY_DIR}/pixman
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
              $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

install(TARGETS pixman-1 ARCHIVE DESTINATION lib)
install(FILES pixman/pixman.h "${CMAKE_CURRENT_BINARY_DIR}/pixman/pixman-version.h"
        DESTINATION include/pixman-1)
'@

[System.IO.File]::WriteAllText("$srcDir\CMakeLists.txt", $cmakeContent)

# Write pixman-compiler.h directly into the source tree so pixman.h's relative
# include "#include "pixman-compiler.h"" resolves without any include-path tricks.
$compilerH = @'
#ifndef PIXMAN_COMPILER_H
#define PIXMAN_COMPILER_H
/* Generated for MSVC static build - replaces meson-generated file */

#define PIXMAN_API
#define PIXMAN_EXPORT

#ifdef _MSC_VER
# define PIXMAN_INLINE    __inline
# define force_inline     __forceinline
# define noinline         __declspec(noinline)
# define unlikely(x)      (x)
# define likely(x)        (x)
# define PIXMAN_RESTRICT  __restrict
# define MAYBE_UNUSED
# define FUNC             __FUNCTION__
# define CONTAINER_OF(type, member, ptr) \
    ((type *)((uint8_t *)(ptr) - offsetof(type, member)))
#else
# define PIXMAN_INLINE    __inline__
# define force_inline     __inline__ __attribute__((always_inline))
# define noinline         __attribute__((noinline))
# define unlikely(x)      __builtin_expect(!!(x), 0)
# define likely(x)        __builtin_expect(!!(x), 1)
# define PIXMAN_RESTRICT  __restrict__
# define MAYBE_UNUSED     __attribute__((unused))
# define FUNC             __func__
# define CONTAINER_OF(type, member, ptr) \
    ((type *)((uint8_t *)(ptr) - offsetof(type, member)))
#endif

#ifdef PIXMAN_NO_TLS
# define PIXMAN_DEFINE_THREAD_LOCAL(type, name)  static type name;
# define PIXMAN_GET_THREAD_LOCAL(name)            (&(name))
#elif defined(_MSC_VER)
# define PIXMAN_DEFINE_THREAD_LOCAL(type, name)  static __declspec(thread) type name;
# define PIXMAN_GET_THREAD_LOCAL(name)            (&(name))
#else
# define PIXMAN_DEFINE_THREAD_LOCAL(type, name)  static __thread type name;
# define PIXMAN_GET_THREAD_LOCAL(name)            (&(name))
#endif

#endif /* PIXMAN_COMPILER_H */
'@
[System.IO.File]::WriteAllText("$srcDir\pixman\pixman-compiler.h", $compilerH)
if (!(Test-Path "$srcDir\pixman\pixman-compiler.h")) {
    Write-Error "Failed to write pixman-compiler.h to $srcDir\pixman\"; exit 1
}
Write-Host "  Wrote pixman-compiler.h to $srcDir\pixman\"

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
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja pixman-1
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"                  -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\pixman-1"     -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "pixman*.lib" -Recurse | Select-Object -First 1
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\pixman-1.lib" -Force; Write-Host "  $($lib.Name) -> pixman-1.lib" }
else { Write-Warning "pixman-1.lib not found in $bldDir" }

Copy-Item "$srcDir\pixman\pixman.h"             "$outDir\include\pixman-1\" -Force
Copy-Item "$srcDir\pixman\pixman-compiler.h"    "$outDir\include\pixman-1\" -Force
Copy-Item "$bldDir\pixman\pixman-version.h"     "$outDir\include\pixman-1\" -Force

# Patch installed pixman.h to self-define PIXMAN_API/PIXMAN_EXPORT for static-build consumers
$ph = "$outDir\include\pixman-1\pixman.h"
$raw = [System.IO.File]::ReadAllText($ph)
$guard = '#ifndef PIXMAN_API' + "`n" + '#  define PIXMAN_API' + "`n" + '#endif' + "`n" + '#ifndef PIXMAN_EXPORT' + "`n" + '#  define PIXMAN_EXPORT' + "`n" + '#endif'
if ($raw -notmatch 'ifndef PIXMAN_API') {
    $raw = $raw -replace '(#include\s+<pixman-version\.h>)', "`$1`n`n$guard"
    [System.IO.File]::WriteAllText($ph, $raw)
    Write-Host "  Patched pixman.h with PIXMAN_API fallback defines"
}

Write-Host ""
Write-Host "Done. pixman ARM32: $outDir\lib\pixman-1.lib"
