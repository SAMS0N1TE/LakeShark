# SUB-GHZ

Watch the sub-GHz bands, find a transmitter, keep the patterns it sends and
decode them where the encoding is known.

| | |
|---|---|
| HOME group | RADIO |
| Radio | READ: RTL-SDR, HackRF, CC1101 (keyboard board) or the LoRa chip. BAND sweeps with the LoRa chip; CATCH uses the RTL-SDR and the LR2021 |

## What it does

SUB-GHZ has three places, on three tabs along the bottom (beside the buttons
in landscape):

| Tab | Key | What it is for |
|---|---|---|
| READ | 1 | Listen on one frequency and keep each pattern heard. LEARN and RECORD run here |
| BAND | 2 | Find a transmitter's frequency: the sweep and its detections, or CATCH. LISTEN HERE hands a frequency to READ |
| SAVED | 3 | Caught patterns, and `.sub` files on the SD card |

READ groups repeats of the same pattern and counts how often it was heard.
Known pulse-width and run-length OOK families are decoded, and a repeated
24-bit pattern shows as OOK24. Captures are kept on the card across reboots.

A tab is lit while its place is doing something: READ while WATCH, LEARN or
RECORD has the radio, BAND while a sweep or CATCH runs. SAVED shows a dot
when a recording has landed since you last looked.

## Radio

RADIO on r picks READ's receiver. It is shared with REC, and each receiver
keeps its own frequency.

- **RTL-SDR or HackRF**: raw OOK pulse capture with adjustable capture setup. RECORD keeps one raw capture as a `.sub`.
- **CC1101** on the keyboard board: raw OOK with a fixed profile (650 kHz, 40 us filter, 30 ms end gap). It tunes 300 to 348, 387 to 464 and 779 to 928 MHz.
- **LoRa chip**: 2-FSK receive with your bit rate, deviation, bandwidth, preamble and sync word. LEARN measures them. MESH pauses while WATCH or LEARN runs, and the banner shows MESH PAUSED.
- **LR2021**: as the LoRa chip, and RECORD keeps up to 262 ms of raw I/Q.

The sweep on BAND always uses the LoRa chip.

## How to use it

Tabs: tap one, or press 1, 2 or 3. ESC goes back to READ, and on READ it
leaves SUB-GHZ. TAB, LEFT and RIGHT walk the button bar.

**READ**

- FREQ or F opens TUNE: TYPE A FREQUENCY (24 to 1766 MHz), six presets from 152.600 to 915.000 MHz, or FROM THE LAST SWEEP.
- WATCH or W starts and stops listening.
- SEND or T sends the selected capture once at the power on its face. POWER or + and - change the power.
- UP/DOWN picks a capture; ENTER or C opens it. On touch, tap a row to select it and tap again to open it. A tall narrow screen also shows PREV and NEXT.
- A capture opens a menu: SEND NOW, SEND AT... (pick the power), SAVE .SUB (a Flipper file on the card), PIN or UNPIN, and JOURNAL (a note with GPS and time).
- Tap the pulse trace to read one pulse's level and length.

**LEARN** (L), on the LoRa chip

Tune to the transmitter and press LEARN. READ stays in front: WATCH steps
aside (its button reads PAUSED), and the banner turns magenta, counts down
30 seconds and says "Press the remote", with what it is trying and how far it
has got.

- On the LR2021 one press is usually enough: LEARN listens through the
  chip's I/Q stream and takes a packet whole - the bit rate from its
  preamble, the deviation from its two tones, the sync word read - and stops
  as soon as it has it. A packet with a CRC that checks, or a radio chip's
  default sync word, is taken from the one burst (95%). One with neither
  could as well be a voice set keying up, so the banner asks for a second
  press and takes it when both bursts carry the same word at the same rate
  and deviation (80%); one such burst alone changes nothing. Frames from 600
  to 100 000 bd are heard.
- On the SX1262 keep pressing the transmitter: it looks at each of seven bit
  rates in turn, round after round, and needs the same frame twice at the
  right rate. Presses every second or two are enough.

