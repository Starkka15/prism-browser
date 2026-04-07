# Builds BoringSSL for ARM32 Windows (MSVC)
# Output: Z:\prism-browser\arm32\lib\ssl.lib + crypto.lib
#         Z:\prism-browser\arm32\include\openssl\
#
# Prerequisites:
#   - Visual Studio with "MSVC ARM build tools" component
#   - Go (x64 host): https://go.dev/dl/  (needed by BoringSSL code gen)
#   - Git

$ErrorActionPreference = 'Stop'

$depsDir = $PSScriptRoot
$srcDir  = "$depsDir\src\boringssl"
$bldDir  = "$depsDir\build\boringssl-arm32"
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"

# ── 1. Find VS ───────────────────────────────────────────────────────────────

Write-Host "Checking prerequisites..."

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
$vsPath = & $vswhere -latest -products * -property installationPath 2>$null
if (!$vsPath) { Write-Error "Visual Studio not found."; exit 1 }

Write-Host "  VS: $vsPath"

# ── 2. Find ARM32 cl.exe directly ───────────────────────────────────────────
# Layout: VC\Tools\MSVC\<ver>\bin\HostX64\arm\cl.exe

$msvcRoot = "$vsPath\VC\Tools\MSVC"
# Find whichever MSVC version has ARM32 tools (may differ from latest toolset version)
$msvcVer = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending |
           Where-Object { Test-Path "$($_.FullName)\bin\HostX64\arm\cl.exe" } |
           Select-Object -First 1
if (!$msvcVer) {
    Write-Error "ARM32 cl.exe not found in any MSVC toolset under $msvcRoot. Install via VS Installer -> Individual Components -> 'MSVC v143 - VS 2022 C++ ARM build tools'."
    exit 1
}

$armBinDir  = "$($msvcVer.FullName)\bin\HostX64\arm"
$clExe      = "$armBinDir\cl.exe"
$armasmExe  = "$armBinDir\armasm.exe"
if (!(Test-Path $armasmExe)) { Write-Warning "armasm.exe not found at $armasmExe" }

Write-Host "  ARM cl.exe: $clExe"

# ── 3. Check Go ──────────────────────────────────────────────────────────────

if (!(Get-Command go -ErrorAction SilentlyContinue)) {
    Write-Error "Go not found. Install from https://go.dev/dl/ then reopen terminal."
    exit 1
}
Write-Host "  Go: $(go version)"

# ── 4. Build PATH for ARM32 cross-compilation ────────────────────────────────
# Need: ARM cl.exe, ARM link/lib tools, x64 host tools (mspdbsrv, cvtres, etc.)

$hostBinDir  = "$($msvcVer.FullName)\bin\HostX64\x64"
$cmakeBin    = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaBin    = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

# Verify cmake/ninja exist at expected paths
if (!(Test-Path "$cmakeBin\cmake.exe")) { Write-Error "cmake.exe not found at $cmakeBin"; exit 1 }
if (!(Test-Path "$ninjaBin\ninja.exe")) { Write-Error "ninja.exe not found at $ninjaBin"; exit 1 }

$cmake = "$cmakeBin\cmake.exe"
$ninja = "$ninjaBin\ninja.exe"

# Find Windows SDK — must have ARM32 lib dirs (Win11 SDKs dropped ARM32 support)
$sdkLibRoot = "C:\Program Files (x86)\Windows Kits\10\Lib"
$sdkVer = Get-ChildItem $sdkLibRoot -Directory -Filter "10.0.*" |
          Sort-Object Name -Descending |
          Where-Object { (Test-Path "$($_.FullName)\um\arm") -and (Test-Path "$($_.FullName)\ucrt\arm") } |
          Select-Object -First 1
if (!$sdkVer) {
    Write-Error @"
No Windows SDK with ARM32 lib support found.
Windows 11 SDKs (10.0.22000+) dropped ARM32. Install Windows 10 SDK 10.0.19041.0:
  VS Installer -> Individual Components -> 'Windows 10 SDK (10.0.19041.0)'
"@
    exit 1
}
Write-Host "  SDK: $($sdkVer.Name) (has ARM32 libs)"

$sdkBinRoot = "C:\Program Files (x86)\Windows Kits\10\bin\$($sdkVer.Name)"
$sdkBinDir  = if (Test-Path $sdkBinRoot) { "$sdkBinRoot\x64" } else { "" }

$env:PATH = "$armBinDir;$hostBinDir;$cmakeBin;$ninjaBin;$sdkBinDir;$env:PATH"

# Set required env vars for MSVC ARM cross-compile
$sdkIncRoot = "C:\Program Files (x86)\Windows Kits\10\Include\$($sdkVer.Name)"
$env:INCLUDE = "$($msvcVer.FullName)\include;" +
               "$sdkIncRoot\ucrt;" +
               "$sdkIncRoot\shared;" +
               "$sdkIncRoot\um;" +
               "$sdkIncRoot\winrt"

