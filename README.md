![lakeshark_banner](https://github.com/user-attachments/assets/34b12b2c-fd64-4fdc-850c-e9c93d7aede7#gh-light-mode-only)
![lakeshark_banner_dark](https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09#gh-dark-mode-only)

LakeShark is a handheld SDR scanner running on the ESP32-P4 with an RTL-SDR
plugged into its USB host port. It builds headless from a serial console or
with an LVGL touch UI on the Waveshare LCD carriers, and a Flipper Zero can
drive the headless builds over BLE/UART.

The LCD4.3 build is a preview: it runs, and the limits below are real.
Start with the [release notes](docs/LCD43_RELEASE_NOTES.md),
[first flash](docs/LCD43_FIRST_FLASH.md) for a board that has never run it,
[quickstart](docs/LCD43_QUICKSTART.md) for upgrading one that has, and the
[troubleshooting guide](docs/LCD43_TROUBLESHOOTING.md).

More context, and where this fits with the operator's other work:
**<https://terminalbay.com>**.

## Hardware

You need one of the ESP32-P4 boards below, an
[RTL-SDR Blog V3 or V4](https://www.rtl-sdr.com/), and a USB host cable. The
GUI boards need soldering; the pin map for the nano's I²S audio is `MCLK=13
BCK=12 WS=10 DOUT=9 DIN=11`, codec PA on GPIO 53, I²C `SDA=7 SCL=8`, USB VBUS
enable on GPIO 46.

| Board | Flash | State |
|---|---|---|
| [ESP32-P4-NANO](https://www.waveshare.com/esp32-p4-nano.htm) | 16 MB | Headless. Console + Flipper link. |
| [ESP32-P4-WIFI6](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6.htm) | 32 MB | Headless. Console + Flipper link. |
| [ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm) (Smart 86 Box) | 32 MB | LVGL GUI on the 720×720 panel. Builds, but **not part of this release** — the home picker overflows this geometry by a few pixels. |
| [ESP32-P4-WIFI6-Touch-LCD-4.3](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm) | 32 MB | Experimental preview candidate on the 480×800 panel; see power and validation limits below. |

Published artifacts, when available, are on the
[releases page](https://github.com/SAMS0N1TE/LakeShark/releases).
Use the board and validation scope stated in each release, not this hardware
inventory, to decide which image is appropriate.

The LCD-4.3 note: the on-board battery port routes 5 V away from GPIO and the
USB HS port. Removing the same MOSFET that unlocks the 86 Box does not fully
unlock this one, so the only reliable way to power it is the UART USB-C port.
LCD-4.3 GUI development is active, with an experimental preview candidate in
local preparation. This is not an all-board release, and does not establish
battery-powered or modified-board operation.

## Build

Requires **ESP-IDF v5.5.4** and its managed Python environment. The current
candidate was built with IDF's Python 3.11 environment. Each board must use a
separate build directory **and SDKCONFIG path**:

```bash
idf.py -B build_nano  -D SDKCONFIG=build_nano/sdkconfig  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/nano_headless_ble.defaults" build
idf.py -B build_wifi6 -D SDKCONFIG=build_wifi6/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/wifi6_headless_ble.defaults" build
idf.py -B build_4b    -D SDKCONFIG=build_4b/sdkconfig    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/touch_lcd_4b_gui.defaults" build
idf.py -B build_lcd43 -D SDKCONFIG=build_lcd43/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/lcd43_gui.defaults" build
```

`bench/configs.json` lists every configuration the gate builds and is the
single source of truth for which board takes which defaults.

## Flash

For the LCD4.3 preview, follow the packaged upgrade instructions and
[quickstart](docs/LCD43_QUICKSTART.md). It is not an initial-provisioning image.

The maintainer's Windows wrapper reads the CH343 serial and refuses a mismatch.
Its recorded identities belong to the maintainer's bench, not arbitrary boards;
do not bypass this check or copy an unrelated identity:

```powershell
pwsh -File bench/flash.ps1 p4-nano
```

For a separately verified complete Nano installation built from source, an
example full-flash command is below. Full flashing can overwrite data images;
it is **not** the LCD preview's application-only upgrade procedure:

```bash
idf.py -B build_nano -D SDKCONFIG=build_nano/sdkconfig -p /dev/ttyACM0 flash monitor
```

`managed_components/` is committed because the Waveshare BSP is patched
in place. After switching host OS, run `idf.py fullclean` once - the `build/`
cache stores absolute paths.

## What it receives

State is not uniform, and the list stays honest about that:

- **P25 Phase 1** - C4FM decoding, OP25's fixed-point IMBE vocoder, TSBK parsing
  and a grant follower are implemented. On the LCD-4.3 preview baseline, a
  controlled 20-frame RF fixture produced 12 valid NIDs with zero reported BCH
  failures and NAC 293. This verifies reception of that control fixture, not
  complete frame recovery. Voice intelligibility and live-system trunk
  following are not hardware-verified for this preview.
- **POCSAG** - synchronous decode at 512 / 1200 / 2400 baud with BCH checks;
  addresses and message bodies.
- **ADS-B** - 1090 MHz Mode-S decode into a live aircraft table.
- **FM** - wideband broadcast, narrowband voice, and band scan sharing one
  integer `rtl_fm`-derived pipeline.
- **Sub-GHz capture** - threshold-triggered raw pulse capture around 433 MHz,
  written to the SD card with a SPIFFS fallback for offline analysis.
- **DMR** - sync detection and BPTC/LC framing land in the tree; there is no
  AMBE, so there is no voice yet. Do not expect audio.
- **ACARS** - receiver mode, decoder and message display are integrated, with
  gain and frequency controls. On-air message reception remains unverified
  for the LCD preview.

Host regression tests live under `bench/tests/`; they do not cover every device
interaction. `pwsh -File bench/verify.ps1 -Level smoke` runs the host tests,
public-header checks and LCD4.3 firmware build. Hardware acceptance is separate.

## Credits & licences

Released under **GNU GPL v3** (see `LICENSE`), as required by its GPL
dependencies. Builds on:

- [rtl-sdr / librtlsdr](https://osmocom.org/projects/rtl-sdr) - Osmocom (GPL-2.0+)
- [xtrsdr](https://github.com/XTR1984/xtrsdr) - librtlsdr trimmed for ESP32-class targets
- [OP25 `imbe_vocoder`](https://github.com/boatbod/op25) - Pavel Yazev (GPL-3.0+)
- [mbelib](https://github.com/szechyjs/mbelib) - ISC, kept as a fallback vocoder
- [DSD / dsd-fme](https://github.com/lwvmobile/dsd-fme) - P25 framing (GPL)
- Espressif ESP-IDF and the Waveshare ESP32-P4 BSP
- LVGL, the shared graphical toolkit; retain its bundled licence.
- Helix MP3 and ESP audio-player components; nested decoder notices differ
  from the outer component licence and must be retained separately.
- SAM speech synthesis; its bundled permission status remains unresolved.

See the [third-party review](docs/THIRD_PARTY_REVIEW.md) for the release
inventory and outstanding font/SAM/source-distribution checks. The project's
top-level licence does not replace third-party notices or resolve missing
permissions. This local preview is not yet cleared for public distribution.

Vendored sources keep their original copyright and licence headers. That claim
is audited, not assumed: `components/lakeshark/radio/UPSTREAM.md` records, file
by file, which headers are upstream, which were verified against upstream as
having no header there either, and which had been lost and were restored from
the upstream text. Files this project modified carry the notice of change that
GPL-2.0-or-later section 2(a) requires.

The librtlsdr material is GPL-2.0-**or-later**, which is what permits the
combined work to be GPLv3. A GPL-2.0-only replacement would break that, so
check the wording before swapping any of it out.

Headers are still missing on the DSD-derived P25 and error-correction helpers
in `components/lakeshark/apps/p25/`; those are tracked as unresolved in the
third-party review below and must not be given invented attribution.

> The IMBE/AMBE voice codecs are covered by patents held by DVSI. This
> firmware is for educational and experimental use; compliance with local law
> and licensing is your responsibility.
