# MAP and TILES

MAP draws offline maps from the SD card with CartoCore, the T-Display-P4's map
engine. TILES downloads and manages those maps over WiFi. Both are in
**HOME > FIELD**.

## MAP styles

**STYLE** (or **S**) opens MAP STYLE:

- **Fill**: field, blocks, lines, **CartoCore smooth** or **CartoCore braille**.
  **V** cycles the fill.
- **Theme fills** or **Theme outlines**. Colours follow the selected theme,
  Daylight included.
- **Map file on the card** picks the map to show. **M** opens the same list:
  `.ctile` maps for the CartoCore fills, `.pmtiles` archives for the others,
  both from `/sdcard/maps`.

Place names, aircraft, mesh nodes and marks share the screen: labels make room
for live data and the basemap dims a step under overlays. Range rings and
aircraft trails are thin amber lines.

## GO TO and place search

**GO TO** (or **F**) lists aircraft, mesh nodes, marks, drawn lines and
places on the map, plus **SEARCH PLACES**. In search, type two or more letters
for a country, state or city, or tap **KEYBOARD**, and pick a result. The map jumps
there without downloading anything.

[Field map](map-field.md) covers routes, marks, layers and following aircraft.

## TILES

Open **HOME > FIELD > TILES** with WiFi connected and an SD card fitted.

1. **OPTIONS > REFRESH CATALOG** reads the list of map regions.
2. Select a region with Up/Down or a tap. Enter opens INFO.
3. **OPTIONS > SELECTED REGION > DOWNLOAD / RE-DOWNLOAD** fetches it. A globe
   shows progress.
4. **OPEN** shows the selected map.

SELECTED REGION also has **SHOW REGION LOCATION** (centres MAP on the region),
**VERIFY**, **DELETE** (asks first) and **INFO**. A newer copy in the catalog
shows as UPDATE AVAILABLE. A map can be re-downloaded while it is open; the new
copy is installed once that map is closed. Every download is checked before it
replaces the installed copy.

## Cell maps

Cell maps cover whole states in small cells, so you download only the area you
need.

1. **SEARCH** (or **S**) in TILES opens place search.
2. Pick a country, state or city. TILES shows which cells are installed, the
   download size and the free space on the card. For a city, **< RADIUS** and
   **RADIUS >** set the radius in 5 km steps, from 25 km up to 500 km.
3. **DOWNLOAD** starts the queue. **OPTIONS > DOWNLOAD PROGRESS** reopens it.

The queue continues after you leave TILES and after a restart, and an
interrupted download picks up where it stopped. **INSTALLED** lists up to 32
places with **UPDATE** and **DELETE**; a cell shared by another place stays.
Cells live in `/sdcard/maps/cells`. Place names come from GeoNames
(CC BY 4.0).

## From a laptop

**OPTIONS > RECEIVE FROM LAPTOP** listens on the board's IP, port 8080, for
five minutes:

```sh
curl --fail --upload-file my_region.ctile "http://BOARD_IP:8080/maps/my_region.ctile"
```

Add `?f=1` to replace an existing file. `tools/carto_push.py` uploads a
prepared cell-map set. Use this on your own trusted network.
