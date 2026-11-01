# DIGI

Decode FT8, FT4 and WSPR from HF single sideband audio: who is on the band now,
how strong they are, and how far away. It only listens.

| | |
|---|---|
| HOME group | RADIO |
| Radio | HF receiver in FM (RTL-SDR or HackRF); no picker of its own |

## What it does

DIGI decodes three weak-signal digital modes:

| Mode | Slot | Decoded |
|---|---|---|
| FT8 | 15 s | At the end of each slot |
| FT4 | 7.5 s | At the end of each slot |
| WSPR | 2 min | At the even minute |

Slots follow UTC. The clock comes from GPS when there is a fix. Without a fix it
uses NTP if the network set the time, or the real-time clock (RTC). The screen
labels the source as GPS, NTP, RTC or NONE.

Each decode shows the signal in dB, the time offset (DT), the audio frequency,
and the message. If the message carries a grid square and the board has a GPS
fix, DIGI also shows the distance in km and the compass bearing to that
station. DIGI never transmits.

Opening DIGI starts listening. It keeps decoding and logging when you leave the
app, until you stop it with S.

## Radio

DIGI has no RADIO picker and does not start a receiver. It decodes the upper
sideband audio from the HF path of the FM app, before squelch and volume. Set up
FM for HF USB at the dial frequency DIGI shows, with a receiver that reaches HF:
an RTL-SDR (direct sampling or the Blog V4 upconverter) or a HackRF.

BAND and MODE change the dial frequency DIGI expects. This build does not
retune the radio for you, so tune FM to that frequency yourself.

With no audio, TEST gives you synthetic FT8 and FT4 traffic so you can check the
screen and the timing without a receiver.

## How to use it

| Key | Touch | Action |
|---|---|---|
| S | START, STOP | Start or stop receiving |
| B | BAND | Next band. Shift+B goes to the previous band |
| M | MODE | Cycle FT8, FT4, WSPR |
| T | TEST | Turn the test signal on or off. Starts DIGI if it is stopped |
| O | OPTIONS | Open OPTIONS |
| Up, Down, wheel | Tap the top or bottom third of the list | Scroll the decode list by three lines |
| Esc | | Back to HOME |

Bands are 160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m and 2m. The
default is 20m with FT8 on 14.074 MHz. FT4 has no dial on 160m or 60m. If you
pick a band that lacks the current mode, DIGI moves to FT8. If you pick a mode
the current band lacks, DIGI moves to the first band that has it.

OPTIONS holds:

- **MODE**: FT8, FT4 or WSPR.
- **BAND**: the band list above.
- **TEST SIGNAL**: OFF or ON.
- **LOG TO SD**: ON by default. Turns the decode log off or on.
- **TIME**: opens a second level.
  - **TIME TRIM**: -2.0 to +2.0 seconds added to the clock, in steps of 0.1.
  - **APPLY DT**: moves the clock by the median DT so decodes centre on zero.
    It needs at least three decodes and says "need 3 decodes" until then.
  - **GPS LAG**: 0 to 900 ms, in steps of 50, subtracted from the GPS time
    because the GPS sentence arrives after its second. The default is 250.
- **CLEAR LIST**: empties the decode list and the count.

In the second level, Esc or BACK goes up one level.

## What the screen shows

**The header** shows the mode and the dial frequency in MHz, then the source:
STOPPED, TEST SIGNAL, the SSB audio source, or NO AUDIO. With audio it also
shows the level in dBFS. Below that are the UTC clock, the time source, a bar
for the slot, and the seconds into the slot (it reads "decoding" while a slot is
decoded). The line at the bottom shows the clock offset dT (red when it is over
1 s), your trim, and your own grid square, or "no fix".

**BAND 0-3 kHz** is the live audio spectrum in braille, with peak marks and a tick
every 500 Hz. In WSPR the 1400 to 1600 Hz range is bracketed. The callsigns from
the last slot are pinned at the audio frequency where they were heard. With no
audio the spectrum stays flat.

**HEARD** is the decode list, newest first, with a divider for each slot that
shows its time, mode and count. New lines are lit for about two seconds.
CQ calls are green. The line on the top edge shows the number of decodes in each
of the last twelve slots. When you scroll back, the corner shows how many newer
lines there are. The list keeps the latest 48 decodes.

**WHERE** appears on a tall screen only, between the band and the list. It plots
each station that has a grid around you by true bearing, with distance on a log
scale (100 km at the centre, 20 000 km at the rim, a ring at 3 000 km). It also
lists the four farthest stations. It needs a GPS fix.

## Tips and limits

- Slot timing matters. Check that the time source reads GPS, or use TIME TRIM and APPLY DT.
- Distance and bearing come from the grid in the message and your GPS fix. Your position is not logged.
- Each decode is appended to `/sdcard/lakeshark/digi/YYYYMMDD.csv` with UTC, mode, dial, audio offset, SNR, DT, message, call, grid, distance and bearing. It needs an SD card.
- From TERMINAL: `digi status|start|stop|mode|band|test|log|trim|lag|clear`. `digi selftest` and `digi wav <file>` decode a test or a 12 kHz 16-bit mono WAV file while DIGI is stopped.

## More guides

- [FM](FM.md)
- [GPS](GPS.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
