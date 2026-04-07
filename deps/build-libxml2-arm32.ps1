# Builds libxml2 for ARM32 Windows (MSVC)
# Run AFTER build-zlib-arm32.ps1, build-icu-arm32.ps1
# Output: arm32\lib\libxml2.lib, arm32\include\libxml\

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

foreach ($req in @("$outDir\lib\zlib.lib","$outDir\lib\icuuc.lib")) {
    if (!(Test-Path $req)) { Write-Error "Missing $req"; exit 1 }
}

$srcDir = "$depsDir\src\libxml2"
$bldDir = "$depsDir\build\libxml2-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning libxml2..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch v2.12.6 https://gitlab.gnome.org/GNOME/libxml2.git $srcDir
} else { Write-Host "libxml2 source already present." }

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
        -DLIBXML2_WITH_TESTS=OFF `
        -DLIBXML2_WITH_PROGRAMS=OFF `
        -DLIBXML2_WITH_PYTHON=OFF `
        -DLIBXML2_WITH_LZMA=OFF `
        -DLIBXML2_WITH_ICONV=OFF `
        -DLIBXML2_WITH_ZLIB=ON `
        -DLIBXML2_WITH_ICU=ON `
        "-DZLIB_ROOT=$outDir" `
        "-DZLIB_INCLUDE_DIR=$outDir\include" `
        "-DZLIB_LIBRARY=$outDir\lib\zlib.lib" `
        "-DICU_ROOT=$outDir" `
        "-DICU_INCLUDE_DIR=$outDir\include" `
        "-DICU_LIBRARY=$outDir\lib\icuuc.lib" `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja LibXml2
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"         -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\libxml" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "libxml2*.lib" -Recurse | Select-Object -First 1
if (!$lib) { $lib = Get-ChildItem "$bldDir" -Filter "xml2*.lib" -Recurse | Select-Object -First 1 }
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\libxml2.lib" -Force; Write-Host "  $($lib.Name) -> libxml2.lib" }
else { Write-Warning "libxml2.lib not found in $bldDir" }

Copy-Item "$srcDir\include\libxml\*" "$outDir\include\libxml\" -Force
if (Test-Path "$bldDir\include\libxml") {
    Copy-Item "$bldDir\include\libxml\*" "$outDir\include\libxml\" -Force
}

Write-Host ""
Write-Host "Done. libxml2 ARM32: $outDir\lib\libxml2.lib"
