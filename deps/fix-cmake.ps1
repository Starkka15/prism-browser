$f = 'C:\vcpkg-webkit\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake'
$txt = [System.IO.File]::ReadAllText($f)
Write-Host "Size before: $($txt.Length)"

# Step 1: Fix the concatenated line - message() and set() on same line with no newline
$badConcat = 'message(STATUS "ARM-cmake-fix: Patched c_winlibs/cpp_winlibs in ${meson_input_file_${buildtype}}")'
$idx = $txt.IndexOf($badConcat + '    set(')
if ($idx -ge 0) {
    $txt = $txt.Substring(0, $idx + $badConcat.Length) + "`r`n" + $txt.Substring($idx + $badConcat.Length)
    Write-Host "Fixed concatenation (message+set on same line)"
} else {
    Write-Host "Concatenated form not found"
}

# Step 2: Remove the file(READ/WRITE) block - try both LF and CRLF variants
$variants = @(
    "        file(READ `"`${meson_input_file_`${buildtype}}`" _z_xf_content)`n        string(REPLACE `"user32.lib']`" `"user32.lib', 'advapi32.lib', 'ws2_32.lib']`" _z_xf_content `"`${_z_xf_content}`")`n        file(WRITE `"`${meson_input_file_`${buildtype}}`" `"`${_z_xf_content}`")`n        message(STATUS `"ARM-cmake-fix: Patched c_winlibs/cpp_winlibs in `${meson_input_file_`${buildtype}}`")`n",
    "        file(READ `"`${meson_input_file_`${buildtype}}`" _z_xf_content)`r`n        string(REPLACE `"user32.lib']`" `"user32.lib', 'advapi32.lib', 'ws2_32.lib']`" _z_xf_content `"`${_z_xf_content}`")`r`n        file(WRITE `"`${meson_input_file_`${buildtype}}`" `"`${_z_xf_content}`")`r`n        message(STATUS `"ARM-cmake-fix: Patched c_winlibs/cpp_winlibs in `${meson_input_file_`${buildtype}}`")`r`n"
)
$removed = $false
foreach ($v in $variants) {
    if ($txt.Contains($v)) {
        $txt = $txt.Replace($v, "")
        Write-Host "Removed file(READ/WRITE) block"
        $removed = $true
        break
    }
}
if (-not $removed) {
    # Try searching for just the file(READ line and removing through message line manually
    $readStart = $txt.IndexOf("        file(READ `"`${meson_input_file_`${buildtype}}`" _z_xf_content)")
    $msgEnd = $txt.IndexOf("        message(STATUS `"ARM-cmake-fix: Patched c_winlibs")
    if ($readStart -ge 0 -and $msgEnd -ge $readStart) {
        # Find end of message line
        $lineEnd = $txt.IndexOf("`n", $msgEnd)
        if ($lineEnd -lt 0) { $lineEnd = $txt.Length - 1 }
        $txt = $txt.Substring(0, $readStart) + $txt.Substring($lineEnd + 1)
        Write-Host "Removed file(READ/WRITE) block via index search"
        $removed = $true
    }
    if (-not $removed) {
        Write-Warning "Could not find/remove file(READ/WRITE) block!"
        $i = $txt.IndexOf("_z_xf_content")
        Write-Host "xf_content at: $i"
    }
}

Write-Host "Size after: $($txt.Length)"
[System.IO.File]::WriteAllText($f, $txt, [System.Text.UTF8Encoding]::new($false))

# Verify: try cmake parse
$parseResult = & cmake -P $f 2>&1 | Select-Object -First 5
Write-Host "CMake parse check: $parseResult"
Write-Host "Done."
