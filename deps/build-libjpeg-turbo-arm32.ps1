# Builds libjpeg-turbo for ARM32 Windows (MSVC), SIMD disabled
# Output: arm32\lib\jpeg.lib, arm32\include\jpeglib.h

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

$srcDir = "$depsDir\src\libjpeg-turbo"
$bldDir = "$depsDir\build\libjpeg-turbo-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning libjpeg-turbo..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch 3.0.3 https://github.com/libjpeg-turbo/libjpeg-turbo.git $srcDir
} else { Write-Host "libjpeg-turbo source already present." }

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
        -DENABLE_SHARED=OFF `
        -DENABLE_STATIC=ON `
        -DWITH_SIMD=OFF `
        -DWITH_TURBOJPEG=OFF `
        -DWITH_JAVA=OFF `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja jpeg-static
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"     -ItemType Directory -Force | Out-Null
New-Item "$outDir\include" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "jpeg-static*.lib" -Recurse | Select-Object -First 1
if (!$lib) { $lib = Get-ChildItem "$bldDir" -Filter "jpeg*.lib" -Recurse | Select-Object -First 1 }
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\jpeg.lib" -Force; Write-Host "  $($lib.Name) -> jpeg.lib" }
else { Write-Warning "jpeg.lib not found in $bldDir" }

foreach ($h in @("jconfig.h","jerror.h","jmorecfg.h","jpeglib.h")) {
    $src = if (Test-Path "$bldDir\$h") { "$bldDir\$h" } else { "$srcDir\$h" }
    if (Test-Path $src) { Copy-Item $src "$outDir\include\" -Force }
}

Write-Host ""
Write-Host "Done. libjpeg-turbo ARM32: $outDir\lib\jpeg.lib"
