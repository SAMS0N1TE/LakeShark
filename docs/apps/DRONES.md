# DRONES

Listens for Bluetooth Remote ID broadcasts from nearby drones and shows each drone, where it reports itself, and where its operator is.

| | |
|---|---|
| HOME group | RADIO |
| Radio | Bluetooth LE receiver on the ESP32-C6. Receive only |

## What it does

- Receives Bluetooth Remote ID (ASTM F3411) broadcasts. It never transmits.
- Shows each drone on a radar and in a list with its ID, range, signal and age.
- Opens a detail page with the drone's reported position, altitude, speed, operator position and IDs.
- Can post a notice when a new drone is heard.
- Keeps nothing on the card.

Everything shown is what the drone broadcasts. IDs, positions and authentication are not verified.

## Radio

DRONES uses the Bluetooth LE receiver on the board's ESP32-C6. It does not use the RADIO picker.

The receiver does not start on its own. Press RECEIVE (R) to start it. When you leave DRONES, a receiver that DRONES started is released.

The second line shows the receiver state: OFF, UNAVAILABLE, STARTING, START FAILED, PAUSED / CONNECTING or LISTENING.

## How to use it

Open **HOME > RADIO > DRONES**, or press G in LINK.

### Keys

| Key | Does |
|---|---|
| R | Start the Bluetooth receiver |
| Up / Down, or [ / ] | Select a drone |
| Enter or D | Open the selected drone's detail |
| B or Esc | Back from detail to the list |
| O | OPTIONS |

In the list, B returns to HOME.

### Buttons and touch

- **DETAIL / SELECTED** opens the selected drone. In the detail page it becomes **BACK / LIST**.
- **RECEIVE / START / R** starts the receiver. Once it is listening, this button becomes **PREV / DRONE**.
- **NEXT / DRONE** selects the next drone.
- **OPTIONS** opens the settings.
- Tap a drone in the list to open its detail.

### OPTIONS

O or the OPTIONS button opens the DRONES list. BACK goes up one level.

- **DISPLAY**: SORT by DISTANCE, RSSI or AGE. UNITS METRIC or IMPERIAL.
- **ALERTS**: NEW DRONE ALERT posts a notice when a new Remote ID source is heard.
- **CLEAR TABLE**: empties the list.
- **START BLE RX**: starts the receiver.
- **RELEASE BLE RX**: stops it.

## What the screen shows

**Top lines.** How many drones are active, and the receiver state. Entries expire 60 seconds after the last broadcast.

**RADAR / RX.** You are the `@` in the centre.

- With your GPS fix and a recent drone position, each drone sits at its true bearing. Farther drones sit nearer the edge.
- Without both, drones are placed by signal strength only. The picture is then a ranking, not a bearing.
- The sweep moves only while broadcasts are arriving.

**HEARD DRONES.** One entry per drone: the ID (or "Awaiting ID"), range, signal in dBm and seconds since it was heard. An EMERGENCY flag shows when the drone reports one.

**DRONE DETAIL / UNVERIFIED.** In landscape the page splits into a second panel, OPERATOR / SYSTEM.

- UAS ID, Bluetooth address and signal.
- Seen time and status: UNDECLARED, ON GROUND, AIRBORNE, EMERGENCY or RID FAILURE.
- Drone position, geodetic and barometric altitude, and height (above ground or above takeoff).
- Speed, track, vertical speed and fix age.
- Operator position and its type: TAKEOFF, LIVE GNSS or FIXED. Operator altitude and ID.
- Area count, radius, floor and ceiling. Classification type, category and class.
- Self ID text.
- Authentication: SEEN, UNVERIFIED or ABSENT.

Values older than 60 seconds are marked STALE and dimmed.

## Tips and limits

- The table holds up to 32 drones.
- Range and true bearing need your own GPS fix. See the GPS app.
- RSSI shows signal strength only. It is not a distance.
- DRONES covers Bluetooth Remote ID only.
- The TERMINAL console has `rid`, `rid detail N` and `rid clear`.
- [SWEEP](../SWEEP.md) also hears nearby Bluetooth and Wi-Fi signals.

## More guides

- [Controls and navigation](../UI_CONTROLS.md)
- [DRONES guide](../DRONES.md)
- [All apps](README.md)
