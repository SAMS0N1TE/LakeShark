# LakeShark release notes

## 2.18.0

- Apps: games and tools that run from the SD card, each with its own tile under USER on HOME.
- App store: terminalbay.com installs, updates and removes apps over USB with one click.
- Store apps are signed. An app from anywhere else asks once before it opens, and one that stopped the board asks again.
- LakeShark hub: the board's screen live in the browser, the SD card as a file manager, and the console.
- App SDK: write apps in C. SNAKE and DICE come as examples.
- MAP picture view: three times lighter when the map is still, and a new picture after a pan arrives in under half the time.
- 3D for apps: a dot engine with meshes, terrain, shadows, fog and cameras. ORBIT and DICE 3D show it off.
- More example apps: PINBALL and MESHLING, with colour icons on HOME.
- CARRIER: an early access space game. Trade, fight and explore a galaxy where live ADS-B aircraft fly as ships. Still in progress.
- The board's screen in a phone browser, served by the board at lakeshark.local.
- LINK: a REMOTE page for using the board from a phone or a PC.
- Desktop: one click opens LakeShark in its own window on your computer, with mouse drag, held keys and a settings panel for sound, voice, mesh and screen.
- ESP32-P4-NANO: works as a desktop computer, the window sets its screen size, and MAP and TILES run on it with map downloads.
- MESH: hashtag channels, plus `mesh chan` and `mesh send` on the console.
- App installs from a computer are about four times faster.
- New icons for LORA LABS and DETECTOR.

## 2.17.1

- FILES: WAV recordings play with a tap, including P25 calls.
- FILES: ACTIONS has PLAY, COPY TO MUSIC and MOVE TO MUSIC.
- MUSIC: FM and P25 receive again after leaving MUSIC, without a reboot.

From 2.17.0:

- Terminal: the whole interface in a terminal on your computer. Over USB with `lsterm.py`, or over Wi-Fi with `term net on` and a password (plain telnet works too).
- lsterm: the screen in a frame with a side panel. CONTROL for snapshots, recordings, screen off, mute, volume, brightness, theme and rotate. CONSOLE with completion and history. FILES for the SD card. HELP for the open app.
- ESP32-P4-NANO: runs the full interface with no display attached; the terminal is its screen. `panel term 160x40` sets its size.
- Boot: the Terminal Bay globe from the first second, with a progress line that follows each startup stage. The home screen arrives sooner.
- Boot: a new radio chirp at power on.
- MAP: a full colour picture view with shaded relief, rivers, lakes and place names.
- MAP: HIDE next to SELECT clears the screen for the map; SHOW brings the controls back.
- MAP: opens the most detailed map under you and stays where you put it when you pan or zoom.
- TILES: download whole states from a globe.
- Scanner: one tap starts FM and P25 scanning, with LIST, BAND and GPS lanes.
- P25: GPS sites follow you to the nearest site.
- DRONES: a receive-only early warning app.
- Ukrainian interface.
- Notification sounds: RADIO KEY, ROGER and CHIRP, chosen in Settings > Sound.
- Hotspot: each board has its own password, shown in Settings > Device > Hotspot.
- COMPASS: the heading follows the edge that is up on screen.
- SWEEP: SELECT DEVICE steps through devices and names each one.
- Smaller firmware, with more room for apps.

## 2.17.0

- Terminal: the whole interface in a terminal on your computer. Over USB with `lsterm.py`, or over Wi-Fi with `term net on` and a password (plain telnet works too).
- lsterm: the screen in a frame with a side panel. CONTROL for snapshots, recordings, screen off, mute, volume, brightness, theme and rotate. CONSOLE with completion and history. FILES for the SD card. HELP for the open app.
- ESP32-P4-NANO: runs the full interface with no display attached; the terminal is its screen. `panel term 160x40` sets its size.
- Boot: the Terminal Bay globe from the first second, with a progress line that follows each startup stage. The home screen arrives sooner.
- Boot: a new radio chirp at power on.
- MAP: a full colour picture view with shaded relief, rivers, lakes and place names.
- MAP: HIDE next to SELECT clears the screen for the map; SHOW brings the controls back.
- MAP: opens the most detailed map under you and stays where you put it when you pan or zoom.
- TILES: download whole states from a globe.
- Scanner: one tap starts FM and P25 scanning, with LIST, BAND and GPS lanes.
- P25: GPS sites follow you to the nearest site.
- DRONES: a receive-only early warning app.
- Ukrainian interface.
- Notification sounds: RADIO KEY, ROGER and CHIRP, chosen in Settings > Sound.
- Hotspot: each board has its own password, shown in Settings > Device > Hotspot.
- COMPASS: the heading follows the edge that is up on screen.
- SWEEP: SELECT DEVICE steps through devices and names each one.
- Smaller firmware, with more room for apps.

