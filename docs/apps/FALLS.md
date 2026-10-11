# FALLS

A waterfall over the receiver you choose: what the band looked like over the last minute, not just now.

| | |
|---|---|
| HOME group | RADIO |
| Radio | RTL-SDR, HackRF or the LoRa chip for a spectrum; other radios show their readings |

<img src="../screenshots/tdp4/motion/falls-flipper.gif" width="300" alt="FALLS showing a waterfall at 433.92 MHz">

## What it does

- Draws a live spectrum above a scrolling waterfall.
- With an SDR, shows the band of the receiver that is running: FM or P25.
- With the LoRa chip, sweeps a band such as the mesh channel, US915, EU868, 433 MHz or 315 MHz.
- Marks a signal and tunes the receiver to it.
- For radios that cannot draw a spectrum, shows what they measure instead.

FALLS does not start a radio by itself. Choosing a radio in RADIO starts it.

## Radio

RADIO on R (or V) opens the radio picker. The choice is remembered.

| Radio | What FALLS shows |
|---|---|
| RTL-SDR or HackRF | The spectrum of the FM or P25 receiver |
| LoRa chip | A swept spectrum of the chosen band. The mesh pauses while it sweeps |
| CC1101, nRF24, NFC | Readings from the T-MixRF keyboard board, once its monitor runs in MIX-RF |
| Wi-Fi, Bluetooth | Link readings |
| GPS | A sky view with each satellite's signal level |

Every LoRa chip offers mesh watch, US915 ISM, EU868, 433 ISM, 315 remotes and full range (150-960 MHz). The LR2021 adds wider bands such as aviation, GPS L1, Iridium and 2.4G ISM.

## How to use it

The buttons along the top change with the radio:

| Key | Button | Action |
|---|---|---|
| R or V | RADIO | Choose the receiver |
| N | BAND | Choose a band, or a P25 control channel from a loaded profile |
| E | MODE | What the SDR listens as: NFM, WFM, AM, POCSAG, FLEX, ACARS or P25 |
| T | TUNE | Type a frequency (FM) |
| W | SWEEP | Start or stop the FM band sweep |
| O | OPTIONS | Waterfall display settings |

MODE appears with an SDR. TUNE and SWEEP appear when FM is the source. In the GPS view the buttons are RADIO, PREV (K) and NEXT (J) to step through satellites.

The waterfall controls:

| Key | Button | Action |
|---|---|---|
| S | SPLIT | Share of the screen for the spectrum |
| I | REF | Reference level. Up and Down also step it |
| G | RANGE | Range of the colour scale |
| P | COLOR | HEAT, ICE, PHOS or NEON |
| A | AVG | Average several sweeps per row |
| D | SPEED | Scroll speed |
| K | PEAK | Peak hold |
| H | HOLD | Freeze the picture. Enter also toggles it |
| F | DETAIL | shade, ascii, dense or bars |
| C | CNTRST | soft, norm, hard or max |
| Y | TUNE MARK | Tune to the marker (TUNE in landscape) |

Tap a signal to place the marker, or use Left and Right. Space tunes to the marker. M shows or hides the marker. In landscape, B hides the waterfall buttons and shows them again.

TUNE MARK sends the frequency to the FM or P25 receiver. On a LoRa sweep it opens LORA LABS on that frequency with DIRECT on.

### OPTIONS

O opens WATERFALL OPTIONS with REF, RANGE, CONTRAST, COLOR, DETAIL, AVG, SPEED, PEAK, SPLIT and HOLD. BACK goes up one level. The data views have no OPTIONS.

## What the screen shows

- The spectrum, with the marker frequency and level and the peak.
- The waterfall below it, newest at the top.
- The frequency scale along the bottom edge.
- A notice when no receiver is producing a spectrum, with what to do next.
- In a data view: SOURCE, FREQ, SIGNAL in dBm, COUNT and a signal bar.

## Tips and limits

- To see the FM or P25 band, start that receiver first or pick the SDR here.
- A LoRa sweep is an energy sweep, not decoded traffic. Brief packets can fall between looks.
- P25 on the LR2021 gives no spectrum.
- The waterfall settings are shared with the waterfalls in FM and P25.

## More guides

- [Controls and navigation](../UI_CONTROLS.md)
- [LoRa Labs and Notes](../FIELD_LABS.md)
- [T-Display-P4 screens](../TDP4_SCREENS.md)
- [All apps](README.md)
