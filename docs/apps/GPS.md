# GPS

The position receiver: fix state, the satellites in view, and a track recorder
that exports GPX.

| | |
|---|---|
| HOME group | FIELD |
| Radio | The GPS receiver on the board |

## What it does

GPS shows what the position receiver is doing and, when it has no position,
why. It tells four states apart: READER OFF, LISTENING, NO FIX and FIX. The
receiver starts when you open the app and keeps running when you leave, so MAP
and the other FIELD apps can use the fix.

GPS also records your track. A point is kept once you have moved 10 metres or
30 seconds have passed, whichever comes first. Points go to
`/sdcard/lakeshark/track.log`, and OPTIONS exports them as a GPX file.

## Radio

GPS uses the board's own GPS receiver. It has no RADIO picker. RADIOS can
switch the receiver off to save power.

## How to use it

| Key | Touch | Action |
|---|---|---|
| G | GPS | Turn the receiver on or off |
| R | TRACK | Start or stop track recording. This starts the receiver too |
| M | TO MAP | Open MAP at your position |
| V | The top line | Switch between SKY and DETAILS |
| Arrows, J, K | PREV, NEXT or a satellite | Select a satellite in SKY |
| O | [ OPTIONS ] | Open OPTIONS |
| Esc | | Back to HOME |

Recording continues after a restart until you stop it.

OPTIONS holds:

- **Export GPX**: writes the saved track to `/sdcard/lakeshark/track-NN.gpx`
  and shows the point count and file name. Stop recording first.
- **Minimum movement**: 0 to 1000 metres. 0 turns the distance rule off.
- **Maximum gap**: 0 to 3600 seconds. 0 turns the time rule off.
- **Clear saved track**: asks you to confirm. Stop recording first.
- **Stop recording / cancel resume**: stops recording and stops it starting
  again after a restart.

In a list, Esc or BACK goes up one level.

## What the screen shows

The header shows the receiver state with satellite counts (used, tracked and
seen), the track state (RECORDING, WAIT FIX or STOPPED) with its point count,
and the time of the last saved point.

**SKY** is a north-up plot of the local sky. The rim is the horizon, the middle
ring is 45 degrees and the centre is overhead. Each satellite has a letter.
Green is used in the fix, amber is seen, cyan is selected. A table lists
each satellite's ID, azimuth, elevation, signal (C/N0) and whether it is used.
SELECTED gives the details of one satellite.

**DETAILS** shows:

- The state in large type: READER OFF, LISTENING, NO FIX or FIX.
- **POSITION**: latitude, longitude, altitude, speed and course, HDOP, UTC
  and antenna state. With no fix it shows a one-line reason instead.
- **SKY**: satellite counts, the constellations being tracked and the best
  signal levels.
- **TRACK**: state, points stored (up to 4096) and mesh nodes met.
- **WIRE**: bytes, sentences, bad checksums and time to first fix.

## Tips and limits

- The reason line points at the cause. "Nothing on the wire" means no data is
  arriving from the receiver. "Sentences are good, no position yet" means the
  receiver is working and needs a clearer view of the sky.
- ANTENNA OPEN or ANTENNA SHORT means the receiver reports an antenna fault.
- A stopped receiver keeps its last fix, but the screen shows `--` because it
  is not a current position.
- Track recording needs an SD card.

## More guides

- [Field map](../map-field.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
