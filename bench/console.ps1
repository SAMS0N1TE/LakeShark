
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)] [string] $Board,
    [Parameter(Mandatory, Position = 1)] [string] $Command,
    [int]    $Wait = 3,
    [string] $Match,
    [switch] $Raw
)

$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfg   = Get-Content (Join-Path $bench 'configs.json') -Raw | ConvertFrom-Json

$c = $cfg.configs | Where-Object { $_.name -eq $Board }
if (-not $c) {
    Write-Host "no board '$Board'. Known:" -ForegroundColor Red
    $cfg.configs | Where-Object { $_.usb_serial } | ForEach-Object { Write-Host "  $($_.name)" }
    exit 2
}
if (-not $c.usb_serial) { Write-Host "'$Board' has no usb_serial recorded" -ForegroundColor Red; exit 2 }

$port = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\(COM\d+\)' -and $_.DeviceID -match [regex]::Escape($c.usb_serial) } |
        ForEach-Object { if ($_.Name -match '(COM\d+)') { $Matches[1] } } |
        Select-Object -First 1

if (-not $port) { Write-Host "$Board (serial $($c.usb_serial)) is not plugged in" -ForegroundColor Red; exit 3 }

$sp = New-Object System.IO.Ports.SerialPort $port, 115200, 'None', 8, 'One'
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 2000
$sp.NewLine = "`n"

try {
    $sp.Open()
    Start-Sleep -Milliseconds 400
    $sp.DiscardInBuffer()
    $sp.WriteLine($Command)




    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($Wait)
    while ((Get-Date) -lt $deadline) {
        try { [void]$sb.Append($sp.ReadExisting()) } catch { }
        Start-Sleep -Milliseconds 120
    }
    $out = $sb.ToString()
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}

$lines = ($out -replace "`r", '') -split "`n" | Where-Object { $_.Trim() }
if ($Match) { $lines = $lines | Where-Object { $_ -match $Match } }
if (-not $Raw) {


    $reply = $lines | Where-Object { $_ -notmatch '^[IWED] \(\d+\)' }
    $log   = $lines | Where-Object { $_ -match '^[IWED] \(\d+\)' }
    $lines = @($reply) + @($log)
}
$lines | ForEach-Object { Write-Host $_ }
