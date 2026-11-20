# MESHLING checks

Run from the repository root. The host test includes the app with its manifest
disabled and supplies radio/file mocks; no firmware files are changed.

```powershell
& 'C:/Program Files/JetBrains/CLion 2026.1/bin/mingw/bin/gcc.exe' -O2 -Wall -Wextra -Wno-unused-function -ffunction-sections -fdata-sections -Iapp-sdk/include app-sdk/apps/meshling/tests/test_meshling.c '-Wl,--gc-sections' -o bench/build/meshling-out/test_meshling.exe
& bench/build/meshling-out/test_meshling.exe
python app-sdk/lap.py build app-sdk/apps/meshling -o meshling.lap
```

Checks cover the exact 2000-byte v1 layout and 2016-byte v2 round trip,
invalid saves, diet thresholds, health recovery margins, permanent DEX history,
XP overflow, diet rescaling, actual radio accounting and deduplication, timed
Bluetooth company, and guarded rendering of every form and mood.

Build `lssim` as described in the repository's bench instructions, then run
`app-sdk/apps/meshling/tests/preview.ps1`. This writes one PNG sheet per form,
an overview, and a transformation strip to `bench/build/meshling-out`. Each form sheet
shows day/night at different animation times and happy/hungry/sleep/chomp crops.
EGG has no face until it hatches. Captures use the real terminal renderer;
System.Drawing only crops and arranges the screenshots.

From `bench/build`, use `LAPSIM=meshling` and a `LAPSIM_SCRIPT` such as
`0:pet=relay;100:pet=swarm;200:pet=scanner;300:pet=dex` with `-A 40 -f 310`.
The `pet=` verb is host-only and seeds actual diet/health inputs. Other values
are `egg`, `hatchling`, `sprout`, `nexus`, `drifter`, `relic`, `happy`, `hungry`,
`sleep`, `chomp`, `room`, and `save`. `save` is explicit; previews otherwise
do not replace the simulator's stored pet. The hooks do not exist in the LAP.

For timing, unset `LSSIM_FRAMES`, set `LAPSIM_PROFILE=1`, and run
`lssim.exe lap -A 40 -f 120 -T 1000`. The shim prints app-draw timings in
100-frame batches using the host's high-resolution clock; `-T` also reports
the complete host frame including presentation. Neither is an ESP32-P4 timing.

Adult forms start at 1200 XP. A radio with at least 60% of diet points selects
RELAY (mesh), SWARM (Bluetooth), or SCANNER (Wi-Fi); otherwise NEXUS. Points:
one per awake mesh packet, 25 per new mesh node, 10 per new Bluetooth device,
25 per new Wi-Fi SSID. Awake Bluetooth company adds 10 per nearby device per
minute, capped at three. Large meal counters are bounded and proportionally
rescaled. New discoveries retain the original XP rewards; company also grants
two XP per device per minute. Pings, strokes and passive egg growth add no diet.

Below 20 food or joy, adults become DRIFTER; both must recover to 30 to leave
it. Below 1 in both, adults become RELIC; it persists while both remain below
12, then recovers through DRIFTER if still depleted. Sleep never triggers a
health branch. DEX discovery bits survive all branch changes and restarts.
