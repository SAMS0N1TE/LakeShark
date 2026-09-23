![lakeshark_banner](https://github.com/user-attachments/assets/34b12b2c-fd64-4fdc-850c-e9c93d7aede7#gh-light-mode-only)
![lakeshark_banner_dark](https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09#gh-dark-mode-only)

<p align="center"><a href="https://terminalbay.com/?m=lakeshark-showcase">Terminalbay.com</a></p>

LakeShark is a handheld SDR scanner and radio workbench built around the **LilyGO T-Display P4**: a 4.1-inch AMOLED, touch controls, detachable keyboard, GPS and a nine-axis motion sensor. Scan radios, follow aircraft and Mesh nodes on offline maps, experiment with LoRa, and save observations in a field journal.

Plug in an [RTL-SDR Blog V3 or V4](https://www.ebay.com/str/rtlsdrblog?_trksid=p4429486.m3561.l161211) for P25 Phase 1 trunking, FM, POCSAG, ADS-B and sub-GHz capture. The onboard SX1262 runs MeshCore or LoRa Labs. The optional MIX-RF keyboard adds CC1101, nRF24 and NFC tools.

<p align="center"><a href="https://terminalbay.com/?m=lakeshark-showcase#renders/01">New case design renders</a></p>

<p align="center">
  <img height="610" alt="LakeShark case turntable" src="assets/lakeshark_turntable.gif">
</p>

**[Flash it from your browser](https://terminalbay.com/?m=lakeshark)** · **[Download a release](https://github.com/SAMS0N1TE/LakeShark/releases/latest)** · **[Wiki and guides](https://terminalbay.com/?m=wiki)**

**[Roadmap](https://terminalbay.com/?m=lakeshark-roadmap)** · **[Submit a recommendation](https://terminalbay.com/?m=lakeshark-roadmap#ask)**

**[Scanning, replay and aircraft tracking](docs/REPLAY_AND_AIRCRAFT.md)** — Animated SUB-GHZ scans, FILES-to-RECORD playback, ADS-B mini maps and saved home. RADIOS adds Wi-Fi and Bluetooth startup toggles.

| RECORD replay | ADS-B mini map |
| --- | --- |
| ![Recording playback](docs/screenshots/tdp4/development-20260923/replay-land.png) | ![ADS-B mini map](docs/screenshots/tdp4/development-20260923/adsb-land.png) |

| SUB-GHZ scan | Wireless startup controls |
| --- | --- |
| ![SUB-GHZ scanning](docs/screenshots/tdp4/development-20260923/subghz-scan-land.png) | ![Wi-Fi and Bluetooth controls](docs/screenshots/tdp4/development-20260923/radios-land.png) |

The site also builds P25 profiles, GPS scan lists, channel memories and offline map tiles for you.

Designed to work with my other project [CartoTUI, a terminal ascii map](https://github.com/SAMS0N1TE/CartoTUI).

## <°)))>< LilyGO T-Display-P4
  <img width="1600" alt="hero-falls" src="https://github.com/user-attachments/assets/bdd8069b-3c1a-4dd6-a821-1526e724c950" />

The primary development target: 4.1-inch 568 x 1232 AMOLED, 16 MB flash, onboard SX1262 LoRa, GPS and nine-axis sensing. Use touch alone or add the detachable keyboard with its MIX-RF radios.

| | | |
| --- | --- | --- |
| <img src="docs/screenshots/tdp4/gallery-20260923/home-port.png" width="200" /> | <img src="docs/screenshots/tdp4/gallery-20260923/p25-port.png" width="200" /> | <img src="docs/screenshots/tdp4/gallery-20260923/adsb-port.png" width="200" /> |
| HOME | P25 | ADS-B |
| <img src="docs/screenshots/tdp4/gallery-20260923/falls-p25-port.png" width="200" /> | <img src="docs/screenshots/tdp4/gallery-20260923/map-port.png" width="200" /> | <img src="docs/screenshots/tdp4/gallery-20260923/subghz-port.png" width="200" /> |
| FALLS, P25 waterfall | MAP | SUB-GHZ |

ASCII controls and labels are painted straight onto the panel, with pixel terrain in the new field map. Every control answers a tap and a key. The screen follows the way you hold the board, and F11 turns it by hand.

| App | What it does |
|---|---|
| P25 | Phase I trunking and conventional voice; AUTO/manual demodulation, channel-list scanning; Phase II `[EXPERIMENTAL]` |
| FM | Analog FM/AM listening, stepped-band scan, mixed P25/FM channel lists and POCSAG pagers |
| ADS-B | Aircraft at 1090 MHz, traffic history; offline mini map and remembered home in 2.2.2 |
| FALLS | P25, FM or LoRa waterfall; tap-to-mark tuning |
| MESH | MeshCore messaging and nodes on the onboard SX1262 |
| LORA LABS | Direct radio controls, packet plots, received-signal history and a full-screen compass |
| JOURNAL | Field notes with radio, GPS and motion attachments; SD archives and bookmarks |
| MIX-RF | Keyboard CC1101 energy monitor, nRF24 activity scan and NFC detection |
| NFC | Classic 4K reads, verified block maps, key entry, hex/ASCII views and manual saves; Mini/1K full reads `[EXPERIMENTAL]` |
| REC | Shared capture workspace and `.sub` export; 2.2.2 adds file replay, inline power and an animated waveform. |
| SUB-GHZ | Scan, learn, watch, decoded detections and capture archive; OOK/FSK replay `[EXPERIMENTAL]` |
| FILES | SD-card browsing, sorting and `.sub` preview; replay opens directly in RECORD |
| MAP | Offline maps, aircraft and Mesh nodes; pixel field view, GPS marker and archive picker `[EXPERIMENTAL]` |
| GPS | Position and a track recorder that exports GPX |
| RADIOS | Radio power, antenna selection, and Wi-Fi/Bluetooth startup toggles |
| DIAG | Memory, radios, sensors, rebuild counts |
| SET | Brightness, theme, font, sounds, Daylight mode for the sun |
| LINK | Wi-Fi and Bluetooth: scan, signal and channel graphs |

<img width="900" height="900" alt="keyboard-home" src="https://github.com/user-attachments/assets/dd1bd6fd-4d65-4800-b7c7-abdfebe80094" />

**[Install it from your browser](https://terminalbay.com/?m=lakeshark&board=tdp4)** or read the [first flash guide](https://terminalbay.com/?m=wiki#tdp4/TDP4_FIRST_FLASH).

This port is for the **568 x 1232 RM69A10 AMOLED with 16 MB flash**. The other T-Display-P4 panel SKU needs different timings and a different touch driver. The LCD-4.3 image and its 32 MB layout belong to a different board.

LINK needs matching ESP-Hosted firmware on the ESP32-C6. Factory ESP-AT will not do.
Flipper control on the T-Display-P4 uses Bluetooth.

> **P4 keyboard power warning:** Do not connect the keyboard's 5V USB output back into the P4 yet. This caused repeated power cycling in testing, even without the RTL attached. I'm investigating the easiest reliable way to power the RTL on battery.

## }<((((-- Headless

The Waveshare ESP32-P4-NANO and ESP32-P4-WIFI6 run the same receivers with no screen. You drive them from a serial console or from the Flipper, which suits leaving the radio in a bag with the antenna.

## ><))))º< ⁠The Flipper head

The [Flipper app](https://github.com/SAMS0N1TE/LakeShark-Flipper) controls the radio over Bluetooth or the GPIO header. No pairing code. The Flipper advertises and the radio connects to it.

| | |
| --- | --- |
| ![map](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_map.png) | ![traffic](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_traffic.png) |
| Offline map with aircraft on it | ADS-B traffic |
| ![launcher](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/launcher.png) | ![p25](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/p25_vfo.png) |
| Launcher | P25 |

**[The full guide](https://terminalbay.com/?m=wiki#flipper/Home)** covers every page, its buttons and all 44 screenshots.

<img width="700" alt="bench" src="https://github.com/user-attachments/assets/3bef1680-aa7d-4df0-a0ab-5b6280b804c1" />

## ><)))°> Waveshare Touch-LCD-4.3

If you already own one, there is a 480 x 800 image for it. It came before the T-Display port and it is not where new work goes, so treat it as worth a try on hardware you already have. [Install it](https://terminalbay.com/?m=lakeshark&board=lcd43) or read the [quick start](https://terminalbay.com/?m=wiki#lcd43/LCD43_QUICKSTART).

The Touch-LCD-4B and Smart 86 Box build from source at 720 x 720.

## }<))•> Experiments

Tags marked `[EXPERIMENTAL]` identify features still awaiting full hardware or live-RF validation.

- **P25 Phase II** `[EXPERIMENTAL]`: manually tuned traffic channel and slot, off after restart. Recorded-symbol replay works on the P4; live RF and automatic call following remain unverified. [Details](docs/P25_PHASE2.md).
- **HackRF** `[EXPERIMENTAL]`: USB IQ transport tested at 2 MSPS. Successful ADS-B decoding is somewhat verified; P25/FM support is deferred. [Status](docs/HACKRF_BRINGUP.md).
- **LoRa bearing plot** `[EXPERIMENTAL]`: RSSI grouped by compass heading, not a validated direction finder.

## ><)))O> Build from source

Use the [pinned ESP-IDF 5.4.3 SDK](sdk/esp-idf-5.4.3/README.md). Give every board its own build directory:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build
idf.py -B build_nano -D SDKCONFIG=build_nano/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/nano_headless_ble.defaults" build
idf.py -B build_lcd43 -D SDKCONFIG=build_lcd43/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/lcd43_gui.defaults" build
```

`boards/` holds the defaults and partition layouts, and `main/idf_component.yml` pins the dependencies. The Waveshare BSP is patched in `managed_components/`, so use a fresh build directory on a new machine rather than an old CMake cache.

Run the host checks from the repository directory:

```powershell
pwsh -File bench/verify.ps1 -Level host
```

[Location scanning](docs/LOCATION_SCAN.md) covers mixed P25/FM lists and GPS filtering.
CSV/JSON imports work without RadioReference. The optional RadioReference adapter
`[EXPERIMENTAL]` needs an approved application key and the user's Premium account;
live account authentication has not been validated.

**Current P4 release: [2.2.2](https://github.com/SAMS0N1TE/LakeShark/releases/tag/v2.2.2).** Full browser install, FILES → RECORD replay, aircraft mini maps, saved home and improved GPS recording startup. Experimental radio features remain marked.

Older changes are in the [release history](https://github.com/SAMS0N1TE/LakeShark/releases).

## °<)))>< Standing on

LakeShark builds on these projects and contributors. Their code, hardware examples and published work make the receivers and tools possible.

| | |
|---|---|
| [rtl-sdr / librtlsdr](https://osmocom.org/projects/rtl-sdr) | Osmocom. The dongle driver everything starts from. GPL-2.0+ |
| [xtrsdr](https://github.com/XTR1984/xtrsdr) | XTR1984. Cut librtlsdr down until it fit an ESP32. Without this there is no project |
| [OP25](https://github.com/boatbod/op25) | Pavel Yazev's fixed-point `imbe_vocoder`; experimental Phase II decoding by Max H. Parke, Graham J. Norbury and contributors. GPL-3.0+ |
| [libmodes](https://github.com/watson/libmodes) / [dump1090](https://github.com/antirez/dump1090) | Thomas Watson and Salvatore Sanfilippo. Mode S decoder lineage; the bundled libmodes notice is BSD-2-Clause |
| [ADS-B Scope](https://github.com/jstockdale/T-Display-P4/tree/adsb) | John Stockdale (@jstockdale) and Off by One, Inc. Original ADS-B Scope implementation; reference for LakeShark's single-frame CPR decoding against the aircraft's previous fix |
| [DSD](https://github.com/szechyjs/dsd) and [dsd-fme](https://github.com/lwvmobile/dsd-fme) | szechyjs and lwvmobile. The P25 framing and symbol lineage the decoders follow. The DSD files here carry its ISC-style notice |
| [mbelib](https://github.com/szechyjs/mbelib) | szechyjs. Kept as a fallback vocoder. ISC |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif. The whole platform |
| [LILYGO device drivers](https://github.com/Xinyuan-LilyGO/lilygo_device_driver) | T-Display P4 and MIX-RF wiring, power sequencing and hardware examples |
| [MeshCore](https://github.com/meshcore-dev/MeshCore) | Scott Powell / Ripple Radios and contributors. Onboard LoRa mesh stack |
| [CartoTUI](https://github.com/SAMS0N1TE/CartoTUI) / [ZeroMesh](https://github.com/SAMS0N1TE/ZeroMesh) | libcarto vector rendering and the embedded PMTiles reader |
| [PMTiles](https://github.com/protomaps/PMTiles) | Protomaps. One file, no server, seekable. What makes offline maps possible here |
| [OpenStreetMap](https://www.openstreetmap.org/copyright) | The map data itself, ODbL |
| [Flipper Zero firmware](https://github.com/flipperdevices/flipperzero-firmware) | Flipper Devices and contributors. Control-head platform and Crypto1 helpers adapted for Classic NFC reads |
| [LVGL](https://github.com/lvgl/lvgl) / [DejaVu](https://dejavu-fonts.github.io/) | UI support and fonts; their notices remain bundled |
| [Waveshare](https://github.com/waveshareteam) | Original P4 board support and hardware examples |

**Field-map design references:** [Cardputer offgrid map](https://github.com/yuiseki/m5-cardputer-offgrid-tiny-map) by yuiseki for rendering reuse, and [HikePod](https://github.com/nongxl/HikePod) by nongxl for field navigation. These informed the design; no code was copied from either project.

LakeShark itself is [GPL-3.0](LICENSE). Third-party code keeps its own notices; the [inventory](docs/THIRD_PARTY_NOTICES.md) lists what is bundled and under what terms.

What you are allowed to listen to is local law and yours to check.
