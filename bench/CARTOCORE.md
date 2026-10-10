# Local CartoCore P4 bench

Set `CARTOCORE_DIR` to your CartoCore checkout using the environment or the
CMake cache (`-DCARTOCORE_DIR=...`). A missing/invalid checkout fails configuration
with a clear error. This branch loads `${CARTOCORE_DIR}/components` only for the
T-Display-P4 profile. It does not change libcarto, the MAP app, partitions,
audio, or radio settings. No flashing or hardware execution was performed.

## Map and build

In `D:/assasdad/CartoCore`, with `C:/mingw64/bin` on PATH:

```powershell
cmake -G Ninja -S . -B build
cmake --build build
./build/cartocore.exe build --pbf data/new-hampshire-latest.osm.pbf --bbox '-71.672,43.432,-71.622,43.458' --zmin 13 --zmax 15 --profile fast --out data/franklin_mini.ctile
```

Fast produced 915,302 bytes, but the linked firmware exceeded the unchanged
4,718,592-byte OTA slot. Rebuilding with `--profile compact` produced 356,054
bytes. The final `data/franklin_mini.ctile` in both repositories is that compact
map, SHA256 `f848335dc4bd346ff46128f8ee930eee3b33151ab251c7b9795a27d6014a97a4`.
The firmware embeds it as flash rodata; it needs no SD card.

From this firmware worktree:

```powershell
$env:CARTOCORE_DIR="D:/assasdad/CartoCore" # example local checkout
. ./bench/idfenv.ps1 -Config t-display-p4
./bench/build.ps1 t-display-p4
```

The scripts support the installed IDF 5.5 Python 3.11 environment and invoke
its Python explicitly. This sandbox also needed a writable compiler cache:

```powershell
$env:CCACHE_DIR="$PWD/build_tdp4/ccache"
$env:CCACHE_TEMPDIR="$PWD/build_tdp4/ccache/tmp"
New-Item -ItemType Directory -Force $env:CCACHE_TEMPDIR | Out-Null
```

The ignored dependency lock was copied from the local IDF-5.5 checkout with
local paths adjusted, allowing the existing managed components to build offline.

## Console

```text
carto bench
carto bench 120 40 14 quadrant crisp 50
carto show 14
carto hide
```

Registration is beside `crumb` in `main/headless_main.c`, which the P4 headless
console compiles. `gui_link.cpp` is not involved.

With no arguments, `carto bench` runs 120x40 quadrant/crisp, 52x70
quadrant/crisp, and 120x40 braille/smooth at z14, 50 samples each. Each
configuration runs both PSRAM/internal arenas and decoded-only/cell-tile cache
paths, reporting cold-first-frame, warm-same-view, sequential one-cell-pan,
and centre-preserving zoom-change medians/p90
plus all seven stages. Decoded-only warm samples invalidate the renderer so
they measure decode/raster/encode work rather than a saved frame. Cell-tile
warm samples intentionally reuse the cached cells. Cold resets both caches.

The decoded cache has a 768 KiB PSRAM budget. Optional aligned cell-tile
rendering has a separate 4 MiB PSRAM budget (seven 512 KiB slots plus one
snapshot slot); failures fall back to decoded rendering and print a fallback
count. Both paths include labels. Origins are snapped to the cell cache's 2x4
alignment; `pan DX DY` moves in cells. Reports include arena capacity/used/peak,
decoded bytes/counters, cell counters, frame bytes and internal/DMA heap deltas.
The PSRAM arena is 2 MiB; internal trials use the largest block less 16 KiB.
Insufficient internal allocations are reported explicitly.

`show` retains its arena, decoded cache, cell cache and output in PSRAM.
`pan` and `zoom` re-render the selected view, publish converted cells to the UI
and print frame microseconds including conversion (excluding the later panel
flush). Zoom preserves the selected map centre. Any key, tap, `hide`, or regrid
dismisses immediately; the idle task releases retained state on its next turn. ANSI indices map directly to TUI colour constants
and TUI_BRIGHT; the active theme resolves them when drawing. There is no RGB
nearest-colour search. Audio and device execution are unchanged.

```text
carto bench
carto bench 120 40 14 quadrant crisp 50
carto bench 52 70 14 quadrant crisp 50
carto bench 120 40 14 braille smooth 50
carto show 14
carto pan 1 0
carto pan 0 -1
carto zoom 15
carto zoom 14
carto hide
```

## Verification

