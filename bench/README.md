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
Only implemented sources, predicates and sinks are accepted, including in
disabled rules. Copy `data/detector/rules.json` to `/sdcard/detector/rules.json`;
future-phase examples require their operators to be implemented before loading.

### DETECTOR aircraft rules (phase 2)

The `adsb` source reads the aircraft table the ADS-B/UAT decoder already fills.
It claims no radio and starts nothing: with no ADS-B receiver running, the
screen shows "source off". An `adsb` rule lists `"sources":["adsb"]` alone and
at least one aircraft predicate, ANDed together (use separate rules for OR):

| key | meaning |
|---|---|
| `squawk_in` | up to 8 Mode A codes, `"7500"` style (digits 0-7); accepted after two agreeing reports |
| `emergency_status` | `true`: the aircraft-status emergency state (type code 28) is not "none" |
| `callsign_prefix_in` | up to 8 uppercase prefixes, 1-8 characters of A-Z0-9 |
| `icao_in`, `icao_ranges` | 6-digit hex addresses and `["lo","hi"]` ranges, 8 entries in all, ICAO-qualified addresses only |
| `adsb_category_in` | `light small large high_vortex heavy high_performance rotorcraft glider lighter_than_air parachutist ultralight uav space emergency_vehicle service_vehicle obstacle` |
| `distance_km_within` | 1-1000 km from a GPS fix newer than 3 s, with the aircraft position newer than 15 s |
| `altitude_baro_ft_below` | 0-60000, barometric altitude as the aircraft reports it (not above ground), newer than 15 s |

An empty list is accepted and matches nothing, so watch lists can ship empty.
Unknown fields never match; an aircraft not heard for 60 s never alerts, and a
squawk or emergency state last confirmed more than 60 s ago is ignored. A
changed squawk or emergency state is a new alert; the same one waits for the
rule's `cooldown_s`. Built in and in the sample file: emergency squawk and
emergency status on; range, altitude and category examples off; watch lists are in the sample file only (empty).
Aircraft events log the broadcast ICAO address, callsign, squawk, emergency
state, barometric altitude and category, never a position, range or RSSI.

```powershell
cmake --build bench/build --target test_adsb_status test_detector_air test_detector test_detector_runtime lssim -j 4
ctest --test-dir bench/build -R '^test_(adsb_status|detector.*)$' --output-on-failure
bench/build/lssim.exe detector --tft -k a -d   # LSSIM_DETECTOR=1 LSSIM_ADSB=1 seeds the table
```
