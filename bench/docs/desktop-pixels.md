# Desktop picture pixels and smooth strokes

The viewer defaults to **Board pixels for pictures ON**, **Smooth lines OFF**.
Both preferences live in this viewer's localStorage. The gear is available
over USB and direct board WebSocket connections. No app geometry or board
artwork changes. The existing cellDraw glyph implementation is unchanged.

## Negotiation and compatibility

After starting the ordinary `ack z cells` session, the page asks `term caps`.
The reply retains the old `term: caps ev1` line for pages that match it
literally, followed by `term: caps ev1 pix1 vec1`. The page then sends
`term visual <pixels:0|1> <vectors:0|1>` using only supported capabilities.
Visual records require both cells and z. Old pages send no visual command;
VT, SSH and telnet rendering stay unchanged. The new picture ops require
the updated screen.js alongside this firmware. Turning both preferences off
forces the ordinary G/P/C/X baseline. The host fixture compares its bytes
against the pre-change production encoder, including a delta and L sequence.

The USB bridge already forwards arbitrary `term` commands, extended capability
lines, and opaque frames. Neither lsweb.py nor lsdesk.py needs a protocol or
baud change; the launcher still defaults to 2,000,000 baud.

## Records

All integers are little endian. These records follow G/P/C/X/L in the existing
stream, compressed together with them through the existing raw-deflate z path,
framed in CRC-16 chunks of at most 4,000 bytes, and acknowledged after paint.

| Record | Fields after the tag | Meaning |
| --- | --- | --- |
| I | u8 op; i16 x,y; u16 w,h,row,rows,sw,sh,x0,y0; optional coded data | Grid-pixel destination rectangle, source dimensions and ring origin. op 0 resets; op 1 patches `rows` complete source rows beginning at `row` (`sw*rows` u16 pixels); op 2 updates the origin; op 3 moves/scales the destination while retaining source pixels; op 4 patches `rows` complete source columns beginning at `row` (`sh*rows` u16 pixels, column-major). |
| O | u8 op; u16 count; entries `(u16 cell, u8 halo, u16 glyph, packed coverage)` | op 0 clears glass masks; op 1 patches them. Each cell carries ceil(cw*ch/2) bytes of row-major 0..15 coverage, low nibble first. |
| V | u16 segments; segments `(u16 x0,y0,x1,y1,color)`; u16 owners; owners `(u16 cell,glyph, u8 attr,bits)` | Replaces this frame's paths and their owned dot bits. Coordinates are quarter grid pixels; color is RGB565. |

I retains source pixels instead of transmitting repeated upscaled pixels. For
destination-relative `(x,y)`, the sampler is exactly the board's:
`sx=(floor(x*sw/w)+x0)%sw`, `sy=(floor(y*sh/h)+y0)%sh`.
Signed destinations preserve this sampling when a desk window is partly off
the grid; only destination clipping changes. Source data is copied while the
UI owns it, before the existing snapshot handoff is released to the worker.

O comes directly from the glass renderer's coverage function. The page uses
the same RGB565 blend, ink coverage times 17, and background halo alpha 200.
Opaque cells cover the image. Glass glyphs keep the image underneath. Masks
whose glyph no longer matches use the legacy cell fallback until refreshed.

MAP line and ellipse observers retain the actual clipped/dashed runs and
sampled arc endpoints. The shared braille writer observes the actual vertical
dot runs used by scopes, spectra and sparklines, including CartoCore's emitted
braille cells. It does not infer connections between independent samples.
Dot-matrix text and SSTV image glyphs use the unobserved writer. V ownership
checks final glyph, attribute and palette, so later text/opaque replacements
remain visible. The browser clips strokes to surviving owned dot slots and
hides only their bits. The board's dot cells are never modified.

## Bounds, scheduling and memory

- I/O/V share **524,288 raw bytes per 100 ms per session**. Contiguous
  MAP pictures use a 16 ms opportunity for small scroll/strip records. The
  ordinary acknowledgement gate remains; no visual payload bypasses it.
- Input frames can carry a 24-byte source-scroll control record. Actual pixel
  spans wait until input is answered. A completed fast render suppresses the
  shifted preview; otherwise MAP publishes it after 50 ms. Pixels-off keeps
  the ordinary cell/L path and its existing compression settings.