The compact firmware built into `build_tdp4/lakeshark.bin`: 4,497,920 bytes,
with 220,672 bytes remaining in each unchanged OTA slot. The link map contains
`ls_cartocore_register_command`, `cmd_carto`, and the embedded ctile symbols.
Rebuilding the base commit `719fda50` with the same SDK/config measured static
internal D/IRAM at 349,428 -> 349,452 bytes: **+24 bytes**, all in BSS
(181,784 -> 181,808). DRAM data remains 72,976 and IRAM text 94,668 bytes.
This excludes dynamic startup allocations such as the overlay mutex.
The comparison and preserved base/final maps are under
`build_tdp4/carto-audit/`. Final sources were restored and rebuilt afterward.

Eight desktop combinations passed 50 fresh samples each; z13 and z15 display
renders also succeeded. Existing TUI router and surface tests passed.
The real `bench/build/lssim.exe` ran portrait, landscape, RS41 tracks and touch
scenarios. `bench/tools/carto_record.c` uses the same firmware quadrant/palette
conversion header to create CartoCore records for the real TUI blitter:

```powershell
gcc -O2 -Wall -Wextra -I D:/assasdad/CartoCore/include -I components/apps/tui bench/tools/carto_record.c components/apps/tui/ls_theme.c D:/assasdad/CartoCore/build/libcartocore.a -o bench/build/carto_record.exe
./bench/build/carto_record.exe data/franklin_mini.ctile bench/build/carto-portrait.lsrec 52 70 0 14
./bench/build/lssim.exe play bench/build/carto-portrait.lsrec bench/build/carto-portrait.rgb
```

The CartoCore portrait/landscape images and MAP tracks/touch images were
visually inspected. They validate cell conversion and the TUI blitter;
console/task/heap behaviour and actual P4 timings still require the user's
hardware run. Audio was neither exercised nor changed.

## ANSI-16/cache round (2026-10-08)

Host CartoCore: all nine CTest targets pass, including the ANSI-16 SGR golden
and semantic cached/uncached equivalence. All ten core/output files compile
with ESP GCC 14.2.0_20260121 for RV32, `-Wall -Wextra -Werror`.
The P4 build and link-map symbol audit passed. Portrait and landscape records
were replayed through `bench/build/lssim.exe` and the resulting images inspected:
water blue, parks green, buildings grey, minor roads white, major roads yellow.
These checks do not establish device timings or physical panel behavior.
No flashing, hardware execution, Forge contact, delegation, or commits.

Final image: `build_tdp4/lakeshark.bin`, 4,517,392 bytes (`0x44ee10`),
201,200 bytes remaining in the unchanged OTA slot.

## Deferred first frames (2026-10-08)

Cell caches use `deferred=1`: a miss queues the latest view and immediately
falls back to `cc_render`. The low-priority `carto-idle` task fills one 128-cell
row every 10 ms, with separate 2 MiB PSRAM scratch. Draw and key/touch dismissal
never wait for that task; draw retains the displayed frame during lock contention.
Completed tiles retain the existing warm-pan correction/snapshot path. Tile
geometry decode is dataset-dependent: the cell budget bounds raster area, not
wall time. Total idle filling is deliberately spread over many turns.

`carto show`/`zoom` print `frame=... us cache_pending=1` for the first frame.
Completion prints `carto idle cache ready` and `carto later ... frame=... us`.
Times include conversion, exclude panel flush and initial allocation. Bench
warms tiles in untimed yielding steps before its warm/pan trials; zoom-change
clears cell tiles and alternates zoom around the same centre. Labels carry a
black background in RGB and ANSI-16; memory initialization accepts 4-byte bases.

Desktop proxies and tests are described in the external CartoCore project's
deferred-cache documentation; that document is not part of this checkout.
The existing host simulator excludes this ESP-only overlay: its portrait,
landscape, track and touch runs verify the surrounding TUI, while CartoCore
renders at z14/z15 were inspected using LakeShark's mono16 glyph bitmap.
No hardware execution, flashing, commits or pushes occurred in this change.

Receive sessions display a random 32-digit code on the console and TILES RECEIVE.
Use `python tools/carto_push.py FILE.ctile BOARD_IP --code CODE [--force]`,
or PUT with `?code=CODE` / `X-Carto-Code: CODE` (add `&f=1` to overwrite).
One upload per session, station-interface requests only, 10-second body idle
timeout, maximum 512 MiB and SD free space minus 1 MiB. Fetch has the same size
limits and a 15-minute overall deadline. Open embedded before replacing/deleting
SD maps. Publication uses `.bak` rollback and recovery on the next SD scan/open
or transfer after boot. SHA sidecars are invalidated and never trusted by size.
Both HTTP-server stack configurations use 8 KiB with heap hash scratch; console
high-water reports still need hardware measurement before reducing that size.

