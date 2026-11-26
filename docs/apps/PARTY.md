# PARTY

A few LakeShark users together over LoRa. See who is in the party and where,
what each of them finds and roughly where, ping a place, and pass small
files round.

| | |
|---|---|
| HOME group | FIELD, beside WARDRIVE |
| Radio | The LoRa chip (SX1262 or LR2021). GPS for positions; WARDRIVE for finds |

## What it does

Everyone in a party types the same party code. The code makes the key that
encrypts the traffic and picks the channel, so the same code always finds the
same party. Case, spaces and punctuation do not count: "fox den 7" and
"FOX-DEN 7" are the same party. A code needs at least four letters or digits.

A party holds up to 12 members. Each board sends who it is, where it is (if
you share it) and its battery. While a WARDRIVE drive runs, its finds go to
the party as pings with a rough place: where they were strongest, with a
radius guessed from the signal. When two members report the same device, the
ping says so, which shows the same car twice.

The party keeps running when you leave the screen. Alerts arrive as a
banner, a sound, or a buzz only, so a board in a pocket can tell you without
lighting up.

## Radio

PARTY uses the LoRa chip. OPTIONS > RADIO sets MODE, MAX AIRTIME (1, 3, 10
or 25 %) and TAKE FILES:

- **AUTO**: positions and pings on LoRa; files on FLRC when every member has
  an LR2021, else GFSK, else LoRa.
- **LONG RANGE**: everything on LoRa at a long-range setting.
- **FAST**: everything on the fastest mode every member shares.

## Setting up

The first time, START (or NEW) opens GUIDED SETUP. Left, Right and Enter move
through it:

1. **THE PARTY**: type the party code (C) and the name shown on screen (N).
2. **YOU**: your call sign, the glyph that marks you on the radar, and its
   colour.
3. **YOUR POSITION**: HIDDEN, ROUGH (about 1 km) or EXACT; the RATE (SMART,
   10 S, 30 S, 1 MIN or 5 MIN); on foot or in a vehicle. SMART sends every
   10 s in a vehicle, 30 s walking and 2 min standing still.
4. **WATCH FOR**: what you send and want to hear about: FLOCK + POLICE,
   DRONES, TRACKERS + FOLLOW, VEHICLES, SKIMMERS + HACKS or EVERYTHING.
5. **ALERTS**: BUZZ ONLY, BANNER QUIET or SOUND + BUZZ, with a test buzz.
6. **READY**: START the party, or SAVE it only.

The setup is saved as a profile. Up to six are kept; on the lobby, Up and
Down pick one and Enter joins.

## How to use it

| Key | Touch | Action |
|---|---|---|
| S | START, STOP | Join or leave the party |
| 1 | ROSTER | Who is in the party, how far and which way, battery and link |
| 2 | RADAR | Members and finds around you |
| 3 | PINGS | What members found, roughly where |
| 4 | FILES | Send a small file, see what arrived |
| M | MARK | Ping the party with where you are |
| F | FIND | Buzz faster as you close on a member or a ping |
| L | LOCK | Pocket lock |
| O | OPTIONS | Open OPTIONS |
| Left, Right | | Step through the pages |
| Esc, B | | Back to RADAR; on RADAR, back to HOME |

**RADAR** puts members and pings at their true bearing and distance. + and -
change the range (100 m to 25 km) and A sets it back to automatic. It turns
heading up when there is a compass, unless OPTIONS > RADAR NORTH UP is on.

**MARK** offers LOOK HERE, POLICE, PLATE READER, CAMERA, DRONE, THIS
VEHICLE, FOLLOWING ME and HELP: COME TO ME. HELP stays on until you pick
CLEAR HELP, and it always sounds on the other boards.

**FILES** sends a file from `/sdcard/party/share`, up to 64 KB, to the whole
party. Files received go into `/sdcard/party`.

## OPTIONS

- **PROFILE**, **PROFILE NAME**, **PARTY NAME**, **PARTY CODE**, **CALL
  SIGN**, **GLYPH**, **COLOUR**.
- **MY POSITION**: SHARE, RATE, IN A VEHICLE.
- **WATCH PRESET**: one of the six sets above.
- **SEND FINDS**: each kind on or off, and ONLY STRONGER THAN (ANY, -85,
  -75 or -65 dBm).
- **RECEIVE**: each kind on or off, and ONLY WITHIN (ANYWHERE, 500 M, 2 KM
  or 10 KM).
- **ALERTS**: each kind OFF, BANNER, BUZZ, SOUND or BANNER+BUZZ.
- **BUZZ PATTERNS**: a pattern per kind (TAP, DOUBLE, TRIPLE, LONG,
  HEARTBEAT, RISING, SOS, CAR), and a test of the motor.
- **KEYWORDS**: up to three words to look for in device names.
- **QUIET HOURS**: FROM and TO. During quiet hours a sound becomes a buzz.
- **RADIO**, **RADAR NORTH UP**, **POCKET LOCK**, **AUTO POCKET LOCK** (OFF,
  30 S, 1 MIN or 2 MIN while PARTY is open).
- **GUIDED SETUP**, **NEW PROFILE**, **DELETE PROFILE**. These and PROFILE
  wait until you leave the party.

## Tips and limits

- Everyone needs the same code. A different code is a different party.
- A ping's place is where the reporting board heard it strongest, not where
  the device is.
- With SHARE on ROUGH, your position and your pings are sent as the middle
  of a square about 1 km across.
- A board without a vibration motor cannot buzz; the setup says so.

## More guides

- [WARDRIVE](WARDRIVE.md)
- [MESH](MESH.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
