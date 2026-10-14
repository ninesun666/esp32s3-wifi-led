$ErrorActionPreference = 'SilentlyContinue'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($f in 'cloudflared.pid','proxy.pid') {
  $p = Join-Path $dir $f
  if (Test-Path $p) {
    $id = (Get-Content $p | Select-Object -First 1).Trim()
    Stop-Process -Id $id -Force
    Write-Output "stopped $f (pid $id)"
    Remove-Item $p
  }
}