- There is one newest source snapshot, no pixel-delta queue. Normal pictures
  use row hashes. Ring counters retain the existing waterfall behavior.
  Contiguous MAP captures compare against the shifted prior snapshot and keep
  one changed span per source row. A viewer with that complete generation gets
  the scroll and spans; a viewer missing a generation gets a full refresh.
  Epoch changes invalidate reuse. The panel still receives contiguous RGB565.
- Once pixels are negotiated, picture/glass X cells carry a neutral placeholder
  instead of also streaming half-block colors. Pixels off restores colored X.
- V accepts at most **512 segments** and **1,024 owner cells**. Excess paths
  or owner cells retain ordinary board dots. The fixture exercises 601 stroke
  submissions: 512 accepted and 89 dropped. Re-enabling midway through a draw
  waits for the next complete root frame.
- Pixel staging is exactly the current source's `sw*sh*2` bytes, plus the
  current glass masks; contiguous MAP dirty spans add `sh*4` bytes. Sources above 4,194,304 pixels or 65,535 on either axis
  fall back to ordinary X cells. Per-view row hashes/validity and column
  validity use `sh*5+sw` bytes. Observer
  grids follow the current grid, not the cap. Visual buffers are allocated on
  demand; source and observer staging is released when unused. Small per-view
  row tables are reused/replaced with the next region. No new cap-sized
  framebuffer exists.
  If source/mask allocation fails, I clears the cached picture and the session
  resumes ordinary colored X cells while later captures retry allocation.
- The browser caches composed cells and sampling maps; source-row, glass,
  glyph and ownership records mark affected cells. Full glyph redraws also
  invalidate composition, preserving the parallel text-font toggle's redraw
  path without editing cellDraw. Direct partial redraw extensions can set
  `cm.visualFull=true` when they invalidate the underlying glyph image.
  The browser tracks known source pixels so partially received columns and
  rows compose correctly without displaying stale incoming strips.

MAP scrolls use I op 5: `row` and `rows` are signed dx/dy in source pixels,
positive meaning `new[x,y]=old[x+dx,y+dy]`. The u16 payload is exposed ground.
Origins are zero. I op 6 uses `row=y, rows=1`, then `u16 x,width; u8 codec`.
Codec 0 carries width RGB565 pixels; codec 1 carries `u16 runs` followed by
`(u16 length,colour)` pairs totaling width. Codec 2 carries width 8-bit palette
indices; codec 3 packs two indices per byte, low nibble first. I op 7 carries
`u16 count; count*u16 RGB565` (1..256 colours), replacing the index palette.
Decoded pixels retain RGB565 values when a later palette arrives. The receiver validates the whole
span before updating it. Both reset and partial refresh preserve glass masks.
MAP uses indexed 8-bit pixels, or 4-bit indices for <=16 colours, with RLE
for long flat runs. It falls back to RLE/raw when more than 256 colours occur
or optional lookup allocation fails. The lookup and palette add 128.5 KiB
PSRAM while MAP pixels are enabled; leaving MAP releases them. Four greedy
ROM deflate probes serve full pictures; small deltas use one. Other feeds
retain 32 probes. An optional 9.5 KiB worker colour cache also avoids repeated relief RGB
arithmetic, preserving exact pixels. The measurements below describe the
original 16 KiB budget implementation.

## Host evidence

Run only the focused fixture (requires a host C compiler, CMake, Node and the
same public-domain miniz v1.14 file as bench_rterm_cells.py):

```powershell
$env:PATH='C:\Espressif\tools\ninja\1.12.1;C:\Program Files\CMake\bin;C:\Program Files\JetBrains\CLion 2026.1\bin\mingw\bin;'+$env:PATH
cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE= -DCMAKE_C_FLAGS=-O2
python bench/tools/mapfast_bench.py
python bench/tools/test_rterm_visual.py gcc bench/build/miniz.c
```

The fixture compiles the production capture observer, cell/visual encoders,
framing, CRC, streaming deflate, VT image sampler and MAP ink drawer. It also
compiles the unchanged HEAD encoder for the compatibility comparison. It
builds lssim and its touched CartoCore boundary audit, and runs only the new
visual tests. The bridge is imported with BOARD still None; no port is opened.

