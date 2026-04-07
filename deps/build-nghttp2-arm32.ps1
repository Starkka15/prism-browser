# Builds nghttp2 (HTTP/2 library) for ARM32 Windows (MSVC)
# Must run AFTER build-boringssl-arm32.ps1 is not required,
# but run this before curl.
# Output: Z:\prism-browser\arm32\lib\nghttp2.lib
#         Z:\prism-browser\arm32\include\nghttp2\

$ErrorActionPreference = 'Stop'

$depsDir = $PSScriptRoot
$srcDir  = "$depsDir\src\nghttp2"
$bldDir  = "$depsDir\build\nghttp2-arm32"
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"

# ── 1. VS toolchain ──────────────────────────────────────────────────────────

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
$vsPath = & $vswhere -latest -products * -property installationPath 2>$null
if (!$vsPath) { Write-Error "Visual Studio installation not found."; exit 1 }

$msvcRoot = "$vsPath\VC\Tools\MSVC"
$msvcVer  = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending |
            Where-Object { Test-Path "$($_.FullName)\bin\HostX64\arm\cl.exe" } |
            Select-Object -First 1
if (!$msvcVer) {
    Write-Error "ARM32 cl.exe not found. Install 'MSVC v143 - VS 2022 C++ ARM build tools' via VS Installer."
    exit 1
}
Write-Host "  ARM toolset: $($msvcVer.Name)"

$vcvarsall = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
$cmakeBin  = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaBin  = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$cmake     = "$cmakeBin\cmake.exe"
$ninja     = "$ninjaBin\ninja.exe"
if (!(Test-Path $cmake)) { Write-Error "cmake.exe not found at $cmakeBin"; exit 1 }
if (!(Test-Path $ninja))  { Write-Error "ninja.exe not found at $ninjaBin";  exit 1 }

# ── 2. Clone nghttp2 ─────────────────────────────────────────────────────────

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning nghttp2..."
    # Pin to a stable release tag
    git clone --depth 1 --branch v1.61.0 https://github.com/nghttp2/nghttp2.git $srcDir
} else {
    Write-Host "nghttp2 source already present."
}

# ── 3. Environment ───────────────────────────────────────────────────────────

function Import-VcVars([string]$Vcvarsall, [string]$Arch) {
    $tmp = [IO.Path]::GetTempFileName() -replace '\.tmp$', '.bat'
    "@echo off`r`ncall `"$Vcvarsall`" $Arch > nul 2>&1`r`nset" |
        Set-Content $tmp -Encoding ASCII
    $output = & "$env:SystemRoot\System32\cmd.exe" /c $tmp
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

Write-Host "Activating ARM32 MSVC toolchain..."
Import-VcVars $vcvarsall "amd64_arm"
$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"

# ── 4. Configure + build ─────────────────────────────────────────────────────

New-Item $bldDir -ItemType Directory -Force | Out-Null
Push-Location $bldDir

try {
    & $cmake -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_SYSTEM_NAME=Windows `
        -DCMAKE_SYSTEM_PROCESSOR=ARM `
        -DBUILD_SHARED_LIBS=OFF `
        -DBUILD_STATIC_LIBS=ON `
        -DENABLE_LIB_ONLY=ON `
        -DENABLE_FAILMALLOC=OFF `
        -DBUILD_TESTING=OFF `
        -DWITH_JEMALLOC=OFF `
        -DWITH_LIBXML2=OFF `
        -DWITH_LIBEVENT_PTHREADS=OFF `
        -DWITH_NEVERBLEED=OFF `
        $srcDir

    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    & $ninja nghttp2_static
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }

} finally {
    Pop-Location
}

# ── 5. Install ───────────────────────────────────────────────────────────────

New-Item "$outDir\lib"               -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\nghttp2"   -ItemType Directory -Force | Out-Null

# nghttp2 static lib may be named nghttp2_static.lib or nghttp2.lib
$lib = Get-ChildItem "$bldDir\lib" -Filter "*nghttp2*.lib" -Recurse |
       Select-Object -First 1
if (!$lib) {
    $lib = Get-ChildItem "$bldDir" -Filter "*nghttp2*.lib" -Recurse |
           Select-Object -First 1
}
if ($lib) {
    Copy-Item $lib.FullName "$outDir\lib\nghttp2.lib" -Force
} else {
    Write-Warning "nghttp2 .lib not found in expected locations - check $bldDir"
}

# Copy headers from source tree
Copy-Item "$srcDir\lib\includes\nghttp2\*" "$outDir\include\nghttp2\" -Force
# Copy generated headers from build tree (e.g. nghttp2ver.h)
$bldIncludes = "$bldDir\lib\includes\nghttp2"
if (Test-Path $bldIncludes) {
    Copy-Item "$bldIncludes\*" "$outDir\include\nghttp2\" -Force
}

Write-Host ""
Write-Host "Done. nghttp2 ARM32 outputs:"
Write-Host "  $outDir\lib\nghttp2.lib"
Write-Host "  $outDir\include\nghttp2\"
