# Common-ARM32.ps1 — dot-source this in each build script
# Sets up ARM32 MSVC toolchain variables and environment

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" }
$vsPath = & $vswhere -latest -products * -property installationPath 2>$null
if (!$vsPath) { Write-Error "Visual Studio not found."; exit 1 }

$msvcRoot = "$vsPath\VC\Tools\MSVC"
$msvcVer  = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending |
            Where-Object { Test-Path "$($_.FullName)\bin\HostX64\arm\cl.exe" } |
            Select-Object -First 1
if (!$msvcVer) { Write-Error "ARM32 cl.exe not found. Install 'MSVC v143 ARM build tools'."; exit 1 }

# Compute platform toolset string by scanning the actual installed PlatformToolsets.
# VS version number (17 → v170, 18 → v180) is encoded in the MSBuild VC targets path.
$vsMajorNum  = [int](Split-Path (Split-Path $vsPath -Parent) -Leaf)
$vcTargetsDir = "$vsPath\MSBuild\Microsoft\VC\v$($vsMajorNum * 10)\Platforms\Win32\PlatformToolsets"
$toolset = "v143"   # safe fallback (VS 2022)
if (Test-Path $vcTargetsDir) {
    $detected = Get-ChildItem $vcTargetsDir -Directory |
                Where-Object { $_.Name -match '^v1\d+$' } |
                Sort-Object Name -Descending |
                Select-Object -First 1 -ExpandProperty Name
    if ($detected) { $toolset = $detected }
}
Write-Host "  Detected platform toolset: $toolset"

$armBinDir  = "$($msvcVer.FullName)\bin\HostX64\arm"
$hostBinDir = "$($msvcVer.FullName)\bin\HostX64\x64"
$clExe      = "$armBinDir\cl.exe"
$armasmExe  = "$armBinDir\armasm.exe"

$cmakeBin = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaBin = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$cmake    = "$cmakeBin\cmake.exe"
$ninja    = "$ninjaBin\ninja.exe"
if (!(Test-Path $cmake)) { Write-Error "cmake.exe not found at $cmakeBin"; exit 1 }
if (!(Test-Path $ninja))  { Write-Error "ninja.exe not found at $ninjaBin";  exit 1 }

$sdkLibRoot = "C:\Program Files (x86)\Windows Kits\10\Lib"
$sdkVer     = Get-ChildItem $sdkLibRoot -Directory -Filter "10.0.*" |
              Sort-Object Name -Descending |
              Where-Object { Test-Path "$($_.FullName)\um\arm" } |
              Select-Object -First 1
if (!$sdkVer) { Write-Error "No ARM32-capable Windows SDK found."; exit 1 }

$sdkName    = $sdkVer.Name
$sdkBinDir  = "C:\Program Files (x86)\Windows Kits\10\bin\$sdkName\x64"
$sdkIncRoot = "C:\Program Files (x86)\Windows Kits\10\Include\$sdkName"

if ($env:PATH -notlike "*$armBinDir*") {
    $env:PATH = "$armBinDir;$hostBinDir;$cmakeBin;$ninjaBin;$sdkBinDir;$env:PATH"
}
$env:INCLUDE = "$($msvcVer.FullName)\include;$sdkIncRoot\ucrt;$sdkIncRoot\shared;$sdkIncRoot\um;$sdkIncRoot\winrt"
$env:LIB     = "$($msvcVer.FullName)\lib\arm;$($sdkVer.FullName)\ucrt\arm;$($sdkVer.FullName)\um\arm"
$env:LIBPATH = $env:LIB

Write-Host "  ARM32 toolchain: MSVC $($msvcVer.Name), SDK $sdkName"

function Patch-CmakeForArm32($srcDir) {
    $files = Get-ChildItem "$srcDir\*" -Recurse |
             Where-Object { $_.Name -eq "CMakeLists.txt" -or $_.Extension -eq ".cmake" }
    foreach ($f in $files) {
        $raw  = [System.IO.File]::ReadAllText($f.FullName)
        $orig = $raw
        # CMP0091: prevent MSVC_RUNTIME_LIBRARY being applied to armasm targets
        if ($raw -match 'cmake_minimum_required' -and $raw -notmatch 'CMP0091') {
            $raw = $raw -replace '(cmake_minimum_required\s*\([^\)]+\))', "`$1`ncmake_policy(SET CMP0091 OLD)"
        }
        $raw = ($raw -split "`n" | Where-Object { $_ -notmatch 'MSVC_RUNTIME_LIBRARY' }) -join "`n"
        # CMAKE_POLICY_VERSION_MINIMUM: cmake_minimum_required < 3.5 is rejected by CMake 4.2
        # Bump any ancient VERSION to 3.5 so CMake 4.2 accepts it
        $raw = $raw -replace '(cmake_minimum_required\s*\(\s*VERSION\s+)([0-2]\.[0-9]+(\.[0-9]+)?)\s*\)', '${1}3.5)'
        if ($raw -ne $orig) { [System.IO.File]::WriteAllText($f.FullName, $raw) }
    }
}
