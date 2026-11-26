# WARDRIVE

Log what the radios hear on a drive or a walk, and where. HUNT picks out
plate readers, body cameras, trackers and other targets, and ATLAS remembers
every drive.

| | |
|---|---|
| HOME group | FIELD. Its folder also holds SWEEP, DRONES and CELL WATCH |
| Radio | Wi-Fi and Bluetooth on the co-processor, and GPS. The LoRa chip and a USB RTL-SDR or HackRF add sub-GHz, aircraft and LoRa |

## What it does

START begins a drive. Each source is logged with the time and where the board
was when it heard it:

| Source | What is logged |
|---|---|
| WI-FI | Access points, with their security |
| BLUETOOTH | Bluetooth devices |
| CELL | LTE cells, through CELL WATCH |
| DRONES | Remote ID drones and the drone watch |
| SUB-GHZ | Tyre sensors, weather stations, remotes and meters, by their own IDs |
| AIRCRAFT | Aircraft by ICAO address |
| LORA | Meshtastic, MeshCore and LoRaWAN nodes |

A drive keeps running when you leave the app. STOP closes it. OPTIONS >
SOURCES turns each source on or off; one the board cannot hear says "not on
this board".

## Radio

Wi-Fi, Bluetooth and drones come from the co-processor. Sub-GHz, aircraft and
LoRa come from the radio jobs on the RADIO page. WARDRIVE also takes what
other apps' receivers and the mesh tables hear, without claiming a radio.

## How to use it

| Key | Touch | Action |
|---|---|---|
| S | START, STOP | Start or stop a drive. Enter on LIVE does the same |
| 1 | LIVE | Speed, heading, counts by kind and new networks as they are found |
| 2 | 3D | The drive as a route with a pillar for each find |
| 3 | LIST | Everything found, by kind |
| 4 | HUNT | Targets, vehicles and alerts |
| 5, R | RADIO | What the LoRa chip and the SDR are doing |
| 6 | ATLAS | Routes and memory across drives |
| M | MAP | Open MAP with the drive drawn over it |
| D | | The drive list, to replay a drive |
| O | OPTIONS | Open OPTIONS |
| Esc, B | | Back to LIVE; on LIVE, back to HOME |

**LIST** shows STRONGEST or NEWEST (N or the SORT chip). Left and Right, or
the chips, pick ALL, WI-FI, OPEN, BT, CELL, DRONE, RF, AIR, LORA or TARGETS.
A row opens its address, when it was first and last heard, its best signal
and where it was strongest.

**3D**: Left and Right turn, Up and Down tilt, + and - zoom, F follows the
board, T steps the tilt and V picks what is shown. The touch keys add FIT
ALL, CHASE, LIGHT MAP and HIDE. HIDE gives the view the whole page until SHOW
KEYS.

**OPTIONS** holds REPLAY A DRIVE, SOURCES, RADIO JOBS, SCAN PACE (FAST,
NORMAL or SAVER), UNITS (KM/H or MPH), ONLY WITH A FIX, POCKET LOCK, AUTO
POCKET LOCK (OFF, 30 S, 1 MIN or 2 MIN while WARDRIVE is open), SAVE CSV +
GPX and SEND TO LOCKER. Until you pick a unit, a fix in the US or the UK
shows mph.

## HUNT

HUNT has three tabs: TARGETS (T), VEHICLES (V) and ALERTS (A). Tab steps
through them.

**TARGETS** lists each kind with how many were found, the loudest and when
this drive first heard one. Enter opens a kind, then a target: what it is,
where its pattern comes from, its address and name, first and last heard,
where it was strongest and how far that is from you.

| Kind | What it is |
|---|---|
| ALPR | Flock and other plate readers |
| BODYCAM | Body cameras |
| POLICE | Police and fleet routers |
| FOLLOWING | Devices that move with you |
| TRACKER | Tags such as AirTag, Tile and SmartTag |
| SKIMMER | Bluetooth serial modules of the kind used in card skimmers |
| DRONE | Drones |
| GLASSES | Camera glasses |
| HACKTOOL | Flipper, Pineapple and similar tools |
| CAMERA | Doorbells and CCTV |
| SENSOR | Raven gunshot sensors |
| VEHICLE | A car's own radios: hotspot, phone key, tyre sensors |
| WATCHED | Devices on your own list in `sigs.txt` |