## 2.16.4

- FM and P25: tapping a volume, gain or squelch bar sets the value where you tap again, instead of opening a number pad. Tap the number for exact entry.
- P25 scanner: the volume bar sets the volume.
- P25: the TUNE card from FM. Step up and down, seek to the next P25 signal with << >>, and NEXT, SKIP, HOLD and STOP while scanning. P25 keeps its own step.

## 2.16.3

- FM: a TUNE card on the main screen steps up and down by the saved step, and << >> seek to the next signal from the current frequency (keys `,` and `.`).
- FM: while a scan or seek runs, the TUNE card shows NEXT, SKIP, HOLD and STOP. SKIP passes a channel for the rest of the session.
- FM on the LR2021: band scan, channel scan and seek now stop on signals.
- Squelch: 0% is fully open and low settings let noise through, like other receivers. 30% and up behave as before.
- Scanner: listens longer on each channel, so short transmissions are caught.
- Waterfall: TUNE MARK works while the frequency is locked, and the lock moves to the new frequency. Marks land on a 1.25 kHz grid, and a refused mark says why.
- P25 uses the FM display: big digits, typed entry over ghost digits, signal trace and peak hold, with the same volume and gain cards.
- GPS: the L76K's NMEA sentences are set at startup, so satellites show even after other firmware changed them. `gps reset` and GPS > OPTIONS > Reset receiver to factory restore the module.
- LR2021 on the header: the older T-LR2021 V0.2 module now works. Its RF switch on DIO5 no longer stops the radio from starting.
- `lora port header v02` sets the CON1 pins and the V0.2 switch table in one word.
- Settings > Device > LoRa module switches between the V0.3/V1.0 and V0.2 modules.
- The LoRa port is saved and read back before the board says it is saved. `lora port` shows how full the settings store is.
- `lora port` notes that the TCXO voltage has no effect on LilyGO's T-LR2021 modules.

## 2.16.2

- LoRa on the feature header: Settings > Device > LoRa radio now switches to an LR2021 module on the CON1 header, with no console needed.
- `lora port header con1` sets the CON1 wiring in one word: chip select IO36, BUSY IO33, reset IO27, interrupt IO25 on DIO11, and the onboard module's RF switch table and TCXO.
- The LoRa radio setting always picks real header pins, so a header module is used instead of the onboard radio. The boot log and `lora port` show which pins are in use.
- LakeShark LR gives a slow-starting LR2021 several tries before giving up, and its message names the pins it tried.

## 2.16.1

- A stalled screen recovers by itself: the board records which task held it and restarts within a minute.
- HOME stays responsive while SWEEP reads its rules from the SD card.

## 2.16.0

