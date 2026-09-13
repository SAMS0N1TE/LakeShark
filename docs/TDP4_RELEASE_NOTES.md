# LakeShark release notes

## 2.1.0-rc1 — T-Display P4

- LoRa Labs with direct radio controls and live plots.
- Rotating compass with an expanded view and large heading display.
- Received-signal history and heard Mesh peer lists.
- Journal notes with radio, GPS and nine-axis attachments.
- Keyboard CC1101, nRF24 and NFC integration.
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

## Optional LCD-4.3 build

The Waveshare Touch-LCD-4.3 configuration is experimental and available for owners who want to try it. It uses its own 32 MB flash layout. See the [LCD guide](LCD43_QUICKSTART.md).
