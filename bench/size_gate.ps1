
# App size gate: every built lakeshark.bin must fit the ota_0 slot of
# boards/partitions_16m_ota.csv, because an update over Wi-Fi writes the other
# slot of the same size. With -Record the sizes are appended to a ledger so a
# series of commits can be compared (bin <= the previous entry for the same
# config); -Against <ledger> fails when a config grew since its last entry.
#
#   pwsh -File bench/size_gate.ps1                         # all built configs
#   pwsh -File bench/size_gate.ps1 t-display-p4 t-display-p4-lr2021
#   pwsh -File bench/size_gate.ps1 -Record bench/build-size.csv -Label step1
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]] $Config,
    [string] $Record,
    [string] $Against,
    [string] $Label = ''
)

$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $bench
$cfg   = Get-Content (Join-Path $bench 'configs.json') -Raw | ConvertFrom-Json

function Get-Ota0Size {
    $csv = Join-Path $root 'boards/partitions_16m_ota.csv'
    foreach ($line in Get-Content $csv) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $f = $t.Split(',') | ForEach-Object { $_.Trim() }
        if ($f[0] -eq 'ota_0') {
            $s = $f[4]
            if ($s -match '^0x') { return [Convert]::ToInt64($s.Substring(2), 16) }
            if ($s -match '^(\d+)([KM])$') { return [int64]$Matches[1] * ($Matches[2] -eq 'K' ? 1KB : 1MB) }
            return [int64]$s
        }
    }
    throw "no ota_0 row in $csv"
}

$limit = Get-Ota0Size
$configs = if ($Config) { $cfg.configs | Where-Object { $Config -contains $_.name } }
           else { $cfg.configs | Where-Object { Test-Path (Join-Path $root "$($_.dir)/lakeshark.bin") } }

$prev = @{}
if ($Against -and (Test-Path $Against)) {
    foreach ($row in Import-Csv $Against) { $prev[$row.config] = [int64]$row.bytes }
}

$bad = 0
foreach ($c in $configs) {
    $bin = Join-Path $root "$($c.dir)/lakeshark.bin"
    if (-not (Test-Path $bin)) { Write-Host "  SKIP  $($c.name): no $($c.dir)/lakeshark.bin" -ForegroundColor Yellow; continue }
    $n = (Get-Item $bin).Length
    $free = $limit - $n
    $line = '{0,-22} {1,9} bytes  0x{2:x6}  free {3,7}' -f $c.name, $n, $n, $free
    $ok = $free -ge 0
    if ($prev.ContainsKey($c.name)) {
        $d = $n - $prev[$c.name]
        $line += '  delta {0:+#;-#;0}' -f $d
        if ($d -gt 0) { $ok = $false; $line += ' (grew)' }
    }
    Write-Host ("  {0}  {1}" -f ($ok ? 'PASS' : 'FAIL'), $line) -ForegroundColor ($ok ? 'Green' : 'Red')
    if (-not $ok) { $bad++ }
    if ($Record) {
        $commit = (& git -C $root rev-parse --short HEAD 2>$null)
        $newFile = -not (Test-Path $Record)
        if ($newFile) { 'config,bytes,limit,commit,label' | Set-Content $Record }
        "$($c.name),$n,$limit,$commit,$Label" | Add-Content $Record
    }
}
exit ($bad ? 1 : 0)
