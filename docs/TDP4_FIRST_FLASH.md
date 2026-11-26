# T-Display-P4 installation

Use the LilyGO T-Display-P4 with the 4.1-inch RM69A10 AMOLED, 568 x 1232 resolution and 16 MB flash.

Both radio variants, SX1262 and LR2021, use this image and build. The firmware identifies the chip at start.

The easiest install is the browser installer at [terminalbay.com](https://terminalbay.com/?m=lakeshark&board=tdp4) in Chrome or Edge. It also updates a board by USB at any time.

## Build

T-Display-P4 builds use ESP-IDF 5.5.4 with the patches checked by `bench/patch-idf.ps1` (see `bench/IDF554.md`). In an exported IDF shell:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build
```

For the TFT version (HI8561, 540 x 1168), add the TFT profile after the AMOLED one. The screen is laid out around the front camera:

```sh
idf.py -B build_tdp4_tft -D SDKCONFIG=build_tdp4_tft/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults;boards/t_display_p4_tft.defaults" build
```

The Wi-Fi and Bluetooth configuration wants ESP-Hosted 2.12.9 firmware on the C6. See [The C6 radio firmware](#the-c6-radio-firmware) below: the P4 updates its C6 from an image on the SD card, with no cable.

## Connect and flash

With the display facing up and USB-C ports toward you, connect the right-hand port beside the power switch, marked P4U. It appears as a CH343 serial device.

For a complete installation built from source:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -p YOUR_PORT flash
```

The P4 images use these offsets:

| Image | Offset |
| --- | --- |
| `bootloader/bootloader.bin` | `0x2000` |
| `partition_table/partition-table.bin` | `0x8000` |
| `lakeshark.bin` | `0x10000` |
| `storage.bin` | `0x910000` |

Use the generated `build_tdp4/flasher_args.json` for the complete installation, including OTA data initialization. Back up existing data before a full installation. Do not use a fixed application offset for later updates: the running app can be in either of two slots.

## Updates over WiFi

From 2.8 or earlier, install the current release once with the web flasher or USB. This installs the two-slot layout added in 2.9.0. A full install rewrites internal storage; settings and saved Wi-Fi are kept. Back up files first.

After that, open HOME > SYSTEM > UPDATE, or keep using the web flasher over USB. Connect to Wi-Fi in LINK, then choose CHECK and INSTALL when a newer build is available. Updates are signed. If the new build fails to start, the board returns to the previous build. SET > DEVICE > Check for updates selects Daily or Off. Daily checks show a NEW tag on SYSTEM and UPDATE.

See [Updates and calls](TDP4_UPDATES_AND_CALLS.md) for the update steps and SD call archive.

## The C6 radio firmware

The P4 reaches Wi-Fi and Bluetooth through the on-board ESP32-C6. LakeShark puts its own C6 firmware on it: **ESP-Hosted 2.12.9 with the Wi-Fi monitor forwarder** (`c6_firmware/net/`), which WIFI WATCH, TAP and WIFI SURVEY need for frame capture. The P4 writes it to the C6 over the board's own link, so you need no cable and no adapter.

It installs itself. A minute after the board starts on Wi-Fi, it checks the C6. If the C6 runs anything else (the factory firmware, or the stock ESP-Hosted image), a C6 RADIO banner appears, the board downloads the signed image from `data.terminalbay.com/c6/` to the card, writes it and restarts. Keep power connected while it runs. It tries at most twice per board.

To run it yourself: HOME > SYSTEM > UPDATE > C6 RADIO FW > UPDATE, or `c6 update auto` in TERMINAL. Without Wi-Fi, copy [ls_c6_net.bin](https://data.terminalbay.com/c6/ls_c6_net.bin) (1,212,672 bytes, also `c6_firmware/net/network_adapter.bin`) to the card as `/lakeshark/c6/ls_c6_net.bin` first.

The board checks the image (size, SHA-256 and the C6 target), writes the C6's spare slot, restarts the C6 and confirms the new version. A failed check or write leaves the old C6 image active. `c6 mon 8` counts the frames the C6 forwards.

If the co-processor link fails, current builds keep running with wireless features unavailable. Older builds may report a transport timeout:

```
E (7589) transport: Init event not received within timeout, Resetting myself
```

**Recovery only: when the C6 link is dead**

The cable is only for a C6 that no longer answers, so the steps above cannot run. The C6 is not on a USB port: the left-hand USB-C is charge only, and the C6 has a dedicated UART header. You need a **3.3 V USB-to-UART adapter**. The C6's own BOOT and RESET buttons put it into download mode.

1. Flash LilyGO's `coprocessor_download_mode` program over P4U and wait for `Coprocessor preparation completed` ([source](https://github.com/Xinyuan-LilyGO/lilygo_device_driver_example/tree/main/main/examples/coprocessor_download_mode)). It overwrites LakeShark on the P4.
2. Wire RX to adapter TX, TX to adapter RX and GND to GND. Leave 3.3V unconnected.
3. Hold C6 `BOOT`, press and release `RESET`, release BOOT, then:

```sh
esptool.py -c esp32c6 -p YOUR_ADAPTER_PORT -b 460800 write_flash \
  0x0     c6_firmware/bootloader.bin \
  0x8000  c6_firmware/partition-table.bin \
  0xd000  c6_firmware/ota_data_initial.bin \
  0x10000 c6_firmware/network_adapter.bin
```

4. Flash LakeShark back onto the P4 over P4U and power-cycle. A good link logs `ESP-Hosted co-processor link: up (0)`.

## Controls

HOME opens the app list. Touch controls and the detachable keyboard provide navigation. The display rotates automatically; F11 rotates it from the keyboard. SET opens on Volume, Brightness, Mute, Screen lock and Rotate lock. Its DISPLAY, SOUND and DEVICE menus hold the theme, Daylight mode, font, sounds, voice, keyboard and update settings.

Open LINK to manage Wi-Fi and the Flipper Bluetooth connection. It has WI-FI, BLUETOOTH, SURVEY and DRONES tabs. On WI-FI, SCAN lists nearby networks, JOIN connects to the selected one, MANUAL accepts a network name, and SAVED joins the strongest saved network in range. Password entry starts hidden; SHOW PASSWORD and HIDE PASSWORD switch visibility. A network is saved once it connects.

LakeShark keeps up to eight saved networks and moves to another saved network when one disappears. K selects a saved network, FORGET removes it and LEAVE disconnects. On BLUETOOTH, SCAN searches for a LakeShark Flipper head.

The `wifi` console command does the same from TERMINAL or USB: `wifi status|list|scan|add <ssid> [pass]|join <ssid> [pass]|connect|forget <ssid>|all|leave`. From a PC, `python bench/lswifi.py --port COM7 add "My Net"` prompts for the password, so it never appears on the command line.

Choosing a receiver in the Flipper app opens its corresponding P4 screen. POCSAG uses the FM pager page.

RADIOS > ANTENNA selects internal or MMCX1. The choice is kept after reboot. Attach a suitable antenna before confirming MMCX1. After a reboot, external transmit use needs confirmation again.
