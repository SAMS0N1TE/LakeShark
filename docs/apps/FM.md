# FM

Listen to one analogue channel, decode pagers and data, or sweep a band to see what is transmitting.

| | |
|---|---|
| HOME group | RADIO |
| Radio | RTL-SDR or HackRF; POCSAG can also run on the LoRa chip |

<img src="../screenshots/tdp4/motion/fm-noaa.gif" width="300" alt="FM listening to a weather channel">

## What it does

FM is one receiver with several modes. MODE (E) picks one:

| Mode | What it does |
|---|---|
| NFM | Narrow FM voice, with squelch and optional tone squelch |
| WFM | Broadcast FM |
| AM | AM voice, such as airband and CB |
| POCSAG | Pager decoder, 512, 1200 and 2400 baud |
| FLEX | Pager decoder |
| ACARS | Aircraft data messages |
| SAME | Weather alert headers on the weather channels |
| APRS | Packet stations on the APRS channel |
| AIS | Ship reports on the two AIS channels |

SWEEP (W) is a separate band sweep that shows the power across a whole band.

Calls that pass squelch can be recorded to the SD card and played back in CALLS.

## Radio

RADIO on R opens the radio picker. FM needs an RTL-SDR or a HackRF in the USB-A port. In POCSAG mode the picker also offers the LoRa chip, which decodes POCSAG by itself. The other modes stay on the SDR.

## How to use it

FM opens on the VFO page. The page tabs are:

- **VFO** (1): the receiver, with a waterfall below it when there is room.
- **PAGER** (2): the decoded list. It reads MESSAGES in ACARS, ALERTS in SAME, STATIONS in APRS and VESSELS in AIS. It only appears in a decoder mode.
- **SPECTRUM** (3): the waterfall. Opening it does not start a sweep.
- **SCANNER** (0): the channel scanner and saved lists. Esc or MORE returns to the VFO.

The control row:

| Key | Button | Action |
|---|---|---|
| E | MODE | Choose the mode |
| N | BAND | Choose a band for the sweep and waterfall |
| V | VOLUME | Type a volume, 0 to 100 |
| Q | SQUELCH | Type a squelch level |
| K | LOCK | Hold the current frequency |
| R | RADIO | Choose the receiver |
| O | OPTIONS | Settings for the current mode |
| 5 | CALLS | Recorded calls |
| B | MUTE | Mute on or off |

On the VFO page the quick controls are TUNE (T), VOLUME (+ / -), GAIN (U / J) and SQUELCH (S / A). [ and ] step the frequency: 12.5 kHz in NFM, 100 kHz in WFM, 25 kHz in AM and ACARS, with smaller steps on the CB and 10 m bands. W starts or stops SWEEP.

On VFO and SPECTRUM, Left and Right move the waterfall marker and Space tunes to it. A tap on the waterfall marks a signal.

On a decoded list, Up and Down move through it and Enter opens the selected entry. With touch, the first tap selects and a second tap opens. APRS, AIS and SAME have UP, OPEN / BACK and DOWN buttons.

### OPTIONS

O opens the settings for the mode in use. BACK goes up one level.

- NFM: TONE (TONE SQUELCH with CTCSS or DCS, SHOW DETECTED), "Call archive", SQUELCH and GAIN.
- AM: "Call archive", SQUELCH (carrier level) and GAIN.
- WFM and FLEX: GAIN.
- POCSAG: BAUD (AUTO, 512, 1200 or 2400 on an SDR; AUTO, 1200 or 2400 on the LoRa chip), POLARITY on the LoRa chip, and GAIN.
- ACARS: CHANNEL (131.550, 130.025 or 129.125 MHz) and GAIN.
- SAME: MY LOCATIONS (up to six FIPS codes), ONLY MY LOCATIONS, INCLUDE TESTS and LOG TO SD.
- APRS: FREQUENCY (US 144.390, EU 144.800 or a custom frequency), KEEP STATIONS, MAX DISTANCE, SHOW MESSAGES and SHOW ON MAP.
- AIS: VESSELS (KEEP VESSELS, MAX DISTANCE) and DISPLAY (SHOW NAMES, SHOW ON MAP).

GAIN runs from 0 to 49.6 dB on an SDR. 0 is automatic. MAX DISTANCE needs a fresh GPS fix; 0 shows everything.

## What the screen shows

- **RECEIVER**: the frequency, CARRIER (OPEN or CLOSED), squelch, volume and gain.
- **SIGNAL**: a signal meter with the squelch point, and decoder counts in a pager mode.
- The waterfall with a marker readout and its display buttons.
- The decoded list for the mode: pages, ACARS messages, alerts, stations or vessels.

## Tips and limits

- NFM squelch measures noise quieting in percent. It is not a calibrated dBm level.
- AM squelch is a carrier level in tenths of a percent.
- WFM and the decoder modes have no squelch, so the squelch reading shows `--`.
- A manual tune or mode change stops channel scanning.
- A/B on the scanner panel swaps the active and standby frequencies on one receiver.

## More guides

- [Controls and navigation](../UI_CONTROLS.md)
- [GPS scanning and list imports](../LOCATION_SCAN.md)
- [Updates and calls](../TDP4_UPDATES_AND_CALLS.md)
- [All apps](README.md)
