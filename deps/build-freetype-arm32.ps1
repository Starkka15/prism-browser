# Builds FreeType for ARM32 Windows (MSVC)
# Run AFTER build-zlib-arm32.ps1, build-libpng-arm32.ps1
# Output: arm32\lib\freetype.lib, arm32\include\freetype2\

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

foreach ($req in @("$outDir\lib\zlib.lib","$outDir\lib\libpng.lib")) {
    if (!(Test-Path $req)) { Write-Error "Missing $req"; exit 1 }
}

$srcDir = "$depsDir\src\freetype"
$bldDir = "$depsDir\build\freetype-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning FreeType..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch VER-2-13-2 https://gitlab.freedesktop.org/freetype/freetype.git $srcDir
} else { Write-Host "FreeType source already present." }

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
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
        -DFT_DISABLE_HARFBUZZ=TRUE `
        -DFT_DISABLE_BROTLI=TRUE `
        -DFT_DISABLE_BZIP2=TRUE `
        -DFT_REQUIRE_ZLIB=TRUE `
        -DFT_REQUIRE_PNG=TRUE `
        "-DZLIB_ROOT=$outDir" `
        "-DZLIB_INCLUDE_DIR=$outDir\include" `
        "-DZLIB_LIBRARY=$outDir\lib\zlib.lib" `
        "-DPNG_PNG_INCLUDE_DIR=$outDir\include" `
        "-DPNG_LIBRARY=$outDir\lib\libpng.lib" `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja freetype
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"                      -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\freetype2"        -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\freetype2\freetype" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "freetype*.lib" -Recurse | Select-Object -First 1
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\freetype.lib" -Force; Write-Host "  $($lib.Name) -> freetype.lib" }
else { Write-Warning "freetype.lib not found in $bldDir" }

Copy-Item "$srcDir\include\ft2build.h" "$outDir\include\freetype2\" -Force
Copy-Item "$srcDir\include\freetype\*" "$outDir\include\freetype2\freetype\" -Recurse -Force

Write-Host ""
Write-Host "Done. FreeType ARM32: $outDir\lib\freetype.lib"
