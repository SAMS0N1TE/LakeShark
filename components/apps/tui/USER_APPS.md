# Writing your own screen

Put a `.lsapp` file in `/sdcard/apps` and it appears in the directory beside
the built-in apps, with its own icon and colour. Up to eight are loaded at boot.

A user app is **data, not code**. It names panels and the values that go in
them; the firmware draws it. That means it cannot crash the radio, cannot
tune, cannot transmit and cannot write anything - and it also means you do not
need a toolchain, a build, or a matching firmware version to write one. Copy a
text file onto the card.

## An example

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

`components/apps/tui/lsapp.example` is this file, ready to copy.

## The rules

- `version=1` must be the first field. Anything else is refused.
- `name=` is required and is what the tab strip and the tile show.
- Blank lines are ignored; `#` and `;` start a comment.
- CRLF and LF both work.
- **A file is applied whole or not at all.** One bad line rejects the file and
  the reason appears on HOME, with the line number. A half-applied layout is
  worse than a refused one.
- Bigger than 8 KB, more than 48 elements, or a label or path longer than 31
  characters: refused, not truncated. These are the caps in `ls_userapp.h` and
  they have moved once already - read them there rather than trusting this
  line if it matters.
- One bad file does not stop the others loading, and a ninth file is reported
  as dropped rather than silently ignored.

## Fields

| Field | Meaning |
|---|---|
| `version=1` | required, first |
| `name=TEXT` | required |
| `sub=TEXT` | one line under the name in the directory |
| `icon=NAME` | `tower` `wave` `plane` `record` `falls` `map` `files` `gear` `pager` `chip` `star` `shark` |
| `color=NAME` | `red` `green` `yellow` `blue` `magenta` `cyan` `white` |
| `panel=TITLE` | starts a framed panel; everything after it goes inside |
| `value=LABEL\|path` | a label and a published value |
| `bar=LABEL\|path` | the value as a meter, for anything 0..1 |
| `text=TEXT` | a fixed line |
| `waterfall=` | the shared waterfall, in the room left in that panel |

Two panels are laid out side by side in landscape and stacked in portrait -
the same rule the built-in screens use, so you do not have to say which
orientation you meant.

## Values you can read

| Path | What it is |
|---|---|
| `p25.freq` | tuned frequency, MHz |
| `p25.nac` `p25.tg` `p25.src` | network access code, talkgroup, source radio |
| `p25.mod` | demodulator in use |
| `p25.sync` | frame sync held |
| `p25.level` | IQ level, 0..1 - good for `bar=` |
| `fm.freq` `fm.mode` `fm.gain` | FM tuning |
| `fm.level` | IQ level, 0..1 |
| `fm.squelch` | squelch open |
| `fm.pages` `fm.pocsag` | decoded pages, POCSAG sync |
| `adsb.aircraft` | aircraft currently tracked |
| `decode.rate` `decode.good` `decode.err` | messages per second, CRC counters |
| `rf.state` `rf.bytes` `rf.recoveries` | dongle state, throughput, recoveries |
| `sys.internal` `sys.psram` | free memory, KB |
| `sys.uptime` `sys.volume` | seconds, percent |

A path the firmware does not publish, or one the radio has no value for right
now, draws as `--`. It never draws as zero: a zero that is really "no value"
is the exact quiet lie this project keeps finding in its own code.

## What a user app cannot do

Tune, transmit, decode, scan, write NVS, read or write the card, or run code.
The list is short because the format has no way to express any of it - not
because there is a check somewhere that has to stay complete. If you want
behaviour rather than a view, that is a firmware change and a queue task.
