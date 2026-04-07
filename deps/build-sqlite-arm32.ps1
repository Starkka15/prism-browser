# Builds SQLite for ARM32 Windows (MSVC) using the amalgamation
# Output: arm32\lib\sqlite3.lib, arm32\include\sqlite3.h

$ErrorActionPreference = 'Stop'
$depsDir = $PSScriptRoot
$outDir  = Split-Path $depsDir -Parent | Join-Path -ChildPath "arm32"
. "$depsDir\Common-ARM32.ps1"

$srcDir = "$depsDir\src\sqlite"
$bldDir = "$depsDir\build\sqlite-arm32"

if (!(Test-Path "$srcDir\sqlite3.c")) {
    Write-Host "Downloading SQLite 3.45.1 amalgamation..."
    $url = "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip"
    $zip = "$depsDir\sqlite-amalgamation.zip"
    Invoke-WebRequest $url -OutFile $zip
    New-Item $srcDir -ItemType Directory -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead($zip)
    foreach ($entry in $z.Entries) {
        if ($entry.Name -ne "") {
            $dest = "$srcDir\$($entry.Name)"
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
        }
    }
    $z.Dispose()
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
} else { Write-Host "SQLite source already present." }

# Write a minimal CMakeLists.txt for the amalgamation
$cmakeContent = @'
cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 OLD)
project(sqlite3 C)
add_library(sqlite3 STATIC sqlite3.c)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_ENABLE_RTREE
    SQLITE_ENABLE_COLUMN_METADATA
)
install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES sqlite3.h DESTINATION include)
'@

[System.IO.File]::WriteAllText("$srcDir\CMakeLists.txt", $cmakeContent)

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
        $srcDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    & $ninja sqlite3
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
} finally { Pop-Location }

New-Item "$outDir\lib"     -ItemType Directory -Force | Out-Null
New-Item "$outDir\include" -ItemType Directory -Force | Out-Null

$lib = Get-ChildItem "$bldDir" -Filter "sqlite3*.lib" -Recurse | Select-Object -First 1
if ($lib) { Copy-Item $lib.FullName "$outDir\lib\sqlite3.lib" -Force; Write-Host "  $($lib.Name) -> sqlite3.lib" }
else { Write-Warning "sqlite3.lib not found in $bldDir" }

Copy-Item "$srcDir\sqlite3.h" "$outDir\include\" -Force

Write-Host ""
Write-Host "Done. SQLite ARM32: $outDir\lib\sqlite3.lib"
