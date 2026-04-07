$f = 'C:\vcpkg-webkit\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake'
$txt = [System.IO.File]::ReadAllText($f)

# Remove the broken Patch 2 injection (file READ/WRITE block with parse error)
$badBlock = @'

        # ARM32: post-process generated meson cross-file to add advapi32+ws2_32
        file(READ "${meson_input_file_${buildtype}}" _z_xf)
        string(REPLACE "user32.lib']" "user32.lib', 'advapi32.lib', 'ws2_32.lib']" _z_xf "${_z_xf}")
        file(WRITE "${meson_input_file_${buildtype}}" "${_z_xf}")
        message(STATUS "ARM32: patched c_winlibs/cpp_winlibs in ${meson_input_file_${buildtype}}")
'@

if ($txt.Contains($badBlock)) {
    $txt = $txt.Replace($badBlock, "")
    Write-Host "Removed broken Patch 2"
} else {
    Write-Host "Broken Patch 2 not found (may already be clean)"
}

# Verify Patch 1 is present (list APPEND for advapi32)
if ($txt -match 'list\(APPEND c_winlibs.*advapi32') {
    Write-Host "Patch 1 (list APPEND advapi32) is present - good"
} else {
    Write-Warning "Patch 1 not found! Something is wrong."
}

[System.IO.File]::WriteAllText($f, $txt, [System.Text.UTF8Encoding]::new($false))
Write-Host "File size: $($txt.Length)"
Write-Host "Done."
