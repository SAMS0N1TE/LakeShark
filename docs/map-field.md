# Field map

- Terrain, place names, aircraft, Mesh nodes and saved marks share the map.
- A fresh GPS fix adds the receiver marker. Old or missing fixes stay hidden.
- STYLE/S chooses a palette and field, blocks or lines fill. V cycles the fill.
- MAPS/M chooses a PMTiles archive from SD `/maps`. A failed open keeps the current map.
- The header shows north, loading state and ground distance across the view.
- LAYERS/L selects overlays, including APRS, AIS, sondes and routes.
- MARK/K saves a place; DRAW/D starts a line. GO TO/F finds a place.

PMTiles v3 with uncompressed MVT tiles is supported. Unsupported compression
or image tiles produces an explanation. Copy compatible `.pmtiles` files to
`/sdcard/maps`. The map uses SD archives; it does not download missing terrain.

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
