# Prism Browser ARM32 Build — Current Status for VM Claude

## Goal
Build WebKit WinCairo port (ARM32 MSVC) for Windows 10 Mobile (Lumia 1520).
All vcpkg dependencies must build before WebKit itself can be configured.

## What's Working
17 of 18 vcpkg dependencies build successfully:
zlib, bzip2, liblzma, libpng, libjpeg-turbo, tiff, libwebp, libxml2, icu,
freetype, harfbuzz, pixman, cairo, woff2, nghttp2, libcurl, openssl

## Current Blocker: libpsl LNK1120
`libpsl[core,libicu]:arm-windows-webkit` fails to link `tools/psl.exe` with:

```
icuuc.lib(wintz.ao) : error LNK2019: unresolved external symbol __imp_RegCloseKey
icuuc.lib(wintz.ao) : error LNK2019: unresolved external symbol __imp_RegEnumKeyExW
icuuc.lib(wintz.ao) : error LNK2019: unresolved external symbol __imp_RegOpenKeyExW
icuuc.lib(wintz.ao) : error LNK2019: unresolved external symbol __imp_RegQueryInfoKeyW
icuuc.lib(wintz.ao) : error LNK2019: unresolved external symbol __imp_RegQueryValueExW
tools\psl.exe : fatal error LNK1120: 5 unresolved externals
```

**Root cause**: `advapi32.lib` (Windows registry API) is missing from the meson
cross-file's `c_winlibs`. cmake's ARM32 cross-compile platform init only provides
`kernel32.lib user32.lib` for `CMAKE_C_STANDARD_LIBRARIES`. The meson cross-file
ends up with `c_winlibs = ['kernel32.lib', 'user32.lib']` — missing advapi32.

## Key Files
- Build script: `Z:\prism-browser\deps\build-webkit-arm32.ps1`
- Triplet: `Z:\prism-browser\deps\vcpkg-triplets\arm-windows-webkit.cmake`
- Chainload: `Z:\prism-browser\deps\vcpkg-triplets\arm32-msvc-chainload.cmake`
- vcpkg dir: `C:\vcpkg-webkit\`
- Build logs (shared to Linux): `Z:\prism-browser\deps\build-logs\`

## What's Been Tried (and why it failed)
1. **`CMAKE_C_STANDARD_LIBRARIES CACHE FORCE` in chainload** — cmake's ARM32
   platform files run after toolchain and override it.

2. **`VCPKG_MESON_CROSS_FILE` in triplet file** — triplet variables aren't
   propagated to cmake portfile context for custom variables like this.

3. **`list(APPEND c_winlibs "advapi32.lib")` patch in
   `scripts/cmake/vcpkg_configure_meson.cmake`** — **WRONG FILE**. vcpkg loads
   cmake helper scripts from the INSTALLED port location, not scripts/cmake/.

4. **Same patch targeting installed file** — The patch code uses
   `file(READ/WRITE)` to post-process the generated meson cross-file. The inject
   DOES land in the file (confirmed in build-logs copy), but the STATUS messages
   from the inject never appear in stdout — suggesting it's STILL not the right
   file, or the configure_file() search string doesn't match the installed version.

## The Real Fix Needed
The cmake helper scripts vcpkg actually executes come from:
```
C:\vcpkg-webkit\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake
```
NOT from `scripts\cmake\vcpkg_configure_meson.cmake`.

The build script has been patching the wrong file. We need to:
1. Confirm what files are in `C:\vcpkg-webkit\installed\x64-windows\share\vcpkg-tool-meson\`
2. Find where `configure_file(... meson.template.in ...)` is called in the INSTALLED version
3. Inject the `file(READ/WRITE)` post-processing there, OR directly patch
   `meson.template.in` in the installed location

## Diagnostic Script
`Z:\prism-browser\deps\diag-meson.ps1` — run this first. It checks both
vcpkg_configure_meson.cmake locations, lists the vcpkg-tool-meson share dir,
and copies everything to build-logs for inspection. Takes ~2 seconds.

## Build Script Patch State
The build script currently patches:
- `installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake`
  (with fallback to scripts\cmake\) via `$vcmFile` variable at line ~758
- The inject block includes both the cmake-fix.ini writer AND the
  `file(READ/WRITE) string(REPLACE "user32.lib']" "user32.lib', 'advapi32.lib', 'ws2_32.lib']")`
  post-processing of the meson cross-file

The patch marker is `'# ARM32: advapi32 winlibs cross-file post-processing'`
so if the installed file was already patched it'll skip. Check with:
```powershell
Select-String "advapi32 winlibs" "C:\vcpkg-webkit\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake"
```

## After libpsl
Once libpsl builds, the next phase is WebKit itself (cmake configure + build).
The WebKit build hasn't started yet — libpsl is the last dependency.

## Project Context
- Source: `Z:\prism-browser\` (Linux: `/mnt/ssd-raid/vm-shared/prism-browser/`)
- Goal: functional WebKit browser for W10M Lumia 1520 (ARM32)
- Distribution plan: ship pre-built DLLs, not expecting users to build from source
