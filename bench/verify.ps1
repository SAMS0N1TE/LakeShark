
[CmdletBinding()]
param(
    [ValidateSet('host', 'smoke', 'full')] [string] $Level = 'host',
    [switch] $Sanitize,
    [switch] $Quiet,
    [string] $Filter
)

$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $bench
$cfg   = Get-Content (Join-Path $bench 'configs.json') -Raw | ConvertFrom-Json

$script:failures = @()

function Say([string] $m, [string] $colour = 'Gray') {
    if (-not $Quiet) { Write-Host $m -ForegroundColor $colour }
}

function Step([string] $name) { Say "==> $name" 'Cyan' }

function Invoke-HostBench {
    Step 'host bench'

    $buildDir = Join-Path $bench ($Sanitize ? 'build-asan' : 'build')
    $sanFlag  = $Sanitize ? 'ON' : 'OFF'


    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if (Test-Path $cache) {
        $cc = Select-String -Path $cache -Pattern '^CMAKE_C_COMPILER:.*=(.*)$' |
              ForEach-Object { $_.Matches[0].Groups[1].Value }
        if ($cc -and ($cc -match 'Espressif|esp-clang|riscv32-esp|xtensa-esp')) {
            Write-Host "  host bench: cached compiler is a cross-compiler" -ForegroundColor Yellow
            Write-Host "    $cc" -ForegroundColor Yellow
            Write-Host "    discarding $buildDir and reconfiguring for the host" -ForegroundColor Yellow
            Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
        }
    }

    $conf = & cmake -G Ninja -S $bench -B $buildDir "-DLS_SANITIZE=$sanFlag" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $script:failures += "cmake configure failed`n$($conf -join "`n")"
        return
    }

    $build = & cmake --build $buildDir 2>&1
    if ($LASTEXITCODE -ne 0) {
        $script:failures += "host bench failed to build`n$($build -join "`n")"
        return
    }



    $warns = $build | Select-String -Pattern 'warning:' -SimpleMatch
    if ($warns) {
        Say "    $($warns.Count) compiler warning(s):" 'Yellow'
        $warns | Select-Object -First 12 | ForEach-Object { Say "      $_" 'Yellow' }
    }














    $registered = @()
    $ctest = & ctest --test-dir $buildDir -N 2>&1
    if ($LASTEXITCODE -eq 0) {
        foreach ($l in $ctest) {
            if ("$l" -match '^\s*Test\s+#\d+:\s+(\S+)\s*$') { $registered += $Matches[1] }
        }
    }

    $exes = @(Get-ChildItem $buildDir -Filter 'test_*.exe' -File -ErrorAction SilentlyContinue)
    if ($registered.Count -gt 0) {
        $orphans = @($exes | Where-Object { $registered -notcontains $_.BaseName })
        foreach ($o in $orphans) {
            Say "    stale  $($o.BaseName)  - not in the current graph, removing" 'Yellow'
            Remove-Item $o.FullName -Force -ErrorAction SilentlyContinue
        }
        $exes = @($exes | Where-Object { $registered -contains $_.BaseName })



        $missing = @($registered | Where-Object { $n = $_; -not ($exes | Where-Object { $_.BaseName -eq $n }) })
        if ($missing) {
            $script:failures += "registered but not built: $($missing -join ', ')"
            Say "    MISSING  $($missing -join ', ')" 'Red'
        }
    } else {
        Say '    ctest inventory unavailable - falling back to directory scan' 'Yellow'
    }

    if (-not $exes -or $exes.Count -eq 0) { $script:failures += 'host bench produced no test binaries'; return }

    foreach ($exe in $exes) {
        $args = @()
        if ($Filter) { $args += $Filter }
        $out = & $exe.FullName @args 2>&1
        $rc  = $LASTEXITCODE

        $tail = ($out | Select-Object -Last 1)
        if ($rc -eq 0) {
            Say "    PASS  $($exe.BaseName)  -  $tail" 'Green'
        } elseif ($rc -eq 3) {
            Say "    skip  $($exe.BaseName)  (no case matched -Filter)" 'DarkGray'
        } else {
            Say "    FAIL  $($exe.BaseName)" 'Red'
            $out | ForEach-Object { Say "      $_" 'Red' }
            $script:failures += "$($exe.BaseName) failed`n$($out -join "`n")"
        }
    }

    $runnerTest = Join-Path $bench 'tests/test_runner_control.ps1'
    if (Test-Path $runnerTest) {
    $runnerOut = & pwsh -NoProfile -File $runnerTest 2>&1
    if ($LASTEXITCODE -eq 0) {
        Say "    PASS  test_runner_control  -  $($runnerOut | Select-Object -Last 1)" 'Green'
    } else {
        Say '    FAIL  test_runner_control' 'Red'
        $runnerOut | ForEach-Object { Say "      $_" 'Red' }
        $script:failures += "test_runner_control failed`n$($runnerOut -join "`n")"
    }
    }
}

