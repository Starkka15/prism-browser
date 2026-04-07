$script = 'Z:\prism-browser\deps\build-webkit-arm32.ps1'
$errLog = 'Z:\prism-browser\deps\build-logs\webkit-build-stderr.log'

# The build script uses Start-Transcript to write all output to webkit-build-live.log.
# We only redirect stderr here as a fallback for startup failures.
Start-Process -FilePath 'powershell.exe' `
    -ArgumentList '-ExecutionPolicy','Bypass','-NonInteractive','-File',$script `
    -RedirectStandardError $errLog `
    -WindowStyle Hidden
Write-Host "Build launched."
