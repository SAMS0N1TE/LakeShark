# USER

Your own screens, loaded from the SD card.

| | |
|---|---|
| HOME group | USER |
| Radio | None of its own. Shows values from the apps that use the radios |

## What it does

The USER group on HOME holds apps you write yourself. Each one is a short text
file with the `.lsapp` extension in `/sdcard/apps`. The firmware reads the
files at boot and adds one tile per file, with the name, icon and colour the
file asks for.

A user app is a layout, not a program. It names panels and the live values to
show in them, and the firmware draws it. It cannot tune, transmit, decode,
scan or write to the card, so it cannot upset the radio. No build tools are
needed: copy a text file to the card and restart.

## Radio

None. A user app reads the same values the built-in apps publish, such as the
P25 talkgroup, the FM frequency or the GPS fix. A value with no current
reading shows as `--`.

## How to use it

1. Write a `.lsapp` file. The example below is a good start.
2. Copy it to `/sdcard/apps` on the SD card.
3. Restart the board. Open HOME and press F4, or tap USER.
4. Open the tile like any other app. Esc returns to HOME.

```
version=1
name=SCAN DESK
sub=my layout
icon=tower
color=green

panel=RECEIVER
value=FREQ|p25.freq
value=NAC|p25.nac
value=TALKGROUP|p25.tg
value=SOURCE|p25.src
bar=SIGNAL|p25.level

panel=SPECTRUM
waterfall=
text=tap a bin to read its frequency
```

The same file is in the repository as `components/apps/tui/lsapp.example`.

## Fields

| Field | Meaning |
|---|---|
| `version=1` | Required. Must be the first field |
| `name=TEXT` | Required. The tile and screen title |
| `sub=TEXT` | One line under the name on the tile |
| `icon=NAME` | `tower` `wave` `plane` `record` `falls` `map` `files` `gear` `pager` `chip` `star` `shark` |
| `color=NAME` | `red` `green` `yellow` `blue` `magenta` `cyan` `white` |
| `panel=TITLE` | Starts a framed panel. Everything after it goes inside |
| `value=LABEL\|path` | A label and a live value |
| `bar=LABEL\|path` | A value from 0 to 1 drawn as a meter |
| `text=TEXT` | A fixed line of text |
| `waterfall=` | The shared waterfall, filling the rest of the panel |

Blank lines are skipped. Lines starting with `#` or `;` are comments. Both
CRLF and LF line endings work.

## Values you can show

| Path | What it is |
|---|---|
| `p25.freq` `p25.nac` `p25.tg` `p25.src` | P25 frequency, NAC, talkgroup and source unit |
| `p25.mod` `p25.sync` `p25.level` `p25.gain` | Demodulator, frame sync, IQ level (0 to 1) and gain |
| `fm.freq` `fm.mode` `fm.gain` `fm.level` | FM tuning, mode, gain and IQ level (0 to 1) |
| `fm.squelch` `fm.sql` | Squelch open, and the squelch setting in percent |
| `fm.pages` `fm.pocsag` | Decoded pages and POCSAG sync |
| `adsb.aircraft` | Aircraft being tracked |
| `decode.rate` `decode.good` `decode.err` | Messages per second and CRC counters |
| `rf.state` `rf.bytes` `rf.recoveries` | SDR state, throughput and recoveries |
| `gps.on` `gps.fix` `gps.sats` `gps.lat` `gps.lon` | GPS power, fix, satellites and position |
| `track.on` `track.points` | Track recorder state and point count |
| `mesh.up` `mesh.node` `mesh.rx` `mesh.tx` `mesh.rssi` `mesh.armed` | MESH node state, counters and last signal |
| `sys.internal` `sys.psram` `sys.uptime` `sys.volume` | Free memory in KB, uptime in seconds, volume in percent |

## What the screen shows

The panels you named, with their values updating live. Two panels sit side
by side in landscape and stack in portrait. A third or later panel shares the
second half of the screen.

## Tips and limits

- A file is used whole or not at all. One bad line rejects the file, and HOME
  shows the file name, line number and reason in the USER group. Tap the
  message, or press E, to read it in full.
- One bad file does not stop the others from loading.
- A file larger than 8 KB is refused. So is a file with more than 48
  elements, or a label or path longer than 31 characters.
- With no card, or no `/sdcard/apps` folder, the USER group is simply empty.

## More guides

- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
