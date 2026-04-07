$vcpkgDir = "C:\vcpkg-webkit"
$logDst   = "Z:\prism-browser\deps\build-logs"

# 1. Which vcpkg_configure_meson.cmake files exist?
$paths = @(
    "$vcpkgDir\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake",
    "$vcpkgDir\scripts\cmake\vcpkg_configure_meson.cmake"
)
foreach ($p in $paths) {
    $exists = Test-Path $p
    Write-Host "EXISTS=$exists  $p"
    if ($exists) {
        $txt = [System.IO.File]::ReadAllText($p)
        $hasConfigure = $txt.Contains('configure_file("${SCRIPTS}/buildsystems/meson/meson.template.in"')
        $hasAdvapi    = $txt.Contains('advapi32 winlibs')
        $hasArmFix    = $txt.Contains('ARM32: CMAKE_TRY_COMPILE_TARGET_TYPE')
        Write-Host "  has configure_file call : $hasConfigure"
        Write-Host "  has advapi32 winlibs    : $hasAdvapi"
        Write-Host "  has ARM32 cmake-fix     : $hasArmFix"
        Write-Host "  size (chars)            : $($txt.Length)"
        # Copy to shared drive
        $leaf = Split-Path $p -Leaf
        $dir  = Split-Path (Split-Path $p -Parent) -Leaf
        Copy-Item $p "$logDst\DIAG-$dir-$leaf" -Force
    }
}

# 2. List installed vcpkg-tool-meson share dir
$shareDir = "$vcpkgDir\installed\x64-windows\share\vcpkg-tool-meson"
Write-Host "`nvcpkg-tool-meson share dir exists: $(Test-Path $shareDir)"
if (Test-Path $shareDir) {
    Get-ChildItem $shareDir | ForEach-Object { Write-Host "  $($_.Name)" }
}

# 3. Check meson.template.in location
$tmpl1 = "$vcpkgDir\scripts\buildsystems\meson\meson.template.in"
$tmpl2 = "$vcpkgDir\installed\x64-windows\share\vcpkg-tool-meson\meson.template.in"
foreach ($t in @($tmpl1, $tmpl2)) {
    Write-Host "`nmeson.template.in EXISTS=$(Test-Path $t)  $t"
    if (Test-Path $t) { Copy-Item $t "$logDst\DIAG-$(Split-Path $t -Leaf)" -Force }
}

Write-Host "`nDone. Check build-logs for DIAG-* files."
