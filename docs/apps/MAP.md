# MAP

An offline map from the SD card, with your position, aircraft, mesh nodes,
marks and routes on it.

| | |
|---|---|
| HOME group | FIELD |
| Radio | None of its own. GPS for your position; overlays come from ADSB, MESH, FM and EXPERIMENTS |

## What it does

MAP draws maps stored on the SD card. No network is needed once a map is on
the card. CartoCore maps (`.ctile`, installed with TILES) draw in the
CartoCore smooth and braille fills. PMTiles archives (`.pmtiles`) draw in the
field, blocks and lines fills. Map colours follow the selected theme,
Daylight included.

Over the map it shows live data: aircraft, mesh nodes, APRS stations, AIS
vessels, radiosonde tracks, and your own marks, lines and routes. It can follow
your GPS position, follow a selected aircraft or node, and guide you along a
GPX route.

## Radio

MAP has no RADIO picker. Your position comes from the GPS receiver, and FOLLOW
starts it. A fix more than ten seconds old is hidden rather than drawn in the
wrong place. Aircraft, APRS and AIS appear only while their receiver apps are
running.

## How to use it

| Key | Touch | Action |
|---|---|---|
| Arrows | Arrow pad, or tap bare map | Pan. A tap recentres on that spot |
| = and - | ZOOM+, ZOOM- | Zoom in and out |
| | Tap a symbol | Select it. Tap again to centre on it |
| Tab, ], [ | | Select the next or previous aircraft |
| Enter | Tap the info card | Go to the selection |
| Esc | | Clear the selection; with nothing selected, back to HOME |
| F | GO TO | Open the GO TO list |
| G | FOLLOW | Follow the selected aircraft or node, or your GPS with nothing selected |
| K | MARK | Drop a marker at the centre |
| D | DRAW | Draw a line point by point |
| E | Tap a marker's card | Edit the selected marker or line |
| U | ROUTE | Follow a GPX route, or BACKTRACK |
| O | OPTIONS | Open MAP OPTIONS |
| L | LAYERS | Turn overlays on and off |
| S | STYLE, or tap the top-left of the header | Open MAP STYLE |
| V | | Step to the next fill |
| M | | Choose a map file on the card |
| H | SET HOME | Set the home position |
| N, T, R | | Show or hide mesh nodes, flight trails, range rings |
| I | | Hide or show the info card |
| X | [HIDE KEYS] | Hide or show the button rows |

**GO TO** lists SEARCH PLACES (a country, state or city), COORDINATES (type a
latitude and longitude), RECEIVER (your live position), HOME, LAST COVERED
VIEW, then aircraft, nodes, marks, lines and places in view.

**DRAW** changes the buttons to ADD POINT (A or Enter, adds the centre), UNDO
(U), DONE (O, keeps the line) and CANCEL (C or Esc). A tap on the map also
adds a point.

**Editing a marker** offers Go to it, Navigate with COMPASS, Rename, Change
symbol, Save to NOTES and Delete. A line offers Go to its start, Rename, Save
to NOTES and Delete. Up to 48 markers are kept. Marks and lines are saved to the SD card.

**SET HOME** offers Use my GPS position, Enter latitude, longitude, Choose a
place on the map, and Save map center as home.

**MAP OPTIONS** holds Layers, Style, Map files, Guidance, Retry saving marks
and Reset coverage. In a list, Esc or BACK goes up one level.

### Styles and layers

MAP STYLE offers Theme fills or Theme outlines, then the fill: field, blocks,
lines, CartoCore smooth or CartoCore braille. Its last row, Map file on the
card, opens the map list.

LAYERS: Aircraft, Flight trails, Speed leaders, Data blocks, Hide labels, Mesh
nodes, APRS stations, AIS vessels, SONDE, Mesh links, Route, Markers and lines,
Range rings, Coverage and Place names. The list stays open while you switch
layers.

### Routes

Put `.gpx` files in `/sdcard/lakeshark/routes`. ROUTE lists them. BACKTRACK
reverses the current or last walk recorded by GPS. A route needs a fresh GPS
fix for guidance. Routes that are invalid or too large are refused.

OPTIONS > Guidance sets Off-route distance (5 to 500 m), Arrival radius (1 to
100 m) and Haptics. The route options also offer Reverse route and Clear route.

## What the screen shows

- The header: the style and fill, counts of aircraft, mesh nodes and marks,
  and the GPS state (WAIT, LIVE, FOLLOW or TRACK). Landscape adds range rings
  and the speed leader time. [HIDE KEYS] sits on the right.
- The map, with labels that make room for live symbols.
- An info card for the selection.
- The button rows, or the DRAW buttons while drawing.

## Tips and limits

- Maps live in `/sdcard/maps`. TILES downloads CartoCore maps over Wi-Fi.
- PMTiles archives need uncompressed vector tiles. Other kinds show an
  explanation instead of a map.
- A plotted position shows where data says something is. It does not prove a
  transmitter's location.

## More guides

- [Field map](../map-field.md)
- [MAP and TILES guide](../MAP_TILES.md)
- [TILES](TILES.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
