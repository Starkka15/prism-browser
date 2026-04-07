# Builds libpng for ARM32 Windows (MSVC)
# Run AFTER build-zlib-arm32.ps1
# Output: arm32\lib\libpng.lib, arm32\include\png.h

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

foreach ($req in @("$outDir\lib\zlib.lib")) {
    if (!(Test-Path $req)) { Write-Error "Missing $req - run build-zlib-arm32.ps1 first."; exit 1 }
}

$srcDir = "$depsDir\src\libpng"
$bldDir = "$depsDir\build\libpng-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning libpng..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch v1.6.43 https://github.com/pnggroup/libpng.git $srcDir
} else { Write-Host "libpng source already present." }

Patch-CmakeForArm32 $srcDir

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
        -DPNG_SHARED=OFF `
        -DPNG_STATIC=ON `
        -DPNG_TESTS=OFF `
        -DPNG_ARM_NEON=off `
        "-DZLIB_ROOT=$outDir" `
        "-DZLIB_INCLUDE_DIR=$outDir\include" `
        "-DZLIB_LIBRARY=$outDir\lib\zlib.lib" `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja png_static
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"     -ItemType Directory -Force | Out-Null
New-Item "$outDir\include" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "libpng*.lib" -Recurse | Select-Object -First 1
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\libpng.lib" -Force; Write-Host "  $($lib.Name) -> libpng.lib" }
else { Write-Warning "libpng.lib not found in $bldDir" }

Copy-Item "$srcDir\png.h"     "$outDir\include\" -Force
Copy-Item "$srcDir\pngconf.h" "$outDir\include\" -Force
Copy-Item "$bldDir\pnglibconf.h" "$outDir\include\" -Force

Write-Host ""
Write-Host "Done. libpng ARM32: $outDir\lib\libpng.lib"
