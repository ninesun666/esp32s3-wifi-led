param(
    # Named -Dir so idf.py's own short flags never collide with our parameter names.
    [string]$Dir = 'C:\Users\gjx\Documents\Qoder\2026-10-07\6be1750c\hello_world',
    # idf.py's `-p COMx` cannot be forwarded: PowerShell drops unmatched short options
    # before ValueFromRemainingArguments sees them. idf.py reads the port from ESPPORT.
    [string]$Port = '',
    [Parameter(ValueFromRemainingArguments=$true)] [string[]]$IdfArgs
)

# Clean MSys/Git-Bash env inherited from parent shell
Remove-Item Env:MSYSTEM,Env:SHELL,Env:TERM,Env:TERM_PROGRAM,Env:OLDPWD,Env:SHLVL,Env:PSModulePath -ErrorAction SilentlyContinue
$env:COMSPEC = 'C:\Windows\System32\cmd.exe'

$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:PATH = "C:\Espressif;$env:PATH"
$env:IDF_PATH = 'C:\Espressif\frameworks\esp-idf-v5.3.5'
. 'C:\Espressif\Initialize-Idf.ps1' | Out-Null

Set-Location $Dir
if ($Port) { $env:ESPPORT = $Port }
idf.py @IdfArgs
