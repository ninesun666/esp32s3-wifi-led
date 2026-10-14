$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $env:USERPROFILE '.local\bin\cloudflared.exe'
$cf = Start-Process -FilePath $exe `
  -ArgumentList 'tunnel','--url','http://127.0.0.1:8788','--no-autoupdate' `
  -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $dir 'cloudflared.log') `
  -RedirectStandardError (Join-Path $dir 'cloudflared.err.log')
$cf.Id | Set-Content -Encoding ascii (Join-Path $dir 'cloudflared.pid')
Write-Output "cloudflared_pid=$($cf.Id)"
$url = $null
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Seconds 1
  $url = Select-String -Path (Join-Path $dir 'cloudflared.log'),(Join-Path $dir 'cloudflared.err.log') `
    -Pattern 'https://[a-z0-9-]+\.trycloudflare\.com' -AllMatches |
    ForEach-Object { $_.Matches.Value } | Select-Object -First 1
  if ($url) { break }
}
if (-not $url) { Write-Error 'no tunnel URL appeared in 40s'; exit 1 }
$url | Set-Content -Encoding ascii (Join-Path $dir 'tunnel.url')
Write-Output "url=$url"
