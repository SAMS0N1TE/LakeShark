# Games and runtime assets (SDK/API 2.5)

`lap_assets.h`, `lap_game.h` and `lap_music.h` are in `app-sdk/lib` and
compile into the app with `lap.py`, Studio and both simulator builds. They
add **zero firmware exports**: SD jobs, theme dots, lap3d and the PCM mixer
already provide the hardware services. Sprite/animation/map/collision/particle
work has no hardware dependency. No new firmware fast path is justified
without board profiling. Existing `.lap` descriptor size remains 228 bytes;
firmware accepts 1.0–1.3 and 2.0–2.5. `LAP_TARGET_API_24` preserves the 2.4 target.

## Load an asset

The caller provides all source, decoded and scratch memory. No helper calls
malloc or hides an allocation. For example, keep these in app state:

```c
static uint8_t input[32768], memory[131072];
static lap_asset_file_t file;
static lap_asset_arena_t arena;
static lap_image_t image;
static uint16_t inks[16];
/* enter: check lap_caps LAP_HW_SD, then: */
lap_asset_arena_init(&arena, memory, sizeof(memory));
for (int i = 0; i < 16; i++) inks[i] = lap_palette(i);
lap_asset_begin(&file, "my_game", "sheet.png", input, sizeof(input));
/* draw: poll once per frame. 0=pending, 1=ready, negative=error. */
int r = lap_asset_poll(&file);
if (r == 1) {
    int error = lap_image_png(&image, input, file.size, &arena, inks);
    /* Handle error, then stop polling/decoding this completed file. */
}
```

Files live in `/sdcard/apps_data/<app-id>/<basename>`, the same app data
folder as saves. Plain ASCII names only, at most 48 characters; no slash,
leading dot or `..`. The loader obtains file size first and refuses files
larger than the supplied source budget; reads are at most 64 KiB each. The
source buffer must remain static or a live tracked app allocation until poll
finishes. A short read, removed card or changed/truncated file returns error.
Do not restart a pending loader or free its retained buffer. Unload uses the
existing generation cleanup. No extra handles/tasks or SD activity in draw.

`lap_image_png` and `lap_mesh_load` also accept byte arrays embedded in a
`.lap`. The templates include embedded fallbacks and attempt a file override
only when SD is fitted. Decode once while loading; decoding is synchronous
CPU work, so keep assets small and measure larger assets on the board.

PNG supports noninterlaced grey, grey-alpha, RGB and RGBA at 8 bits/channel,
and indexed PNG at 1/2/4/8 bits with PLTE/tRNS. All five filters, stored/fixed/
dynamic DEFLATE, chunk CRCs and zlib Adler checksums are handled. Adam7 and
16-bit channels return `LAP_ASSET_EFORMAT`. Dimensions are at most 4096 each.
RGB is mapped to the nearest of the supplied 16 RGB565 theme inks; alpha
below 128 is transparent (255). This matches `lap_canvas`'s dot model rather
than retaining full-colour pixels. There is no gamma/ICC colour management.
Load a whole sheet and set its frame width/height; frames run row-major.

Budget PNG source separately. Peak arena use is output dots (w×h), filtered
scanlines ((row_bytes+1)×h), concatenated compressed IDAT, roughly 2 KiB of
DEFLATE workspace and alignment. Only output dots remain charged after
success. Output is unchanged and arena usage rolls back on failure. Input
must not overlap arena memory. Reset an arena only when all its assets are
dead, including any arrays retained by the renderer through `l3_end` (or the
next `l3_begin` for raycasts). Do **not** pass arena meshes to `l3_mesh_free`.

## Runtime lap3d meshes

The OBJ converter's original C output remains the default. `--binary`
serializes the same vertices, triangulated indices and material ordinals:

```text
python app-sdk/tools/obj2l3.py ship.obj --binary --center --size 4 -o ship.l3m
```

L3M1 is little-endian and pointer-free:

| Offset | Field |
|---|---|
| 0 | `L3M1` |
| 4 | u16 version=1 |
| 6 | u16 flags=1 (material bytes present) |
| 8 | u32 vertex count, 1..65535 |
| 12 | u32 triangle count, 1..65535 |
| 16 | vertex count × xyz IEEE754 float32 |
| next | triangle count × three u16 vertex indices |
| next | triangle count × u8 material ordinal, 0..31 |

The loader validates exact length, finite floats, counts, indices, materials
and version before publishing a mesh. It decodes into aligned arena arrays;
no cast of file bytes into host/target structs. Charge roughly 12 bytes per
vertex + 7 per triangle plus alignment. The converter rejects invalid OBJ
indices, nonfinite/overflowing coordinates and too many materials/triangles.
Material ordinals must be remapped to scene IDs if they differ. The flight
starter uses a uniform material. For large models reserve lap3d vertex and
triangle capacities between frames; loading does not bypass renderer limits.
External files are mutable app data, not signed bundles; these loaders do not
claim authenticity. The existing app artifact contains the fallback bytes.

## 2D helpers

