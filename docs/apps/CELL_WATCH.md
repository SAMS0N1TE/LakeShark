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

Bands (the RTL-SDR windows BAND offers):

| Band | Window | EARFCN | Where it is a downlink |
|---|---|---|---|
| 850 DL / B5 | 869-894 MHz | 2400-2649 | Americas, including Argentina |
| 900 DL / B8 | 925-960 MHz | 3450-3799 | Europe, Asia, Africa |
| 700 DL / B12 | 729-746 MHz | 5010-5179 | US |
| 700 DL / B13 | 746-756 MHz | 5180-5279 | US |
| 800 DL / B20 EU | 791-821 MHz | 6150-6449 | Europe only |
| 700 DL / B14 | 758-768 MHz | 5280-5379 | US |
| 700 DL / B17 | 734-746 MHz | 5730-5849 | US |
| 850 DL / B26 | 859-894 MHz | 8690-9039 | US |
| 600 DL / B71 | 617-652 MHz | 68586-68935 | US |
| 700 DL / B28 lo | 758-783 MHz | 9210-9459 | APT 700: Argentina, Brazil, Chile and others |
| 700 DL / B28 hi | 778-803 MHz | 9410-9659 | APT 700 |

B28 is 45 MHz, wider than one survey window holds, so it is two windows that share 778-783 MHz. In Argentina the B28 carriers are Movistar 758-768, Personal 768-778 and Claro 778-793 MHz; survey B28 lo and B28 hi, not B20. In the Americas 791-803 MHz is B28 and 806-821 MHz is phones transmitting (uplink), so B20 EU there shows bursts of uplink, not cells.

Bands overlap between regions: 758-768 MHz is B14 in the US and B28 in Argentina. A frequency is labelled by the window being surveyed; where there is no window (HIGH RATE) both bands are shown, as in `B14 5330/B28 9260`.

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
- RTL GAIN: 7.7, 14.4, 19.7, 25.4, 29.7 (the usual) or 37.2 dB, for the survey and the LTE search. A baseline is compared only at the gain it was learned at, so LEARN again after changing it.
- LTE SCAN (GATED or FULL), LTE MIB, DONGLE PPM and ALL CELLS: the LTE search.
- LORA REPORTS: the report destination.
- RECENT SD LTE: the last saved LTE cells from the card.
- REPORT TEST / HEALTH: sends a test report and shows the report status.

### HIGH RATE

After the restart the buttons are AUTO (S), ONCE (C), CHANNEL (B), VIEW (V), OPTIONS (O) and EXIT (H). AUTO repeats captures on the selected channel or on all nine preset channels: 739.0, 751.0, 881.5, 1981.25, 1992.5 and 2150.0 MHz for the US, and 763.0, 773.0 and 785.5 MHz for Argentina's B28 carriers (763.0 is also the US B14 carrier). 881.5 MHz is the middle of B5 in both countries. ONCE takes one capture. EXIT restarts back to the normal firmware. HIGH RATE OPTIONS sets AUTO PLAN, CHANNEL, sample rate and capture length.

## What the screen shows

- The band, the state (READY, RECEIVING or CAPTURE HELD) and the frequency in large digits.
- A progress bar with tunes completed.
- RF POWER / LAST COMPLETE PASS: the spectrum against this pass's noise floor, from about 6 dB below it to 54 dB above. The floor is the dotted line and its level is in the legend. Within 5 dB of the floor the trace is faint: that is the receiver's own noise. Carriers found on the pass are green, other signals above the noise cyan, and the baseline, where it held more than noise, is yellow dashes.
- `RTL overloaded at 786 MHz - lower gain`, in red, when the receiver clipped during the pass. That pass is neither learned nor compared. Lower OPTIONS > RTL GAIN and LEARN again at that gain.
- The baseline state, GPS or LOCAL, and whether the board is still enough to compare.
- A table of frequency, power, rise over the baseline, and REVIEW for persistent changes.
- In LTE search: CELL SYNC / SSS MATCH, then each cell's frequency, PCI, repeats and match quality.

## Tips and limits

- A flagged change means the RF level moved. It does not identify a cell site simulator.
- Power is uncalibrated dBFS, not dBm.
- Use the same receiver, antenna and gain for the baseline and later passes. Learn again after changing any of them.
- PCI is reused across networks. It is not a unique cell identity.
- LTE search covers FDD with normal cyclic prefix. GSM and 5G NR are not searched.
- LTE detections are saved to `/sdcard/cell/lte.jsonl`. A report carries a position only with a fix under five seconds old and HDOP of five or better.

## More guides

- [Cell Watch](../CELL_WATCH.md)
- [Cell detector focus](../CELL_DETECTOR_FOCUS.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
