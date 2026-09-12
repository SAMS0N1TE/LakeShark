
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Config,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$cfgPath = Join-Path $PSScriptRoot 'configs.json'
if (-not (Test-Path $cfgPath)) { throw "no bench/configs.json at $cfgPath" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json

$c = $cfg.configs | Where-Object { $_.name -eq $Config }
if (-not $c) {
    $names = ($cfg.configs | ForEach-Object { $_.name }) -join ', '
    throw "no configuration '$Config'. configs.json has: $names"
}

$export = $null
if ($env:IDF_PATH) { $export = Join-Path $env:IDF_PATH 'export.ps1' }
elseif ($c.idf_export) { $export = $c.idf_export }
elseif ($cfg.idf_export) { $export = $cfg.idf_export }

if (-not $export -or -not (Test-Path $export)) {
    throw "no ESP-IDF export script at '$export'. Set IDF_PATH to your ESP-IDF checkout, or fix idf_export in bench/configs.json."
}

if ($env:MSYSTEM -or $env:MSYS2_PATH_TYPE) {
    throw "IDF's export refuses to run under MSys/Mingw. Run this from PowerShell."
}

$dir = $c.dir
$target = if ($c.target) { $c.target } else { $cfg.target }

if ($Clean -and (Test-Path (Join-Path $root $dir))) {
    Write-Host "removing $dir" -ForegroundColor Yellow
    Remove-Item (Join-Path $root $dir) -Recurse -Force
}

$script = @"
`$ErrorActionPreference = 'Stop'
`$idfPy = Get-ChildItem 'C:/Espressif/tools/idf-python' -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
if (`$idfPy) { `$env:PATH = "`$(`$idfPy.FullName);`$env:PATH" }
. '$export' | Out-Null
Set-Location '$root'
idf.py -B '$dir' -D SDKCONFIG='$dir/sdkconfig' -D SDKCONFIG_DEFAULTS='$($c.defaults)' -D IDF_TARGET='$target' build
exit `$LASTEXITCODE
"@

$tmp = Join-Path $env:TEMP "ls_build_$($c.name)_$([guid]::NewGuid().ToString('N')).ps1"
Set-Content -Path $tmp -Value $script -Encoding UTF8
try {
    & pwsh -NoProfile -ExecutionPolicy Bypass -File $tmp
    $rc = $LASTEXITCODE
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

if ($rc -ne 0) { throw "$($c.name): build failed (exit $rc)" }
Write-Host "built $($c.name) into $dir" -ForegroundColor Green
Write-Host "flash it with:  pwsh -File bench/flash.ps1 $($c.name) -AppOnly" -ForegroundColor DarkGray
