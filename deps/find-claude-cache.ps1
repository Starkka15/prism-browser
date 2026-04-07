Write-Host "=== Searching for Claude/Anthropic files and registry entries ===" -ForegroundColor Cyan

Write-Host "`n-- AppData\Roaming --"
Get-ChildItem "$env:APPDATA" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "claude|anthropic" } |
    ForEach-Object { Write-Host "  $($_.FullName)" }

Write-Host "`n-- AppData\Local --"
Get-ChildItem "$env:LOCALAPPDATA" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "claude|anthropic" } |
    ForEach-Object { Write-Host "  $($_.FullName)" }

Write-Host "`n-- AppData\Local\Programs --"
Get-ChildItem "$env:LOCALAPPDATA\Programs" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "claude|anthropic" } |
    ForEach-Object { Write-Host "  $($_.FullName)" }

Write-Host "`n-- Registry HKCU\Software --"
Get-ChildItem "HKCU:\Software" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "claude|anthropic" } |
    ForEach-Object { Write-Host "  $($_.Name)" }

Write-Host "`n-- Registry HKLM\Software --"
Get-ChildItem "HKLM:\Software" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "claude|anthropic" } |
    ForEach-Object { Write-Host "  $($_.Name)" }

Write-Host "`n-- Windows Credential Manager --"
cmdkey /list | Select-String -Pattern "claude|anthropic" -CaseSensitive:$false

Write-Host "`nDone." -ForegroundColor Cyan
