# Creates two scheduled tasks:
#   WebKitBuild - runs ninja WebCore via run-webkit-build.ps1, output to C:\webkit-ninja.log
#   WebKitSync  - copies C:\webkit-ninja.log -> Z: every 20 seconds

$ninjaLog  = 'C:\webkit-ninja.log'
$sharedLog = 'Z:\prism-browser\deps\build-logs\webkit-build-live.log'

# Truncate local log
'' | Set-Content $ninjaLog -Encoding UTF8

# Stop + unregister any existing tasks
foreach ($t in @('WebKitBuild','WebKitSync')) {
    Stop-ScheduledTask  -TaskName $t -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue
}

# Build task: truncate log then run build, all output -> C:\webkit-ninja.log
$buildArg = '/c echo. > C:\webkit-ninja.log && powershell.exe -ExecutionPolicy Bypass -NonInteractive -File "Z:\prism-browser\deps\run-webkit-build.ps1" >> C:\webkit-ninja.log 2>&1'
$buildAction = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument $buildArg

# Sync task: loop that copies C: log -> Z: every 20 s
$syncScript = 'while($true){ Start-Sleep 20; try { Copy-Item C:\webkit-ninja.log "Z:\prism-browser\deps\build-logs\webkit-build-live.log" -Force -ErrorAction SilentlyContinue } catch {} }'
$syncArg = "-NonInteractive -Command `"$syncScript`""
$syncAction = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $syncArg

$trigger  = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(5)
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 4) -MultipleInstances IgnoreNew

$principal = New-ScheduledTaskPrincipal -UserId (whoami) -LogonType Interactive -RunLevel Highest

Register-ScheduledTask -TaskName 'WebKitBuild' -Action $buildAction -Trigger $trigger -Settings $settings -Principal $principal -Force
Register-ScheduledTask -TaskName 'WebKitSync'  -Action $syncAction  -Trigger $trigger -Settings $settings -Principal $principal -Force

Write-Host "Tasks registered. Starting..."
Start-ScheduledTask -TaskName 'WebKitSync'
Start-ScheduledTask -TaskName 'WebKitBuild'
Write-Host "Build and sync tasks running detached from SSH."