## Sweep 3 verification

`test_carto_sd.py`, `test_carto_recv.py`, `test_tiles.py`,
`test_tiles_state.py`, and `test_tui_png_console.py` pass. Fault injection covers
publication rollback/recovery, alias blocking, HTTP bounds/deadlines, receiver
authentication/interface/idle/stop retry, sender codes, catalog depth/token/input
bounds, same-size replacements, and mid-hash cancellation/expiry. TUI surface,
router and touch CTests pass. The real simulator's portrait, landscape, tracks
and touch renders pass; receive codes were visually inspected in both layouts.
Missing `CARTOCORE_DIR` fails configuration with the expected diagnostic.
The t-display-p4-lr2021 firmware builds. No flashing or hardware execution;
HTTP-server stack high-water sizing and actual FAT power-loss behavior still
need hardware measurement. No commits, public pushes, Forge or delegation.

## MAP integration (2026-10-09, host only)

MAP STYLE now retains five fills in the existing settings word: the three
libcarto fills, CartoCore smooth (quadrant/smooth) and CartoCore braille (smooth).
CartoCore uses the selected CTILE, falling back to embedded Franklin if its saved
SD file is missing. Source selection and view are a versioned NVS blob. Hiding
or leaving releases the source without changing that selection. An explicit
embedded selection clears it. Verified downloads blocked by an open SD source
remain `.ctile.pending` with verification metadata; close and boot recovery
publish them through the existing backup/rollback transaction.

The console and MAP share render_state, split arenas, decoded/cell caches,
streaming source and the low-priority idle task. MAP's projection uses a 256-dot
tile and 2x4 cells; pan, inverse touch projection, GPS, aircraft, mesh, marks,
notes, GPX and radiosonde overlays keep their existing geographic view. The
active CTILE bounds zoom, and the places layer controls CartoCore labels. MAP
releases its retained renderer on exit and frees unused libcarto pixel scratch
while borrowing only its geographic view.

Cells now have a signed 16-bit glyph field: legacy signed byte values retain
their meaning, while U+2800..U+28FF carry all braille patterns. Procedural dot
painting allocates no glyph table or buffer. The back/front/glass grids and
console conversion buffers already live in PSRAM and grow by two bytes per
cell. There is no new internal-RAM buffer proportional to viewport size.
Firmware/static-link size and actual dynamic P4 heap usage were not measured.

Configure the host bench with `-DCARTOCORE_DIR=C:/Users/DEV/CartoCore-wip`, build
`lssim`, and run `python bench/tools/test_cartomap.py`. `LSSIM_CTILE` can select
a terrain-enabled file; the default is the embedded mini fixture. Simulator
source/allocation adapters compile the same renderer and MAP entry points.
The production NVS, transfer and SD loader tests separately substitute only
their host persistence, filesystem and network/RTOS adapters.

Validation: 26 CartoCore CTests; all 295 LakeShark host test executables run from
the repository root; SD/fetch, receiver, catalog/TILES state, NVS record and
concurrent PNG tests. The dot-pixel regression checks all 256 braille patterns
in both orientations and unchanged-frame detection. Configure-contract tests
reject missing and incompatible headers without invoking IDF.

The inspected PNGs in `bench/out/cartomap/` show blue water, green land cover,
grey buildings, white/yellow roads, faint grey terrain texture, and distinct
braille dots. Aircraft/GPS labels and rings remain legible; tracks and touch
frames show the route and shifted map. Some CartoCore place text can be covered
by overlaid markers, as the overlays paint after the base map. Portrait and
landscape renders include both fills, pan, zoom, touch and GPX/RS41 fixtures.

`verify.ps1 -Level host` remains blocked by existing board-identity rules and
regression-tooling errors (speech-level wiring and historical missing document
references). Its release incident list is also unresolved. The large Central
NH fixture was absent; SD streaming used real terrain-enabled Franklin compact.
No firmware build, hardware/serial execution, flashing, audio exercise or remote
git operation was performed. Physical panel output, SD/FAT power-loss behavior,
NVS persistence on the P4, actual audio and device timings remain unverified.

