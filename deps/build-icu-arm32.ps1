# Builds ICU 74.2 for ARM32 Windows (MSVC)
# Uses ICU's native Visual Studio solution (allinone.sln) via MSBuild.
# Cross-compilation strategy:
#   1. Build full x64 solution → generates host tools + ICU data file
#   2. Build ARM solution (stubdata + common + i18n only) → ARM DLLs + import libs
#   3. Ship icudt74l.dat from x64 build alongside ARM DLLs at runtime
#
# Output: arm32\lib\icuuc.lib, icuin.lib, icudt.lib (import libs)
#         arm32\lib\icuuc74.dll, icuin74.dll, icudt74.dll
#         arm32\include\unicode\
#         arm32\icu-data\icudt74l.dat

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

$srcDir = "$depsDir\src\icu"
$slnDir = "$srcDir\icu4c\source\allinone"
$sln    = "$slnDir\allinone.sln"
$icuSrc = "$srcDir\icu4c\source"

# ── 1. Clone ICU ─────────────────────────────────────────────────────────────

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning ICU 74.2..."
    $git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
           Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) { $git = "git" }
    & $git clone --depth 1 --branch release-74-2 https://github.com/unicode-org/icu.git $srcDir
} else { Write-Host "ICU source already present." }

# ── 2. Find MSBuild ──────────────────────────────────────────────────────────

$msbuild = "$vsPath\MSBuild\Current\Bin\MSBuild.exe"
if (!(Test-Path $msbuild)) {
    $msbuild = "$vsPath\MSBuild\Current\Bin\amd64\MSBuild.exe"
}
if (!(Test-Path $msbuild)) { Write-Error "MSBuild.exe not found under $vsPath"; exit 1 }
Write-Host "  MSBuild: $msbuild"

# MSBuild flags — x64 uses the detected toolset; ARM32 tools only ship under v143
$msBuildFlags     = @("/nologo"; "/m"; "/p:PlatformToolset=$toolset")
$msBuildFlagsArm  = @("/nologo"; "/m"; "/p:PlatformToolset=v143")

# ── 3. Build x64 (host tools + data generation) ──────────────────────────────

Write-Host ""
Write-Host "Building ICU x64 (host tools + data)..."
# Build only makedata — it pulls in all tools needed for data generation.
# This avoids the *_uwp.vcxproj projects which require a bogus SDK 10.0.0.0.
& $msbuild $sln @msBuildFlags `
    "/p:Configuration=Release" `
    "/p:Platform=x64" `
    "/t:makedata"
if ($LASTEXITCODE -ne 0) { throw "ICU x64 build failed" }

# ── 4. Build ARM (libraries only — no tools/data generation) ─────────────────
# We build: stubdata (icudt), common (icuuc), i18n (icuin)
# makedata and tool projects are excluded — they need to run on host x64 to
# generate data, which the x64 build above already did.

Write-Host ""
Write-Host "Building ICU ARM32 (stubdata, common, i18n)..."

$armTargets = "stubdata;common;i18n"

& $msbuild $sln @msBuildFlagsArm `
    "/p:Configuration=Release" `
    "/p:Platform=ARM" `
    "/t:$armTargets"
if ($LASTEXITCODE -ne 0) { throw "ICU ARM32 build failed" }

# ── 5. Install ────────────────────────────────────────────────────────────────

New-Item "$outDir\lib"          -ItemType Directory -Force | Out-Null
New-Item "$outDir\include"      -ItemType Directory -Force | Out-Null
New-Item "$outDir\icu-data"     -ItemType Directory -Force | Out-Null

# ARM import libraries land in icu4c\libARM\; DLLs in icu4c\binARM\
$icuRoot  = "$srcDir\icu4c"
$armLibDir = "$icuRoot\libARM"
$armBinDir = "$icuRoot\binARM"

foreach ($name in @("icuuc","icuin","icudt")) {
    $lib = Get-ChildItem $armLibDir -Filter "${name}*.lib" -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($lib) {
        Copy-Item $lib.FullName "$outDir\lib\${name}.lib" -Force
        Write-Host "  $($lib.Name) -> ${name}.lib"
    } else { Write-Warning "${name}.lib not found in $armLibDir" }

    $dll = Get-ChildItem $armBinDir -Filter "${name}*.dll" -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($dll) {
        Copy-Item $dll.FullName "$outDir\lib\$($dll.Name)" -Force
        Write-Host "  $($dll.Name) -> lib\"
    } else { Write-Warning "${name}.dll not found in $armBinDir" }
}

# ICU data file from x64 build (deployed alongside ARM DLLs at runtime)
$datFile = Get-ChildItem "$icuSrc\data" -Filter "icudt*.dat" -Recurse -ErrorAction SilentlyContinue |
           Select-Object -First 1
if ($datFile) {
    Copy-Item $datFile.FullName "$outDir\icu-data\$($datFile.Name)" -Force
    Write-Host "  $($datFile.Name) -> icu-data\"
} else { Write-Warning "icudt*.dat not found - data file missing" }

# Headers from common and i18n source
$hdirCommon = "$icuSrc\common\unicode"
$hdirI18n   = "$icuSrc\i18n\unicode"
New-Item "$outDir\include\unicode" -ItemType Directory -Force | Out-Null
if (Test-Path $hdirCommon) { Copy-Item "$hdirCommon\*" "$outDir\include\unicode" -Force }
if (Test-Path $hdirI18n)   { Copy-Item "$hdirI18n\*"   "$outDir\include\unicode" -Force }

Write-Host ""
Write-Host "Done. ICU ARM32: $outDir\lib\icuuc.lib + icudt.lib + icuin.lib"
Write-Host "  Data file:     $outDir\icu-data"
