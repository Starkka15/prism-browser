[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

. 'Z:\prism-browser\deps\Common-ARM32.ps1'

# Perl
$perlExe = @(
    'C:\Strawberry\perl\bin\perl.exe',
    'C:\Program Files\Git\usr\bin\perl.exe',
    'C:\Program Files (x86)\Git\usr\bin\perl.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($perlExe) { $env:PATH = (Split-Path $perlExe) + ";$env:PATH"; Write-Host "Perl: $perlExe" }
else { Write-Host "WARNING: perl not found"; exit 1 }

# Ruby
$rubyExe = @(
    'C:\Ruby34-x64\bin\ruby.exe','C:\Ruby33-x64\bin\ruby.exe',
    'C:\Ruby32-x64\bin\ruby.exe','C:\Ruby31-x64\bin\ruby.exe',
    'C:\Ruby30-x64\bin\ruby.exe','C:\Ruby27-x64\bin\ruby.exe',
    'C:\Ruby34\bin\ruby.exe','C:\Ruby33\bin\ruby.exe','C:\Ruby\bin\ruby.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$rubyExe) { $rc = Get-Command ruby.exe -ErrorAction SilentlyContinue; if ($rc) { $rubyExe = $rc.Source } }
if ($rubyExe) { $env:PATH = (Split-Path $rubyExe) + ";$env:PATH"; Write-Host "Ruby: $rubyExe" }
else { Write-Host "WARNING: ruby not found"; exit 1 }

# gperf
$gperfExe = @(
    'C:\Program Files (x86)\GnuWin32\bin\gperf.exe',
    'C:\Program Files\Git\usr\bin\gperf.exe',
    'C:\msys64\usr\bin\gperf.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$gperfExe) { $gc = Get-Command gperf -ErrorAction SilentlyContinue; if ($gc) { $gperfExe = $gc.Source } }
if ($gperfExe) { $env:PATH = (Split-Path $gperfExe) + ";$env:PATH"; Write-Host "gperf: $gperfExe" }
else { Write-Host "WARNING: gperf not found" }

Write-Host "==> Building WebCore objects..."
& $ninja -C 'Z:\prism-browser\deps\build\webkit-arm32' WebCore -j2 2>&1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Building WebKit..."
& $ninja -C 'Z:\prism-browser\deps\build\webkit-arm32' WebKit -j2 2>&1
exit $LASTEXITCODE
