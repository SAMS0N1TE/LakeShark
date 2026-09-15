# LakeShark release notes

## 2.1.0 — T-Display P4

- GPS coverage filtering for saved scan channels.
- Mixed conventional Phase I P25 and analog FM scan lists.
- CSV/JSON list imports over USB or SD; optional RadioReference adapter.
- Safer channel-list saving and transactional imports.
- Bluetooth discovery duplicate filtering and burst recovery.
- Expanded P25 settings, including AUTO/manual demodulation and CQPSK tuning.
- Experimental manual P25 Phase II voice reception, off by default.
- Separate channel-list and stepped-band scanning controls.
- Tap-to-mark waterfall tuning.
- Unified P25 radio/decoder page with the large frequency readout, scan
  controls, and retained NAC/talkgroup receive history.
- Reworked FM receiver controls with mode and band pickers, an integrated
  waterfall, and a carrier lock that survives a sweep and restores the exact
  custom frequency afterward.
- Explicit NFM selection is analogue-only and clears inherited mixed P25
  demodulation.
- More reliable simultaneous Wi-Fi, MeshCore, USB receiver, touch, and audio
  startup on T-Display P4; the web console has additional stack margin.
- Shared RTL/CC1101 recorder workspace and repeated OOK24 decoding.
- Single-tap app opening without a blocking launch splash.
- Buttons inset around the display's rounded corners.
- Reliable LoRa/Mesh startup with a USB receiver attached.
- USB Phase II capture replay and on-device performance diagnostics.
- ADS-B Scope original implementation credit: John Stockdale (@jstockdale) and Off by One, Inc.; reference for single-frame CPR decoding.

P25 AUTO demodulation selects C4FM or CQPSK using valid protocol results.
Phase II grant/slot recognition is present. Experimental voice reception requires
a manually selected traffic channel and slot; automatic Phase II call following
and live RF validation remain pending. See [Phase II testing](P25_PHASE2.md).

## 2.1.0-rc1 — T-Display P4

- LoRa Labs with direct radio controls and live plots.
- Rotating compass with an expanded view and large heading display.
- Received-signal history and heard Mesh peer lists.
- Journal notes with radio, GPS and nine-axis attachments.
- Keyboard CC1101 energy monitor, nRF24 activity scan and NFC reader.
- NFC Classic reader with block maps, hex views and manual saves.
- Passive Sub-GHz watch with bounded storage and duplicate grouping.
- Grouped launcher and improved portrait touch controls.
- Animated map markers and live activity feedback.
- More stable rotation without the pixel wipe.
- Mesh notification and radio-restoration fixes.
- Compact P25 waterfall status and corrected shortcuts.
- ADS-B position-decoding fixes.
- Experimental HackRF USB reception support.

## 2.0.1

- Direct FM mode selection and frequency step controls.
- AM reception and separate band, mode and sweep controls.
- POCSAG message navigation in list and detail views.
- Live FM waterfall with a signal-level graph and optional band sweeps.
- Full-width waterfall, thin traces and faster live updates.

## T-Display-P4

The primary target is the LilyGO T-Display-P4 with the 4.1-inch, 568 x 1232 RM69A10 AMOLED and 16 MB flash.

- Touch and keyboard controls with automatic screen rotation.
- P25 Phase 1, FM, POCSAG, ADS-B and sub-GHz recording.
- MeshCore, LoRa spectrum view, GPS and SD-card maps.
- LINK app for Wi-Fi connections and Flipper Bluetooth controls.
- Flipper app selection opens the matching screen. POCSAG opens the FM pager page.
- Larger keyboard touch targets, key color feedback and optional password visibility.
- Brightness, themes and Daylight mode in SET.

See [first flash](TDP4_FIRST_FLASH.md) and [screens](TDP4_SCREENS.md).

## Headless boards

ESP32-P4-NANO and ESP32-P4-WIFI6 configurations provide serial and Flipper control without a display.

## Flipper Zero

The LakeShark control app supports UART and Bluetooth connections, receiver selection, tuning, volume and recording transfer.

## LCD-4.3 status

The Waveshare Touch-LCD-4.3 target is not supported by this release and no RC
binary is provided for it. Its files remain in the source tree for possible
future work; do not flash the T-Display-P4 image to that board.
