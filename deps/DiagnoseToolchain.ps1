$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) {
    $vswhere = "C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
}
$vsPath = & $vswhere -latest -products * -property installationPath 2>$null
if (!$vsPath) { Write-Host "Visual Studio not found via vswhere."; exit 1 }
Write-Host "VS path: $vsPath"

$msvcRoot = "$vsPath\VC\Tools\MSVC"
$versions = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending
Write-Host "MSVC versions found:"
foreach ($ver in $versions) {
    Write-Host "  $($ver.Name)"
    $hostX64 = "$($ver.FullName)\bin\HostX64"
    if (Test-Path $hostX64) {
        Get-ChildItem $hostX64 -Directory | ForEach-Object {
            $has = Test-Path "$($_.FullName)\cl.exe"
            Write-Host "    HostX64\$($_.Name)\cl.exe = $has"
        }
    }
    $libDir = "$($ver.FullName)\lib"
    if (Test-Path $libDir) {
        $libs = (Get-ChildItem $libDir -Directory | ForEach-Object { $_.Name }) -join ", "
        Write-Host "    lib dirs: $libs"
    }
}

Write-Host ""
Write-Host "Searching for cmake.exe under VS..."
Get-ChildItem "$vsPath" -Filter "cmake.exe" -Recurse -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "  $($_.FullName)" }

Write-Host ""
Write-Host "Searching for ninja.exe under VS..."
Get-ChildItem "$vsPath" -Filter "ninja.exe" -Recurse -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "  $($_.FullName)" }