- LR2021: every LR app receives from 104 to 2740 MHz, well past the chip's datasheet bands. Transmit stays at 150 MHz and up.
- LR2021: pagers (POCSAG and FLEX), APRS, SAME weather alerts, ACARS and AM airband from its own I/Q, with no SDR.
- LR2021 I/Q gain follows the signal (`lora lrgain auto`, the default): 9 to 18 dB more signal to noise than the chip's AGC on medium and strong signals.
- REC decodes 2-FSK packets from every I/Q capture and every FSK capture: rate, sync word, payload and a CRC check, with nothing set in advance.
- LR433 adds 433 PKT and 915 PKT sessions for CC1101, RFM69 and Si443x style packets, and OFFSET for a transmitter that sits off its band's usual frequency.
- ACARS on an RTL-SDR reads the AM envelope through a proper channel filter: 18 of 18 test bursts decode, up from 3.
- LoRa on the feature header (experimental): an external LR2021 module with its own chip select, BUSY, reset, interrupt DIO, TCXO supply and RF switch table, set with `lora port` and switched in Settings > Device > LoRa radio. Header pins the keyboard uses turn the keyboard off while it is in use.
- LakeShark LR (experimental): a build for LR2021 boards only, without USB host, RTL-SDR, HackRF or SX1262 support, which frees about 250 KB of flash and 34 KB of internal RAM. Install it over USB; it does not update over Wi-Fi yet.
- RADIO lists a dongle once it has been plugged in this boot, and the keyboard's radios once the keyboard has been on.
- An empty USB port is left alone at boot: no power cycling and no replug warning.
- Flipper link: while the receiver is parked the head shows no mode as live.
- Console: `lora lrprobe`, `lora lrble` and `lora sxprobe`, receive-only probes of the LR2021 and SX1262 at any frequency. `rec arm` says when there is no SDR to record from.

## 2.15.0

- DETECTOR watches aircraft: rules over the ADS-B table, with emergency squawks and emergency status on by default. The AIRCRAFT view lists what matched.
- ADS-B decodes emergency status and marks squawk and emergency in the aircraft table.
- DETECTOR notices use the standard banner, and long notices scroll instead of being cut off.
- CELL (experimental): LTE band table, a cell table with power and MIB, and dongle clock correction from the cells it finds.
- CATCH watches only the channels you choose.
- ANTENNA: one tap selects MMCX1. MMCX1 is receive-only by default; transmit stays on the internal antenna.
- MAP: drag to pan, and a followed target stays on screen. Saved mark names show in landscape with the touch keys up.
- SPEECH: more even voice loudness on the board speaker.
- Flipper link: the head is told which receiver each mode uses, and can read MeshCore channels, direct messages and DETECTOR alerts, send messages and arm transmit.
- MeshCore: malformed encrypted packets are rejected instead of overrunning a buffer (upstream fix).
- The title bar no longer shows black boxes at its ends while switching screens.
- A UI that stops drawing is reported, and the board restarts with a core dump instead of staying frozen.

## 2.14.0

- DETECTOR: a new app that watches BLE Remote ID and SWEEP in the background and raises a notice when a rule matches. Built-in rules are used when the card has no rules file. The optional log on the card is off by default and stores no MAC addresses or locations.
- SURVEY under LINK: Wi-Fi war driving that writes WiGLE CSV files, estimates access point positions and shows a live dashboard.
- MAP: add, move and delete markers by touch, draw routes, and share markers with other nodes over MeshCore.
- REC on the LR2021: record I/Q at 125 kS/s and replay it. OOK captures are rebuilt from their edges; FSK and other signals replay as I/Q.
- COMPASS (still experimental): a steadier heading when the device is still, tilt from the viewing direction, and a warning when the magnetic field is disturbed.
- MeshCore listening uses about half the CPU it did, and the UI stays smoother while it runs.
- Meters and graphs in DETECTOR, SURVEY and SWEEP draw in ASCII and braille.
- UPDATE > C6 FIRMWARE installs a C6 co-processor image from the card. The `c6` console command adds `update` and `status`.
- The `perf` console command reports UI frame time, frame rate and input latency. `tui touch` lists recent touches and whether each one counted as a tap.
- [APP_FRAMEWORK.md](../bench/APP_FRAMEWORK.md) explains how to write an app, with an example app that is off by default.

## 2.13.2

- Opening animations finish within a 250 ms decoration budget. Navigation and OPTIONS presses cancel the animation and proceed immediately.
- RADIOS stays responsive during continuous LR2021 IQ reception. Reading transmit status no longer waits for the capture session to release the radio.
- LR2021 P25 reception now always uses raw IQ. The retired P25 FSK receiver and classifier models are excluded from firmware; FSK used by other apps remains available.
- Older saved P25 FSK selections use IQ automatically. The former LR control-channel/RTL voice split option falls back to RTL reception.
- LR IQ stays on C4FM. Its OPTIONS menu no longer exposes incompatible demodulation modes or ineffective FSK gain/path controls; SDR demodulation and gain settings remain available.
- Select the antenna connected to the board under RADIOS > ANTENNA. MMCX1 must be selected to use that external connector.

