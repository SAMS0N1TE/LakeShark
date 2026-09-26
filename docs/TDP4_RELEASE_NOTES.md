# LakeShark release notes

## 2.4.0

- NOTES replaces JOURNAL: Markdown notes on the SD card with checklists and live GPS, heading and radio lines.
- COMPASS: tilt-compensated compass with true north, GO TO a saved place, and a level.
- FIND: point to a transmitter by signal strength on any radio.
- FIND scans up to eight channels and runs two radios at once, with presets for FRS, MURS, NOAA, marine, ISM and LoRa.
- FIND hit log, and RADAR and HEAT views beside the dial.
- FIND calibrates against the Flipper's DF Beacon on 915 MHz, or 433.92 MHz.
- COMPASS LOOKS: orange direction letters, and settings for the signal fill, the heard trail and the style.
- Steady compass heading when the board is still.
- RTL-SDR gain follows the signal in FIND, so a nearby transmitter no longer overloads it.
- RTL-SDR keeps streaming through USB transfer errors.
- Battery percent from pack voltage.
- Console: `find` reports and sets FIND; `trail` shows where each task was after a watchdog reset; `tui rec` streams the screen, and `tools/ls_record.py` turns it into video.
- Flipper app 2.7: DF Beacon, 915 MHz at about -10 dBm by default.

## 2.3.2

- Waterfall runs at about 25 fps in both orientations.
- Unplugging the RTL-SDR no longer reboots the board; replug and it resumes.
- Swapping RTL-SDR dongles no longer crashes P25.
- Reconnecting the RTL-SDR no longer leaks memory.
- No colour-bar test screen at boot.

## 2.3.1

- P25 SYNC no longer flickers on noise: a NAC must repeat before it counts.
- P25 profiles with site coordinates follow the nearest site by GPS.
- A profile can turn on Phase II follow (`phase2_follow=true`).
- Load a P25 profile from the console: `p25 profile [name]`.
- Flipper app 2.6: a P25 SYSTEM page to see the profile and site, switch Phase II follow and load profiles.

## 2.3.0

- P25 voice no longer chops: whole calls play, including their first frames.
- Cleaner P25 audio from a steadier symbol clock.
- P25 SYNC, NAC and scanner ignore noise.
- P25 Phase II grant following (experimental, off by default).
- Phase II handles reversed polarity and up to 1.6 kHz offset.

## 2.2.2

- Wi-Fi and Bluetooth startup toggles in RADIOS; changes take effect after reboot.
- SUB-GHZ scanning with an animated sweep, selectable styles and colours, a draggable threshold and a detections list.
- SD-card file browsing and recording previews; FILES opens replay in RECORD with power controls and animated playback.
- ADS-B offline mini map and saved home, set from GPS, coordinates or the map.
- P25 profile picker and Journal marks.
- FM volume and squelch controls, a signal meter with a threshold marker, and animated tuning.
- Improved FM tuning and pager navigation, receiver recovery and GPS recording startup.

## 2.2.1 - T-Display P4

Install writes the complete firmware. Works on a new board or over any
existing install.

- Boots to the interface whatever state the storage partition is in, and a
  blank one formats itself on first mount.
- Sub-GHz scan and learn, SX1262 FSK transmit and a capture archive.
- GFSK and POCSAG in LORA LABS, with launch animations restored.
- One .sub writer, universal record and replay, and readable menus.
- FM touch routing fixed, and the FM spectrum leaves the radio alone.
- SUB-GHZ keeps its landscape rows, the tab strip is centred, and the top
  bar reaches the glass in portrait.
- Async sampler programmed at twice the capture bit rate.
- P25 profile format version 2 carries site coordinates.
- Waterfall and LoRa sweep measurements carried by the gauge.

Supersedes 2.2.0.

## 2.2.0-rc1 — experimental T-Display P4 candidate

- FM/P25 waterfalls show a cursor: left/right selects a frequency; Space tunes to it. Radio panels retain direct arrow tuning and lists retain navigation.
- ADS-B map access and home selection from GPS or the map center.
- Clearer FM mode/sweep controls and centered portrait button rows.
- GPS landscape panels with borders, a larger sky plot and separate satellite details.
- Map follow controls, animated markers, additional zoom, and recording timestamps.
- Keyboard backlight control and more efficient HOME and recorder layouts.
- PCM16 queue alignment correction to prevent partial-sample corruption on overflow.
- Settings worker waits for suspension before deleting and reusing static task storage.
- Companion shutdown detaches its GUI before freeing data that drawing callbacks use.

Validation is in progress. An intermittent watchdog reboot, prior P25 audio
reports, GPS drive recording and other tracked hardware checks remain unresolved.
This candidate is experimental and is not cleared as a stable replacement for 2.1.1.


## 2.1.1 — T-Display P4 hotfix

- Corrects ESP-Hosted transport-pool alignment for the P4's 128-byte external-RAM cache line.
- Prevents short Wi-Fi packets from failing SDMMC DMA validation and taking down the shared Wi-Fi/Bluetooth SDIO transport while the LakeShark Flipper app is connected.
- Hardware-tested with Wi-Fi connected, continuous Flipper BLE telemetry, RTL-SDR streaming and repeated FM/P25 transitions beyond the previous deterministic failure interval.

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
