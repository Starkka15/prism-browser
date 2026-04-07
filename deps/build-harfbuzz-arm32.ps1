# Builds HarfBuzz for ARM32 Windows (MSVC)
# Run AFTER build-freetype-arm32.ps1
# Output: arm32\lib\harfbuzz.lib, arm32\include\harfbuzz\

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

foreach ($req in @("$outDir\lib\freetype.lib")) {
    if (!(Test-Path $req)) { Write-Error "Missing $req - run build-freetype-arm32.ps1 first."; exit 1 }
}

$srcDir = "$depsDir\src\harfbuzz"
$bldDir = "$depsDir\build\harfbuzz-arm32"

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning HarfBuzz..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch 8.5.0 https://github.com/harfbuzz/harfbuzz.git $srcDir
} else { Write-Host "HarfBuzz source already present." }

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
        -DHB_BUILD_TESTS=OFF `
        -DHB_BUILD_UTILS=OFF `
        -DHB_BUILD_SUBSET=OFF `
        -DHB_HAVE_GLIB=OFF `
        -DHB_HAVE_ICU=OFF `
        -DHB_HAVE_FREETYPE=ON `
        "-DFREETYPE_INCLUDE_DIRS=$outDir\include\freetype2" `
        "-DFREETYPE_LIBRARY=$outDir\lib\freetype.lib" `
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja harfbuzz
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"              -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\harfbuzz" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "harfbuzz*.lib" -Recurse |
       Where-Object { $_.Name -notmatch "icu|subset|gobject" } |
       Select-Object -First 1
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\harfbuzz.lib" -Force; Write-Host "  $($lib.Name) -> harfbuzz.lib" }
else { Write-Warning "harfbuzz.lib not found in $bldDir" }

Copy-Item "$srcDir\src\*.h" "$outDir\include\harfbuzz\" -Force

Write-Host ""
Write-Host "Done. HarfBuzz ARM32: $outDir\lib\harfbuzz.lib"