## 2.13.1

- REC and SUB-GHZ share one layout: RADIO, the page's controls and OPTIONS on filled keys across the top, and READ, SCAN, SAVED, RAW, LEARN and CATCH tabs along the bottom.
- One RADIO choice drives the whole app. Every setting is under OPTIONS.
- The LR2021 now reads OOK remotes from its I/Q stream, and sends OOK captures and OOK .sub files. FSK packet receive is under OPTIONS > LR2021 receive.
- SCAN on the LR2021 shows a live I/Q waterfall and sweeps without freezing the screen.
- Retuning or changing capture settings during a sweep asks to stop the sweep first.
- P25 now matches the FM VFO: VFO, WATERFALL and SCANNER tabs on every view, so the way back from the waterfall is one tap.
- P25 volume and gain are tall cards with large minus and plus keys that fill the screen down to the tabs.
- P25 uses the same inline frequency keypad as FM, opened from a full-size KEYBOARD key in the bottom-right corner.
- The scanner's MORE button is now BACK.
- Scanner status and waterfall zoom use less CPU per frame.
- FM and P25 view changes preserve active stored-channel scans; explicit tuning, mode changes and starting a sweep stop the scan. Unsupported saved channels are skipped when the selected LR2021 cannot receive them.
- Scanner controls add filled radio-style cards, direct level-bar adjustment, separate numeric entry, larger step targets, and accessible squelch and settings.
- FM, P25 and sweep waterfalls share a session-local 1x/2x/4x display zoom. Waterfall updates avoid redundant FFT windows and pixel renders.
- ASCII opening animations advance with presented frames, so they do not skip ahead or end early when rendering falls behind.

## 2.13.0

- Listen to analog narrowband FM through the onboard LR2021, with live I/Q waterfalls and quicker FM/P25 screen switches.
- FM VFO feels like a handheld radio: a large frequency display, animated VFD-style signal lines, and filled touch controls for volume, gain, and squelch. KEYBOARD replaces the lower controls with an inline frequency keypad; CONTROLS switches back. Spectrum combines the receiver panel with the detailed signal plot.
- LR2021 waveform playback preserves amplitude and phase for short custom radio bursts. Upload once and replay through the console; P25 and amplitude-coded examples are included in the [waveform guide](LR2021_IQ_TX.md).
- Waveform uploads use PSRAM only while loaded, leaving the receiver's internal RAM available. Abort releases the buffer.
- Memory diagnostics keep their quick summaries during LR2021 IQ reception and defer task-stack scans until the radio is idle. This avoids interrupting reception while inspecting memory.
- Stopping or changing the receiver through the UI cancels an in-progress waveform burst and restores the radio through the normal cleanup path.

## 2.12.0

- P25 on the LR2021 board: a raw I/Q receive path. The LR2021 streams 125 kS/s complex samples continuously and LakeShark demodulates C4FM from them. Choose it in P25 OPTIONS > LR2021 receive path > Raw I/Q (experimental). Phase 1 only; FSK stays the default.
- The P25 screen shows the I/Q stream state, block counts and any measured gaps. Lost samples are counted, never hidden.
- ADS-B dual band (experimental): the LR2021 takes 1090 MHz while the RTL takes UAT on 978 MHz, merged into one list and map. UAT weather text is under OPTIONS.
- P25 split (experimental): the LR2021 holds the trunked control channel while the RTL follows each call, and falls back to RTL only if control is lost.
- CATCH in REC (experimental): the RTL watches a 2.4 MHz slice and the LR2021 jumps to each burst to decode it.
- Waterfalls in FALLS, FM, P25, SUB-GHZ and user apps draw in pixels at about 40 ms a frame. DETAIL > ascii keeps the old look.
- MUSIC: restyled screen, a pixel spectrogram with HOLD, axis and zoom, and LEVEL follows volume and mute.
- FM VFO: one RECEIVER / SIGNAL panel and a larger spectrum. Tap either side of a stepper to step, or the middle to type a value.
- Speech stays smooth while heavy radio work runs.
- Accented text (Polish and more) shows as plain letters in MESH, MAP and pickers. Thanks to @zenobik.