Label contrast (2026-10-09): semantic label classes use bright white/cyan/yellow
on full-cell index-0 plates. The shared board conversion also promotes legacy
text ink (including black class ink) in both console and MAP, without changing
quadrant/braille shape colours or allocating buffers. Terminal Bay label pairs
measure above 7:1; Daylight uses its inverse primary-ink/ground palette.
Truecolor label colours and black plates are retained.

Host regression: set CARTOCORE_DIR and LSSIM_CTILE to a terrain-enabled .ctile,
then run `python bench/tools/test_carto_labels.py` after building lssim. It checks
all six legacy class colours, Unicode fallback, shape preservation and the actual
P4 theme palette, and writes eight `bench/out/cartomap/labels-*.png` views at
z13/z14 in both orientations. All eight were visually inspected: bright ink on
black plates over water, parks, terrain and roads; existing rings/GPS/air overlays
can still occlude letters. No physical board or firmware build was exercised.

Hardware follow-up (1353f0c2): the MAP header now includes sdkconfig before
checking the board define. Otherwise a first include silently compiled host-like
unavailable stubs on the P4. The relative scr_map.c include is retained.

Flash-call audit: carto console workers, TILES work, MAP enter/leave/draw, and
carto-idle no longer call raw NVS APIs. Active-map load is explicit during
headless boot, after ls_nvs_init and before those tasks. Save producers only
update the record under view_lock and mark it dirty. Idle snapshots the latest
record, drops view_lock, and uses ls_nvs_call; failed writes remain dirty and
retry after five seconds. The existing 3072-byte DRAM-checked dispatcher is
reused: no new internal worker stack. The only cartocore NVS sites are the boot
load and save callbacks invoked through that dispatcher.

MAP style reads now use a boot-loaded RAM word, including palette_load on
entry; writes use the settings queue. The existing settings writer's stack/TCB
are explicitly DRAM and its creator checks the full stack range, excluding P4
TCM. Home/location reads are already RAM-only. Carto/TILES WiFi calls only read
station state/IP; they do not start WiFi or load credentials. Downloads, digest
sidecars, pending publication/recovery, marks and notes access SD FAT paths.
No esp_partition, SPI-flash write/erase, or SPIFFS call is reachable from those
map operations; embedded maps and TLS certificates are ordinary mapped reads.

CartoCore fills (including preview) close PMTiles and release all its buffers.
Without a saved selection, valid SD headers are ranked by covering the requested
centre first, then file size, then name; otherwise the embedded mini is used.
Initial out-of-bounds centres move to the selected map's bbox midpoint and zoom
is clamped to its range. Subsequent pans remain free. MAPS lists .ctile files in
these fills. Creation/render failures report a named step, requested arena and
cell sizes, and free internal/PSRAM bytes, at most once per five seconds. The
placeholder names .ctile and zoom/across remains visible on failure.

Host checks: test_carto_saved.py uses an NVS shim which asserts on every raw call
outside the allowed worker; test_carto_sd.py applies it to the production SD
open/save path. test_carto_startup.py tests the firmware include order, covering
versus largest selection, fresh selection outside mini, zoom clamp, failed
internal allocation with PSRAM fallback, failed arenas and rate-limited logging.
It produces startup-*.png under bench/out/cartomap. Inspected successful SD,
embedded and fallback renders plus failure placeholders: both looks retain
terrain/roads/overlays and the header; the failure plate says .ctile.
Physical LR2021 cache-off behavior, firmware linking and SD timings remain
unverified; no firmware build, flashing or device access was performed.

Final host run: all 295 bench test executables passed from the repository root,
including 27 map cases, 5 layer/style cases and 7 DRAM-worker cases. SD/fetch,
strict NVS/task and startup regressions passed. The existing real-MAP simulator
pan/zoom/touch/tracks/leave matrix passed in both fills and orientations. All
eight startup PNGs were inspected; no new CartoCore engine changes were needed.

### Asynchronous MAP boundary (LR2021 stack fault follow-up)

MAP's CartoCore prepare/draw/limits/select/leave functions now exchange RAM-only
requests and published cells. A missing source produces `opening map...` on the
first frame. The existing carto-idle worker selects/restores/recentres sources,
scans the cached MAPS list, renders, prefetches and closes sources and recovers pending swaps on leave.
It serializes source transitions with console jobs. TUI lock attempts have zero
timeout; limits never call open_map, restore_map or recovery. Cancelled opens
are released by the worker, including cancellation before renderer allocation.

