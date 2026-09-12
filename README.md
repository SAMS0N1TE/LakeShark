![lakeshark_banner](https://github.com/user-attachments/assets/34b12b2c-fd64-4fdc-850c-e9c93d7aede7#gh-light-mode-only)
![lakeshark_banner_dark](https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09#gh-dark-mode-only)

A handheld SDR scanner running on the ESP32-P4 with an [RTL-SDR Blog V3 or V4](https://www.ebay.com/str/rtlsdrblog?_trksid=p4429486.m3561.l161211) plugged into its USB host port. P25 Phase 1 trunking, FM and POCSAG, ADS-B aircraft, sub-GHz capture to Flipper `.sub` files, and a MeshCore node on LoRa.

### ><> Everything else is on [terminalbay.com](https://terminalbay.com/?m=lakeshark)

The setup page flashes a board from Chrome or Edge with one button, no toolchain and no Python. It also builds P25 profiles, channel memories and offline map tiles for you. The [wiki](https://terminalbay.com/?m=wiki) has the guides and every screenshot.

Designed to work with my other project [CartoTUI, a terminal ascii map](https://github.com/SAMS0N1TE/CartoTUI).

## <°)))>< LilyGO T-Display-P4

The main board. 4.1 inch 568 x 1232 AMOLED, 16 MB flash, detachable keyboard.

| | | |
| --- | --- | --- |
| <img src="https://terminalbay.com/wiki/tdp4/images/link_home.png" width="200" /> | <img src="https://terminalbay.com/wiki/tdp4/images/p25_port.png" width="200" /> | <img src="https://terminalbay.com/wiki/tdp4/images/adsb_port.png" width="200" /> |
| HOME | P25 | ADS-B |
| <img src="https://terminalbay.com/wiki/tdp4/images/falls_lora_port.png" width="200" /> | <img src="https://terminalbay.com/wiki/tdp4/images/map_port.png" width="200" /> | <img src="https://terminalbay.com/wiki/tdp4/images/link_wifi.png" width="200" /> |
| FALLS, LoRa sweep | MAP | LINK |

The whole interface is a character grid painted straight onto the panel. Every control answers a tap and a key. The screen follows the way you hold the board, and F11 turns it by hand.

| App | What it does |
|---|---|
| P25 | Phase 1 trunking: control channel, grants, talkgroups, IMBE voice |
| FM | Analogue listening, band scan, POCSAG pagers |
| ADS-B | Aircraft at 1090 MHz: table, radar, traffic history |
| FALLS | One waterfall, fed by P25, FM, or the board's own LoRa radio sweeping a band |
| MESH | A MeshCore node on the SX1262. The transmitter stays disarmed until you arm it |
| REC | Sub-GHz OOK capture to Flipper `.sub` files |
| MAP | Vector tiles from the SD card with mesh nodes on them |
| GPS | Position and a track recorder that exports GPX |
| RADIOS | What is powered, and the switch for each |
| DIAG | Memory, radios, sensors, rebuild counts |
| SET | Brightness, theme, font, sounds, Daylight mode for the sun |
| LINK | Wi-Fi and Bluetooth: scan, join with a masked keyboard, signal and channel graphs |

**[Install it from your browser](https://terminalbay.com/?m=lakeshark&board=tdp4)** or read the [first flash guide](https://terminalbay.com/?m=wiki#tdp4/TDP4_FIRST_FLASH).

This port is for the **568 x 1232 RM69A10 AMOLED with 16 MB flash**. The other T-Display-P4 panel SKU needs different timings and a different touch driver. The LCD-4.3 image and its 32 MB layout belong to a different board.

LINK needs matching ESP-Hosted firmware on the ESP32-C6. Factory ESP-AT will not do.
Flipper control on the T-Display-P4 uses Bluetooth.

## }<((((()°> Headless

The Waveshare ESP32-P4-NANO and ESP32-P4-WIFI6 run the same receivers with no screen. You drive them from a serial console or from the Flipper, which suits leaving the radio in a bag with the antenna.

## ><)))°> The Flipper head

The [Flipper app](https://github.com/SAMS0N1TE/LakeShark-Flipper) controls the radio over Bluetooth or the GPIO header. No pairing code. The Flipper advertises and the radio connects to it.

| | |
| --- | --- |
| ![map](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_map.png) | ![traffic](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_traffic.png) |
| Offline map with aircraft on it | ADS-B traffic |
| ![launcher](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/launcher.png) | ![p25](https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/p25_vfo.png) |
| Launcher | P25 |

**[The full guide](https://terminalbay.com/?m=wiki#flipper/Home)** covers every page, its buttons and all 44 screenshots.

## ><)))°> Waveshare Touch-LCD-4.3

If you already own one, there is a 480 x 800 image for it. It came before the T-Display port and it is not where new work goes, so treat it as worth a try on hardware you already have. [Install it](https://terminalbay.com/?m=lakeshark&board=lcd43) or read the [quick start](https://terminalbay.com/?m=wiki#lcd43/LCD43_QUICKSTART).

The Touch-LCD-4B and Smart 86 Box build from source at 720 x 720.

## ><)))O> Build from source

Use ESP-IDF 5.4.3. Give every board its own build directory:

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

## °<)))>< Standing on

None of the hard parts are mine. A P25 receiver on a microcontroller only exists because people spent years getting these right and then gave them away.

| | |
|---|---|
| [rtl-sdr / librtlsdr](https://osmocom.org/projects/rtl-sdr) | Osmocom. The dongle driver everything starts from. GPL-2.0+ |
| [xtrsdr](https://github.com/XTR1984/xtrsdr) | XTR1984. Cut librtlsdr down until it fit an ESP32. Without this there is no project |
| [OP25](https://github.com/boatbod/op25) | Pavel Yazev's fixed-point `imbe_vocoder` for P25 voice. GPL-3.0+ |
| [DSD](https://github.com/szechyjs/dsd) and [dsd-fme](https://github.com/lwvmobile/dsd-fme) | szechyjs and lwvmobile. The P25 framing and symbol lineage the decoders follow. The DSD files here carry its ISC-style notice |
| [mbelib](https://github.com/szechyjs/mbelib) | szechyjs. Kept as a fallback vocoder. ISC |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif. The whole platform |
| [PMTiles](https://github.com/protomaps/PMTiles) | Protomaps. One file, no server, seekable. What makes offline maps possible here |
| [OpenStreetMap](https://www.openstreetmap.org/copyright) | The map data itself, ODbL |
| [Flipper Zero](https://flipperzero.one/) | The head runs as a Flipper app |

LakeShark itself is [GPL-3.0](LICENSE). Third-party code keeps its own notices; the [inventory](docs/THIRD_PARTY_NOTICES.md) lists what is bundled and under what terms.

What you are allowed to listen to is local law and yours to check.
