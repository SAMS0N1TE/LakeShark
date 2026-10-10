# DRONES

DRONES receives Bluetooth Remote ID broadcasts from nearby drones and shows
who is flying, where, and where the operator is. It only listens.

Open **HOME > RADIO > DRONES**, or **LINK > DRONES** (or **N** in LINK).

## The list

The top line shows how many drones are active. Below it, a radar places each
drone around you, and **HEARD DRONES** lists them with ID, range, signal and
age. Entries expire 60 seconds after the last broadcast.

- With your GPS fix and a recent drone position, the radar shows true bearing
  and distance.
- Without both, the radar ranks drones by signal strength only. Placement is
  then illustrative, not a bearing.

Up/Down or **PREV / NEXT** select a drone. Enter, a tap or **DETAIL** opens it.

## Detail

- UAS ID, address, signal, last heard and status (on ground, airborne,
  emergency).
- Reported position, geodetic and barometric altitude, height, speed, track and
  vertical speed.
- Operator position and its type (takeoff, live GNSS or fixed), operator ID.
- Area, classification and self ID, when broadcast.
- Authentication messages are shown as seen; signatures are not verified.

**BACK** returns to the list.

## OPTIONS

- **DISPLAY**: SORT by DISTANCE, RSSI or AGE; UNITS METRIC or IMPERIAL.
- **ALERTS**: NEW DRONE ALERT posts a notice when a new Remote ID source is
  heard.
- **CLEAR TABLE** empties the list.

## Good to know

- Covers Bluetooth Remote ID (ASTM F3411). Broadcast IDs and positions are
  reports from the drone, shown unverified.
- DRONES keeps no records. The console has `rid`, `rid detail N` and
  `rid clear`.
- [SWEEP](SWEEP.md) also counts drones and can guide you toward one.
- The [Flipper companion](https://github.com/SAMS0N1TE/LakeShark-Flipper)
  (app 2.10) shows the drone count, the nearest range and its ID.