The existing 16 KiB PSRAM idle stack is reused, with no new internal task stack.
Flash calls remain exclusively dispatched to the existing DRAM NVS worker.
The published PSRAM cell frame is shared with the console overlay. Recovery and
publication path workspaces are static PSRAM under file_lock; other path buffers
are bounded to short MAP_DIR names. The new MAP file catalogue uses 4 KiB PSRAM.
Markers also defer load/save to idle using three approximately 9 KiB PSRAM
snapshots; the UI adopts completed loads and enqueues immutable saves. Live
marker state remains owned by the UI, and the latest queued save wins.

The simulator now compiles ls_cartocore.c and its real streaming SD loader,
instead of a separate whole-file host renderer. Its platform shim rejects file
operations on the TUI context and raw NVS outside the dispatcher. Host builds
emit GCC stack-usage and call-graph files; test_carto_stack.py is a mandatory
lssim post-link gate for nine TUI roots. Maximum measured entry/helper frame is
192 bytes (limit 512); TUI geometry/frame primitives are included in the measured graph; allocator/
libc/atomic leaves are explicitly reviewed. This is a host bound, not a RISC-V
stack high-water measurement. Six negative file/NVS probes verify the shim.

Verification: 296 host test executables passed, including deferred marker
snapshot/load/save checks. CartoCore's 26 tests passed. SD/fetch, receiver, NVS,
startup, cancellation/busy-lock, allocation failure, labels and MAP pan/zoom/
touch/tracks/leave scenarios passed. Inspected opening, startup, both fills and
label PNGs under bench/out/cartomap: opening is brief text, ready maps retain
roads/water/parks, bright plated labels and aircraft/mesh/GPS/mark overlays.
The host verify wrapper still fails existing board-identity lint and three
quality cases (speech-level source matching and missing/ignored historical
references). Firmware compilation, hardware stack watermark and real scheduling
remain unverified. No firmware build, device access or remote operation was run.

### CartoCore label/overlay composition

MAP now receives clean terrain plus whole CartoCore label runs. The worker
collects names, then uses the same retained renderer/caches for the label-free
terrain pass. MAP reserves high-priority marker glyphs and their text first,
then places whole names at their original row or up to two rows away. Solid
TUI cells (including spaces) supply the same bright-ink/dark-plate attributes
as the console view. Low-priority ink lies below these plates; ring scale text
and the centre reticle consult the newly reserved name spans. A name with no
clear placement is omitted whole, never punched into fragments.

Two fixed 64-entry label snapshots add 16.75 KiB PSRAM; only counters use internal
RAM. No TUI file operations or new task stack. The label-copy API joins the
mandatory stack/call-graph guard (10 roots, 192-byte maximum host frame).

The overlay regression renders 43.45,-71.65 at z12 and z13, both fills and
orientations, with rings, live markers, and a separate HOME/GPS-wait case.
All 16 overlay-labels-*.png images were inspected: complete river/place names,
solid plates, visible rings/HOME/GPS and no label holes. The final-cell audit
checks every placed character, including spaces and plate attributes, after
MAP's full draw pass; existing reservations check marker/label overlaps. The
fixture also verifies actual low-priority dot cells are masked by labels.
All 296 host executables, startup/picker, MAP interaction and stack checks pass.
Board appearance and timing of the extra cached terrain pass remain unverified.

### Complete frame publication and thin strokes

The worker converts a successful render into the inactive PSRAM frame, then
swaps the frame index under a short atomic guard. The TUI never takes the
render mutex to read pixels. While a requested view is pending or fails, MAP
copies its previous complete, composited body and postpones overlay redraw;
console show reads the previous published frame. Source opening is the only
initial placeholder. Resizing can resample the last complete frame. Idle cache
fills and scratch buffers are never published independently of render success.

Two reusable cell buffers replace the old single view (one additional
`cols * rows * sizeof(tui_cell)` allocation, at most 14.3 KiB for a 52x70 grid).
Their two label snapshots plus a TUI label snapshot add 25.125 KiB PSRAM.
Only indices, atomic flags and small UI metadata use internal RAM; no new task
or stack. Worker cleanup frees both frame allocations. The host boundary gate
covers 11 TUI roots, at most 208 bytes per function, and rejects file/flash I/O.

