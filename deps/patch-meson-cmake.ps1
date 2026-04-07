$f = 'C:\vcpkg-webkit\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake'
$txt = [System.IO.File]::ReadAllText($f)

Write-Host "File size before: $($txt.Length)"

# Check if already patched
if ($txt -match 'ARM32: advapi32 needed by ICU') {
    Write-Host "Already patched, skipping."
    exit 0
}

# Patch 1: inject list(APPEND) after separate_arguments(cpp_winlibs...)
$old1 = "    separate_arguments(cpp_winlibs NATIVE_COMMAND `"`${VCPKG_DETECTED_CMAKE_CXX_STANDARD_LIBRARIES}`")"
$new1 = $old1 + "`r`n    # ARM32: advapi32 needed by ICU wintz.c (registry API calls)`r`n    list(APPEND c_winlibs `"advapi32.lib`" `"ws2_32.lib`")`r`n    list(APPEND cpp_winlibs `"advapi32.lib`" `"ws2_32.lib`")"
if ($txt.Contains($old1)) {
    $txt = $txt.Replace($old1, $new1)
    Write-Host "Patch 1 applied (list APPEND)"
} else {
    Write-Warning "Patch 1: search string not found"
    # Show surrounding context to debug
    $idx = $txt.IndexOf("separate_arguments(cpp_winlibs")
    if ($idx -ge 0) {
        Write-Host "Found at idx $idx : $($txt.Substring($idx, [Math]::Min(120, $txt.Length - $idx)))"
    }
}

# Patch 2: file(READ/WRITE) post-process after configure_file(...)
$cfSearch = 'configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/meson.template.in"'
$cfIdx = $txt.IndexOf($cfSearch)
if ($cfIdx -ge 0) {
    $closeIdx = $txt.IndexOf(')', $cfIdx + $cfSearch.Length)
    $insertAt = $closeIdx + 1
    if ($insertAt -lt $txt.Length -and $txt[$insertAt] -eq [char]13) { $insertAt++ }
    if ($insertAt -lt $txt.Length -and $txt[$insertAt] -eq [char]10) { $insertAt++ }
    $inject = @'

        # ARM32: post-process generated meson cross-file to add advapi32+ws2_32
        file(READ "${meson_input_file_${buildtype}}" _z_xf)
        string(REPLACE "user32.lib']" "user32.lib', 'advapi32.lib', 'ws2_32.lib']" _z_xf "${_z_xf}")
        file(WRITE "${meson_input_file_${buildtype}}" "${_z_xf}")
        message(STATUS "ARM32: patched c_winlibs/cpp_winlibs in ${meson_input_file_${buildtype}}")
'@
    $txt = $txt.Substring(0, $insertAt) + $inject + $txt.Substring($insertAt)
    Write-Host "Patch 2 applied (file READ/WRITE post-process)"
} else {
    Write-Warning "Patch 2: configure_file search string not found"
    Write-Host "Searching for any configure_file..."
    $cfIdx2 = $txt.IndexOf("configure_file(")
    if ($cfIdx2 -ge 0) {
        Write-Host "Found at $cfIdx2 : $($txt.Substring($cfIdx2, [Math]::Min(120, $txt.Length - $cfIdx2)))"
    }
}

[System.IO.File]::WriteAllText($f, $txt, [System.Text.UTF8Encoding]::new($false))
Write-Host "File size after: $($txt.Length)"
Write-Host "Done."
