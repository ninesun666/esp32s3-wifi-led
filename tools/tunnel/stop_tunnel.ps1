$ErrorActionPreference = 'SilentlyContinue'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Unregister-ScheduledTask -TaskName 'Esp32MonitorTunnel' -Confirm:$false
Write-Output 'scheduled task Esp32MonitorTunnel removed (if it existed)'
foreach ($f in 'cloudflared.pid','proxy.pid') {
  $p = Join-Path $dir $f
  if (Test-Path $p) {
    $id = (Get-Content $p | Select-Object -First 1).Trim()
    Stop-Process -Id $id -Force
    Write-Output "stopped $f (pid $id)"
    Remove-Item $p
  }
}
