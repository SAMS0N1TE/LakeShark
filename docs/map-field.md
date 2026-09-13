# Field map

- Pixel terrain with ASCII controls, place names, aircraft and Mesh nodes.
- Blue water, muted parks and warm main roads keep live markers readable.
- A fresh GPS fix adds a receiver marker. Old or missing fixes stay hidden.
- Tap the style heading or press V for field, blocks or lines.
- MAPS chooses an archive from SD `/maps`. Failed opens preserve the current map.
- The header shows north, loading state and the ground distance across the view.

PMTiles v3 with uncompressed MVT tiles is supported. The picker checks header
compatibility; opening also checks the reader's directory limits. Unsupported
compression and image tiles get a specific explanation.

One completed view is cached in PSRAM, capped at 512 KiB of terrain. Returning
from a pan or zoom can reuse its pixels and labels. A separate bounded image
comparison buffer lets unchanged screen cells skip pixel writes. Neither cache
writes files to the SD card.

Design references: [Cardputer offgrid map](https://github.com/yuiseki/m5-cardputer-offgrid-tiny-map)
for rendering reuse, [HikePod](https://github.com/nongxl/HikePod) for field navigation,
and the [PMTiles specification](https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md)
for archive compatibility. Existing ZeroMesh and CartoTUI provenance remains
with the reader and renderer; no code was copied from these design references.
