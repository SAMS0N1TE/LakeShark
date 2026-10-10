# Field map

- Terrain, place names, aircraft, Mesh nodes and saved marks share the map.
- A fresh GPS fix adds the receiver marker. Old or missing fixes stay hidden.
- STYLE/S chooses the fill: field, blocks, lines, CartoCore smooth or CartoCore braille. V cycles the fill.
- M chooses a map from SD `/maps`: `.ctile` for CartoCore, `.pmtiles` for the others. A failed open keeps the current map.
- The header shows north, loading state and ground distance across the view.
- LAYERS/L selects overlays, including APRS, AIS, sondes and routes.
- MARK/K saves a place; DRAW/D starts a line. GO TO/F jumps to an aircraft, node, mark or searched place.

TILES downloads CartoCore maps over WiFi, including whole-state cell maps with
place search. See [MAP and TILES](MAP_TILES.md). For the other fills, copy
PMTiles v3 archives with uncompressed MVT tiles to `/sdcard/maps`; other
compression or image tiles produce an explanation.

ROUTE/U opens a GPX picker. Put `.gpx` files under `/sdcard/lakeshark/routes`.
Choose one to follow it, or BACKTRACK to reverse the current or last recorded
walk from `/sdcard/lakeshark/track.log`. A route needs a fresh GPS fix for
guidance. Invalid or oversized routes are refused.

The guidance strip shows bearing, remaining distance and cross-track distance.
Walked sections and the remaining path have different colours. OPTIONS/O sets
Off-route distance, Arrival radius and Haptics under Guidance. Reverse route
changes direction; Clear route removes guidance. The Route layer controls
whether the planned and walked path is visible.

ADSB's mini map has its own FOLLOW choice for all aircraft, the selected one
or the nearest. The choice is kept after reboot. In MAP, FOLLOW/G follows the
selected aircraft. Aircraft markers move between the latest received fixes;
missing positions are not predicted beyond the last fix.

NOTES can insert a saved map picture. A new note keeps the picture too.
EXPERIMENTS > RS41 SONDES > OPTIONS > TRACKS > SHOW ON MAP adds heard sonde
tracks. APRS and AIS overlays require their FM receivers to be running.
A plotted receiver position or heard device does not prove a transmitter's
location beyond the decoded position data.
