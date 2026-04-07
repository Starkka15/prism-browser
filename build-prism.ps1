# Build Prism Browser UWP app (ARM)
# Run this on the Windows build machine:  .\build-prism.ps1

$ErrorActionPreference = 'Stop'

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    throw "vswhere.exe not found — install Visual Studio"
}

$vsPath = & $vsWhere -latest -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    throw "MSBuild.exe not found at $msbuild"
}

$prismDir = Split-Path $MyInvocation.MyCommand.Path
$sln      = Join-Path $prismDir "Prism\Prism.sln"

Write-Host "==> Building Prism (ARM Release)..." -ForegroundColor Cyan
& $msbuild $sln `
    /p:Configuration=Release `
    /p:Platform=ARM `
    /m /nologo /verbosity:minimal

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "==> Build succeeded." -ForegroundColor Green
$appx = Get-ChildItem -Path $prismDir -Filter "*.appx" -Recurse | Sort-Object LastWriteTime | Select-Object -Last 1
if ($appx) {
    Write-Host "AppX: $($appx.FullName)" -ForegroundColor Yellow
}
