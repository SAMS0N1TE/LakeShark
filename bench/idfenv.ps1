
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$cfg  = Get-Content (Join-Path $root 'bench\configs.json') -Raw | ConvertFrom-Json

$env:IDF_PATH       = (Split-Path -Parent ($cfg.idf_export -replace '/','\'))
$env:IDF_TOOLS_PATH = 'C:\Espressif'

$venv = Get-ChildItem "$env:IDF_TOOLS_PATH\python_env" -Directory |
        Where-Object { $_.Name -like 'idf5.4_py3.11*' } | Select-Object -First 1
if (-not $venv) { throw "no idf5.4 py3.11 venv under $env:IDF_TOOLS_PATH\python_env" }
$py = Join-Path $venv.FullName 'Scripts\python.exe'

foreach ($line in (& $py "$env:IDF_PATH\tools\idf_tools.py" export --format key-value 2>$null)) {
    if ($line -match '^([A-Z_]+)=(.*)$') {

        Set-Item -Path "env:$($Matches[1])" -Value ($Matches[2] -replace '%PATH%', $env:PATH)
    }
}
$env:PATH = "$(Join-Path $venv.FullName 'Scripts');$env:IDF_PATH\tools;$env:PATH"

foreach ($t in 'ninja','cmake','riscv32-esp-elf-gcc') {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t not on PATH after export" }
}
Write-Host "IDF $env:IDF_PATH ready (python $($venv.Name))"