function Invoke-HeaderCxxCheck {
    Step 'headers as C++'

    $gxx = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $gxx) { Say '    skipped (no g++)' 'DarkGray'; return }

    $incs = @(
        "$root/components/lakeshark/apps/p25"
        "$root/components/lakeshark/apps/fm"
        "$root/components/lakeshark/apps/rec"
        "$root/components/lakeshark/apps/adsb"
        "$root/components/lakeshark/apps/acars"
        "$root/components/lakeshark/board"
        "$root/components/lakeshark/core"
        "$root/components/lakeshark/radio"
        "$root/components/lakeshark/dsp"
        "$root/components/mbelib"
        "$bench/shims"
    ) | Where-Object { Test-Path $_ } | ForEach-Object { "-I$_" }






    $wanted = @{}
    Get-ChildItem "$root/components/apps", "$root/main" -Include '*.cpp', '*.hpp' -Recurse -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            foreach ($m in [regex]::Matches((Get-Content $_.FullName -Raw), '#include\s+"([^"]+)"')) {
                $wanted[[IO.Path]::GetFileName($m.Groups[1].Value)] = $true
            }
        }

    $headers = Get-ChildItem "$root/components/lakeshark" -Filter '*.h' -Recurse -File |
               Where-Object { $wanted.ContainsKey($_.Name) -and $_.FullName -notlike '*board*variants*' }

    $tmp  = Join-Path $env:TEMP "ls_hdr_$([guid]::NewGuid().ToString('N')).cpp"
    $bad  = 0

    foreach ($h in $headers) {






        $tu = @("#include <stdint.h>", "#include <stdbool.h>", "#include <stddef.h>",
                "#include <stdarg.h>", "#include <string.h>", "#include <stdio.h>",
                ("#include " + [char]34 + $h.FullName.Replace([char]92,[char]47) + [char]34))
        Set-Content -Path $tmp -Value $tu -Encoding UTF8



        $out = & g++ -std=gnu++2b -Wno-narrowing -fsyntax-only -x c++ @incs $tmp 2>&1
        if ($LASTEXITCODE -ne 0) {



            $real = $out | Select-String -Pattern 'error:' |
                    Where-Object { $_ -notmatch 'No such file or directory' } |



                    Where-Object { $_ -notmatch 'error: #error' }
            if ($real) {
                $bad++
                Say "    FAIL  $($h.Name)" 'Red'
                $real | Select-Object -First 4 | ForEach-Object { Say "      $_" 'Red' }
                $script:failures += "header $($h.Name) does not compile as C++`n$($real -join "`n")"
            }
        }
    }
    Remove-Item $tmp -ErrorAction SilentlyContinue

    if ($bad -eq 0) { Say "    PASS  $($headers.Count) headers compile as C++" 'Green' }
}

function Invoke-Firmware([object[]] $targets) {






    $defaultExport = $cfg.idf_export
    if ($env:IDF_PATH) { $defaultExport = Join-Path $env:IDF_PATH 'export.ps1' }




    if ($env:MSYSTEM -or $env:MSYS2_PATH_TYPE) {
        $script:failures += "the firmware build cannot run from an MSys/Mingw shell - IDF's export refuses it. Run bench/verify.ps1 from PowerShell."
        Say '    cannot build firmware from an MSys shell - run this from PowerShell' 'Red'
        return
    }

    foreach ($c in $targets) {
        Step "firmware: $($c.name)"








        $export = $defaultExport
        if (-not $env:IDF_PATH -and $c.idf_export) { $export = $c.idf_export }
        if (-not $export -or -not (Test-Path $export)) {
            $script:failures += "$($c.name): no IDF export script at '$export'. Set IDF_PATH to your ESP-IDF checkout, or fix idf_export in bench/configs.json."
            Say "    no IDF export at $export - set IDF_PATH, or fix bench/configs.json" 'Red'
            continue
        }






        $cache = Join-Path $root "$($c.dir)/CMakeCache.txt"
        if (Test-Path $cache) {
            $stale = $null



            $home_ = (Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' |
                      Select-Object -First 1).Matches.Groups[1].Value
            if ($home_ -and ([IO.Path]::GetFullPath($home_).TrimEnd('')) -ne
                            ([IO.Path]::GetFullPath($root).TrimEnd(''))) {
                $stale = "configured for $home_"
            }





            if (-not $stale) {
                $pyLine = Select-String -Path $cache -Pattern '^PYTHON:\w+=(.*)$' | Select-Object -First 1
                if ($pyLine) {
                    $wasPy = $pyLine.Matches.Groups[1].Value









                    $idfVer = ''
                    if ($export -match 'esp-idf-v(\d+\.\d+)') { $idfVer = $Matches[1] }
                    elseif ($export -match '[\\/]v(\d+\.\d+)[\.\d]*[\\/]') { $idfVer = $Matches[1] }

                    if ($wasPy -and -not (Test-Path $wasPy)) {
                        $stale = "built with a python that is gone: $wasPy"
                    } elseif ($wasPy -and $idfVer -and $wasPy -notlike "*idf$idfVer" + "_*") {
                        $stale = "configured with $wasPy, but IDF is v$idfVer"
                    }
                }
            }

            if ($stale) {
                Say "    stale build dir ($stale) - removing" 'Yellow'
                Remove-Item (Join-Path $root $c.dir) -Recurse -Force -ErrorAction SilentlyContinue
            }
        }




        $script = @"
`$ErrorActionPreference = 'Stop'
# export.ps1 derives its virtualenv name from whichever python is first on
# PATH. A system Python 3.14 therefore sends it looking for an
# idf5.5_py3.14_env that was never created, and it fails with "virtual
# environment not found" while the real idf5.5_py3.11_env sits beside it.
# Put IDF's own interpreter in front before dotting it.
`$idfPy = Get-ChildItem 'C:/Espressif/tools/idf-python' -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
if (`$idfPy) { `$env:PATH = "`$(`$idfPy.FullName);`$env:PATH" }
. '$export' | Out-Null
Set-Location '$root'
# -D SDKCONFIG per build directory, or all five configs share the one
# sdkconfig at the project root and whichever built last wins for every
# board. SDKCONFIG_DEFAULTS only applies when a fresh sdkconfig is
# generated, so a shared one is never regenerated and never right.
#
# That is how build_nano came to be configured for 32 MB flash: the LCD
# built after it. Flashing the result to the 16 MB nano fails outright -
# "storage.bin will not fit" - and flashing it to a 32 MB board silently
# succeeds with the wrong app.
idf.py -B '$($c.dir)' -D SDKCONFIG='$($c.dir)/sdkconfig' -D SDKCONFIG_DEFAULTS='$($c.defaults)' -D IDF_TARGET='$($cfg.target)' build
exit `$LASTEXITCODE
"@
        $tmp = Join-Path $env:TEMP "ls_build_$($c.name)_$([guid]::NewGuid().ToString('N')).ps1"
        Set-Content -Path $tmp -Value $script -Encoding UTF8

        $out = & pwsh -NoProfile -ExecutionPolicy Bypass -File $tmp 2>&1
        $rc  = $LASTEXITCODE
        Remove-Item $tmp -ErrorAction SilentlyContinue

        if ($rc -eq 0) {
            $sz = $out | Select-String -Pattern 'bytes.*free|Project build complete' | Select-Object -Last 1
            Say "    PASS  $($c.name)" 'Green'
        } else {
            Say "    FAIL  $($c.name)" 'Red'




            $errs = $out |
                Select-String -Pattern 'error:|undefined reference|multiple definition|cannot find -l|region .* overflowed|does not fit' |
                Select-Object -Unique -First 25


            if (-not $errs) { $errs = $out | Select-Object -Last 20 }
            $errs | ForEach-Object { Say "      $_" 'Red' }
            $script:failures += "firmware $($c.name) failed to build`n$($errs -join "`n")"
        }
    }
}

function Invoke-BoardRule {
    Step 'board rule'

    foreach ($f in @('tools/check_board_identity.py', 'tools/test_check_board_identity.py')) {
        if (-not (Test-Path $f)) { $script:failures += "board rule: $f is missing"; continue }
        if (Test-Path '.git') {
            $tracked = & git ls-files --error-unmatch $f 2>$null
            if (-not $tracked) {
                $script:failures += "board rule: $f exists but is not tracked by git, so CI cannot see it. Check the .gitignore negation."
            }
        }
    }

    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $script:failures += 'board rule: python not on PATH'; return }

    $t = & python -m unittest discover -s tools -p test_check_board_identity.py 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $t -match 'NO TESTS RAN') {
        $script:failures += "board rule: checker tests did not pass`n$t"
    }

    $c = & python tools/check_board_identity.py 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { $script:failures += "board rule: board identity outside board/`n$c" }

    $hits = & git grep -n -E '#define[ \t]+LS_HAS_' -- components/lakeshark/board/variants 2>$null
    if ($hits) {
        $script:failures += "board rule: a variant defined an LS_HAS_* capability directly`n$($hits -join "`n")"
    }

    $hits = & git grep -n 'LS_BOARD_HAS_' -- 'components/*.c' 'components/*.h' 'components/*.cpp' 'main/*.c' 'main/*.h' 'main/*.cpp' 2>$null |
            Where-Object { $_ -notmatch '^components/lakeshark/board/' }
    if ($hits) {
        $script:failures += "board rule: LS_BOARD_HAS_* is the pre-ls_caps.h spelling, use LS_HAS_*`n$($hits -join "`n")"
    }
}

