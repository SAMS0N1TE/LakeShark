# LakeShark release notes

## 2.1.0-rc1 — T-Display P4

This release candidate targets the 4.1-inch LilyGO T-Display P4. It is an
application-only update for an existing installation with the matching 16 MB
partition layout and ESP-Hosted C6 firmware. No partition migration is included.

- LoRa Labs with explicit Mesh/direct ownership, configurable radio controls,
  live plots and a rotating compass. The expanded compass adds degree marks,
  large heading digits, live sensor/GPS details and Journal bookmarks.
- Journal notes with radio, GPS and nine-axis attachments, checksummed SD
  recovery, bounded recent history and capped sample logs.
- MIX-RF support for the keyboard's CC1101, nRF24 and ST25R3916. Energy monitors
  and observations can continue in the background and attach to Journal.
- NFC workbench with actual card identification, known-key Classic reads,
  per-block status, hex/ASCII inspection and manual saves. Classic 4K full
  reads were verified on hardware; Mini/1K geometry has host coverage.
- Passive Sub-GHz watch with bounded capture storage, duplicate grouping and
  explicit Journal integration.
- Grouped app launcher, portrait touch controls, corrected shortcut badges,
  NFC block paging, map motion and event-based feedback animations.
- More stable automatic rotation with a clean layout switch. Keyboard
  presence changes require repeated probes before changing orientation.
- Mesh radio restoration and notification fixes; P25 voice status stays below
  the waterfall instead of reserving a large blank panel.
- ADS-B CPR position corrections and tests, including local decode and bounds.
- HackRF USB startup and descriptor allocation fixes. Sustained 2 MSPS USB
  input was measured; successful aircraft decoding was not established.

Experimental limits: SX1262 FSK/POCSAG is not a validated replacement for RTL;
HackRF FM/P25 needs rate conversion; keyboard-radio energy views do not identify
protocols or locate transmitters. Compass headings are magnetic and require a
flat board. See [release checks](TDP4_RC_CHECKLIST.md), [Labs and Journal](FIELD_LABS.md),
[MIX-RF](MIX_RF.md), [controls](UI_CONTROLS.md) and [HackRF](HACKRF_BRINGUP.md).

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