## 2.11.0

Every app was reviewed and run on both boards for this release: about 250 fixes across 27 apps.

- ADS-B: callsign labels stay on one side of each aircraft, and two aircraft on the same path are both tracked. Speeds use the right units, ground track and zero readings show correctly, and old readings are marked stale.
- MESH: packets are handled by one owner task, each queued message gets its own delivery status, long messages arrive whole, and restarting MESH starts clean.
- MUSIC: a failed read still finishes the recording, STOP ends recording and is reachable by touch, and MUTE keeps your volume.
- NOTES and MAP: saves are confirmed, a failed save keeps your edits, and an interrupted save recovers the previous copy.
- GPS: an old fix expires everywhere it is used (GPS, MESH, Flipper, TRACK), and GPX exports are checked before they are kept.
- SWEEP, LINK and COMPASS share Bluetooth, so SWEEP, FIND and a Flipper connection run together.
- FM: ACARS messages have their own list and detail view, and a stalled receiver clears its readings.
- UPDATE: checks finish within a set time, RETRY rejoins WiFi, and the rollback guard stays on.
- P25, CELL, FALLS, SUB-GHZ and REC: radio hand-offs between apps keep each app's settings and no longer stop another app's scan.
- FILES, TILES, DRONES, LABS and more: touch paging on long lists, BACK works everywhere, and clear messages when a radio is busy or something fails.

## 2.10.2

- LINK scans and joins WiFi networks while SWEEP is running; before, it said a connection update was already running.

From 2.10.1:

- SWEEP uses the WiFi scan results the board already has instead of running its own scans, so the link to the WiFi and Bluetooth radio stays steady when SWEEP runs for hours.
- If that link ever stalls, the console names the stalled channel once a second instead of filling the log.
- T-Display-P4 TFT version (HI8561): build it from source with `boards/t_display_p4_tft.defaults`; the screen is laid out around the front camera. Thanks to @zenobik.

From 2.10.0:

- MAP: a new map engine, CartoCore, draws offline maps in smooth and braille styles alongside FIELD. Place names, aircraft, mesh nodes and marks share the screen cleanly: labels make room for live data and the basemap dims a step under overlays.
- MAP: range rings and aircraft trails are thin amber lines that stand out in every theme, and the map follows the selected colour theme, Daylight included.
- TILES: download map regions over WiFi, re-download a newer copy of a map that is open, and resume an interrupted download. Downloads run about 2.6x faster.
- TILES: whole-state cell maps with place search, and GO TO in MAP.
- SWEEP (new, FIELD): a passive scanner that lists trackers (Apple Find My, Google, Samsung, Tile), Remote ID drones and camera or bodycam hints, ranked as WITH YOU, NEW and PASSING, with a cold-to-hot HUNT view and a hand-off to FIND. Starts muted.
- DRONES (new): Remote ID receiver with drone and operator details.
- WiFi: keeps up to eight saved networks and joins the strongest one in range, and moves to another saved network when one disappears. A `wifi` console command adds, lists and forgets networks.
- CALLS: a media table with an embedded player and waterfall, keeps 30 days by default and pauses recording when the card runs low.
- Volume and mute respond instantly again, and mute is remembered after a restart.
- Faster screens: HOME, P25 and MUSIC switch quicker and idle work in the background is lighter.
- Steadier: built on ESP-IDF 5.5.4 for both the SX1262 and LR2021 boards, and fixes a rare freeze during flash writes.
- Flipper app 2.10 shows WiFi, SWEEP and DRONES pages.

## 2.10.1