`lap_sprite_draw` clips a frame to a canvas, mirrors either axis, scales 1..16
by nearest neighbour, and optionally applies a 16-entry palette remap. Palette
swapping preserves transparency. Invalid frame/scale requests do nothing.
`lap_animation_step` advances elapsed frame time with bounded arithmetic;
one-shot animations stop on the last frame and looping animations wrap.

`lap_tilemap_draw` visits only visible cells using a camera origin in dots,
including negative origins. Tiles contain sheet frame IDs (65535 is empty),
and a parallel optional byte array supplies per-cell flags. `lap_tile_flags`
returns zero outside the map. `lap_collision_tiles` checks a box against
any requested flag bit; outside space is empty. AABB and circle helpers
exclude exact edge contact. These are overlap tests; apps own movement,
resolution and substeps for fast objects. Coordinates/maps must describe
valid arrays; particle pools, maps and sheets are never dynamically grown.

Particles use a caller-sized pool, dead-slot insertion and seconds-based
motion with a 250ms step clamp. Emit returns false when full. Screen shake
uses an explicit deterministic seed and decaying offsets. Apply the offsets
to camera/sprite coordinates, then call `lap_fade` for an ordered-dither fade
to a theme ink. All drawing uses dots (two across, three down per cell), not
physical pixels. Input x/y are client pixels: convert using `lap_cell_px`.

## LTM1 music

`lap_music_parse` reads a bounded hand-written tracker text into caller state:

```text
LTM1
tempo 120 4
inst 0 2 40 5 60
pattern 0 8
row 0 C4/0 ... ... ...
row 2 E4/0 ... ... ...
row 4 G4/0 ... ... ...
row 6 C5/0 ... ... ...
order 0
loop 0
```

Four channels, eight instruments, eight patterns of up to 64 rows, 32 order
entries. `tempo` is BPM (30..300) and rows/beat (1..16); rational sample timing
avoids accumulated rounding drift. `inst` is ID, `LAP_WAVE_*` number (sine,
square, triangle, saw, noise), volume 0..100, attack/release ms 0..1000. Notes
are C0..B8 with optional sharp and `/instrument`; C4=MIDI60. `...` holds an
existing voice, `---` cuts its gate and releases it. Unspecified rows hold.
Notes gate for 75% of their row plus instrument release. Sine uses a bounded
polynomial approximation instead of per-sample libm. No sample instruments.

`order` lists pattern IDs; `loop` is an order index or 255 for one-shot.
Declarations precede references. `#` at a token boundary starts a comment;
sharps inside notes are preserved. Unknown commands, overlong lines/counts,
bad notes/references or malformed values return EFORMAT and a line number.
Parsing failure clears the song. `arcade.ltm` and `night.ltm` are original
demo tunes under the arcade template's assets; N switches slots/tunes.

Configure MUSIC with `lap_mixer_config` after the worker is ready (retry
EAGAIN/EBUSY from draw), call `lap_music_start` and `lap_music_pump` each
frame. A pump generates/writes at most 1024 samples (64ms), retains unwritten
samples across backpressure and never waits. The queue drain determines
playback time; stalled frames slow music without a catch-up burst. A caller
can use `lap_music_render` for offline PCM. Stop on focus loss/leave and
restart on focus regain; `lap_music_stop` clears MUSIC while effects remain
independent. Apps without `LAP_HW_SPEAKER` run silently using the same binary.

## Templates and checks

```text
lsdk new my_arcade --template arcade2d
lsdk new my_flight --template flight3d
```

Arcade: loaded/embedded sheet, animation, palette swap, scrolling map, flag
collision, overlap tests, particles, shake, fade, arrows/WASD and held touch,
two score save slots, effects and music. P saves, N changes slot/tune. Save
jobs are polled; missing SD leaves session scores and embedded assets usable.
Flight: loaded/embedded mesh, lap3d scene, held arrows/WASD or mouse steering,
fixed-step updates and focus/resize handling. R centres. Binary asset files
are preserved when scaffolding; embedded fallbacks run immediately in Studio.
Copy the external override files into `apps_data/<new-app-id>/` on SD (or
the headless run's `sdcard/` directory). Source OBJ and original CC0 art/music
are included; rebuild the generated arrays with `tools/game_demo_assets.py`.

Host tests: `test_lap_games_assets` exercises decoder budgets/rollback, chunked
SD reads, golden sprite pixels, clipping/flip/scale/palette, map flags,
collision, animation, particles, fades, tracker timing and partial writes.
`tests/test_games_assets.py` adds PNG filter/colour/DEFLATE variants, corrupt
and oversized files, OBJ errors, both templates with/without SD/speaker,
scripted input/focus/resize, PCM capture and durable score slots. These use
the shared code in `bench/build`; no desktop windows are opened.
`tests/test_example_contracts.py` builds every app/template with `lap.py` and
runs launch and hide/show/stop scenarios. Generated coverage includes each
helper under the SDK libraries table, separate from runtime exports.

Board checks remain: actual SD latency/removal, PSRAM budget and large-asset
decode/frame time; sprite/dither appearance on each display, touch/mouse
geometry; lap3d throughput/reserve limits; mixer underruns, instrument
quality, effects coexistence and pause/resume with live radio audio; durable
score recovery after power interruption. Simulator PCM capture is not a
physical speaker test.
