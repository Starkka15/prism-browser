# Builds zlib for ARM32 Windows (MSVC)
# Output: arm32\lib\zlib.lib, arm32\include\zlib.h

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

$srcDir = "$depsDir\src\zlib"
$bldDir = "$depsDir\build\zlib-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning zlib..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git $srcDir
} else { Write-Host "zlib source already present." }

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
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja zlibstatic
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"     -ItemType Directory -Force | Out-Null
New-Item "$outDir\include" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "zlibstatic*.lib" -Recurse | Select-Object -First 1
if (!$lib) { $lib = Get-ChildItem "$bldDir" -Filter "zlib*.lib" -Recurse | Select-Object -First 1 }
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\zlib.lib" -Force; Write-Host "  $($lib.Name) -> zlib.lib" }
else { Write-Warning "zlib.lib not found in $bldDir" }

Copy-Item "$srcDir\zlib.h"  "$outDir\include\" -Force
Copy-Item "$bldDir\zconf.h" "$outDir\include\" -Force

Write-Host ""
Write-Host "Done. zlib ARM32: $outDir\lib\zlib.lib"
