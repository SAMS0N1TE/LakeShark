
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)] [string] $Mode,
    [Parameter(Position = 1)] [string] $Command = '',
    [string] $Board = 't-display-p4',
    [double] $Wait = 3,
    [int]    $n = 60
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py   = Join-Path $here 'lsconsole\lsconsole.py'

$argv = @($py, $Mode)
if ($Command) { $argv += $Command }
$argv += @('--board', $Board, '--wait', $Wait, '-n', $n)

python @argv
exit $LASTEXITCODE
