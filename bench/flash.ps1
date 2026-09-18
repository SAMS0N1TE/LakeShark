
[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string] $Config,
    [string] $Port,
    [switch] $List,
    [switch] $AppOnly,
    [switch] $EraseCoredump,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $bench
$cfg   = Get-Content (Join-Path $bench 'configs.json') -Raw | ConvertFrom-Json

function Get-PortSerials {
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\(COM\d+\)' } |
        ForEach-Object {
            $p = if ($_.Name -match '(COM\d+)') { $Matches[1] }
            $s = if ($_.DeviceID -match '\\([^\\]+)$') { $Matches[1] }
            [pscustomobject]@{ Port = $p; Serial = $s; Name = $_.Name }
        }
}

$ports = @(Get-PortSerials)

if ($List -or -not $Config) {
    Write-Host "`nports:" -ForegroundColor Cyan
    foreach ($p in $ports) {
        $match = $cfg.configs | Where-Object { $_.usb_serial -and $_.usb_serial -eq $p.Serial }
        $tag = if ($match) { "  <- $($match.name)" } else { '' }
        Write-Host ("  {0,-6} {1,-14} {2}{3}" -f $p.Port, $p.Serial, $p.Name, $tag)
    }
    Write-Host "`nconfigs with a known board:" -ForegroundColor Cyan
    foreach ($c in ($cfg.configs | Where-Object { $_.usb_serial })) {
        Write-Host ("  {0,-18} serial {1,-12} {2} MB  {3}" -f $c.name, $c.usb_serial, $c.flash_mb, $c.board)
    }
    Write-Host ''
    exit 0
}

$c = $cfg.configs | Where-Object { $_.name -eq $Config }
if (-not $c) { Write-Host "no config '$Config' - try -List" -ForegroundColor Red; exit 2 }
if (-not $c.usb_serial) {
    Write-Host "config '$Config' has no usb_serial recorded, so the board cannot be identified." -ForegroundColor Red
    Write-Host "Add it to bench/configs.json rather than flashing blind." -ForegroundColor Red
    exit 2
}

$want = $ports | Where-Object { $_.Serial -eq $c.usb_serial }
if (-not $want) {
    Write-Host "board for '$Config' (serial $($c.usb_serial)) is not plugged in." -ForegroundColor Red
    Write-Host 'attached:' -ForegroundColor DarkGray
    $ports | ForEach-Object { Write-Host "  $($_.Port)  $($_.Serial)" -ForegroundColor DarkGray }
    exit 3
}

if ($Port -and $Port -ne $want.Port) {
    Write-Host "you asked for $Port, but '$Config' (serial $($c.usb_serial)) is on $($want.Port)." -ForegroundColor Red
    Write-Host 'Refusing. This is the check that exists because the LCD got the nano image.' -ForegroundColor Red
    exit 4
}
$Port = $want.Port

$sdk = Join-Path $root "$($c.dir)/sdkconfig"
if (-not (Test-Path $sdk)) {
    Write-Host "no $($c.dir)/sdkconfig - build it first: pwsh -File bench/verify.ps1 -Level full" -ForegroundColor Red
    exit 5
}
$sizeLine = Select-String -Path $sdk -Pattern '^CONFIG_ESPTOOLPY_FLASHSIZE_(\d+)MB=y' | Select-Object -First 1
if ($sizeLine) {
    $builtMb = [int]$sizeLine.Matches.Groups[1].Value
    if ($c.flash_mb -and $builtMb -gt [int]$c.flash_mb) {
        Write-Host "$($c.dir) is built for ${builtMb} MB but $($c.board) has $($c.flash_mb) MB." -ForegroundColor Red
        Write-Host 'That image cannot fit. Rebuild with bench/verify.ps1 -Level full.' -ForegroundColor Red
        if (-not $Force) { exit 6 }
    }
}

Write-Host ""
Write-Host "flashing $($c.board)" -ForegroundColor Cyan
Write-Host "  config $($c.name)   dir $($c.dir)   port $Port   serial $($c.usb_serial)   $($c.flash_mb) MB"
Write-Host ""

$flashAction = if ($AppOnly) { 'app-flash' } else { 'flash' }

$idfExport = if ($c.idf_export) { $c.idf_export } else { $cfg.idf_export }
if (-not $idfExport -or -not (Test-Path $idfExport)) {
    Write-Host "no IDF export at '$idfExport'" -ForegroundColor Red
    Write-Host "  set idf_export for $($c.name) in bench/configs.json" -ForegroundColor Red
    exit 7
}

$script = @"
`$ErrorActionPreference = 'Stop'
`$idfPy = Get-ChildItem 'C:/Espressif/tools/idf-python' -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
if (`$idfPy) { `$env:PATH = "`$(`$idfPy.FullName);`$env:PATH" }
. '$idfExport' | Out-Null
Set-Location '$root'
idf.py -B '$($c.dir)' -D SDKCONFIG='$($c.dir)/sdkconfig' -p $Port $flashAction
exit `$LASTEXITCODE
"@
$tmp = Join-Path $env:TEMP "ls_flash_$($c.name).ps1"
Set-Content -Path $tmp -Value $script -Encoding UTF8
$out = & pwsh -NoProfile -ExecutionPolicy Bypass -File $tmp 2>&1
$rc = $LASTEXITCODE
Remove-Item $tmp -ErrorAction SilentlyContinue

$out | Select-String -Pattern 'Hash of data|Hard resetting|error|Failed|will not fit' |
    Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }

if ($rc -ne 0) {
    Write-Host "`nFLASH FAILED ($rc)" -ForegroundColor Red
    $out | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    exit $rc
}

# Keep the ELF that was just flashed, filed under the hash the firmware
# reports and esp-coredump checks against.
#
# A core dump is only decodable with the exact ELF of the build that wrote
# it, and this repo kept none - so every crash on a build that had since been
# replaced was unreadable, which is exactly the position three crashes were
# lost to. One ELF is a few megabytes and a debuggable crash is worth far
# more than that.
$elf = Join-Path $root "$($c.dir)\lakeshark.elf"
if (Test-Path $elf) {
    $sha = (Get-FileHash -Algorithm SHA256 $elf).Hash.ToLower()
    $keep = Join-Path $root "$($c.dir)\elf-archive"
    if (-not (Test-Path $keep)) { New-Item -ItemType Directory -Force $keep | Out-Null }
    # The app SHA256 that esp-coredump compares is the image hash, not the
    # ELF file hash, so the name carries both: the file hash identifies this
    # copy, and app-desc.txt beside it records what the image reported.
    $dest = Join-Path $keep "$($sha.Substring(0,12)).elf"
    if (-not (Test-Path $dest)) {
        Copy-Item $elf $dest
        Write-Host "  kept $([IO.Path]::GetFileName($dest)) for decoding a future core dump" -ForegroundColor DarkGray
    }
    # Oldest first, keep the last twelve.
    Get-ChildItem $keep -Filter *.elf | Sort-Object LastWriteTime |
        Select-Object -SkipLast 12 | Remove-Item -ErrorAction SilentlyContinue
}

Write-Host "`nflashed. This is Build-verified only." -ForegroundColor Green
Write-Host "It is not Hardware-verified until something is observed working on the device." -ForegroundColor DarkGray
