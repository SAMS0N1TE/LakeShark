# SD streaming integration

All SD files use CartoCore callback sources. The embedded map keeps the mapped
reader. One file descriptor stays open until map replacement; all reads, rendering, idle
prefetch and benchmark fills share view_lock. Failed open retains the old source.
Directory and staging allocations and the decoded cache live in PSRAM. Reads
use an aligned internal DMA bounce buffer, with a direct PSRAM fallback.
See [SD_READ_PERFORMANCE.md](SD_READ_PERFORMANCE.md) for the benchmark,
automatic path selection and verified 40 MHz mount with 20 MHz fallback.

## Paper comparison

Central NH is 23,732,294 bytes. Whole-file loading consumes 22.63 MiB before
renderer allocations and fails the previous half-free-PSRAM test. Streaming uses
a 256 KiB directory allocation (214,368 bytes populated, resident), 192 KiB
staging (184,624-byte maximum blob), and a 2 MiB decoded cache: 2,555,904 bytes
(2.44 MiB). Directory opening reads 214,432 bytes instead of 23,732,294 bytes.
Warm cache hits read no SD blobs. Small SD maps could save their initial decode
reads by mapping, but using one SD lifetime/locking path bounds memory as region
sizes grow; desktop timings in CartoCore/docs/ctile-streaming.md show essentially
equal warm throughput. Actual SD latency has not been measured on hardware.

SD views retain the 2 MiB scene arena and 2 MiB idle scratch in addition to the
decoded/source budgets and grid buffers (6.44 MiB before grid buffers). The
existing 4 MiB raster cell cache remains for embedded views only: its retained
borrowed tile scenes are incompatible with ephemeral streaming staging. SD views
use the source-aware renderer with decoded caching instead. After a
successful show/goto, hints queue a tile halo; after pan/zoom they also predict
the next pan. The existing
carto-idle task consumes one hint each turn after pending cell raster work,
restoring scratch and invalidating the renderer before its next frame. The
64-entry queue bounds work; further pans replace stale predictions.

An active streamed file cannot be deleted or replaced on FAT. Open embedded (or
another region) first. Transfer partials remain available if publication fails.

## Verification

`python bench/tools/test_carto_sd.py` tests the production loader against the
real central_nh_compact.ctile and renders 240x67 at z10..15 with a 2 MiB arena
and decoded cache. Every repeat has zero blob reads. It also checks failed-open
retention, active-file deletion protection, allocation failure, embedded cleanup,
geographic rollback and asynchronous fetch failure/cleanup paths.

`python bench/tools/test_tiles.py` and `test_tiles_state.py` check catalog/digest
validation, queue failures, keyboard/touch/options/cancel, and external console
transfer transitions. Actual lssim renders are in bench/out/tiles, including
portrait, landscape, tracks and touch. List renders include selected-region
ASCII globe, ACTIVE marker, and a directly accessible OPEN button. REFRESH
remains in OPTIONS and on R. Transfer progress uses ASCII # and . characters.

`tui png` framing and encoding now run in one task with stdout locked through
begin/data/end and flushed before unlocking. This prevents task printf/vprintf
logs from splitting base64 records. The host still checks size and CRC.

No flashing, hardware execution, audio changes, commits or pushes.

The concurrent PNG regression (`python bench/tools/test_tui_png_console.py`)
runs the production task framing with host FILE locking and a vprintf log flood,
then feeds fragmented bytes through the actual ls_console.py receiver. The
received PNG matches byte-for-byte with correct size and CRC. This is host
verification; live board capture has not been run.

Final t-display-p4-lr2021 build: 0x461f70 bytes (4,595,568), app slot 0x480000;
0x1e090 bytes (123,024, 2.61%) free. No features removed for flash size.

MAP's two CartoCore fills now borrow the same retained renderer and SD source.
Leaving MAP releases both while retaining the NVS selection. Completed transfers
blocked by an open source retain `.ctile.pending` instead of being discarded;
source close and boot recovery use the existing rollback publication path.
The host SD regression uses `CARTOCORE_DIR` and explicitly reports a Franklin
compact substitute if the large Central NH fixture is unavailable.

CartoCore MAP opening, source recovery, rendering and release now run on the
existing carto-idle worker. TUI draws only copy the last matching published
PSRAM frame, with zero-timeout locking; opening and failed states remain visible
without synchronous SD work. The host simulator exercises the production SD
callback loader and rejects TUI-side file calls (see test_carto_stack.py).
