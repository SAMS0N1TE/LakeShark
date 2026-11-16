# LINK

Join Wi-Fi networks and connect a Flipper running LakeShark over Bluetooth.
The Wi-Fi and Bluetooth survey recorder is in WIFI SURVEY: see
[Survey](#survey).

| | |
|---|---|
| HOME group | SYSTEM |
| Radio | Wi-Fi and Bluetooth (the co-processor on the board) |

## What it does

LINK has three tabs (WI-FI, BLUETOOTH, REMOTE) and two shortcuts:

- **WI-FI**: scan for networks, join one, and manage up to eight saved
  networks. UPDATE and TILES use this connection.
- **BLUETOOTH**: link a Flipper running the LakeShark app as a control head.
  It sends commands to the board and receives radio telemetry.
- **REMOTE**: the web page, terminal and SSH ways in from a phone or PC.
- **DRONES** (G): opens the DRONES app.
- **Survey** (V): opens WIFI SURVEY, an app on the card. Its RECORD tab is
  the survey recorder that used to be a LINK tab.

## Radio

LINK drives the Wi-Fi and Bluetooth co-processor. It has no RADIO picker.
RADIOS > OPTIONS sets whether Wi-Fi and Bluetooth start at boot.

## How to use it

| Key | Touch | Action |
|---|---|---|
| W, B, E | WI-FI, BLUETOOTH, REMOTE | Change tab |
| Tab | | Next tab |
| G | DRONES | Open DRONES |
| V | | Open WIFI SURVEY (says where it is if it is not installed) |
| Esc | | Back to HOME |

### WI-FI

| Key | Touch | Action |
|---|---|---|
| S | SCAN | Scan for nearby networks |
| Up, Down | | Select a network |
| Enter, J | Tap a network, or JOIN | Join it. A secured network asks for its password |
| [ ] | PREV, NEXT | Page through nearby networks |
| R | AUTO | Join a saved network in range. Reads CONNECT when a saved network is selected |
| M | MANUAL | Type a network name, for a hidden network |
| D | LEAVE | Disconnect |
| K | Tap a saved network | Select a saved network |
| P, N | < PREV saved, NEXT saved > | Page through saved networks |
| F | FORGET | Remove the selected saved network. Type YES to confirm |

A password must be 8 to 63 characters. Leave it empty for an open network.

### BLUETOOTH

| Key | Touch | Action |
|---|---|---|
| S | SCAN | Search for LakeShark heads |
| D | STOP | Stop Bluetooth |
| O | OPTIONS | Open BLUETOOTH options |

BLUETOOTH OPTIONS:

- **PIN CONNECTED HEAD**: remember the linked Flipper.
- **UNPIN HEAD**: forget the pinned head.
- **HEAD NAME FILTER**: a name filter for heads, up to 23 characters.
- **TELEMETRY RATE**: 0 to 20 Hz. 0 turns telemetry off.

### Survey

Survey is in WIFI SURVEY: its RECORD tab records the Wi-Fi access points and
Bluetooth devices heard, with a position when a fresh GPS fix exists, to the
same files LINK's SURVEY tab wrote (`/sdcard/lakeshark/survey`, WiGLE CSV, and
a `_summary.csv` beside each). See the app's README.

## What the screen shows

**WI-FI** shows the connection state, the IP address, signal and channel when
connected, the nearby networks (name, dBm, channel, PASSWORD or OPEN, SAVED),
a signal trace for the last 60 seconds and the saved list. The connected saved
network is marked with `*`.

**BLUETOOTH** shows the link state and the linked head, a FLIPPER CONTROL HEAD
panel that reads LINKED once connected, counts of commands received, frames
sent and drops, a signal trace and a control traffic trace.

## Tips and limits

- LINK records nothing to the card. The survey recorder is WIFI SURVEY's.

## More guides

- [DRONES](../DRONES.md)
- [UPDATE](UPDATE.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
