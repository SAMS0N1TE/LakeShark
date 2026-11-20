# SUB-GHZ

Read a remote, record it raw, replay it, and find the frequency of one you
do not know - a Flipper's Sub-GHz app on a large touch screen.

| | |
|---|---|
| HOME group | RADIO |
| Radio | READ: RTL-SDR, HackRF, CC1101 (keyboard board) or the LoRa chip. The FREQUENCY ANALYZER sweeps with the LoRa chip; CATCH uses the RTL-SDR and the LR2021 |

## What it does

SUB-GHZ opens on a menu of five large items, the way a Flipper's Sub-GHz app
does. Each opens a page with one main key, the frequency as a key to tap,
and what was heard:

| Item | Key | What it is for |
|---|---|---|
| READ | 1 | Listen on one frequency. A remote it knows is decoded and listed; tap one to SEND or SAVE it |
| RECORD RAW | 2 | Keep the next burst as a `.sub` file, whatever it is, and replay it |
| SAVED | 3 | Caught patterns, and `.sub` files on the SD card: SEND, SAVE, RENAME, DELETE |
| FREQUENCY ANALYZER | 4 | Find the frequency of a remote you do not know. LISTEN HERE hands it to READ |
| SETTINGS | 5 | Everything else: radio, TX power, LEARN, detection threshold, chart, and the rest |

A beginner needs only the first four and never has to open SETTINGS: every
setting has a default that works.

A page owns what it starts. Leaving READ stops listening, leaving RECORD RAW
disarms it, leaving the analyzer stops the sweep, and leaving SUB-GHZ stops
whichever was running. The menu shows what is running beside each item
(LISTENING, ARMED, SWEEPING) and NEW on SAVED when a recording has landed.

READ groups repeats of the same pattern and counts how often it was heard.
Known pulse-width and run-length OOK families are decoded, and a repeated
24-bit pattern shows as OOK24. Captures are kept on the card across reboots.

## Radio

SETTINGS > Radio (or R) picks the receiver. It is shared with REC, and each
receiver keeps its own frequency.

- **RTL-SDR or HackRF**: raw OOK pulse capture with adjustable capture setup. RECORD keeps one raw capture as a `.sub`.
- **CC1101** on the keyboard board: raw OOK with a fixed profile (650 kHz, 40 us filter, 30 ms end gap). It tunes 300 to 348, 387 to 464 and 779 to 928 MHz.
- **LoRa chip**: 2-FSK receive with your bit rate, deviation, bandwidth, preamble and sync word. LEARN measures them. MESH pauses while READ listens or LEARN runs, and the banner shows MESH PAUSED.
- **LR2021**: as the LoRa chip, and RECORD keeps up to 262 ms of raw I/Q.

The FREQUENCY ANALYZER's sweep always uses the LoRa chip.

## How to use it

Tap an item, or press 1 to 5; UP/DOWN and ENTER work too. Every page has
BACK in its top-left corner; ESC does the same, and on the menu ESC leaves
SUB-GHZ. In portrait the page's main key is the wide one along the bottom;
in landscape it is the last key of the top row. TAB, LEFT and RIGHT walk the
keys.

**READ**

- START (W) listens; the same key reads STOP while it does.
- FREQUENCY (F) shows what it is tuned to and how it listens. Tap it for the presets: 315, 433.92, 868.35 and 915 MHz, two VHF business channels, CUSTOM... (type one, 24 to 1766 MHz) and FROM THE LAST SWEEP. Changing frequency while listening just retunes.
- Each burst becomes a row: [RX] for this boot or [SD] for an earlier one, its number, frequency and repeat count, and under it the decode.
- Tap a row (or UP/DOWN and ENTER) to open the capture.

**A capture**

- SEND (T, or ENTER) transmits it once, at the power set in SETTINGS; + and - change the power.
- SAVE (E) writes it to the card as a Flipper `.sub` file.
- MORE (Q): SEND AT... (pick the power for this send), PIN or UNPIN, and JOURNAL (a note with GPS and time).
- The PULSE INSPECTOR shows its first 48 edges; tap the trace to read one pulse. SETTINGS > Pulse inspector turns it off.
- BACK returns to the list it was opened from. A capture opened from READ goes on listening.