- SWEEP uses the WiFi scan results the board already has instead of running its own scans, so the link to the WiFi and Bluetooth radio stays steady when SWEEP runs for hours.
- If that link ever stalls, the console names the stalled channel once a second instead of filling the log.
- T-Display-P4 TFT version (HI8561): build it from source with `boards/t_display_p4_tft.defaults`; the screen is laid out around the front camera. Thanks to @zenobik.

From 2.10.0:

- MAP: a new map engine, CartoCore, draws offline maps in smooth and braille styles alongside FIELD. Place names, aircraft, mesh nodes and marks share the screen cleanly: labels make room for live data and the basemap dims a step under overlays.
- MAP: range rings and aircraft trails are thin amber lines that stand out in every theme, and the map follows the selected colour theme, Daylight included.
- TILES: download map regions over WiFi, re-download a newer copy of a map that is open, and resume an interrupted download. Downloads run about 2.6x faster.
- TILES: whole-state cell maps with place search, and GO TO in MAP.
- SWEEP (new, FIELD): a passive scanner that lists trackers (Apple Find My, Google, Samsung, Tile), Remote ID drones and camera or bodycam hints, ranked as WITH YOU, NEW and PASSING, with a cold-to-hot HUNT view and a hand-off to FIND. Starts muted.
- DRONES (new): Remote ID receiver with drone and operator details.
- WiFi: keeps up to eight saved networks and joins the strongest one in range, and moves to another saved network when one disappears. A `wifi` console command adds, lists and forgets networks.
- CALLS: a media table with an embedded player and waterfall, keeps 30 days by default and pauses recording when the card runs low.
- Volume and mute respond instantly again, and mute is remembered after a restart.
- Faster screens: HOME, P25 and MUSIC switch quicker and idle work in the background is lighter.
- Steadier: built on ESP-IDF 5.5.4 for both the SX1262 and LR2021 boards, and fixes a rare freeze during flash writes.
- Flipper app 2.10 shows WiFi, SWEEP and DRONES pages.

## 2.10.0

- MAP: a new map engine, CartoCore, draws offline maps in smooth and braille styles alongside FIELD. Place names, aircraft, mesh nodes and marks share the screen cleanly: labels make room for live data and the basemap dims a step under overlays.
- MAP: range rings and aircraft trails are thin amber lines that stand out in every theme, and the map follows the selected colour theme, Daylight included.
- TILES: download map regions over WiFi, re-download a newer copy of a map that is open, and resume an interrupted download. Downloads run about 2.6x faster.
- TILES: whole-state cell maps with place search, and GO TO in MAP.
- SWEEP (new, FIELD): a passive scanner that lists trackers (Apple Find My, Google, Samsung, Tile), Remote ID drones and camera or bodycam hints, ranked as WITH YOU, NEW and PASSING, with a cold-to-hot HUNT view and a hand-off to FIND. Starts muted.
- DRONES (new): Remote ID receiver with drone and operator details.
- WiFi: keeps up to eight saved networks and joins the strongest one in range, and moves to another saved network when one disappears. A `wifi` console command adds, lists and forgets networks.
- CALLS: a media table with an embedded player and waterfall, keeps 30 days by default and pauses recording when the card runs low.
- Volume and mute respond instantly again, and mute is remembered after a restart.
- Faster screens: HOME, P25 and MUSIC switch quicker and idle work in the background is lighter.
- Steadier: built on ESP-IDF 5.5.4 for both the SX1262 and LR2021 boards, and fixes a rare freeze during flash writes.
- Flipper app 2.10 shows WiFi, SWEEP and DRONES pages.

## 2.9.1

- LR2021: the radio clock settles correctly every time receive starts, so pagers and every other receiver on the chip start cleanly after each hop.
- POCSAG on the LR2021: PAGER RECON keeps receiving after the first sync of a transmission and runs the chip at full gain, so many more batches decode. NATIVE STREAM is the new default; PACKET stays in OPTIONS.
- PAGER RECON keeps its frequency, probe and receive settings after a reboot or an update.
- POCSAG in LoRa Labs and FM on the LR2021 hears pagers, with the same detector and gain as PAGER RECON.

From 2.9.0:

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
