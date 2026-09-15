[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$IdfPath,[switch]$Apply)
$ErrorActionPreference='Stop'
$file=Join-Path $IdfPath 'components/esp_system/port/soc/esp32p4/system_internal.c'
$source=Get-Content -LiteralPath $file -Raw
$resets=@('HP_SYS_CLKRST_REG_RST_EN_AHB_PDMA','LP_CLKRST_RST_EN_SDMMC','HP_SYS_CLKRST_REG_RST_EN_H264')
$missing=@($resets | Where-Object { $source -notmatch [regex]::Escape($_) })
if(!$missing.Count){Write-Host 'P4 restart DMA reset fix present';exit 0}
if(!$Apply){throw 'P4 SDK lacks the DMA restart fix. Run tools/cell_lab.ps1 prepare before building.'}
$patch=Join-Path $PSScriptRoot 'patches/esp-idf-v5.4.3-p4-dma-reset.patch'
& git -C $IdfPath apply --check $patch
if($LASTEXITCODE){throw 'SDK does not match the reviewed backport; no SDK files changed'}
& git -C $IdfPath apply $patch
if($LASTEXITCODE){throw 'SDK patch failed'}
Write-Host 'Applied Espressif 40dd5e3 P4 restart DMA reset backport to SDK'
