# SUB-GHZ

Watch the sub-GHz bands, find a transmitter, keep the patterns it sends and
decode them where the encoding is known.

| | |
|---|---|
| HOME group | RADIO |
| Radio | READ: RTL-SDR, HackRF, CC1101 (keyboard board) or the LoRa chip. ANALYZER: the LoRa chip |

## What it does

SUB-GHZ opens on six tiles, one per job:

| Tile | Job |
|---|---|
| READ | Listen on one frequency and keep each pattern heard |
| ANALYZER | Sweep a band to find a transmitter's frequency |
| SAVED | Caught patterns, and `.sub` files on the SD card |
| READ RAW | One raw capture to a file, with the RTL-SDR or HackRF |
| LEARN | Measure a 2-FSK transmitter's bit rate, deviation and sync word |
| SETTINGS | Every setting, each with its value |

READ groups repeats of the same pattern and counts how often it was heard.
Known pulse-width and run-length OOK families are decoded, and a repeated
24-bit pattern shows as OOK24. Captures are kept on the card across reboots.

## Radio

RADIO on r in READ picks the receiver. It is shared with REC, and each
receiver keeps its own frequency.

- **RTL-SDR or HackRF**: raw OOK pulse capture with adjustable capture setup.
- **CC1101** on the keyboard board: raw OOK with a fixed profile (650 kHz, 40 us filter, 30 ms end gap). It tunes 300 to 348, 387 to 464 and 779 to 928 MHz.
- **LoRa chip**: 2-FSK receive with your bit rate, deviation, bandwidth, preamble and sync word. MESH pauses while WATCH runs, and the banner shows MESH PAUSED.

ANALYZER sweeps with the LoRa chip. Its RADIO button (R on that page) shows
the picker; the other radios say why they cannot sweep.

Stop WATCH before changing the frequency, the receiver or the capture setup.

## How to use it

HOME tiles: tap a tile, press 1 to 6, or use the arrows and ENTER. ESC leaves
SUB-GHZ. On every other page, BACK, B, ESC or Backspace returns to the tiles.
TAB, LEFT and RIGHT walk the button bar.

**READ**

- FREQ or F opens TUNE: TYPE A FREQUENCY (24 to 1766 MHz), six presets from 152.600 to 915.000 MHz, or FROM THE LAST SWEEP.
- WATCH or W starts and stops listening.
- UP/DOWN picks a capture; ENTER, OPEN or C opens it. On touch, tap a row to select it and tap again to open it. A tall narrow screen also shows PREV and NEXT.
- A capture opens a menu: REPLAY (pick the power), SAVE .SUB (a Flipper file on the card), PIN or UNPIN, and JOURNAL (a note with GPS and time).
- Tap the pulse trace to read one pulse's level and length.

**ANALYZER**

- SCAN or N opens SCAN WHERE: around this frequency (+/-150 kHz), around here (+/-1 MHz), 433 MHz, 868 MHz, 915 MHz or VHF 150 to 170. An LR2021 adds 315 MHz, Aviation, GPS L1, Iridium and 2.4 GHz ISM. The sweep runs until SCAN is pressed again.
- BAND or W picks a band. DETECT or T types the threshold, 3 to 45 dB over the floor.
- Drag or tap the red strip on the right edge of the spectrum to set the threshold.
- VIEW or V, or a tap on the DETECTED counter, switches between the spectrum and the detections.
- ENTER or a tap on a detection opens it: CAPTURE IT (tune and start WATCH), TUNE HERE, or ZOOM IN (sweep +/-150 kHz around it).

**SAVED**

- UP/DOWN and ENTER, or OPEN or O, opens an entry. RELOAD or U reads the card again.
- A caught pattern opens the same menu as in READ.
- A RAW `.sub` file opens in the player: PLAY ONCE or P sends it once, POWER- and POWER+ (or - and +) set the power. BACK returns to SAVED.

**LEARN**

Tune to the transmitter first. LISTEN or L measures for 30 seconds while you
keep pressing the transmitter. APPLY or A sets the LoRa chip to receive what
it found.

**OPTIONS** (O) holds the settings for the page in front of you:

- READ: Frequency, Capture setup, Mesh alerts, List legend.
- ANALYZER: Detect threshold, On detection, Spectrum style, Spectrum colour.
- LEARN: Frequency.

SETTINGS lists all nine rows: Frequency, Radio, Capture setup, Detect
threshold, On detection, Mesh alerts, Spectrum style, Spectrum colour and List
legend.

- **Capture setup**, RTL-SDR or HackRF: Threshold (0 is automatic), End gap, Minimum pulse, Maximum capture, Minimum edges, and Restore defaults.
- **Capture setup**, LoRa chip: Bit rate, Deviation, Bandwidth, Preamble, Sync word, and Restore defaults (4800 / 25k / 2DD42DD4). The CC1101 profile is fixed.
- **On detection**: NOTHING, BUZZ, or BUZZ + CATCH (vibrate, tune to it and start WATCH).
- **Mesh alerts**: pick a mesh contact to get a message about new patterns, at most one a minute. Mesh transmit must be armed.
- **Spectrum style**: SHADE, ASCII or BARS. **Spectrum colour**: dB BANDS, HEAT, ICE, PHOSPHOR or NEON.

## What the screen shows

- **Banner**: LOADING, STOPPED, LISTENING or NO RECEIVER, and the frequency. The second line shows time listening, captures and how many were heard, or the DETECTED counter during a sweep. On the right: IN RAM, SAVING, ON CARD or SD FAIL, and any LOST count.
- **READ list**: [RX] for this boot or [SD] for an earlier one, the capture number, frequency and repeat count. The second line shows the decode, or REPEAT, RAW? or LIMIT.
- **PULSE INSPECTOR**: the first 48 edges as HIGH and LOW, with TRUNCATED, OOK24 VERIFIED, REPEAT MATCH or RAW CANDIDATE.
- **ANALYZER spectrum**: one column per bin, coloured by dB over the noise floor, with a falling peak cap, the red threshold line, and the strongest bin at the top. A HIT line flashes when something crosses the threshold.
- **DETECTIONS**: frequency, dBm, dB over the floor, times seen and how long ago, strongest first.

## Tips and limits

- A pattern match is not a device identity. List legend (i) explains the list.
- REPLAY appears for CC1101 and LoRa chip captures. Power is -9 to +10 dBm (-10 to +10 dBm on the CC1101). Replay works in 300 to 348, 387 to 464 and 779 to 928 MHz, not at 406.0 to 406.1 MHz, and for 6 to 4096 edges up to 10 seconds.
- A JOURNAL note carries the GPS fix from when you kept it, not from when the pattern was first heard.
- SAVED shows the newest 48 `.sub` files.
- Check your local rules before transmitting.

## More guides

- [Sub-GHz decoders](../TDP4_DECODERS.md)
- [Keyboard radio monitor](../MIX_RF.md)
- [Replay and aircraft](../REPLAY_AND_AIRCRAFT.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
