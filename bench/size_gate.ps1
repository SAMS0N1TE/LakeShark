
# App size gate: every built lakeshark.bin must fit the ota_0 slot of its own
# partition table (from the config's sdkconfig), because an update over Wi-Fi
# writes the other slot of the same size; a table with no OTA slots (the Nano
# DSI builds' 9M factory app) is checked against its factory slot. With -Record the sizes are appended to a ledger so a
# series of commits can be compared (bin <= the previous entry for the same
# config); -Against <ledger> fails when a config grew since its last entry.
#
#   pwsh -File bench/size_gate.ps1                         # all built configs
#   pwsh -File bench/size_gate.ps1 t-display-p4 t-display-p4-lr2021
#   pwsh -File bench/size_gate.ps1 -Record bench/build-size.csv -Label step1
#   pwsh -File bench/size_gate.ps1 -Ram                    # + internal-RAM statics vs budget/baseline
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]] $Config,
    [string] $Record,
    [string] $Against,
    [string] $Label = '',
    [switch] $Ram    # also run the internal-RAM gate (bench/ramgate.py) on each built config; failures count
)

$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $bench
$cfg   = Get-Content (Join-Path $bench 'configs.json') -Raw | ConvertFrom-Json

function ConvertTo-Bytes([string]$s) {
    if ($s -match '^0x') { return [Convert]::ToInt64($s.Substring(2), 16) }
    if ($s -match '^(\d+)([KM])$') { return [int64]$Matches[1] * ($Matches[2] -eq 'K' ? 1KB : 1MB) }
    return [int64]$s
}

# The app slot a config's image must fit: ota_0 of its own partition table (an update over Wi-Fi
# writes the other slot of the same size), or its factory slot when the table has no OTA slots.
# The table comes from the config's built sdkconfig; boards/partitions_16m_ota.csv when unknown.
function Get-AppSlot($c) {
    $csv = Join-Path $root 'boards/partitions_16m_ota.csv'
    $sdk = Join-Path $root "$($c.dir)/sdkconfig"
    if (Test-Path $sdk) {
        $m = Select-String -Path $sdk -Pattern '^CONFIG_PARTITION_TABLE_FILENAME="(.+)"' | Select-Object -First 1
        if ($m) { $p = Join-Path $root $m.Matches[0].Groups[1].Value; if (Test-Path $p) { $csv = $p } }
    }
    $ota0 = $null; $factory = $null
    foreach ($line in Get-Content $csv) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $f = $t.Split(',') | ForEach-Object { $_.Trim() }
        if ($f.Count -lt 5 -or $f[1] -ne 'app') { continue }
        if ($f[2] -eq 'ota_0') { $ota0 = ConvertTo-Bytes $f[4] }
        elseif ($f[2] -eq 'factory') { $factory = ConvertTo-Bytes $f[4] }
    }
    if ($ota0) { return @($ota0, (Split-Path $csv -Leaf)) }
    if ($factory) { return @($factory, (Split-Path $csv -Leaf)) }
    throw "no ota_0 or factory app row in $csv"
}
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
    $limit, $table = Get-AppSlot $c
    $free = $limit - $n
    $line = '{0,-22} {1,9} bytes  0x{2:x6}  free {3,7}  ({4})' -f $c.name, $n, $n, $free, $table
    $ok = $free -ge 0
    if ($prev.ContainsKey($c.name)) {
        $d = $n - $prev[$c.name]
        $line += '  delta {0:+#;-#;0}' -f $d
        if ($d -gt 0) { $ok = $false; $line += ' (grew)' }
    }
    Write-Host ("  {0}  {1}" -f ($ok ? 'PASS' : 'FAIL'), $line) -ForegroundColor ($ok ? 'Green' : 'Red')
    if (-not $ok) { $bad++ }
    if ($Ram -and (Test-Path (Join-Path $bench 'ramgate.py'))) {
        & python (Join-Path $bench 'ramgate.py') --config $c.name --top 5
        if ($LASTEXITCODE) { $bad++ }
    }
    if ($Record) {
        $commit = (& git -C $root rev-parse --short HEAD 2>$null)
        $newFile = -not (Test-Path $Record)
        if ($newFile) { 'config,bytes,limit,commit,label' | Set-Content $Record }
        "$($c.name),$n,$limit,$commit,$Label" | Add-Content $Record
    }
}
exit ($bad ? 1 : 0)