function Invoke-ConsoleBroker {
    Step 'console broker'

    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $script:failures += 'console broker: python not on PATH'; return }

    $t = & python -m unittest discover -s bench/lsconsole -p 'test_*.py' 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $t -match 'NO TESTS RAN') {
        $script:failures += "console broker: tests did not pass`n$t"
    }
}

function Invoke-PrivatePathGuard {
    $releaseTests = & python -m unittest discover -s tools -p 'test_check_public_source.py' 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { $script:failures += "public source guard tests`n$releaseTests" }
    Step 'module shadow'
    $py0 = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py0) { $script:failures += 'module shadow: python not on PATH'; return }
    $sh = & python tools/check_module_shadow.py 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { $script:failures += "module shadow`n$sh" }

    Step 'private paths'
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $script:failures += 'private paths: python not on PATH'; return }
    if (Test-Path '.git') {
        $out = & python tools/check_private_paths.py 2>&1 | Out-String
    } else {
        $out = & python tools/check_public_source.py 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) { $script:failures += "private paths`n$out" }
    else { $out -split "`n" | Where-Object { $_ -match 'note:|^\s+' } | ForEach-Object { Say "    $_" 'DarkYellow' } }
}

$started = Get-Date

Invoke-HostBench
Invoke-HeaderCxxCheck
Invoke-BoardRule
Invoke-PrivatePathGuard
Invoke-ConsoleBroker

if ($Level -eq 'smoke') {
    Invoke-Firmware @($cfg.configs | Where-Object { $_.name -eq $cfg.smoke })
} elseif ($Level -eq 'full') {
    Invoke-Firmware @($cfg.configs | Where-Object { $_.enabled })
}

$secs = [int]((Get-Date) - $started).TotalSeconds

if ($script:failures.Count -eq 0) {
    Say ""
    Say "VERIFY OK  (level=$Level, ${secs}s)" 'Green'
    exit 0
}

Say ""
Say "VERIFY FAILED  (level=$Level, ${secs}s)" 'Red'
foreach ($f in $script:failures) { Say "--- $f" 'Red' }
exit 1