When it ends, what it measured is applied to the receiver straight away and
WATCH comes back. The line under READ says what it set, with its confidence:
"Learned 2400 bd 18.5k D391D391 76%  Z undo". Tap that line, or press Z, to
put the old settings back. OPTIONS also has Undo LEARN and Apply last LEARN.
If it heard nothing it could use, the line says so and nothing changes. L
again stops it early. Leaving SUB-GHZ stops it. The list legend (I) shows the
last measurement in full.

**RECORD** (D), on the RTL-SDR, HackRF or LR2021

Press RECORD to arm for the next signal over the trigger. READ stays in
front: the banner turns red with RECORDING, "Press the remote", and a one-row
level meter with the trigger marked in yellow, then the edges as they come.
When the burst lands, the line under READ says "Recorded ... in SAVED", and
the `.sub` is in SAVED, where SEND sends it.

RECORD stops WATCH, because WATCH re-arms the same receiver for its list.
WATCH comes back by itself once the recording is on the card (or when the
recording is stopped or gives up). With autosave off the recording is only
in memory until you SAVE it, so WATCH stays off then: press W after saving.
D again stops a recording that has not triggered. Tap the RECORDING chip, or press V, for REC's full view (LEVEL,
RX RATE, ARM and SEND LAST); BACK returns to READ.

**BAND**

- SCAN or N opens SCAN WHERE: around this frequency (+/-150 kHz), around here (+/-1 MHz), 433 MHz, 868 MHz, 915 MHz or VHF 150 to 170. An LR2021 adds 315 MHz, Aviation, GPS L1, Iridium and 2.4 GHz ISM. Picking a band stops WATCH. The sweep runs until SCAN is pressed again.
- BAND or W picks another band, even while a sweep runs. DETECT or T types the threshold, 3 to 45 dB over the floor.
- The chart and the detections share the page: the chart above the list in portrait, beside it in landscape. The detection picked in the list is marked on the chart with a dotted line. On a pane too small for both, VIEW or V, or a tap on the DETECTED counter, switches between them.
- ENTER, or a tap on a detection, opens it: LISTEN HERE, TUNE HERE, or ZOOM IN (sweep +/-150 kHz around it). A tap on a column of the chart, or ENTER with no detections, offers that column the same way.
- **LISTEN HERE** stops the sweep, tunes READ to the frequency, starts WATCH and goes to READ. The frequency in the banner spins to the new figures.
- The gate is the solid red track down the right edge, a finger wide, with its number on it and a dashed line across the chart at the same height. Tap the track at the height you want, or drag on it; the value shows while you hold it ("Detect above N dB over the floor"). A tap anywhere else on the chart never moves it. The track always reaches above the tallest column, so a cut-off can be put over a strong transmitter nearby (up to 90 dB over the floor). Columns, the line and the track share one scale: a column that reaches the line is one that triggers.
- Columns rise from the top of the receiver's own noise (two noise spreads over the floor), so a sensitive receiver whose quiet readings scatter, like the LR2021, reads as calm as a flat one until something transmits.
- The bottom edge of the chart says what a hit does. With On detection set to BUZZ + LISTEN it reads ARMED: 1ST HIT > READ while the sweep runs, and on the first hit SUB-GHZ goes to READ listening there.

**CATCH** (X), on BAND, with an RTL-SDR

The RTL-SDR watches a 2.4 MHz slice and the LR2021 decodes each burst it
spots. CATCH or X starts and stops it, and its slice and bursts take BAND's
body. WATCH or H switches between every burst and the chosen channels. RAW
SAVE or E keeps the selected burst's samples. Tap the slice strip for the
channels. ENTER, or a tap on the selected burst, opens it: LISTEN HERE or SAVE
RAW. OPTIONS while CATCH is shown is CATCH's (band, watch, channels, only
433.92). SCAN brings the sweep back. Leaving BAND stops CATCH.

**SAVED**

- UP/DOWN and ENTER, or OPEN or O, opens an entry. RELOAD or U reads the card again. SEND or T sends the selected entry.
- A caught pattern opens the same menu as in READ.
- A RAW `.sub` file opens in the player: PLAY ONCE or P sends it once, POWER- and POWER+ (or - and +) set the power. BACK returns to SAVED.

**OPTIONS** (O) holds the settings for the place in front of you:

