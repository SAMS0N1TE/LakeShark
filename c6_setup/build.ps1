# Builds C6 SETUP into c6_setup/build. Needs ESP-IDF 5.5.x (bench/idfenv.ps1 finds it).
[CmdletBinding()]
param([switch] $Clean)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path -Parent $here
$flasher = Join-Path $here 'deps\esp-serial-flasher'
$commit = '24997fe693ff17e6598d7eb1d37af312752100aa'   # v2.1.0

if (-not (Test-Path (Join-Path $flasher 'CMakeLists.txt'))) {
    git clone -q https://github.com/espressif/esp-serial-flasher.git $flasher
    if ($LASTEXITCODE) { throw 'clone of esp-serial-flasher failed' }
}
$have = (git -C $flasher rev-parse HEAD).Trim()
if ($have -ne $commit) {
    git -C $flasher checkout -q $commit
    if ($LASTEXITCODE) { throw "esp-serial-flasher: cannot check out $commit" }
}

if (-not $env:IDF_PATH) { . (Join-Path $repo 'bench\idfenv.ps1') }
$env:IDF_COMPONENT_MANAGER = '0'
Push-Location $here
try {
    if ($Clean -and (Test-Path build)) { Remove-Item -Recurse -Force build, sdkconfig -ErrorAction SilentlyContinue }
    $py = if ($env:IDF_PYTHON_ENV_PATH) { Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe' } else { 'python' }
    & $py "$env:IDF_PATH/tools/idf.py" -D "FLASHER_SOURCE=$flasher" build
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