$env:LIB = "$($msvcVer.FullName)\lib\arm;" +
           "$($sdkVer.FullName)\ucrt\arm;" +
           "$($sdkVer.FullName)\um\arm"

$env:LIBPATH = $env:LIB

# Verify (cl.exe prints version to stderr; suppress NativeCommandError)
$clVer = cmd /c "cl.exe 2>&1" 2>$null | Select-Object -First 1
Write-Host "  Compiler: $clVer"

# ── 5. Clone BoringSSL ───────────────────────────────────────────────────────

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning BoringSSL (shallow)..."
    git clone --depth 1 https://boringssl.googlesource.com/boringssl $srcDir
} else {
    Write-Host "BoringSSL source already present."
}

# ── 5b. Patch BoringSSL for CMake 4.2 + armasm compatibility ────────────────
# BoringSSL sets cmake_policy(SET CMP0091 NEW) which makes CMake apply
# MSVC_RUNTIME_LIBRARY to ALL targets including armasm — which CMake 4.2 rejects.
# Fix: restore clean source, then change CMP0091 NEW->OLD and strip MSVC_RUNTIME_LIBRARY.
Write-Host "Patching BoringSSL for CMake 4.2 armasm compatibility..."
# Use Where-Object filter to avoid Get-ChildItem -Include quirks on network shares
$cmakeFiles = Get-ChildItem "$srcDir\*" -Recurse |
    Where-Object { ($_.Name -eq "CMakeLists.txt" -or $_.Extension -eq ".cmake") -and $_.Extension -ne ".md" -and $_.Extension -ne ".py" }
Write-Host "  Found $($cmakeFiles.Count) cmake files to scan"
foreach ($f in $cmakeFiles) {
    $raw = [System.IO.File]::ReadAllText($f.FullName)
    $changed = $false

    # CMP0091 is implicitly NEW via cmake_minimum_required >= 3.15.
    # Inject cmake_policy(SET CMP0091 OLD) after cmake_minimum_required to override it.
    # This prevents CMake from applying MSVC_RUNTIME_LIBRARY to armasm targets.
    if ($raw -match 'cmake_minimum_required' -and $raw -notmatch 'CMP0091') {
        $raw = $raw -replace '(cmake_minimum_required\s*\([^\)]+\))', "`$1`ncmake_policy(SET CMP0091 OLD)  # patched for armasm compat"
        $changed = $true
        Write-Host "  Injected CMP0091 OLD after cmake_minimum_required: $($f.Name)"
    }
    # Also handle any explicit CMP0091 NEW
    if ($raw -match 'cmake_policy.*CMP0091.*NEW') {
        $raw = $raw -replace '(cmake_policy\s*\(\s*SET\s+CMP0091\s+)NEW', '${1}OLD'
        $changed = $true
        Write-Host "  CMP0091 NEW->OLD: $($f.Name)"
    }
    if ($raw -match 'MSVC_RUNTIME_LIBRARY') {
        $raw = ($raw -split "`n" | Where-Object { $_ -notmatch 'MSVC_RUNTIME_LIBRARY' }) -join "`n"
        $changed = $true
        Write-Host "  Removed MSVC_RUNTIME_LIBRARY: $($f.Name)"
    }
    if ($changed) { [System.IO.File]::WriteAllText($f.FullName, $raw) }
}

# ── 6. CMake configure ───────────────────────────────────────────────────────

Write-Host "Configuring BoringSSL for ARM32..."
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
        -DCMAKE_ASM_COMPILER="$armasmExe" `
        "-DCMAKE_POLICY_DEFAULT_CMP0091=OLD" `
        -DBUILD_SHARED_LIBS=OFF `
        -DBUILD_TESTING=OFF `
        -DOPENSSL_NO_ASM=1 `
        $srcDir

    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

    # ── 7. Build ─────────────────────────────────────────────────────────────

    Write-Host "Building ssl + crypto..."
    & $ninja ssl crypto
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

} finally {
    Pop-Location
}

# ── 8. Install ───────────────────────────────────────────────────────────────

Write-Host "Copying outputs to $outDir..."
New-Item "$outDir\lib"             -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\openssl" -ItemType Directory -Force | Out-Null

Copy-Item "$bldDir\ssl.lib"    "$outDir\lib\" -Force
Copy-Item "$bldDir\crypto.lib" "$outDir\lib\" -Force
Copy-Item "$srcDir\include\openssl\*" "$outDir\include\openssl\" -Recurse -Force

if (Test-Path "$bldDir\decrepit.lib") {
    Copy-Item "$bldDir\decrepit.lib" "$outDir\lib\" -Force
}

Write-Host ""
Write-Host "Done. BoringSSL ARM32 outputs:"
Write-Host "  $outDir\lib\ssl.lib"
Write-Host "  $outDir\lib\crypto.lib"
Write-Host "  $outDir\include\openssl\"
