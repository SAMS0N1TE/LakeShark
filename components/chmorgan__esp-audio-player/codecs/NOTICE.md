# Decoder provenance and licenses

The streaming bridge uses these upstream sources. Exact base commit IDs
are in `vendor/REVISIONS.txt`.

- minimp3 and minimp3_ex: https://github.com/lieff/minimp3. CC0 public-domain
  dedication. Complete license: `vendor/minimp3-LICENSE`.
- dr_flac: https://github.com/mackron/dr_libs. Distributed under the MIT option
  of its dual license. Complete license: `vendor/dr_libs-LICENSE`.
- stb_vorbis: https://github.com/nothings/stb. Distributed under the MIT option
  of its dual license. Complete license: `vendor/stb-LICENSE`.

The bridge redirects upstream heap allocations to PSRAM and gives Vorbis a
bounded PSRAM arena for setup and frame scratch. The local preprocessor alias
for Vorbis's private `get_bits` avoids a name collision with minimp3 without
renaming upstream functions. Decode calls use the worker's PSRAM stack.
Local table-only patches mark dr_flac's CRC tables and Vorbis's lookup tables
read-only, along with the fixed FLAC predictor coefficients and Vorbis tag.
Vorbis's CRC initializer is precomputed into a constant table,
removing its repeated writes and its mutable 1 KB internal buffer.
No complete decoded file is held in memory.
