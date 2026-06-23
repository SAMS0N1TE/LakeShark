![lakeshark_banner](https://github.com/user-attachments/assets/34b12b2c-fd64-4fdc-850c-e9c93d7aede7#gh-light-mode-only)
![lakeshark_banner_dark](https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09#gh-dark-mode-only)

https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09
A handheld SDR scanner running on the Waveshare [ESP32-P4-NANO](https://www.waveshare.com/esp32-p4-nano.htm?srsltid=AfmBOoqwx_UtnddP57XurmPjLDD6xyBxvlo3kfWMzl45RvUZGmMNA4tY) and the [ESP32-P4 Smart 86 Box](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) with an [RTL-SDR Blog V3 or V4](https://www.ebay.com/str/rtlsdrblog?_trksid=p4429486.m3561.l161211) plugged into its USB host port.

It's currently in a very early devlopment stage and will be broken up into a few different releases. Right now I will release a headless firmware and a GUI version. The GUI started on [esp-brookesia](https://github.com/espressif/esp-brookesia) but I've since swapped it for my own handheld-radio LCD shell (boots straight into the last app, no launcher, a bottom row switches between systems). I will try my best to get these to be cross compatible with different boards and configurations, so please submit an issue if you have trouble.

Designed to work with my other project [CartoTUI - a terminal ascii map.](https://github.com/SAMS0N1TE/CartoTUI) Only very basic ADS-B tracking. Has issues, but works as a proof of concept. Will focus on this once the ADS-B/P25 is more refined.

<img src="https://github.com/user-attachments/assets/d4b078d6-095f-4f21-92f9-066e885df719" />

## <°)))><

- **P25** — Project 25 Phase 1 (C4FM) trunked/conventional digital voice.
  On-device IMBE voice decode using OP25's fixed-point vocoder; live NAC / TG /
  SRC, BCH health, and display.
- **FM Monitor** — wideband broadcast FM, narrowband FM voice (LISTEN), band
  SCAN, and POCSAG pager decode — sharing one integer `rtl_fm`-style front end.
- **ADS-B** — 1090 MHz aircraft tracking.
- **LoRa Mesh** — on-board SX1262 + LoRaMesher gateway with a live node/link view, VERY WIP.

## Hardware notes

You need a Waveshare ESP32-P4-NANO or ESP32-P4 Smart 86 Box because of how the USB host pinout and PSRAM are wired. Other ESP32-P4 boards probably work but I haven't tried them. RTL-SDR V3/V4 are the target dongles; older V3 sticks work but you'd lose the triplexer routing (Not crazy important in my tests). In my opinion, I wouldn't try sourcing the V4's as they're a dead end in terms of support. 

Pin mapping for the audio: I²S MCLK=13 BCK=12 WS=10 DOUT=9 DIN=11, codec PA enable on GPIO 53, I²C SDA=7 SCL=8, USB VBUS enable on 46 for nano board. Smart 86 Box requires soldering. Will type out a tutorial soon. Not a hard thing to do at all though. 

- Optional SX1262 LoRa module for the mesh app WIP.

## Build & flash

Requires **ESP-IDF v5.4.3** (configured with Python 3.12). From the project dir:

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

A convenience wrapper (`idf12.sh` / `idf12.ps1`) that sources the IDF environment
is included one level up in the original tree; plain `idf.py` works the same.

> The `managed_components/` tree is committed because the Waveshare BSP is patched
> in-place. After switching host OS, run `idf.py fullclean` once (the `build/`
> cache stores absolute paths).

## Architecture highlights

- **Integer FM front end** — a fixed-point `rtl_fm`-derived pipeline at 256 kSPS
  (FM) / 240 kSPS (P25) replaces a float chain that saturated a core; the demod
  runs comfortably real-time with zero IQ drops.
- **Decoupled USB streaming** — self-resubmitting USB transfers fill a PSRAM IQ
  ring; a pump task reposts so the demod never blocks USB servicing.
- **Fixed-point IMBE** — P25 voice uses OP25's integer `imbe_vocoder` (no
  per-sample `cosf`), bringing one LDU's synthesis well under real-time on the P4.
- **Low-power TUI graphics** — the radio UIs use monospace text meters/sparklines
  instead of live `lv_bar`/`lv_chart` redraws to keep audio glitch-free.
- **Custom LCD shell** — a hand-rolled LVGL UI styled like a handheld radio screen
  (pastel-on-black, mono font, bordered LCD faces, segmented ASCII touch sliders).
  No launcher: it boots into the last-used app, a persistent bottom rail (and the
  BOOT button) switches systems, and `< >` edge buttons flip each app's tabs.


## Sneak Peaks for Box 86 3D prints WIP

<img width="1920" height="1920" alt="HighFront" src="https://github.com/user-attachments/assets/9d375356-a956-4024-9c1d-b8af96045b2a" />
<img width="1920" height="1920" alt="Back" src="https://github.com/user-attachments/assets/675aa297-e619-4799-ae6c-2f93e1925994" />
<img width="1920" height="1920" alt="Low - Copy" src="https://github.com/user-attachments/assets/43cc9a11-f0ca-4ed7-8ecd-8e9f56073d97" />

## Credits & licenses

This project is released under the **GNU GPL v3** (see `LICENSE`), as required by
its GPL dependencies. It bundles and builds on:

- [rtl-sdr / librtlsdr](https://osmocom.org/projects/rtl-sdr) — Osmocom (GPL-2.0+)
- [xtrsdr](https://github.com/XTR1984/xtrsdr) - For the amazing work with getting it lean enough for the ESP32.
- [OP25 `imbe_vocoder`](https://github.com/boatbod/op25) — Pavel Yazev (GPL-3.0+)
- [mbelib](https://github.com/szechyjs/mbelib) — ISC (kept as a fallback decoder)
- [DSD / dsd-fme](https://github.com/lwvmobile/dsd-fme) — P25 framing (GPL)
- [LoRaMesher](https://github.com/LoRaMesher/LoRaMesher) — mesh networking
- [esp-brookesia](https://github.com/espressif/esp-brookesia) — UI launcher
- Espressif ESP-IDF and the Waveshare ESP32-P4 BSP

Original copyright/license headers in third-party sources are preserved verbatim.

> **Legal note:** the IMBE/AMBE voice codecs are covered by patents held by DVSI.
> This firmware is provided for educational and experimental use; you are
> responsible for compliance with applicable laws and licenses in your region.
