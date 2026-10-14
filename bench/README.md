# Tests and builds

Run from the repository root with PowerShell 7, Python 3, CMake and a C/C++ compiler available.

```powershell
pwsh -NoProfile -File bench/verify.ps1 -Level host
pwsh -NoProfile -File bench/verify.ps1 -Level smoke
pwsh -NoProfile -File bench/verify.ps1 -Level full
```

The host gate runs decoder and UI tests, C++ header checks and source guards. The smoke gate also builds T-Display-P4. The full gate builds every enabled configuration in `bench/configs.json`.

Use `-Filter pocsag` to select tests or `-Sanitize` for address and undefined-behavior checks with a supported compiler.

Firmware builds require ESP-IDF 5.5.4 with the patches in `bench/patches/` (`bench/patch-idf.ps1 -IdfPath <esp-idf> -Apply`). Set `IDF_PATH` to the local installation.

```powershell
pwsh -NoProfile -File bench/build.ps1 t-display-p4
pwsh -NoProfile -File bench/build.ps1 p4-nano
pwsh -NoProfile -File bench/build.ps1 p4-touch-lcd-43
```

Host tests do not verify RF reception or physical hardware.

Preview the HI8561 layout around the camera hole:

```powershell
bench/build/lssim.exe home --tft -o bench/build/tft-home.bmp
```

Use `-l` for clockwise landscape or `--ccw` for counter-clockwise landscape.
The BMP stays in native portrait coordinates. The hole is drawn black with a
grey rim. Omit `--tft` for AMOLED.

Run only the DETECTOR Phase 1 host tests (no firmware build or hardware access):

```powershell
cmake -G Ninja -S bench -B bench/build
cmake --build bench/build --target test_detector test_detector_runtime -j 4
ctest --test-dir bench/build -R '^test_detector(_json|_runtime)?$' --output-on-failure
```

These targets cover the parser, atomic reload, shared RID/SWEEP observations,
policy limits, layered options callbacks, quiet-banner delivery, JSONL privacy
and rotation, and shared receiver ownership. Test rotation uses 4 KiB files;
production uses 4 MiB, retaining eight files. The JSON test reads the writer
fixtures produced by `test_detector`. Files remain under `bench/build`.
DETECTOR options are session settings; explicit rule reload replaces rule edits.
Only Phase 1 sources, predicates and sinks are accepted, including in disabled
rules. Copy `data/detector/rules.json` to `/sdcard/detector/rules.json`; future
phase examples require their operators to be implemented before loading them.
