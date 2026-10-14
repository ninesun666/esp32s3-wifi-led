$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $dir 'watchdog.ps1'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $exe + '"') `
  -WorkingDirectory $dir
$triggers = @(
  (New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(5) `
    -RepetitionInterval (New-TimeSpan -Minutes 1) `
    -RepetitionDuration (New-TimeSpan -Days 9999))
)
$settings = New-ScheduledTaskSettingsSet -Hidden -AllowStartIfOnBatteries `
  -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -StartWhenAvailable `
  -ExecutionTimeLimit (New-TimeSpan -Minutes 3)
Unregister-ScheduledTask -TaskName 'Esp32MonitorTunnel' -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName 'Esp32MonitorTunnel' -Action $action -Trigger $triggers `
  -Settings $settings -Description 'ESP32 monitor page: read-only proxy + cloudflared quick tunnel watchdog (NIN-13)' |
  Format-List TaskName, State