Actual MAP and FM spectrum/waterfall screens are rendered at 160x45, font
7x13, and MAP at 360x105. `LSSIM_VISUAL_OUT` exports the real source/ring/glass
and RGB565 framebuffer; the production encoded frames pass through screen.js's
FrameParser, deflate chain, cellsApply and compositor, painting every update.
The page matches **347,984 / 287,560 / 2,750,384 picture-and-glass pixels
exactly**, respectively. Additional checks cover clipping, signed legacy
sextants, glyph/palette ownership changes, the segment cap, shared byte budget,
unchanged images, X suppression, row/column scrolls and skipped revolutions,
allocation-failure recovery, input/ack/rate priority, localStorage/defaults,
old capabilities, and opaque bridge forwarding. Canvas path calls/clips are
inspected for smoothing; native browser anti-aliasing is not raster-tested.

Representative Windows host timings, including the production cell diff,
compression and framing, measured with QueryPerformanceCounter:

| Case | Destination / source | Framed bytes | Host encode |
| --- | --- | ---: | ---: |
| MAP at 160x45, full initial picture | 1120x312 / 480x144 | 12,944 total, max update 5,432 | 2.33 ms total / 0.194 ms per update |
| FM waterfall at 160x45, full initial picture | 1106x260 / 1106x260 | 44,328 total, max update 2,763 | 8.10 ms total / 0.213 ms per update |
| FM waterfall, one source-row ring scroll | 1106x260 | 2,281 | 0.141 ms; capture copies 2,212 bytes |
| FM raster, one source-column ring scroll | 1106x260 | 589 | 0.112 ms; capture copies 520 bytes |
| MAP at 360x105, full initial picture | 2520x1092 / 1080x504 | 13,502 total, max update 5,258 | 42.43 ms total / 0.581 ms per update |
| Synthetic full-native noise, 360x105 | 2520x1365 source, 6,879,600 staging bytes | max observed update 16,414, including framing/ordinary cells | 1.041 ms per update |

These are host timings, not MCU timings or a serial-throughput measurement.
The scroll cases use the real waterfall raster with a deterministic new
RGB565 row/column; they do not replay hardware. Timings vary with host load;
the measurements include neither UI capture time nor browser painting.
Machine-readable measurements, framed
streams, framebuffer fixtures, glass BMPs and page PNGs live under
`bench/build-pixels/`. The MAP/FM page PNGs were visually inspected.

## Changed files and handoff

- Protocol/capture: `components/apps/tui/ls_rterm.c`, new
  `ls_rterm_visual.h`, new `ls_rterm_visual_encode.h`.
- Exact glass masks and origin: `components/apps/tui/ls_tui.c`, `ls_tui.h`.
- Stroke observation: `components/apps/tui/ls_braille.h`, `ls_map_ink.c`,
  `ls_cartocore.c`, `ls_cartocore_map_impl.h`, `tuilib/tui_core.c`,
  `tuilib/tui_core.h`.
- Text/picture observation opt-outs, with identical board cells:
  `components/apps/tui/ls_calls.c`, `screens/main/fm_rds_view.c`,
  `screens/main/fm_sstv_view.c`.
- Viewer records, composition and gear entries:
  `components/apps/tui/web/screen.js`.
- Bench: `bench/tools/lssim.c`, `test_carto_stack.py`, `bench_rterm_cells.py`, new
  `test_rterm_visual.py`, new `bench/tests/test_screen_visual.js`, this file.

Required firmware builds are `p4-nano-v3-dsi` and `t-display-p4`, using
`pwsh -NoProfile -File bench/build.ps1 <profile>`. Both passed:

| Profile | App bytes | Smallest app partition | Free |
| --- | ---: | ---: | ---: |
| p4-nano-v3-dsi | 0x552160 | 0x900000 | 0x3adea0 (41%) |
| t-display-p4 | 0x46d550 | 0x480000 | 0x12ab0 (2%) |

Final size results are in the worktree's
`bench/pixels-<profile>-verified.log` files. T-Display still fits with a narrow
app-partition margin. No hardware, COM
port, flash, commit, Forge or delegated work was used. Physical-device timing,
actual 2 Mbaud throughput and native-browser smoothing raster checks remain
unmeasured by design; no claim about them is made here.
