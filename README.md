<div align="center">

![lakeshark_banner](https://github.com/user-attachments/assets/34b12b2c-fd64-4fdc-850c-e9c93d7aede7#gh-light-mode-only)
![lakeshark_banner_dark](https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09#gh-dark-mode-only)

### Your radio world. In your hand.

Listen to local radio, watch aircraft, follow Mesh nodes and explore the airwaves.

**[Install LakeShark](https://terminalbay.com/?m=lakeshark)** &nbsp;•&nbsp;
**[Explore the guides](https://terminalbay.com/?m=wiki)** &nbsp;•&nbsp;
**[Latest release](https://github.com/SAMS0N1TE/LakeShark/releases/latest)**

LakeShark turns the **[LilyGO T-Display P4](https://lilygo.cc/en-us/products/t-display-p4)** into a handheld scanner and radio workbench,<br>
with animated ASCII displays, touch controls and an [optional keyboard](https://lilygo.cc/products/t-display-p4-keyboard).

<table>
  <tr>
    <th width="33%">Listen in</th>
    <th width="33%">Watch the sky</th>
    <th width="33%">Take it outside</th>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/tdp4/motion/p25-decode.gif" alt="LakeShark decoding a P25 radio call" width="260"></td>
    <td align="center"><img src="docs/screenshots/tdp4/motion/adsb.gif" alt="Aircraft positions on LakeShark" width="260"></td>
    <td align="center"><img src="docs/screenshots/tdp4/motion/map.gif" alt="LakeShark following an aircraft on an offline map" width="260"></td>
  </tr>
  <tr>
    <td align="center">Local FM, P25 digital voice, pagers and weather alerts.</td>
    <td align="center">Aircraft positions, trails and spoken callouts.</td>
    <td align="center">Offline maps, Mesh messaging, GPS tracks and field notes.</td>
  </tr>
</table>

</div>

<h2 align="center"><°)))>< A LoRa chip. A whole new radio.</h2>

<div align="center">

**Your onboard LR2021 does more than LoRa.**<br>
P25 voice, FM, pagers and aircraft. No SDR dongle needed.

<table>
  <tr>
    <th width="33%">Listen</th>
    <th width="33%">Explore</th>
    <th width="33%">Create</th>
  </tr>
  <tr>
    <td align="center">P25 Phase 1 voice, narrowband FM and VHF/UHF pagers.</td>
    <td align="center">Track ADS-B aircraft and watch live signal waterfalls.</td>
    <td align="center">Transmit custom waveforms with amplitude and phase preserved.<br><a href="docs/LR2021_IQ_TX.md">Experimental TX →</a></td>
  </tr>
</table>

LakeShark accesses the LR2021's raw signal path so software can decode signals beyond LoRa.<br>
The companion project includes a receive library, capture firmware, waveform generators and measurements.

<table>
  <tr>
    <th width="33%"><a href="https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/p25.md">P25 in action</a></th>
    <th width="33%"><a href="https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/voice-capture.md">Signal recording</a></th>
    <th width="33%"><a href="docs/LR2021_IQ_TX.md">Waveform playback</a></th>
  </tr>
  <tr>
    <td align="center">Digital voice on a LoRa chip.</td>
    <td align="center">Save raw signals and decode them again.</td>
    <td align="center">Short transmit bursts, including P25 test tones and temperature packets.</td>
  </tr>
</table>

**[Explore LR2021-SDR →](https://github.com/SAMS0N1TE/lr2021-sdr)**<br>
The open research and tools behind the radio.

</div>

<h2 align="center">New in 2.13</h2>

<div align="center">

**Handheld FM controls · Animated VFD meters · Inline keypad**

Narrowband FM now runs on the onboard LR2021, with live I/Q waterfalls and faster switching between FM and P25.

[Full release notes →](https://github.com/SAMS0N1TE/LakeShark/releases/tag/v2.13.0) &nbsp;•&nbsp; [Release history](https://github.com/SAMS0N1TE/LakeShark/releases)

</div>
<h2 align="center">Choose your setup</h2>

<div align="center">

**LR2021 or SX1262?** Both run LakeShark. The LR2021 unlocks the onboard receivers above.

</div>

| Your hardware | What it opens up |
| --- | --- |
| **T-Display P4 · LR2021** | Onboard FM, P25 voice, pagers, ADS-B, MeshCore and LoRa tools. |
| **T-Display P4 · SX1262** | Onboard MeshCore, LoRa tools and POCSAG. Add an RTL-SDR for FM, P25 and ADS-B. |
| **RTL-SDR Blog V3 or V4** | An external receiver for either board, including P25 Phase 1 trunking and sub-GHz capture. |
| **Optional MIX-RF keyboard** | Physical keys plus CC1101, nRF24 and NFC tools. |

<h2 align="center">><)))°> Start here</h2>

<div align="center">

**[Flash LakeShark from your browser](https://terminalbay.com/?m=lakeshark)** &nbsp;•&nbsp;
**[First flash guide](https://terminalbay.com/?m=wiki#tdp4/TDP4_FIRST_FLASH)** &nbsp;•&nbsp;
**[Download the latest release](https://github.com/SAMS0N1TE/LakeShark/releases/latest)**

</div>

1. **Check your board.** The primary build is for the **LilyGO T-Display P4 with the 4.1-inch 568 × 1232 AMOLED and 16 MB flash**. The SX1262 and LR2021 radio variants run the same firmware. Other display variants need different firmware. The keyboard is optional.
2. **Flash from Chrome or Edge on a computer.** Open the browser flasher and select your exact board. Follow the first flash guide for setup.
3. **Tune in.** Open FM, P25, ADS-B or MESH, choose your radio and connect an antenna suited to what you want to hear.

**Already running LakeShark?** Open **UPDATE** for one-tap Wi-Fi updates. If you are coming from 2.8 or earlier, install by USB once.

> **Powering the T-Display P4:** use the P4's USB-C power input for now. Do not feed the keyboard's 5V USB output back into the P4. It caused repeated power cycling in testing, even without the RTL attached. I'm investigating the easiest reliable way to power the RTL on battery. The battery case below is still in development.

<div align="center">

**[New case design renders](https://terminalbay.com/?m=lakeshark-showcase#renders/01)**

<img width="48%" alt="LakeShark P4 case, plastic" src="assets/lakeshark_p4_plastic.gif">
<img width="48%" alt="LakeShark P4 case, clear acrylic" src="assets/lakeshark_p4_acrylic.gif">

**[Wiki and setup guides](https://terminalbay.com/?m=wiki)** &nbsp;•&nbsp;
**[Project overview](https://terminalbay.com/?m=lakeshark-showcase)** &nbsp;•&nbsp;
**[Roadmap](https://terminalbay.com/?m=lakeshark-roadmap)** &nbsp;•&nbsp;
**[Submit a recommendation](https://terminalbay.com/?m=lakeshark-roadmap#ask)**

</div>

<h2 align="center">}<((((-- LilyGO T-Display P4</h2>

<p align="center">
  <img width="100%" alt="hero-falls" src="https://github.com/user-attachments/assets/bdd8069b-3c1a-4dd6-a821-1526e724c950" />
</p>

<div align="center">

The primary development target: 4.1-inch 568 x 1232 AMOLED, 16 MB flash, onboard SX1262 or LR2021 LoRa, GPS and nine-axis sensing.<br>
Use touch alone or add the detachable keyboard with its MIX-RF radios.

</div>

<p align="center">
<img src="docs/screenshots/tdp4/gallery-20261007/home-port.png" width="32%" alt="HOME" />
<img src="docs/screenshots/tdp4/motion/p25-decode.gif" width="32%" alt="P25 decoding a call" />
<img src="docs/screenshots/tdp4/motion/music-scan-waterfall.gif" width="32%" alt="MUSIC analyzer and waterfall" />
<img src="docs/screenshots/tdp4/motion/update.gif" width="32%" alt="UPDATE over WiFi" />
<img src="docs/screenshots/tdp4/motion/adsb.gif" width="32%" alt="ADS-B" />
<img src="docs/screenshots/tdp4/motion/map.gif" width="32%" alt="MAP following an aircraft" />
<img src="docs/screenshots/tdp4/motion/compass.gif" width="32%" alt="COMPASS FIND" />
<img src="docs/screenshots/tdp4/motion/sweep.gif" width="32%" alt="SWEEP finding nearby trackers" />
<img src="docs/screenshots/tdp4/motion/falls-flipper.gif" width="32%" alt="FALLS with Flipper bursts" />
<img src="docs/screenshots/tdp4/motion/subghz_scan2.gif" width="32%" alt="SUB-GHZ scan" />
<img src="docs/screenshots/tdp4/motion/subghz-flipper.gif" width="32%" alt="SUB-GHZ decoding a Flipper" />
<img src="docs/screenshots/tdp4/motion/fm-noaa.gif" width="32%" alt="FM on NOAA weather" />
<img src="docs/screenshots/tdp4/motion/mesh-notice.gif" width="32%" alt="Mesh message notices" />
<img src="docs/screenshots/tdp4/gallery-20261007/experiments-port.png" width="32%" alt="EXPERIMENTS" />
<img src="docs/screenshots/tdp4/motion/night-theme.gif" width="32%" alt="Night theme" />
</p>

<div align="center">

Recorded on the board: P25 locking on a live call, MUSIC playing through its analyzer and waterfall, UPDATE waiting for WiFi, ADS-B traffic on the mini map, MAP following an aircraft, COMPASS FIND turning on two channels, the P25 waterfall, FALLS and SUB-GHZ catching a Flipper's 433.92 MHz remote bursts, FM on NOAA weather, mesh messages arriving as notices, the LR2021 EXPERIMENTS list and the Night theme.

ASCII controls and labels are painted straight onto the panel, with pixel terrain in the new field map.<br>
Every control answers a tap and a key. The screen follows the way you hold the board, and F11 turns it by hand.

</div>

<h3 align="center">Apps</h3>

<h4 align="center">Radio</h4>

| App | What it does |
| --- | --- |
| P25 | Phase I trunking and conventional voice. AUTO/manual demodulation, channel-list scanning, a DMR activity view and a CALLS archive of heard calls. Phase II `[EXPERIMENTAL]` |
| FM | Analog FM/AM listening, stepped-band scan, mixed P25/FM channel lists, POCSAG pagers, CTCSS/DCS tone squelch, APRS, AIS ships and SAME/EAS weather alerts |
| ADS-B | Aircraft at 1090 MHz, traffic history, offline mini map and remembered home. Spoken callouts for new, located and lost aircraft |
| FALLS | P25, FM or LoRa waterfall with tap-to-mark tuning |
| CELL WATCH | Cell band survey |

<h4 align="center">Mesh and LoRa</h4>

| App | What it does |
| --- | --- |
| MESH | MeshCore messaging and nodes on the onboard radio. New messages show as a notice on any screen and can be read aloud. INSPECT traces a route, logs in to a node and reads its telemetry |
| LORA LABS | Direct radio controls, packet plots, received-signal history and a full-screen compass |

<h4 align="center">Sub-GHz and RF tools</h4>

| App | What it does |
| --- | --- |
| SUB-GHZ | Six tiles: read, analyzer, saved, read raw, learn and settings. Decoded detections and capture archive. OOK/FSK replay `[EXPERIMENTAL]` |
| REC | Shared capture workspace and `.sub` export, file replay, inline power and an animated waveform |
| MIX-RF | Keyboard CC1101 energy monitor, nRF24 activity scan and NFC detection |
| NFC | Classic 4K reads, verified block maps, key entry, hex/ASCII views and manual saves. Mini/1K full reads `[EXPERIMENTAL]` |
| EXPERIMENTS | LR2021 trials that may become apps, one at a time, including RS41 sondes and LR433 sensors with history. NOTE saves a readout to NOTES `[EXPERIMENTAL]` |

<h4 align="center">Maps and navigation</h4>

| App | What it does |
| --- | --- |
| MAP | Offline maps with aircraft trails, Mesh nodes, ships, APRS stations, marks and drawn lines. Follow a route or GPX file. FOLLOW, GO TO, layers and eight palettes. Pixel field view and archive picker `[EXPERIMENTAL]` |
| COMPASS | Tilt-compensated compass with true north. FIND points to a transmitter on any radio, with channel scanning, two radios at once, a hit log, and RADAR and HEAT views. GO TO and a level |
| GPS | Position and a track recorder that exports GPX |
| NOTES | Markdown notes on the SD card with checklists and live GPS, heading and radio lines |

<h4 align="center">Media and files</h4>

| App | What it does |
| --- | --- |
| MUSIC | SD-card audio player with level meters, octave bands, a scanning analyzer and a waterfall. The meters also follow the microphone |
| FILES | SD-card browsing, sorting and `.sub` preview. Replay opens directly in RECORD |

<h4 align="center">System</h4>

| App | What it does |
| --- | --- |
| UPDATE | Checks for a newer signed build over WiFi and installs it with one tap |
| SET | Brightness, theme (Night keeps everything red), font, sounds, voice and callouts, Daylight mode for the sun |
| RADIOS | Radio power, antenna selection, and Wi-Fi/Bluetooth startup toggles |
| LINK | Wi-Fi and Bluetooth: scan, signal and channel graphs. LINK SURVEY logs what it hears, with position and best signal, to a CSV per walk |
| DIAG | Memory, radios, sensors, rebuild counts and an error log |
| TERMINAL | The console on the screen, typed from the keyboard |

<h3 align="center">Scanning, replay and aircraft tracking</h3>

<div align="center">

Animated SUB-GHZ scans, FILES-to-RECORD playback, ADS-B mini maps and saved home. RADIOS adds Wi-Fi and Bluetooth startup toggles.<br>
**[Read the details →](docs/REPLAY_AND_AIRCRAFT.md)**

| RECORD replay | ADS-B mini map |
| :---: | :---: |
| ![Recording playback](docs/screenshots/tdp4/development-20260923/replay-land.png) | ![ADS-B mini map](docs/screenshots/tdp4/development-20260923/adsb-land.png) |

| SUB-GHZ scan | Radio startup controls |
| :---: | :---: |
| ![SUB-GHZ scanning](docs/screenshots/tdp4/development-20260923/subghz-scan-land.png) | ![Wi-Fi and Bluetooth controls](docs/screenshots/tdp4/development-20260923/radios-land.png) |

The site also builds P25 profiles, GPS scan lists, channel memories and offline map tiles for you.

Designed to work with my other project [CartoTUI, a terminal ascii map](https://github.com/SAMS0N1TE/CartoTUI).<br>
*Note: this integration is currently in development, untested, and may not work due to recent firmware updates.*

<img width="500" alt="keyboard-home" src="https://github.com/user-attachments/assets/dd1bd6fd-4d65-4800-b7c7-abdfebe80094" />

**[Install it from your browser](https://terminalbay.com/?m=lakeshark&board=tdp4)** &nbsp;•&nbsp; **[First flash guide](https://terminalbay.com/?m=wiki#tdp4/TDP4_FIRST_FLASH)**

</div>

<h3 align="center">Board notes</h3>

- This port defaults to the **568 x 1232 RM69A10 AMOLED with 16 MB flash**.
- For the HI8561 TFT (540 x 1168), add `boards/t_display_p4_tft.defaults` after `boards/t_display_p4.defaults`.
- The LCD-4.3 image and its 32 MB layout belong to a different board.
- LINK needs matching ESP-Hosted firmware on the ESP32-C6. Factory ESP-AT will not do.
- Flipper control on the T-Display P4 uses Bluetooth.

<h2 align="center">More ways to run it</h2>

<h3 align="center">Headless</h3>

<div align="center">

The Waveshare ESP32-P4-NANO and ESP32-P4-WIFI6 run the same receivers with no screen.<br>
You drive them from a serial console or from the Flipper, which suits leaving the radio in a bag with the antenna.

</div>

<h3 align="center">The Flipper head</h3>

<div align="center">

The [Flipper app](https://github.com/SAMS0N1TE/LakeShark-Flipper) controls the radio over Bluetooth or the GPIO header.<br>
No pairing code. The Flipper advertises and the radio connects to it.

| | |
| :---: | :---: |
| ![map](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_map.png) | ![traffic](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_traffic.png) |
| Offline map with aircraft on it | ADS-B traffic |
| ![launcher](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/launcher.png) | ![p25](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/p25_vfo.png) |
| Launcher | P25 |

**[The full guide](https://terminalbay.com/?m=wiki#flipper/Home)** covers every page, its buttons and all 44 screenshots.

<img width="700" alt="bench" src="https://github.com/user-attachments/assets/3bef1680-aa7d-4df0-a0ab-5b6280b804c1" />

</div>

<h3 align="center">Waveshare Touch-LCD-4.3</h3>

<div align="center">

If you already own one, there is a 480 x 800 image for it.<br>
It came before the T-Display port and it is not where new work goes, so treat it as worth a try on hardware you already have.

[Install it](https://terminalbay.com/?m=lakeshark&board=lcd43) &nbsp;•&nbsp; [Quick start](https://terminalbay.com/?m=wiki#lcd43/LCD43_QUICKSTART)

The Touch-LCD-4B and Smart 86 Box build from source at 720 x 720.

</div>

<h2 align="center">Experiments</h2>

<div align="center">

Tags marked `[EXPERIMENTAL]` identify features still awaiting full hardware or live-RF validation.

</div>

- **P25 Phase II** `[EXPERIMENTAL]`: manual traffic channel or automatic grant following, both off after restart. Bench-tested on a public recording, live RF unverified. [Details](docs/P25_PHASE2.md).
- **HackRF** `[EXPERIMENTAL]`: FM, pagers, ACARS, P25, REC and ADS-B can use a HackRF One as their SDR, and rates below 2 MSPS run by decimation. Pagers decode the same pages as on an RTL-SDR. P25 decoding is not yet confirmed on live signals. [Status](docs/HACKRF_BRINGUP.md).
- **LoRa bearing plot** `[EXPERIMENTAL]`: RSSI grouped by compass heading, not a validated direction finder.
- **Location scanning:** [mixed P25/FM lists and GPS filtering](docs/LOCATION_SCAN.md). CSV/JSON imports work without RadioReference. The optional RadioReference adapter `[EXPERIMENTAL]` needs an approved application key and the user's Premium account, and live account authentication has not been validated.

<h2 align="center">Build from source</h2>

Use the [pinned ESP-IDF 5.4.3 SDK](sdk/esp-idf-5.4.3/README.md). Give every board its own build directory:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build
idf.py -B build_tdp4_tft -D SDKCONFIG=build_tdp4_tft/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults;boards/t_display_p4_tft.defaults" build
idf.py -B build_nano -D SDKCONFIG=build_nano/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/nano_headless_ble.defaults" build
idf.py -B build_lcd43 -D SDKCONFIG=build_lcd43/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/lcd43_gui.defaults" build
```

- `boards/` holds the defaults and partition layouts, and `main/idf_component.yml` pins the dependencies.
- The Waveshare BSP is patched in `managed_components/`, so use a fresh build directory on a new machine rather than an old CMake cache.

Run the host checks from the repository directory:

```powershell
pwsh -File bench/verify.ps1 -Level host
```

<h2 align="center">°<)))>< Standing on</h2>

<div align="center">

LakeShark builds on these projects and contributors.<br>
Their code, hardware examples and published work make the receivers and tools possible.

</div>

| Project | Credit |
| --- | --- |
| [rtl-sdr / librtlsdr](https://osmocom.org/projects/rtl-sdr) | Osmocom. The dongle driver everything starts from. GPL-2.0+ |
| [xtrsdr](https://github.com/XTR1984/xtrsdr) | XTR1984. Cut librtlsdr down until it fit an ESP32. Without this there is no project |
| [OP25](https://github.com/boatbod/op25) | Pavel Yazev's fixed-point `imbe_vocoder`. Experimental Phase II decoding by Max H. Parke, Graham J. Norbury and contributors. GPL-3.0+ |
| [libmodes](https://github.com/watson/libmodes) / [dump1090](https://github.com/antirez/dump1090) | Thomas Watson and Salvatore Sanfilippo. Mode S decoder lineage. The bundled libmodes notice is BSD-2-Clause |
| [ADS-B Scope](https://github.com/jstockdale/T-Display-P4/tree/adsb) | John Stockdale (@jstockdale) and Off by One, Inc. Original ADS-B Scope implementation, and the reference for LakeShark's single-frame CPR decoding against the aircraft's previous fix |
| [DSD](https://github.com/szechyjs/dsd) and [dsd-fme](https://github.com/lwvmobile/dsd-fme) | szechyjs and lwvmobile. The P25 framing and symbol lineage the decoders follow. The DSD files here carry its ISC-style notice |
| [mbelib](https://github.com/szechyjs/mbelib) | szechyjs. Kept as a fallback vocoder. ISC |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif. The whole platform |
| [LILYGO device drivers](https://github.com/Xinyuan-LilyGO/lilygo_device_driver) | T-Display P4 and MIX-RF wiring, power sequencing and hardware examples |
| [MeshCore](https://github.com/meshcore-dev/MeshCore) | Scott Powell / Ripple Radios and contributors. Onboard LoRa mesh stack |
| [CartoTUI](https://github.com/SAMS0N1TE/CartoTUI) / [ZeroMesh](https://github.com/SAMS0N1TE/ZeroMesh) | libcarto vector rendering and the embedded PMTiles reader |
| [PMTiles](https://github.com/protomaps/PMTiles) | Protomaps. One file, no server, seekable. What makes offline maps possible here |
| [OpenStreetMap](https://www.openstreetmap.org/copyright) | The map data itself, ODbL |
| [Flipper Zero firmware](https://github.com/flipperdevices/flipperzero-firmware) | Flipper Devices and contributors. Control-head platform and Crypto1 helpers adapted for Classic NFC reads |
| [LVGL](https://github.com/lvgl/lvgl) / [DejaVu](https://dejavu-fonts.github.io/) | UI support and fonts. Their notices remain bundled |
| [Waveshare](https://github.com/waveshareteam) | Original P4 board support and hardware examples |

**Field-map design references:** [Cardputer offgrid map](https://github.com/yuiseki/m5-cardputer-offgrid-tiny-map) by yuiseki for rendering reuse, and [HikePod](https://github.com/nongxl/HikePod) by nongxl for field navigation. These informed the design, and no code was copied from either project.

<div align="center">

---

LakeShark itself is [GPL-3.0](LICENSE). Third-party code keeps its own notices.<br>
The [inventory](docs/THIRD_PARTY_NOTICES.md) lists what is bundled and under what terms.

<sub>What you are allowed to listen to is local law and yours to check.</sub>

</div>
