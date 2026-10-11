# ADSB

Track aircraft heard directly on 1090 MHz, as a list and on an offline mini map.

| | |
|---|---|
| HOME group | RADIO |
| Radio | RTL-SDR, HackRF or LR2021 |

## What it does

- Decodes ADS-B and Mode S messages from aircraft in range.
- Lists up to 16 aircraft with their identity, altitude, speed, direction and age.
- Draws them on an offline mini map around your home or your GPS position.
- Follows the traffic on the map: all aircraft, the selected one, or the nearest one.
- Shows full details for one aircraft, with a short altitude history.
- Can announce new and lost aircraft with a tone or a spoken callout.

## Radio

RADIO on R opens the radio picker. ADSB runs on an RTL-SDR or a HackRF in the USB-A port. A board with the LR2021 can receive 1090 MHz with the LoRa chip's own Mode S receiver. The SX1262 cannot receive 1090 MHz.

## How to use it

The tool row:

| Key | Button | Action |
|---|---|---|
| M | FULL MAP | Open the MAP app on your position |
| H | SET HOME | Choose where home is |
| V | MAP ONLY / LIST | Give the map the whole screen, or bring the list back |
| F | FOLLOW | Step through OFF, FIT ALL, SELECTED and NEAREST |
| + or = | ZOOM+ | Zoom the mini map in |
| - | ZOOM- | Zoom the mini map out |
| R | RADIO | Choose the receiver |
| O | OPTIONS | ADS-B settings |

Below the list are PREVIOUS, DETAILS and NEXT. Up and Down select an aircraft. Enter or DETAILS opens it, and Enter, BACK or Esc closes it again. Up and Down keep working while details are open.

Touch:

- Tap a row in the list to open that aircraft.
- Tap the map to select the nearest aircraft. Tap it again to open it.

### SET HOME

SET HOME offers four ways to set home: your GPS position (fresh fix only), typed latitude and longitude, a place chosen on the map, or the current map center. Home is kept after a restart. GPS is not needed to set it by hand.

### FOLLOW

- **OFF**: the map stays on home.
- **FIT ALL**: keeps every aircraft with a position in view.
- **SELECTED**: keeps the chosen aircraft in the middle.
- **NEAREST**: shows home and the closest aircraft.

With a fitted view, ZOOM+ and ZOOM- set the view a step closer or wider than the fit.

### OPTIONS

O opens the list. BACK goes up one level.

- **RECEIVER**: GAIN on an SDR (0 to 49.6 dB on the RTL-SDR, 0 is automatic; 0 to 113 on the HackRF). On the LR2021: GAIN STEP, BOOST, RX BANDWIDTH, DETECTION (AUTO or FIXED) and DETECT LEVEL.
- **VOICE**: NEW AIRCRAFT, LOST AIRCRAFT and POSITION, each OFF, TONE or SPOKEN, plus VOICE, VOICE LEVEL, SPEAKER and TEST.
- **DISPLAY**: VIEW (LIST + MAP or MAP ONLY), FOLLOW and MAP ZOOM.
- **QUALITY**: STATUS, SOURCE, RATES and VERDICTS for the receiver, and RESET QUALITY.

## What the screen shows

- **AIRCRAFT (\* stale)**: ICAO address, callsign, altitude, speed, direction, age and message count. A star marks an aircraft that has not been heard for a while.
- **TRAFFIC**: active aircraft, messages per second and CRC errors.
- **MINI MAP**: home or your GPS position, the aircraft coloured by altitude (yellow under 5000 ft, green under 20000 ft, cyan above) with trails, and the zoom and width of the view.
- Details: CALLSIGN, CATEGORY, SQUAWK, ALTITUDE, vertical rate, SPEED, HEADING or TRACK, POSITION, LAST SEEN, RX CRC and a recent altitude graph.

In landscape the list and the map sit side by side, with a traffic history along the bottom when there is room.

## Tips and limits

- The mini map needs a map archive on the SD card. Where there are no tiles it says so; FULL MAP opens the map controls.
- Without a saved home, the map uses a fresh GPS fix.
- ADSB does not record aircraft to the card.
- Aircraft without a position report appear in the list but not on the map.

## More guides

- [Scanning, replay and aircraft tracking](../REPLAY_AND_AIRCRAFT.md)
- [Field map](../map-field.md)
- [HackRF](../HACKRF_BRINGUP.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
