# TILES

Install and manage offline map regions on the SD card for MAP.

| | |
|---|---|
| HOME group | FIELD |
| Radio | Wi-Fi (the Wi-Fi and Bluetooth co-processor) |

## What it does

TILES is the map library. It downloads CartoCore map regions over Wi-Fi,
receives a map file from a laptop on the same network, and shows what is
installed and how much card space it uses. OPEN shows the selected map in MAP.

It also downloads cell maps: whole states split into small cells, so you
fetch only the area around a country, state or city you pick in place search.

## Radio

TILES uses Wi-Fi. Connect to a network in LINK first. It has no RADIO picker.

## How to use it

| Key | Touch | Action |
|---|---|---|
| Up, Down | Tap a region, or < PREV / NEXT > | Select a region |
| Enter | Tap a region | Open REGION INFO |
| A | OPEN | Show the selected map in MAP |
| s | SEARCH | Open place search |
| O | OPTIONS | Open OPTIONS |
| R | | Refresh the catalog |
| V | | Receive a map from a laptop |
| Tab | | Move focus to the buttons |
| Esc, Backspace, B | BACK | Go back one page; on the main page, back to HOME |

OPTIONS holds:

- **SELECTED REGION**: OPEN SELECTED MAP, SHOW REGION LOCATION (centres MAP on
  the region), DOWNLOAD / RE-DOWNLOAD, VERIFY (checks the SHA256), DELETE
  (asks first) and INFO.
- **REFRESH CATALOG**: read the list of regions available to download.
- **RECEIVE FROM LAPTOP**: accept one `.ctile` file over Wi-Fi.
- **SEARCH PLACES**: open place search.
- **DOWNLOAD PROGRESS**: show the cell map download queue.

In a list, Esc or BACK goes up one level.

### Place search and cell maps

1. Press s, or tap SEARCH. Type two or more letters, or tap KEYBOARD (F1).
2. Pick a COUNTRY, STATE or CITY with Up/Down and Enter.
3. TILES shows the cells, how many are installed, the download size and the
   free space on the card. For a city, `<` and `>` (or < RADIUS and RADIUS >)
   set the radius in 5 km steps, from 5 to 500 km. It starts at 25 km.
4. DOWNLOAD (F1) starts the queue.

The other search buttons: INSTALLED (F2) lists downloaded places with UPDATE
and DELETE, SERVER (F3) edits the map server address, INFO (F4) shows the
data credits and PROGRESS (F5) shows the queue. A cell shared with another
place is kept when you delete one.

### From a laptop

RECEIVE FROM LAPTOP shows an address, a port and a session code, and waits
five minutes for one file. Send the file with the code from the screen:

```sh
curl --fail --upload-file my_region.ctile "http://BOARD_IP:8080/maps/my_region.ctile?code=CODE"
```

Add `&f=1` to replace a file that is already on the card. Use this on a
network you trust.

## What the screen shows

- **MAP REGIONS**: each region with its name, state (NOT INSTALLED, INSTALLED,
  PARTIAL or UPDATE AVAILABLE), size, zoom range and bounds. The map in use is
  marked [ACTIVE]. A globe highlights the selected region.
- SD card free and used space, and the total size of installed maps.
- **REGION INFO**: file name, size on the card, any partial download, zoom
  range, bounds, description and SHA256.
- **TRANSFER**: a globe that fills as the download runs, percent, megabytes,
  speed and time left.

## Tips and limits

- Every download is checked before it replaces the installed copy.
- BACK during a transfer asks before cancelling. A cancelled transfer removes
  its partial file.
- The cell map queue is saved on the card, so it keeps going after you leave
  TILES. An interrupted cell download keeps its partial file and resumes.
- Place names come from GeoNames (CC BY 4.0). Map data comes from
  OpenStreetMap contributors.

## More guides

- [MAP and TILES guide](../MAP_TILES.md)
- [MAP](MAP.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
