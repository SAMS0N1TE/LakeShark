# What this copy of libcarto does differently, and why

`upstream/` is CartoTUI's `libcarto/` at the commit in `upstream/PROVENANCE.txt`.
It is not pristine: four things had to change to run on an MCU, and each one is
marked `LS DEVIATION <n>` in the source so a re-vendor can find them. Re-apply
them or re-derive them; do not assume a newer upstream has fixed them.

## 1. The scanline crossing table is not on the stack

`carto_fill_polygon` declared `int xints[2048]`, an 8 KB automatic array. The
FreeRTOS task stacks on this board are 4 to 8 KB, so the first polygon with a
scanline to fill would have run off the end of one. It is `static` now, sized
by `CARTO_MAX_CROSSINGS` in `raster.h`.

The size is unchanged. A crossing needs an edge, so a scanline cannot have more
crossings than the polygon has edges, and 2048 is generous - but the existing
code drops crossings past the cap rather than failing, which draws a polygon
with a hole in it. Trimming the number would turn a stack problem into a
rendering problem.

`static` means the fill is not reentrant. The renderer runs on one task. If a
second one is ever added, this is the thing that breaks first and it will not
announce itself.

## 2. `cells.c` is not built

Upstream's cell renderer draws a map into a character grid. This firmware has a
character grid and deliberately does not use it for the map: CartoTUI measured
the cell diff at 12 ms for a full-screen map against roughly 3 ms to write the
bytes, and moving the map out of the diff took a frame from 14.0 ms to 2.7 ms.
The grid wins for chrome, lists and status, where one field changing costs one
field; it loses for a region where every cell changes.

So the map gets its own rect of direct pixels. `cells.c` is the file that
called `malloc` three times per invocation, so not building it is also what
retires that problem rather than arenaing it.

## 3. The scratch size is stated, not inherited

`carto_begin` takes 65536 points of 8 bytes, which is 512 KB, out of the arena.
That is unchanged and now says so. It comes out of PSRAM, and a tile whose
geometry exceeds the cap is truncated rather than refused - so trimming it to
save PSRAM would show up as missing roads and not as an error.

## 4. Filled spans do not go through `carto_put_px`

`carto_put_px` switches on the pixel format and bounds-checks every pixel, and
the polygon fill called it once per pixel across a whole scanline. A filled
landuse polygon on a 720 wide map is hundreds of thousands of switches a frame.
`carto_fill_span` decides the format once and writes a run; RGB565 is the only
format this board has and gets straight stores, everything else falls back to
the general path.

## Licence

CartoTUI is GPL-3.0 and so is LakeShark, so vendoring carries no new
obligation. `LICENSE.libcarto` is upstream's copy, kept alongside the code it
covers.
