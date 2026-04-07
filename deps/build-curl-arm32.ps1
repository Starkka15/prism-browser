# Builds libcurl for ARM32 Windows (MSVC), using BoringSSL for TLS and
# nghttp2 for HTTP/2.
#
# Run AFTER:  build-boringssl-arm32.ps1
#             build-nghttp2-arm32.ps1
#
# Output: Z:\prism-browser\arm32\lib\libcurl.lib
#         Z:\prism-browser\arm32\include\curl\

$ErrorActionPreference = 'Stop'

$depsDir = $PSScriptRoot
$srcDir  = "$depsDir\src\curl"
$bldDir  = "$depsDir\build\curl-arm32"
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"

# ── 1. Verify upstream deps are built ────────────────────────────────────────

foreach ($required in @("$outDir\lib\ssl.lib", "$outDir\lib\crypto.lib", "$outDir\lib\nghttp2.lib")) {
    if (!(Test-Path $required)) {
        Write-Error "Missing $required - run build-boringssl-arm32.ps1 and build-nghttp2-arm32.ps1 first."
        exit 1
    }
}

# ── 2. VS toolchain ──────────────────────────────────────────────────────────

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

# ── 3. Clone curl (before Import-VcVars which overwrites PATH) ───────────────

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "Cloning curl..."
    $gitPaths = @(
        "C:\Program Files\Git\cmd\git.exe",
        "C:\Program Files\Git\bin\git.exe",
        "C:\Program Files (x86)\Git\cmd\git.exe"
    )
    $git = $gitPaths | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$git) {
        $gitCmd = Get-Command git -ErrorAction SilentlyContinue
        $git = if ($gitCmd) { $gitCmd.Source } else { $null }
    }
    if (!$git) { Write-Error "git.exe not found. Install Git for Windows."; exit 1 }
    & $git clone --depth 1 --branch curl-8_7_1 https://github.com/curl/curl.git $srcDir
} else {
    Write-Host "curl source already present."
}

# ── 4. Environment ───────────────────────────────────────────────────────────

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

# ── 5. Configure ─────────────────────────────────────────────────────────────
#
# curl treats BoringSSL as OpenSSL — API compatible, just point OPENSSL_ROOT_DIR
# at our arm32 output. curl auto-detects OPENSSL_IS_BORINGSSL from the headers.

New-Item $bldDir -ItemType Directory -Force | Out-Null
Push-Location $bldDir

try {
    & $cmake -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_SYSTEM_NAME=Windows `
        -DCMAKE_SYSTEM_PROCESSOR=ARM `
        -DBUILD_SHARED_LIBS=OFF `
        -DBUILD_CURL_EXE=OFF `
        -DBUILD_TESTING=OFF `
        -DCURL_USE_OPENSSL=ON `
        "-DOPENSSL_ROOT_DIR=$outDir" `
        "-DOPENSSL_INCLUDE_DIR=$outDir\include" `
        "-DOPENSSL_SSL_LIBRARY=$outDir\lib\ssl.lib" `
        "-DOPENSSL_CRYPTO_LIBRARY=$outDir\lib\crypto.lib" `
        -DUSE_NGHTTP2=ON `
        "-DNGHTTP2_INCLUDE_DIR=$outDir\include" `
        "-DNGHTTP2_LIBRARY=$outDir\lib\nghttp2.lib" `
        -DCURL_DISABLE_LDAP=ON `
        -DCURL_DISABLE_LDAPS=ON `
        -DCURL_DISABLE_TELNET=ON `
        -DCURL_DISABLE_DICT=ON `
        -DCURL_DISABLE_FILE=ON `
        -DCURL_DISABLE_TFTP=ON `
        -DCURL_DISABLE_GOPHER=ON `
        -DCURL_DISABLE_POP3=ON `
        -DCURL_DISABLE_IMAP=ON `
        -DCURL_DISABLE_SMTP=ON `
        -DCURL_DISABLE_FTP=ON `
        -DCURL_DISABLE_RTSP=ON `
        -DCURL_DISABLE_HTTP=OFF `
        -DCURL_DISABLE_WEBSOCKETS=OFF `
        -DUSE_WIN32_LDAP=OFF `
        -DCURL_USE_LIBPSL=OFF `
        -DCURL_USE_LIBSSH2=OFF `
        -DCURL_USE_SCHANNEL=OFF `
        $srcDir

    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    Write-Host "Building libcurl..."
    & $ninja libcurl_static
    if ($LASTEXITCODE -ne 0) {
        # Some curl versions name the target differently
        & $ninja curl libcurl
    }
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }

} finally {
    Pop-Location
}

# ── 6. Install ───────────────────────────────────────────────────────────────

New-Item "$outDir\lib"          -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\curl" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir\lib" -Filter "libcurl*.lib" -Recurse |
       Select-Object -First 1
if (!$lib) {
    $lib = Get-ChildItem "$bldDir" -Filter "libcurl*.lib" -Recurse |
           Select-Object -First 1
}
if ($lib) {
    Copy-Item $lib.FullName "$outDir\lib\libcurl.lib" -Force
    Write-Host "  Copied: $($lib.Name) -> libcurl.lib"
} else {
    Write-Warning "libcurl .lib not found - check $bldDir"
}

Copy-Item "$srcDir\include\curl\*" "$outDir\include\curl\" -Force

Write-Host ""
Write-Host "Done. libcurl ARM32 outputs (BoringSSL TLS 1.3 + HTTP/2):"
Write-Host "  $outDir\lib\libcurl.lib"
Write-Host "  $outDir\include\curl\"
Write-Host ""
Write-Host "Link order for your app:"
Write-Host "  libcurl.lib ssl.lib crypto.lib nghttp2.lib ws2_32.lib crypt32.lib"
