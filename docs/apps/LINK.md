# LINK

Join Wi-Fi networks, connect a Flipper running LakeShark over Bluetooth, and
record a passive Wi-Fi and Bluetooth survey.

| | |
|---|---|
| HOME group | SYSTEM |
| Radio | Wi-Fi and Bluetooth (the co-processor on the board) |

## What it does

LINK has three tabs and a shortcut:

- **WI-FI**: scan for networks, join one, and manage up to eight saved
  networks. UPDATE and TILES use this connection.
- **BLUETOOTH**: link a Flipper running the LakeShark app as a control head.
  It sends commands to the board and receives radio telemetry.
- **SURVEY**: a passive walk that records the Wi-Fi networks and Bluetooth
  devices heard, with their signal and, with a GPS fix, where the signal was
  strongest. It saves a CSV file to the SD card.
- **DRONES**: opens the DRONES app.

## Radio

LINK drives the Wi-Fi and Bluetooth co-processor. It has no RADIO picker.
RADIOS > OPTIONS sets whether Wi-Fi and Bluetooth start at boot.

## How to use it

| Key | Touch | Action |
|---|---|---|
| W, B, V | WI-FI, BLUETOOTH, SURVEY | Change tab |
| Tab | | Switch between WI-FI and BLUETOOTH |
| G | DRONES | Open DRONES |
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

### SURVEY

| Key | Touch | Action |
|---|---|---|
| S | START, STOP | Start or stop a survey session |
| R | SORT | Sort by STRONGEST, NEWEST or NAME |
| O | OPTIONS | Open SURVEY options |
| Enter, D | DETAIL, or tap an entry | Show details. BACK or Esc returns |
| Up, Down, [ ] | PREV, NEXT | Select an entry, or page |

SURVEY OPTIONS, changed while no session is running:

- **RADIOS**: SCAN WI-FI, LISTEN BLE and SCAN INTERVAL (3 to 120 seconds).
- **RECORDING**: ONLY WITH GPS FIX, and KEEP SESSIONS (1 to 50).

## What the screen shows

**WI-FI** shows the connection state, the IP address, signal and channel when
connected, the nearby networks (name, dBm, channel, PASSWORD or OPEN, SAVED),
a signal trace for the last 60 seconds and the saved list. The connected saved
network is marked with `*`.

**BLUETOOTH** shows the link state and the linked head, a FLIPPER CONTROL HEAD
panel that reads LINKED once connected, counts of commands received, frames
sent and drops, a signal trace and a control traffic trace.

**SURVEY** shows Wi-Fi and Bluetooth counts, new devices per minute, elapsed
time, entries stored (up to 512), drops and GPS state (FRESH or WAIT). The
detail view shows the name, address, signal, channel, first and last time
heard and the position of the best signal when a fresh fix was available.

## Tips and limits

- SURVEY only listens. It gives no bearings.
- Random Bluetooth addresses rotate, so one device can appear more than once.
- Survey files go to `/sdcard/lakeshark/survey`, named by date and time.
- A survey position marks where this board heard the strongest signal, not
  where the device is.

## More guides

- [DRONES](../DRONES.md)
- [UPDATE](UPDATE.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