Both engines use 256 * 2^z world coordinates and a 2x4 sample cell. MAP's
SUB_X/sub_y projection is converted through the viewport ratio; it does not
multiply stroke widths. Engine lines already use unit-width Bresenham. The
engine now selects a single source row when reducing a diagonal to quadrants,
and prevents lower-priority line classes borrowing the winning class's colour.
Braille stays one dot wide; polygon water/park coverage retains its area edges.

Run `python bench/tools/test_carto_frames.py` with the host CartoCore build.
It captures every presented frame of z11->16->11 and pan in both fills and
orientations under `bench/out/cartomap/frames-*/` (generated, not versioned).
It delays the worker, poisons one failed render, requires exact retained body
cells while pending, and compares every worker publication to a separate
cache-free full render. Console SD and embedded deferred-cache paths also run
with poisoned scratch and busy render/publication locks. Before/after z12/14/16
images, contact sheets and truecolor desktop renders are `lines-*.png`.
Inspected images show thinner diagonals, complete dark label plates, preserved
water areas and coherent retained frames. Desktop PNGs rasterize actual CLI
HTML cells; terminal font-specific appearance and LR2021 timing need hardware
confirmation. No firmware build or hardware execution was performed.

Validation for this change: 296 host executables passed (two simulator launch
errors during an overlapping relink passed on rerun); the stack/IO gate, SD,
NVS, startup, MAP interaction and label/overlay regressions passed. All 26
CartoCore CTest cases passed, including the added stroke assertions. Full
`verify.ps1 -Level host` still reports existing board-identity violations,
speech wiring/reference tooling failures and unresolved release incidents.
These are separate from the passing executable suites; hardware is unverified.

### Yellow map labels

CartoCore cell labels now use bright yellow on solid black in normal themes;
Daylight retains the previous reversed class ink. `map_collect_labels()` copies
`carto_attr(*c)` into each published `ls_carto_label.attr`, and MAP draws every
character and space using that attribute. Ring scales, HOME and other overlays
retain their own styles and the existing collision reservations.

Phosphor and Ice now give BR_YELLOW a real warm yellow accent (RGB565 FE4A).
Night uses its strongest existing warm ink (FCEF) in that slot to keep its red
palette. The contrast suite includes a map-label row plus a >=7:1 and warm-hue
check across every palette. The label adapter test covers all six classes in
normal/Daylight modes, with shapes unchanged. Inspected `labels-*-z12.png` and
`labels-*-z14.png` show the requested 43.45,-71.65 view in both fills and both
orientations; `labels-yellow-*.png` are comparison sheets.


### Firmware without an embedded map (2026-10-09)

The firmware no longer links `data/franklin_mini.ctile`. MAP and `carto show`
select the saved SD source, an installed cellset, or a suitable SD `.ctile`.
With none available, MAP shows "No map installed" and directs the user to
TILES > SEARCH; console show reports the same guidance. An unavailable saved
map can fall back to another installed source. `carto bench` uses an already
open source or `/sdcard/maps/franklin_mini.ctile`, and reports a missing fixture
without allocating render arenas. The former `carto open embedded` alias now
opens that SD path. Host fixtures remain embedded only in lssim.

`python bench/tools/test_carto_no_map.py` disables the host fixture and checks
empty SD, a missing saved file, both MAP fills, TILES's empty library, console
failure and a successful SD-only bench. Screenshots are in `bench/out/no-map/`.

The supplied failed build is 4,765,344 bytes (`0x48b6a0`); removing the 356,054-byte
fixture alone predicts about 4,409,290 bytes and 309,302 bytes (~302 KiB) free
in each unchanged `0x480000` OTA slot, before small code/alignment differences.
Existing map-file live allocated input sections show CartoCore zstd 52,931 B,
places 13,578 B and cellset 6,086 B (whole CartoCore archive 140,676 B).
No CartoCore PNG/JPEG decoder input sections are linked, so trimming those
sources would not save firmware space. Largest archive contributions are apps
~1.51 MiB (including the fixture) and lakeshark ~878 KiB. This is analysis of
existing artifacts, not a fresh firmware link; the operator must confirm the
final binary size. The partition CSV is unchanged.

Verification for this removal: 296/296 host executables pass, along with the
no-map, SD-loader, authenticated-receiver and cellset/search integration tests.
The final stack guard passes all 16 roots / 38 reachable symbols, maximum
208 bytes, and all six file/NVS negative probes. The four no-map PNGs were
visually inspected. Firmware was not built or flashed.
