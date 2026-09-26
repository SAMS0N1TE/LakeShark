[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$IdfPath,[switch]$Apply)
$ErrorActionPreference='Stop'

# Each SDK patch is recognised by text it adds, checked on every build and
# applied only with -Apply, all-or-nothing through git apply --check.
$patches=@(
    @{ File='components/esp_system/port/soc/esp32p4/system_internal.c'
       Markers=@('HP_SYS_CLKRST_REG_RST_EN_AHB_PDMA','LP_CLKRST_RST_EN_SDMMC','HP_SYS_CLKRST_REG_RST_EN_H264')
       Patch='esp-idf-v5.4.3-p4-dma-reset.patch'
       Name='Espressif 40dd5e3 P4 restart DMA reset backport' },
    # A physical RTL-SDR unplug can deliver a second root-port event after the
    # device is freed; stock hub.c then aborts on ESP_ERR_NOT_FOUND.
    @{ File='components/usb/hub.c'
       Markers=@('after its device was freed - ignored')
       Patch='esp-idf-v5.4.3-usb-hub-freed-node.patch'
       Name='USB hub freed-device root port event fix' },
    # A bulk or control transfer whose DMA descriptor comes back failed
    # (packet or buffer error, seen streaming an RTL-SDR) asserts in the USB
    # interrupt and panics the board; this fails that one transfer instead.
    @{ File='components/usb/hcd_dwc.c'
       Markers=@('hcd_dwc_desc_errors++')
       Patch='esp-idf-v5.4.3-usb-hcd-desc-error.patch'
       Name='USB host failed-descriptor transfer error' }
)

foreach($p in $patches){
    $source=Get-Content -LiteralPath (Join-Path $IdfPath $p.File) -Raw
    $missing=@($p.Markers | Where-Object { $source -notmatch [regex]::Escape($_) })
    if(!$missing.Count){Write-Host "$($p.Name) present";continue}
    if(!$Apply){throw "SDK lacks the $($p.Name). Run tools/cell_lab.ps1 prepare before building."}
    $patch=Join-Path $PSScriptRoot "patches/$($p.Patch)"
    & git -C $IdfPath apply --check $patch
    if($LASTEXITCODE){throw "SDK does not match the reviewed $($p.Patch); no SDK files changed"}
    & git -C $IdfPath apply $patch
    if($LASTEXITCODE){throw "SDK patch $($p.Patch) failed"}
    Write-Host "Applied $($p.Name) to SDK"
}
