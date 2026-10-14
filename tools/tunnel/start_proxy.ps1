$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Start-Process -FilePath python -ArgumentList (Join-Path $dir 'monitor_proxy.py') `
  -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $dir 'proxy.log') `
  -RedirectStandardError (Join-Path $dir 'proxy.err.log')
$py.Id | Set-Content -Encoding ascii (Join-Path $dir 'proxy.pid')
Write-Output "proxy_pid=$($py.Id)"
