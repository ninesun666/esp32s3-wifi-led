param(
    # Named -Dir, not -ProjectDir: idf.py's own -p flag would prefix-match -ProjectDir.
    [string]$Dir = 'C:\Users\gjx\Documents\Qoder\2026-10-07\6be1750c\hello_world',
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
idf.py @IdfArgs
