# Watchdog for the read-only ESP32 monitor tunnel. One pass, then exits.
# Ensures: (1) the local read-only proxy answers, (2) cloudflared answers and its
# public URL does too. The public URL changes on every cloudflared restart, so
# tunnel.url is the single source of truth and tunnel_history.log keeps the trail.
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$urlFile = Join-Path $dir 'tunnel.url'
$hist = Join-Path $dir 'tunnel_history.log'
$beat = Join-Path $dir 'watchdog.log'
$proxyPid = Join-Path $dir 'proxy.pid'
$cfPid = Join-Path $dir 'cloudflared.pid'
$cf = Join-Path $env:USERPROFILE '.local\bin\cloudflared.exe'
$proxy = Join-Path $dir 'monitor_proxy.py'

function Log([string]$m) {
  $line = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $m
  Add-Content -Path $beat -Value $line
  Write-Output $line
}

function Test-Http([string]$u, [int]$maxTime) {
  if (-not $u) { return $false }
  & curl.exe -s -o $null --max-time $maxTime $u
  return ($LASTEXITCODE -eq 0)
}

function Read-Pid([string]$pf) {
  if (-not (Test-Path $pf)) { return $null }
  $id = 0
  if (-not [int]::TryParse(((Get-Content $pf | Select-Object -First 1) -as [string]).Trim(), [ref]$id)) { return $null }
  if (Get-Process -Id $id -ErrorAction SilentlyContinue) { return $id }
  return $null
}

function Start-Detached([string]$file, [string]$argLine, [string]$outName) {
  $p = Start-Process -FilePath $file -ArgumentList $argLine -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $dir ($outName + '.log')) `
    -RedirectStandardError (Join-Path $dir ($outName + '.err.log'))
  return $p.Id
}

$changed = @()

if (-not (Read-Pid $proxyPid) -or -not (Test-Http 'http://127.0.0.1:8788/' 8)) {
  $old = Read-Pid $proxyPid
  if ($old) { Stop-Process -Id $old -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Seconds 1
  $id = Start-Detached 'python.exe' ('"' + $proxy + '"') 'proxy'
  Start-Sleep -Seconds 2
  if (Test-Http 'http://127.0.0.1:8788/' 8) {
    $id | Set-Content -Encoding ascii $proxyPid
    $changed += "proxy pid $id up"
  } else {
    Log "PROBLEM: proxy (pid $id) did not answer on 127.0.0.1:8788"
  }
}

$cur = (Get-Content $urlFile -ErrorAction SilentlyContinue) | Select-Object -First 1
$cfDead = -not (Read-Pid $cfPid)
if ($cfDead -or -not (Test-Http $cur 20)) {
  $old = Read-Pid $cfPid
  if ($old) { Stop-Process -Id $old -Force -ErrorAction SilentlyContinue }
  if (Test-Path (Join-Path $dir 'cloudflared.log')) { Remove-Item (Join-Path $dir 'cloudflared.log') -Force }
  Start-Sleep -Seconds 2
  $id = Start-Detached $cf 'tunnel --url http://127.0.0.1:8788 --no-autoupdate' 'cloudflared'
  $id | Set-Content -Encoding ascii $cfPid
  $new = $null
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 1
    $new = Select-String -Path (Join-Path $dir 'cloudflared.log'),(Join-Path $dir 'cloudflared.err.log') `
      -Pattern 'https://[a-z0-9-]+\.trycloudflare\.com' -AllMatches -ErrorAction SilentlyContinue |
      ForEach-Object { $_.Matches.Value } | Select-Object -First 1
    if ($new) { break }
  }
  if ($new) {
    $new | Set-Content -Encoding ascii $urlFile
    Add-Content -Path $hist -Value ((Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  ' + $new)
    $changed += "tunnel pid $id -> $new"
  } else {
    Log 'PROBLEM: cloudflared started but no trycloudflare URL within 40s'
  }
}

if ($changed.Count) { Log ('RECOVERED: ' + ($changed -join '; ')) }
Log ('OK url=' + ((Get-Content $urlFile -ErrorAction SilentlyContinue) | Select-Object -First 1))
exit 0
