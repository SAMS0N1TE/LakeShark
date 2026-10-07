# LakeShark release notes

## 2.9.0

- Updates over WiFi: UPDATE in SYSTEM checks for a newer build and installs it with one tap. Updates are signed, and a build that does not start is rolled back to the one before it.
- A daily check in the background shows a NEW tag on SYSTEM and UPDATE when a newer build is out. Builds come from ota.terminalbay.com.
- Install 2.9.0 once with the web flasher or USB. The partition layout now holds two app slots, so this one install is not an update over WiFi. Storage is rewritten, as with every full install; settings and WiFi are kept. Later releases update over WiFi.
- NOTES: a map picture in a new note is now saved.
- ADS-B mini map: FOLLOW fits all aircraft, the selected one or the nearest, and the choice is kept.
- EXPERIMENTS: a NOTE button and the N key save an experiment's readout to NOTES.
- COMPASS: the heading now moves smoothly when the board is raised, with no jump between its top and its back.
- New receivers: RS41 weather balloon sondes, AIS ship tracking, APRS, and SAME/EAS weather alerts.
- CTCSS and DCS tone squelch for voice receive.
- DMR view for digital voice activity. Encrypted traffic is shown as encrypted and is never decoded.
- CALLS keeps an archive of heard calls.
- Mesh INSPECT: trace a route, log in to a node and read its telemetry. LakeShark answers telemetry requests for peers you allow.
- LINK SURVEY logs the Wi-Fi and BLE devices it hears, with position and best signal, to a CSV per walk.
- Routes and GPX files can be followed on the map.
- LR433 sensor readings keep a history.
- LR2021: the radio now runs from the module's TCXO, as LilyGO's own driver does, for a steadier frequency. `lora lrclk xtal` returns to the old setting.
- The antenna choice, internal or MMCX1, is kept after a reboot.
- P25: one DECODE / SIGNAL window, the spectrum as a toggle tile, and full-size page tiles. CALLS tiles are taller.
- Fixes in FILES, LABS, MIX-RF and FALLS.
- Includes everything in [2.8.3](https://github.com/SAMS0N1TE/LakeShark/releases/tag/v2.8.3).

## 2.8.3

- The VOLUME controls on the P25 and FM screens show the volume again.

## 2.8.2

- P25 on the LR2021 plays much more of every call. A small neural network on the P4's vector unit reads the chip's bits, and the voice error correction uses how sure it is of each symbol.
- P25 on the LR2021: the GAIN keys choose the chip's gain, AUTO or MAX, and the choice is kept. MAX helps far from the transmitter; AUTO is best close to it.
- P25: a clear call keeps playing through one ESS that reads encrypted, and a voice frame that needed too many corrections is replaced by the last good one.
- Settings: Rotate lock on the first page holds the screen the way it is.
- DIAG: ERROR LOG keeps crashes, watchdog resets and brownouts through power-off, with the last log lines before each one. `crumb log` shows them on the console.
- TERMINAL runs console commands from the board's own keyboard.
- Scanner: NFM channels hold on the noise squelch, which sets itself from the noise. New band presets for the 7.5 kHz VHF raster, NOAA weather and UHF 450-470.
- `search` finds active frequencies from 136 to 174 MHz with an RTL-SDR and logs them to the SD card.
- Steadier on long runs: every address-range cache operation takes one lock.
- Console: `p25 lr` shows and sets the LR2021's receive settings and captures its raw bits; `p25 sym` and `p25 imbe` capture the decoder's symbols and voice frames.

## 2.8.1

- P25 and ADS-B on the LR2021 hand the chip to EXPERIMENTS, LoRa Labs, the LoRa waterfall and `lora` whenever one of them asks, and take it back when it is free. P25 shows LR2021 IN USE ELSEWHERE in the meantime.
- PAGER RECON shows the probe picked in OPTIONS > PROBE before a run.

## 2.8.0

- P25 on the LR2021: the LR2021 version of the T-Display P4 decodes P25 Phase 1 voice by itself, with no SDR. Open P25 and it listens on the LR2021 whenever no SDR is plugged in, or pick it with RADIO. 150 MHz and up.
- P25 shows the LR2021's signal strength and stream rate while it is the receiver.
- PAGER RECON: POCSAG on VHF from the LR2021, from 150 MHz. POCSAG probes listen behind the LR2021's preamble detector, and OPTIONS > PROBE holds a one-frequency run on one rate and polarity. The console run also sets the filter, deviation and detector, and takes up to three batches in one capture.
- P25 SITE FINDER listens from 150 MHz, VHF sites included, and its console run sets the polarity, filter, deviation and detector.
- LoRa chip: FSK sessions can stream every received bit and set the LR2021's preamble detector. `lora` shows an FSK session's receive counters.
- COMPASS points calibration at OPTIONS, and SUB-GHZ READ RAW names both SDRs.

## 2.7.1

- HackRF One: FM, pagers, ACARS, P25 and REC can use it as their SDR. Rates below 2 MSPS run by decimation, and pagers decode the same pages as on an RTL-SDR.
- HackRF One: gain reaches 113 dB in P25, FM, REC and ADS-B, split between LNA and VGA. SUB-GHZ READ captures from it too.
- HOME, RADIOS and the value readers show whichever SDR is plugged in.
- Speech: a female voice, the third choice in Settings > Sound > Voice and `say voice female`.
- OPTIONS: MESH and ADS-B settings open in layers with BACK, and levels step with < and >. MESH SETUP is now part of OPTIONS.
- Settings: Volume, Brightness, Mute and Screen lock up front, the rest in DISPLAY, SOUND and DEVICE menus with BACK. Volume and Brightness step from a tap on either half of their box, and turning the volume unmutes.
- Settings: Voice uses the same lists, and mesh voice can speak direct messages only.
- MUSIC: pause is silent, and a tap away from the controls stays on the player.
- HOME: every tile subtitle fits its tile.
- Start-up: the audio driver starts without errors, and a USB setup fault leaves the board running with the USB radios off.
- Console: `hackrf` shows the HackRF's rates, gain and levels, and `status` counts POCSAG frames and pages.

## 2.7.0

- New board: the LilyGO T-Display-P4 with the LR2021 radio. The same firmware runs on both T-Display-P4 radios and finds the chip at start.
- LR2021: LoRa packets, FSK listening up to 1100 MHz at 500 bps to 2 Mbps, and a sweep of 150 to 1100 MHz and 1500 to 2500 MHz. It transmits at 960 MHz and below only.
- The LR2021 is found reliably at start, and resets and recovers by itself if it stops answering.
- ADS-B decodes aircraft from the LR2021 with no RTL-SDR attached, and the mini map names the receiver. DF18 messages decode too.
- RADIO: one radio picker in every app, with real chip names, remembered for each job.
- OPTIONS: one settings list on `o` in every app, showing only what the current mode and radio take. The waterfall's REF moved to `i`.
- SUB-GHZ opens on six tiles: READ, ANALYZER, SAVED, READ RAW, LEARN and SETTINGS. Every page has BACK.
- LoRa Labs has more bands, and POCSAG pages (512 baud too) are read on the LoRa chip.
- REC: FSK mode with a preset, settings kept across a reboot, autosave, and the newest files first.
- REC replays CC1101 raw 2-FSK captures, and REC SCAN sweeps a band on the SX1262 while the RTL-SDR keeps recording.
- EXPERIMENTAL: an EXPERIMENTS tab for trials that may become apps, one at a time. It has CARRIER LEVEL, BAND SURVEY, BURST SCOPE, RADIOSONDE (GRAW DFM), PAGER RECON, P25 SITE FINDER, IRIDIUM, LR433 (tire pressure, weather and meter sensors), AVIATION and UAT 978 receiver diagnostics, a LoRa monitor, and Z-Wave and Wi-SUN meters. Trials that need the LR2021 are marked. Expect rough edges.
- Speech: the Settings volume reaches the speaker.
- Settings are steady on every task. REC saves need an SD card.
- LoRa: `lora` and ADS-B share the SPI bus safely.
- Mesh: `mesh dm <n>` picks the same peer that `mesh peers` lists when signals tie.
- Console: `sd cat` prints a text file, `lora fsk` takes a short sync word, `labs` shows LoRa Labs, `exp` runs experiments, and `adsb chips` shows the chip's Mode S counters.

## 2.6.0

- MUSIC: a WAV player for the SD card, with a library, VFD level meters, an FFT spectrum, and scan and waterfall views.
- MUSIC: the microphone as a live source, and a recorder that saves WAV files to the SD card.
- COMPASS: gyro-fused heading that follows a turn without lag.
- COMPASS learns its own magnetometer drift, including the offset from battery and USB current.
- FIND aims using the whole turn and files each reading at the heading it was taken; FIND BAND.
- Landscape touch keys are taller when no keyboard is attached, and key hints follow the attached keyboard.
- Boot recovers from an I2C bus lockup instead of starting with no screen, touch or SD card.
- LoRa start-up reports what went wrong instead of blaming the radio.
- RTL-SDR retunes with about half the USB traffic.
- USB boot recovery counts only devices that actually enumerated.
- The LoRa sweep waterfall draws in its own window.
- The console starts once memory allows, instead of stopping the boot.
- GPS keeps satellite in-use flags whatever order the receiver reports them in.
- Flipper app 2.8 is unchanged and tested with this release.

## 2.5.0

- MAP draws aircraft, mesh nodes and your own marks over the map, with trails, callsigns and squawk codes.
- MAP: tap or TAB to select, FOLLOW an aircraft, GO TO a place, range rings, coverage and LAYERS.
- MAP: MARK places and DRAW lines, saved to the SD card.
- MAP: eight palettes, including Daylight, Night and two radar scopes.
- Night theme: the whole interface in reds.
- Speech: a formant voice announces aircraft as they appear, get a position and drop out, and reads mesh messages. Settings > Voice sets the voice, level and callouts.
- Mesh messages show in a small red notice.
- MESH lists saved contacts after a restart.
- Keyboard backlight dims with the screen.
- NOTES keyboard uses the full width; touch keys hide while a keyboard is attached.
- COMPASS keeps its heading moving while FIND scans.
- About 3 KB more memory free while ADS-B runs, so MESH keeps receiving alongside it.
- SD card writes keep working when memory is tight.
- Smoother switching between P25 and FM.
- Console: `say` speaks text and tests the voice; `crumb` shows where a crash stopped.
- Screen recordings show the map, and a recorder window: `tools/ls_record_gui.py`.

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
