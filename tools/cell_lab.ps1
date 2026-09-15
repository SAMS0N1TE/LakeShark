[CmdletBinding()]
param(
    [ValidateSet('doctor','prepare','verify','build','archive')][string]$Action='doctor',
    [string]$Port,
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    switch($Action) {
        prepare {
            $sdk=if($env:IDF_PATH){$env:IDF_PATH}else{'C:/esp/v5.4.3/esp-idf'}
            & pwsh -NoProfile -File bench/patch-idf.ps1 -IdfPath $sdk -Apply
            if($LASTEXITCODE){throw 'SDK preparation failed'}
        }
        doctor {
            git status --short --branch
            python -c 'import sys,numpy,serial; print(sys.executable); print("numpy",numpy.__version__,"pyserial",serial.__version__)'
            if($LASTEXITCODE){throw 'Python needs numpy and pyserial'}
            gcc --version | Select-Object -First 1
            python -m serial.tools.list_ports -v
            Write-Host "Firmware SDK: C:/esp/v5.4.3/esp-idf (override with IDF_PATH)"
        }
        verify {
            python integrations/cell-recordings/replay.py
            if($LASTEXITCODE){throw 'Recorded IQ regression failed'}
            python tools/test_cell_archive.py
            if($LASTEXITCODE){throw 'Archive integrity tests failed'}
            & pwsh -NoProfile -File bench/verify.ps1 -Level host
            if($LASTEXITCODE){throw 'Host tests failed'}
        }
        build {
            & pwsh -NoProfile -File bench/build.ps1 t-display-p4
            if($LASTEXITCODE){throw 'Firmware build failed'}
        }
        archive {
            if(!$Port -or !$OutputDirectory){throw 'Use -Port and -OutputDirectory; inspect doctor to identify P4'}
            python tools/cell_archive.py --port $Port --out $OutputDirectory
            if($LASTEXITCODE){throw 'Archive failed; originals stay on SD'}
        }
    }
} finally { Pop-Location }
