# LORA LABS

A measurement tool for the LoRa chip: packet capture, a spectrum sweep, a bearing plot and FSK and pager receive.

| | |
|---|---|
| HOME group | RADIO |
| Radio | The LoRa chip (SX1262 or LR2021) |

## What it does

LORA LABS takes the LoRa radio from MESH while DIRECT is on and runs it with its own settings. MODE (M) picks what it does:

| Mode | What it does |
|---|---|
| PACKETS | Channel RSSI over time and a hex preview of matching LoRa packets |
| SPECTRUM | Energy swept around the frequency, or across a chosen span |
| BEARING | Signal strength in 36 magnetic heading bins, on a polar plot |
| GFSK | Receive only FSK with your own bitrate, deviation, bandwidth and sync word |
| POCSAG | Receive only pager decoding, with a list of pages |

MARK keeps an observation in the field journal. REC logs readings to the SD card.

## Radio

LORA LABS always works the LoRa chip. RADIO on R opens the picker, which explains this for the other radios. Frequencies run from 150 to 959 MHz. On the LR2021, BAND also offers HF RX 1900-2200 MHz and HF RX 2400-2500 MHz. SEND shows RX ONLY above 960 MHz.

Leaving LORA LABS gives the radio back to MESH.

## How to use it

The top row:

| Key | Button | Action |
|---|---|---|
| D | DIRECT | Take the radio (ON) or leave MESH in control (OFF) |
| M | MODE | Step through the modes |
| S | SETUP | Settings for the current mode |
| B | BAND | Choose a starting frequency or a sweep span |
| T | SEND | Type and send one LoRa packet (PACKETS or BEARING, DIRECT on) |
| J | MARK | Save this observation to the journal |
| E | REC | Start or stop recording |
| R | RADIO | Radio picker |
| O | OPTIONS | The current mode's settings |

The second row:

| Key | Button | Action |
|---|---|---|
| C | CRC | CRC on or off |
| I | IQ | NORMAL or INVERT |
| P | SYNC | PRIVATE (0x12) or PUBLIC (0x34) |
| H | HOLD | Freeze the plot. The compass and receiver stay live |
| X | CLEAR | Clear the plot |
| K | CAL | Calibrate the compass |

Arrow keys move between buttons and Enter presses one. In POCSAG, Up and Down move through the pages, and a tap on a page selects it.

BAND offers mesh US, mesh EU, US915 low, mid and high, EU868, 433 ISM, 315 remotes and VHF pager. In SPECTRUM on the LR2021 it also offers sweep spans such as aviation, GPS L1, Iridium and 2.4G ISM.

### BEARING and the compass

BEARING plots signal strength against the board's magnetic heading. CAL starts a guided compass calibration that walks through each orientation and a figure eight. EXPAND (V) opens a full screen compass with BACK, VIEW, MARK and CAL. Esc or V closes it.

### OPTIONS

O opens the settings for the mode in use. BACK goes up one level.

- PACKETS and BEARING: FREQUENCY, SPREAD FACTOR, BANDWIDTH, CODING RATE, POWER, PREAMBLE, SYNC WORD, CRC and INVERT IQ.
- SPECTRUM: FREQUENCY.
- GFSK: FREQUENCY, BITRATE, DEVIATION, BANDWIDTH, SYNC WORD and PAYLOAD.
- POCSAG: FREQUENCY, BAUD (1200 or 2400, plus 512 on the LR2021) and POLARITY (NORMAL or INVERTED).

## What the screen shows

- The frequency and radio settings in use.
- MESH PAUSED or MESH CONTROL, with received, bad and sent packet counts.
- The plot for the mode: the RSSI trace, the spectrum with its peak, the polar bearing plot, or the page list.
- In BEARING, the heading and whether the compass is calibrated.
- LIVE CONTEXT on tall screens: owner, RSSI, GPS and heading.
- The last packet bytes, or the number of pages decoded this session.

## Tips and limits

- Frequency, spreading factor, bandwidth, coding rate, preamble, sync word, CRC and IQ must all match the transmitter to see its packets.
- SPECTRUM is an RSSI sweep, not decoded traffic. Brief packets can fall between looks.
- BEARING is an experimental plot, not a measured transmitter bearing.
- SPECTRUM, GFSK and POCSAG never transmit.
- The chip's tuning range is not the antenna's useful range.
- A LoRa sweep in FALLS can hand a marked frequency to LORA LABS with DIRECT on.

## More guides

- [LoRa Labs and Notes](../FIELD_LABS.md)
- [LR2021 native POCSAG receive](../POCSAG_NATIVE_STREAM.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