**RECORD RAW**

- RECORD (D) arms for the next signal over the trigger; the key reads STOP while armed, and the banner shows RECORDING with a one-row level meter and the trigger marked in yellow.
- When the burst lands it is saved as a `.sub` and appears in the list on this page, newest first. Tap it to open it.
- It needs an RTL-SDR, a HackRF or the LR2021 (which keeps up to 262 ms of raw I/Q). With another radio the page says so: SETTINGS > Radio.
- SETTINGS > Recorder view (V) is REC's full view (LEVEL, RX RATE, ARM) for setting the trigger; BACK returns here.

**SAVED**

- Caught patterns first, then the `.sub` files on the card (the newest 48). Tap one to open it.
- A caught pattern opens as a capture, above.
- A file opens with SEND (T), RENAME (N) and DELETE (X). DELETE asks first. + and - set the power, LEFT and RIGHT walk its pulses.

**FREQUENCY ANALYZER**

Nothing is received until you press START.

- START (N) sweeps the band on the BAND key; the key reads STOP while it runs. Hold the remote's button and its column jumps.
- BAND (W) picks where: around this frequency (+/-150 kHz), around here (+/-1 MHz), 433 MHz, 868 MHz, 915 MHz or VHF 150 to 170. An LR2021 adds 315 MHz, Aviation, GPS L1, Iridium and 2.4 GHz ISM. Picking a band starts sweeping it, and START uses it from then on.
- The chart and the detections share the page: the chart above the list in portrait, beside it in landscape. On a pane too small for both, V or a tap on the DETECTED counter swaps them.
- ENTER, or a tap on a detection or on a column of the chart, opens it: LISTEN HERE, TUNE HERE, or ZOOM IN (sweep +/-150 kHz around it).
- **LISTEN HERE** stops the sweep, tunes READ to the frequency, starts it listening and goes to READ.
- The gate is the solid red track down the right edge, a finger wide, with its number on it and a dashed line across the chart at the same height. Tap the track at the height you want, or drag on it. SETTINGS > Detect threshold types it, 3 to 45 dB over the floor.
- The frame's title names the chart being drawn: ANALYZER / BARS, ASCII, WATERFALL or LIVE I/Q. SETTINGS > Analyzer chart picks it (U swaps bars and waterfall). LIVE I/Q is the LR2021's own 125 kHz around READ's frequency, on a board with an LR2021 and no SDR; START starts it and STOP stops it like the sweep. Where it is not available the chart is BARS, and says BARS.
- The bottom edge of the chart says what a hit does. With On detection set to BUZZ + LISTEN it reads ARMED: 1ST HIT > READ while the sweep runs, and on the first hit SUB-GHZ goes to READ listening there.

**SETTINGS** (5, or O from any page)

- **Radio**, **Frequency**, **TX power** (kept across a reboot), **After a send** (keep listening, or stay stopped).
- **LR2021 receive**: OOK from its I/Q stream, or FSK packets.
- **Capture setup**, RTL-SDR or HackRF: Threshold (0 is automatic), End gap, Minimum pulse, Maximum capture, Minimum edges, and Restore defaults.
- **Capture setup**, LoRa chip: Bit rate, Deviation, Bandwidth, Preamble, Sync word, and Restore defaults (4800 / 25k / 2DD42DD4). The CC1101 profile is fixed.
- **LEARN a remote**, **LEARN window** (30, 15 or 60 seconds), **Undo LEARN**, **Apply last LEARN**: below.
- **Pulse inspector** on or off, **List legend**.
- **Detect threshold**, **On detection** (NOTHING, BUZZ, or BUZZ + LISTEN), **Analyzer chart** (BARS, ASCII, WATERFALL, LIVE I/Q), **Chart colour** (dB BANDS, HEAT, ICE, PHOSPHOR or NEON).
- **Mesh alerts**: pick a mesh contact to get a message about new patterns, at most one a minute. Mesh transmit must be armed.
- **Recorder view**, **Save to FREQS**, and with an RTL-SDR **CATCH bursts** and **Catch** (its settings).

