# Hand-rolled parsing, on purpose. PowerShell binds script parameters positionally in
# declaration order, so a declared [string] silently swallows a bare word meant for
# idf.py (`-Dir wifi_led build` once put "build" into the port), and `-p` prefix-matches
# any parameter starting with p. The automatic $args has no such binding, so both
# `-Port COM7` and idf.py's own `-p COM7` reach the tool unchanged.
$Dir = 'C:\Users\gjx\Documents\Qoder\2026-10-07\6be1750c\hello_world'
$Port = ''
$IdfArgs = @()

for ($i = 0; $i -lt $args.Count; $i++) {
    if ($args[$i] -eq '-Dir') { $Dir = $args[++$i]; continue }
    if ($args[$i] -eq '-Port') { $Port = $args[++$i]; continue }
    $IdfArgs += $args[$i]
}

# Clean MSys/Git-Bash env inherited from parent shell
Remove-Item Env:MSYSTEM,Env:SHELL,Env:TERM,Env:TERM_PROGRAM,Env:OLDPWD,Env:SHLVL,Env:PSModulePath -ErrorAction SilentlyContinue
$env:COMSPEC = 'C:\Windows\System32\cmd.exe'

$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:PATH = "C:\Espressif;$env:PATH"
$env:IDF_PATH = 'C:\Espressif\frameworks\esp-idf-v5.3.5'
. 'C:\Espressif\Initialize-Idf.ps1' | Out-Null

if ($Port) { $env:ESPPORT = $Port }

# Chinese Windows consoles are GBK, and idf.py pipes a child's stdout through a
# GBK encoder: the asset pack script's own U+2713 tick kills the build with
# UnicodeEncodeError after ninja has already done the work. Force UTF-8 on every
# Python in the chain.
$env:PYTHONIOENCODING = 'utf-8'

if ($IdfArgs.Count -eq 0) {
    Write-Error "no idf.py action given (usage: idf_run.ps1 -Dir <project> [-Port COMx] <action ...>)"
    exit 2
}

Set-Location $Dir
idf.py @IdfArgs