A device is FOLLOWING once it has been heard with you for the "with you"
time (10 minutes at first), at four places and over more road than its own
range. Parked, nothing is learned. This needs a GPS fix.

**VEHICLES** groups radios that travel together, such as four tyre sensors,
or a car hotspot and its phone key. Each shows how long and how far it has
been with you, and IN RANGE or GONE.

**ALERTS** sets each kind to OFF, BANNER, BUZZ, SOUND or BANNER+BUZZ. BUZZ
vibrates only, with no banner and no sound. SOUND gives a banner, a buzz and
a sound. Left and Right change the selected kind; T or Enter tests it. Each
kind has its own vibration rhythm. The choices are kept.

### Your own signatures

WARDRIVE reads `/sdcard/wardrive/sigs.txt` each time it opens. SWEEP uses the
same list. A row with the same id as a built-in one replaces it, a new id
adds one, and `id|off` turns one off. Each row has seven fields split by `|`:

```
id | tags | kinds | alert | label | patterns | source
```

Patterns can match a network or device name, an address prefix
(`oui:AABBCC`), one address (`mac:AABBCCDDEEFF`), a Bluetooth company ID or a
service UUID. The full format and the built-in rows are in
`data/wardrive/sigs.txt` in the repository.

## RADIO

One panel for the LoRa chip and one for the USB SDR. Each runs one job at a
time:

| Job | Listens for | LoRa chip | SDR |
|---|---|---|---|
| TPMS | Tyre sensors, on 315 or 433.92 MHz by region | Yes | Yes |
| ISM | Weather stations, remotes and meters, on 433.92 MHz and 868 or 915 MHz | Yes | Yes |
| ADS-B | Aircraft on 1090 MHz | | Yes |
| MESH | Meshtastic, MeshCore and LoRaWAN nodes | Yes | |

On AUTO the radios choose their job from where you are, how fast you are
moving and what each job is finding. Up and Down pick a radio, Left, Right or
Enter pin it to a job, and A sets it back to AUTO. OPTIONS > RADIO JOBS does
the same. A radio another app is using shows LENT OUT.

## ATLAS

Every drive adds to one map of what you have heard, kept in
`/sdcard/wardrive/atlas`. A route you drive again is recognised.

- **ROUTE**: the route's shape, this run against the best run, and the
  devices heard on each run.
- **COVERAGE**: how much of the road has been heard with each of WI-FI, BT,
  RF, AIR, LORA and CELL.
- **Lists**: NEW here, regulars MISSING, MOVED, and REGULAR. Tab or L steps
  through them.
- < and > pick a route; F follows the drive again.
- I imports old drives from the card; C compacts the index. Both wait until
  no drive is running.

When the GPS is weak or gone, access points the atlas knows from two or more
drives place you. LIVE then shows WIFI FIX with its error in metres. It needs
three such access points that agree. It moves your track only; finds are
never placed from it.

## Saved drives

Each drive has its own folder in `/sdcard/wardrive`, named for when it began
(`WD_YYYYMMDD_HHMMSS`):

- a `.csv` in WiGLE format: Wi-Fi and Bluetooth rows with position and time.
- a `.gpx`: the track, with cells and drones as waypoints.
- `drive.txt`: the drive's summary.

D, or OPTIONS > REPLAY A DRIVE, opens the drive list: the day, time,
length, distance, finds and size of each drive. Pick one to replay it
through LIVE, 3D, LIST and MAP. During a replay, Space pauses, comma and
period change the speed (1x to 64x), [ and ] jump back or ahead, and X ends
it. STOP the drive before replaying.

## HOME base sync

A home base board keeps everyone's drives in one atlas and syncs with field
units over LoRa when they are in range. The LakeShark Desktop side of this comes
with the next Desktop release. Every
board needs the same key: `lsx key phrase <words>` in TERMINAL. Then `lsx
field on` on the handheld. LIVE's HOME line shows the home base, a sync in
progress, or when the last sync was.

## Tips and limits

- Names and signatures are hints, not proof of what a device is.
- With no card, nothing is saved.
- A position is where the board was when it heard the strongest signal, not
  where the transmitter is.
- While a party and a drive both run, PARTY members' finds show in WARDRIVE
  too.

## More guides

- [PARTY](PARTY.md)
- [SWEEP](SWEEP.md)
- [MAP](MAP.md)
- [DETECTOR](DETECTOR.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
