# EXPERIMENTS

Radio trials on the LoRa chip that may grow into apps of their own.

| | |
|---|---|
| HOME group | RADIO |
| Radio | The LoRa chip in the socket: SX1262 or LR2021 |

<img src="../screenshots/tdp4/gallery-20261007/experiments-port.png" width="300" alt="EXPERIMENTS list with each trial, its badge and what it listens for">

## What it does

EXPERIMENTS lists radio trials. Each row has a badge that says how far along
it is: IDEA, TRYING or WORKS. Open one to start it and watch its live readout.
One experiment runs at a time. Starting another stops the one that is running.

A running experiment keeps going when you leave the app. When you come back,
EXPERIMENTS opens on its page. The `exp` command in TERMINAL reaches the same
list.

## Radio

Every experiment uses the LoRa chip. The RADIO picker on r lists every radio,
but only the LoRa chip can be chosen here. Other radios show "Experiments run
on the LoRa chip".

While an experiment runs, it borrows the chip from MESH and gives it back when
it stops. It will not start while LORA LABS DIRECT, a sweep, an FSK or OOK
session, or an ADS-B Mode S session is using the chip. The status line says
which one. P25 or ADS-B left listening in the background gives the chip up.

Rows marked "LR2021 only" need an LR2021 module. On an SX1262 they refuse to
start and say so.

## The experiments

| Name | Badge | Listens for |
|---|---|---|
| AVIATION PULSES | TRYING, LR2021 only | 1030 MHz SSR interrogations, 1090 MHz replies and DME pulse pairs |
| BURST SCOPE | TRYING | Burst length, gap and rhythm on one frequency: key fobs, tyre sensors, toll readers |
| CARRIER LEVEL | WORKS | The signal level of one frequency, 150 to 1100 MHz |
| RADIOSONDE | TRYING | DFM-17 weather balloons at 400 to 406 MHz, with their telemetry |
| IRIDIUM BURSTS | TRYING, LR2021 only | Satellite bursts at 1616 to 1626.5 MHz |
| LORA MONITOR | TRYING, LR2021 only | MeshCore or Meshtastic traffic on one US mesh frequency, four spreading factors at once |
| LR433 | TRYING | Tyre pressure sensors, weather sensors and utility meters at 315 to 928 MHz |
| P25 SITE FINDER | TRYING | P25 Phase 1 sites: the NAC and frame type, VHF and UHF |
| PAGER RECON | TRYING | POCSAG paging channels: rate, polarity, signal quality and how many pagers they address |
| RS41 SONDES | TRYING, LR2021 only | Vaisala RS41 weather balloons at 400 to 406 MHz |
| BAND SURVEY | TRYING | What is active, band by band, across everything the chip can reach |
| UAT 978 | TRYING, LR2021 only | 978 MHz UAT aircraft and ground uplinks, with error checks |
| WI-SUN METERS | TRYING, LR2021 only | US Wi-SUN smart meter traffic, 2-FSK modes 1b, 2a and 3 |
| Z-WAVE SNIFFER | TRYING, LR2021 only | US Z-Wave traffic on the R1, R2 and R3 channels |

Some notes on what each one offers in OPTIONS:

- **AVIATION PULSES**: VIEW (1030 SSR, 1090 REPLIES, DME/TACAN), DME PRESET and DME FREQUENCY (962 to 1213 MHz).
- **BURST SCOPE**: PLACE presets for fobs, tyre sensors and toll readers, FREQUENCY, FILTER and THRESHOLD (3 to 40 dB over the floor). Energy and timing only.
- **CARRIER LEVEL**: FREQUENCY.
- **RADIOSONDE**: FREQUENCY (400 to 406 MHz), AUTO SWEEP and FILTER (12, 24 or 36 kHz). Launches are usually near 00Z and 12Z.
- **IRIDIUM BURSTS**: PLAN (the simplex channels or the whole band), DWELL and THRESHOLD.
- **LORA MONITOR**: PRESET (Mesh US, LongFast US, LongFast high SF).
- **LR433**: BANDS (ALL, 315 TPMS, 433, 915, ERT METERS), DWELL (2 to 600 s), UNITS (PSI / F or KPA / C), OOK GAIN, HISTORY and CLEAR. An SX1262 runs the FSK sessions; the OOK and meter sessions need an LR2021.
- **P25 SITE FINDER**: CHANNELS (PROFILE from the P25 profile on the card, or ONE FREQ), FREQUENCY and DWELL.
- **PAGER RECON**: CHANNELS, FREQUENCY, DWELL, MESSAGE TEXT (off by default), PROBE (AUTO or a baud and polarity such as 1200N) and RECEIVE (NATIVE STREAM, PACKET, SIGN STREAM). The stream methods need an LR2021; an SX1262 uses packet receive.
- **RS41 SONDES**: RECEIVER (FREQ MHz, PRESET MHz) and TRACKS (KEEP HOURS 1 to 24, SHOW ON MAP).
- **BAND SURVEY**: one row per band group to switch it on or off, LOOKS PER CHUNK and ABOVE FLOOR. An LR2021 covers 150 to 1100 MHz and 1500 to 2500 MHz; an SX1262 covers 150 to 960 MHz.
- **WI-SUN METERS**: MODE, CHANNEL and FEC.

## How to use it

On the list:

- UP/DOWN picks a row and ENTER opens it. On touch, tap a row to select it and tap it again to open it.
- STOP or X stops the running experiment.
- NOTE or N saves the running experiment's readout to NOTES.
- BACK, B or ESC returns to HOME.

On an experiment's page:

- START/STOP, S or ENTER starts or stops it. Starting it again restarts it, which is how a changed setting takes effect.
- RADIO or R opens the RADIO picker. Stop the experiment first to change it.
- OPTIONS or O opens its settings, when it has any. BACK in the list goes up one level.
- NOTE or N saves the readout to NOTES. This needs an SD card.
- SONDES on RS41 SONDES and SENSORS on LR433 open their history. H does the same.
- BACK, B, ESC or Backspace returns to the list.
- TAB, LEFT and RIGHT walk the button bar; ENTER presses the focused button.

LR433 SENSORS lists every device heard. Open one for its reading history,
then GRAPH or G changes the field shown, NAME or N names it, and OPTIONS sets
an alert threshold. A sensor over its threshold raises a SENSOR ALERT notice and a vibration.

RS41 SONDES lists each flight heard. Open one for its position, battery,
range and bearing, and an altitude profile. RECOVER or G sets the last fix as
the COMPASS target.

## What the screen shows

- **List**: the badge, the name, "LR2021 only" where it applies, an ON marker on the running one, and a one-line summary.
- **Page title**: the name and badge.
- **NEEDS**: in yellow, what the experiment needs to hear anything, such as a sonde aloft or traffic in range.
- **Readout**: the live lines the experiment writes, bright while it runs.
- **Status line**: running with its time, starting, stopping, stopped, or why the last start failed.

## Tips and limits

- RADIOSONDE gives a balloon's range and bearing from HOME when one is saved, otherwise from a GPS fix less than ten seconds old.
- RS41 SONDES range needs a fresh GPS fix on the board.
- Only the experiment that is running uses the chip, so MESH is paused while it runs.
- A NOTE is a snapshot of the readout at that moment, with a time stamp in its title.

## More guides

- [Field and lab guide](../FIELD_LABS.md)
- [LR2021 native POCSAG receive](../POCSAG_NATIVE_STREAM.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