**LEARN** (SETTINGS > LEARN a remote, or L), on the LoRa chip

Tune READ to the transmitter and start LEARN. READ comes to the front: its
main key reads STOP LEARNING, and the banner turns magenta, counts down the
window and says "Press the remote", with what it is trying and how far it has
got.

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

When it ends, what it measured is applied to the receiver straight away,
and READ listens again if it was listening. The line under READ says what it
set, with its confidence: "Learned 2400 bd 18.5k D391D391 76%  Z undo". Tap
that line, or press Z, to put the old settings back. If it heard nothing it
could use, the line says so and nothing changes. L again stops it early.
Leaving READ stops it. The list legend (I) shows the last measurement in
full.

**CATCH** (X, or SETTINGS > CATCH bursts), with an RTL-SDR

The RTL-SDR watches a 2.4 MHz slice and the LR2021 decodes each burst it
spots. It takes the analyzer's page: CATCH starts and stops it, WATCH (H)
switches between every burst and the chosen channels, RAW SAVE (E) keeps the
selected burst's samples. Tap the slice strip for the channels. ENTER, or a
tap on the selected burst, opens it: LISTEN HERE or SAVE RAW. O while CATCH
is shown is CATCH's own settings. BACK returns to the sweep; leaving the
analyzer stops CATCH.

## Keys

| Key | Does | Where |
|---|---|---|
| 1 to 5 | READ, RECORD RAW, SAVED, FREQUENCY ANALYZER, SETTINGS | everywhere |
| ESC, B | BACK: to the list, to the menu; on the menu, leave SUB-GHZ | everywhere |
| W | START or STOP listening; goes to READ (on the analyzer: BAND) | everywhere |
| F | FREQUENCY | everywhere |
| ENTER | Open the selected capture, detection or file; on a capture, SEND | lists |
| T | SEND | READ, a capture, a file |
| E | SAVE as `.sub` | a capture |
| Q | MORE | a capture |
| N, X | RENAME, DELETE | a file |
| + and - | TX power | all but the analyzer |
| D | RECORD or STOP; goes to RECORD RAW | RTL-SDR, HackRF, LR2021 |
| V | Recorder view (on the analyzer: swap chart and list) | RTL-SDR, HackRF, LR2021 |
| N | START or STOP the sweep; goes to the analyzer | everywhere but a file |
| U | Chart: bars or waterfall | analyzer |
| L | LEARN start or stop; goes to READ | LoRa chip |
| Z | Undo the last LEARN | LoRa chip |
| X | CATCH start or stop; goes to the analyzer | with an RTL-SDR |
| R | RADIO | everywhere |
| O | SETTINGS | everywhere |

R stays RADIO, as in every app, so RECORD is D, the key READ RAW had.

## What the screen shows

- **Menu**: the five items, each with what it is for and, on the right, what it is doing now.
- **Banner** (READ, and the other pages while their job runs): LOADING, STOPPED, LISTENING, NO RECEIVER, SENDING, LEARNING (with the seconds left), RECORDING, SWEEPING or TUNING, and the frequency, which spins to a new value and lights up after LISTEN HERE. The second line shows time listening, captures and how many were heard, or the detections while a sweep runs.
- **READ list**: [RX] for this boot or [SD] for an earlier one, the capture number, frequency and repeat count. The second line shows the decode, or REPEAT, RAW? or LIMIT. The frame flashes green when LEARN has set the receiver.
- **PULSE INSPECTOR** (on a capture): the first 48 edges as HIGH and LOW, with TRUNCATED, OOK24 VERIFIED, REPEAT MATCH or RAW CANDIDATE.
- **Analyzer chart**: one column per bin, coloured by dB over the noise floor, with a falling peak cap, the red gate line, and the strongest bin at the top. A HIT line flashes when something crosses the gate. Columns rise from the top of the receiver's own noise, so a sensitive receiver whose quiet readings scatter, like the LR2021, reads as calm as a flat one until something transmits.
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
