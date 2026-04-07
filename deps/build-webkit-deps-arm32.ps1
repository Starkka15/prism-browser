# Builds all WebKit WinCairo dependencies for ARM32 Windows (MSVC)
# Run AFTER: build-boringssl-arm32.ps1, build-nghttp2-arm32.ps1, build-curl-arm32.ps1
#
# Build order (dependency chain):
#   zlib -> libpng, libjpeg-turbo
#   zlib + libpng -> freetype
#   freetype -> pixman -> cairo
#   freetype -> harfbuzz
#   zlib -> icu -> libxml2
#   (separate) sqlite

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot

function Run-Script($name) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host " Building: $name" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    & "$depsDir\$name"
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
}

Run-Script "build-zlib-arm32.ps1"
Run-Script "build-libpng-arm32.ps1"
Run-Script "build-libjpeg-turbo-arm32.ps1"
Run-Script "build-freetype-arm32.ps1"
Run-Script "build-pixman-arm32.ps1"
Run-Script "build-cairo-arm32.ps1"
Run-Script "build-harfbuzz-arm32.ps1"
Run-Script "build-icu-arm32.ps1"
Run-Script "build-libxml2-arm32.ps1"
Run-Script "build-sqlite-arm32.ps1"

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host " All WebKit deps built successfully." -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