- READ: Frequency, Capture setup, Mesh alerts, List legend, LR2021 receive, TX power, After a send; on the LoRa chip Undo LEARN and Apply last LEARN; on a radio that records, Recorder view.
- BAND: Detect threshold, On detection, Spectrum style, Spectrum colour, and Catch (CATCH's own settings).
- SAVED: every row.

- **Capture setup**, RTL-SDR or HackRF: Threshold (0 is automatic), End gap, Minimum pulse, Maximum capture, Minimum edges, and Restore defaults.
- **Capture setup**, LoRa chip: Bit rate, Deviation, Bandwidth, Preamble, Sync word, and Restore defaults (4800 / 25k / 2DD42DD4). The CC1101 profile is fixed.
- **On detection**: NOTHING, BUZZ, or BUZZ + LISTEN (vibrate, then LISTEN HERE on READ).
- **Mesh alerts**: pick a mesh contact to get a message about new patterns, at most one a minute. Mesh transmit must be armed.
- **Spectrum style**: SHADE, ASCII or BARS. **Spectrum colour**: dB BANDS, HEAT, ICE, PHOSPHOR or NEON.

## Keys

| Key | Does | Where |
|---|---|---|
| 1, 2, 3 | READ, BAND, SAVED | everywhere |
| W | WATCH on or off (on BAND: BAND PICK) | READ, SAVED |
| F | TUNE | everywhere |
| T | SEND (on BAND: DETECT) | READ, SAVED |
| + and - | TX power | READ, SAVED |
| L | LEARN start or stop; goes to READ | LoRa chip |
| Z | Undo the last LEARN | LoRa chip |
| D | RECORD arm or stop; goes to READ | RTL-SDR, HackRF, LR2021 |
| V | RECORD's full view (on BAND: VIEW) | RTL-SDR, HackRF, LR2021 |
| N | SCAN start or stop; goes to BAND | everywhere |
| X | CATCH start or stop; goes to BAND | with an RTL-SDR |
| R | RADIO | everywhere |
| O | OPTIONS | everywhere |
| ENTER | Open the selected capture, detection or file | READ, BAND, SAVED |
| ESC | Back to READ; on READ, leave SUB-GHZ | everywhere |

R stays RADIO, as in every app, so RECORD is D, the key READ RAW had.

## What the screen shows

- **Banner**: LOADING, STOPPED, LISTENING, NO RECEIVER, SENDING, LEARNING (with the seconds left), RECORDING or TUNING, and the frequency, which spins to a new value and lights up after LISTEN HERE. The second line shows time listening, captures and how many were heard; the DETECTED counter while there are detections (a tap opens BAND); or what LEARN or RECORD is doing. On the right: IN RAM, SAVING, ON CARD or SD FAIL, and any LOST count.
- **READ list**: [RX] for this boot or [SD] for an earlier one, the capture number, frequency and repeat count. The second line shows the decode, or REPEAT, RAW? or LIMIT. The frame flashes green when LEARN has set the receiver or a recording has landed.
- **PULSE INSPECTOR**: the first 48 edges as HIGH and LOW, with TRUNCATED, OOK24 VERIFIED, REPEAT MATCH or RAW CANDIDATE.
- **BAND chart**: one column per bin, coloured by dB over the noise floor, with a falling peak cap, the red gate line, and the strongest bin at the top. A HIT line flashes when something crosses the gate.
- **DETECTIONS**: frequency, dBm, dB over the floor, times seen and how long ago, strongest first.
- Changing page sweeps a shaded column across the new page, behind its text.

## Tips and limits

- A pattern match is not a device identity. List legend (i) explains the list.
- SEND appears for CC1101 and LoRa chip captures. Power is -9 to +10 dBm (-10 to +10 dBm on the CC1101). Replay works in 300 to 348, 387 to 464 and 779 to 928 MHz, for 6 to 4096 edges up to 10 seconds.
- A JOURNAL note carries the GPS fix from when you kept it, not from when the pattern was first heard.
- SAVED shows the newest 48 `.sub` files.
- Check your local rules before transmitting.

## More guides

- [Sub-GHz decoders](../TDP4_DECODERS.md)
- [Keyboard radio monitor](../MIX_RF.md)
- [Replay and aircraft](../REPLAY_AND_AIRCRAFT.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
