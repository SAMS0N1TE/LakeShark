[CmdletBinding()]
param([string]$Config = 't-display-p4')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$cfg  = Get-Content (Join-Path $root 'bench\configs.json') -Raw | ConvertFrom-Json

$board = $cfg.configs | Where-Object { $_.name -eq $Config }
if (-not $board) { throw "unknown board profile '$Config'" }
$export = if ($board.idf_export) { $board.idf_export } else { $cfg.idf_export }
$env:IDF_PATH       = (Split-Path -Parent ($export -replace '/','\'))
$env:IDF_TOOLS_PATH = 'C:\Espressif'

$version = 'v' + ((Select-String -Path (Join-Path $env:IDF_PATH 'tools\cmake\version.cmake') -Pattern '^set\(IDF_VERSION_(MAJOR|MINOR|PATCH) (\d+)\)' | ForEach-Object { $_.Matches[0].Groups[2].Value }) -join '.')
$pattern = if ($version -eq 'v5.5.4') { 'idf5.5_py3.12*' } else { 'idf5.4_py3.11*' }
$venv = Get-ChildItem "$env:IDF_TOOLS_PATH\python_env" -Directory |
        Where-Object { $_.Name -like $pattern } | Select-Object -First 1
if (-not $venv -and $version -eq 'v5.5.4') {
    # IDF 5.5 also supports Python 3.11; not every workstation uses 3.12.
    $venv = Get-ChildItem "$env:IDF_TOOLS_PATH\python_env" -Directory |
            Where-Object { $_.Name -like 'idf5.5_py3.11*' } | Select-Object -First 1
}
if (-not $venv) { throw "no $pattern venv under $env:IDF_TOOLS_PATH\python_env" }
$py = Join-Path $venv.FullName 'Scripts\python.exe'
$env:IDF_PYTHON_ENV_PATH = $venv.FullName

$exports = & $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value
if ($LASTEXITCODE) { throw 'IDF tool export failed; install tools for this checkout and target first' }
foreach ($line in $exports) {
    if ($line -match '^([A-Z_]+)=(.*)$') {

        Set-Item -Path "env:$($Matches[1])" -Value ($Matches[2] -replace '%PATH%', $env:PATH)
    }
}
$env:PATH = "$(Join-Path $venv.FullName 'Scripts');$env:IDF_PATH\tools;$env:PATH"

foreach ($t in 'ninja','cmake','riscv32-esp-elf-gcc') {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t not on PATH after export" }
}
Write-Host "IDF $env:IDF_PATH ready (python $($venv.Name))"
