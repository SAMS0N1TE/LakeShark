# CELL WATCH

Compare cellular downlink power with a learned baseline for this site, and search for LTE cells.

| | |
|---|---|
| HOME group | RADIO |
| Radio | RTL-SDR for the survey; HackRF for HIGH RATE |

## What it does

- Sweeps one cellular downlink band and plots the power across it.
- Learns a baseline for the site from three complete passes and saves it to the SD card.
- Compares later passes with that baseline and flags power that rises and stays.
- Searches a band for LTE cells and lists each one's physical cell identity (PCI).
- Can send short reports over LoRa to a chosen MeshCore contact.
- HIGH RATE restarts the board into a HackRF capture mode that also checks the LTE broadcast block (MIB).

CELL WATCH only listens to broadcast downlink signals. It does not read subscriber traffic, decode network identity or decode SIB messages.

## Radio

RADIO on R opens the radio picker. The survey and the LTE search run on the RTL-SDR. HackRF is used in HIGH RATE mode.

Bands:

| Band | Downlink |
|---|---|
| 850 DL / B5 | 869-894 MHz |
| 900 DL / B8 | 925-960 MHz |
| 700 DL / B12 | 729-746 MHz |
| 700 DL / B13 | 746-756 MHz |
| 800 DL / B20 | 791-821 MHz |

## How to use it

| Key | Button | Action |
|---|---|---|
| S | START / STOP | Start or stop the repeating survey |
| B | BAND | Choose a band |
| L | LEARN | Take three passes and save a baseline |
| D | LOAD | Load a saved baseline from the SD card |
| G | SITE | GPS or LOCAL |
| H or Esc | HOME | Leave CELL WATCH |
| T | REPORT | Choose a LoRa report destination, or Reports OFF |
| E | LTE | One pass of LTE cell search over the band |
| P | HIGH RATE | Offer a restart into HIGH RATE |
| R | RADIO | Choose the receiver |
| O | OPTIONS | Survey settings |

Arrow keys move between buttons and Enter presses one. Every button can be tapped. BAND, LEARN, LOAD, SITE, LTE and HIGH RATE wait until the survey is stopped.

1. Choose a band.
2. Choose SITE: GPS uses a fresh fix to pick the baseline for this place. LOCAL ignores GPS, which helps indoors.
3. Hold the board still and press LEARN. It takes three complete passes.
4. Later, press LOAD or START at the same place to compare against the baseline.

### OPTIONS

O opens CELL OPTIONS. BACK goes up one level.

- BAND and SITE.
- LORA REPORTS: the report destination.
- RECENT SD LTE: the last saved LTE cells from the card.
- REPORT TEST / HEALTH: sends a test report and shows the report status.

### HIGH RATE

After the restart the buttons are AUTO (S), ONCE (C), CHANNEL (B), VIEW (V), OPTIONS (O) and EXIT (H). AUTO repeats captures on the selected channel or on six preset channels. ONCE takes one capture. EXIT restarts back to the normal firmware. HIGH RATE OPTIONS sets AUTO PLAN, CHANNEL, sample rate and capture length.

## What the screen shows

- The band, the state (READY, RECEIVING or CAPTURE HELD) and the frequency in large digits.
- A progress bar with tunes completed.
- RF POWER / LAST COMPLETE PASS: the spectrum from -110 to -30 dBFS, with the baseline as dots.
- The baseline state, GPS or LOCAL, and whether the board is still enough to compare.
- A table of frequency, power, rise over the baseline, and REVIEW for persistent changes.
- In LTE search: CELL SYNC / SSS MATCH, then each cell's frequency, PCI, repeats and match quality.

## Tips and limits

- A flagged change means the RF level moved. It does not identify a cell site simulator.
- Power is uncalibrated dBFS, not dBm.
- Use the same receiver and antenna for the baseline and later passes. Learn again after changing either.
- PCI is reused across networks. It is not a unique cell identity.
- LTE search covers FDD with normal cyclic prefix. GSM and 5G NR are not searched.
- LTE detections are saved to `/sdcard/cell/lte.jsonl`. A report carries a position only with a fix under five seconds old and HDOP of five or better.

## More guides

- [Cell Watch](../CELL_WATCH.md)
- [Cell detector focus](../CELL_DETECTOR_FOCUS.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
