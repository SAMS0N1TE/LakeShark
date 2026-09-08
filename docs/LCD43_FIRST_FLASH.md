# LCD4.3 first flash

For a board that has never run LakeShark. [LCD43_QUICKSTART.md](LCD43_QUICKSTART.md)
covers application-only upgrades and says so; this is the other half, and the
two are not interchangeable.

Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 only — 480×800 panel, 32 MB flash.

## Write all four images, not just the application

    0x2000     bootloader.bin
    0x8000     partition-table.bin
    0x10000    lakeshark.bin
    0xe10000   storage.bin

The storage partition is a SPIFFS image and it is **not optional**.
`CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL` is off and `bsp_spiffs_mount()` is
called inside `ESP_ERROR_CHECK`, so a blank storage partition aborts the boot
rather than formatting itself. Flashing the application alone onto a fresh
board produces a device that looks bricked and is not.

## From a terminal

Use the board's **UART** USB-C port, not the USB host one. It enumerates as a
CH343.

```bash
esptool --chip esp32p4 --baud 921600 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 32MB \
  0x2000    bootloader.bin \
  0x8000    partition-table.bin \
  0x10000   lakeshark.bin \
  0xe10000  storage.bin
```

Building from source produces all four under `build_lcd43/`; `flash_args` in
that directory is the authoritative offset list if this document ever drifts
from it.

```bash
idf.py -B build_lcd43 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/lcd43_gui.defaults" build
```

## Relationship to the upgrade package

`tools/package_lcd43_app.py` packages an **application-only upgrade** — one
image, with its ELF, a notice list and checksums, for a board already running
LakeShark. That is the careful path and it stays the careful path; it never
builds, flashes or touches a port.

The bundle described here is the other artifact: the four images a blank board
needs. Neither replaces the other, and the offsets in both come from
`build_lcd43/flash_args`.

## From a browser

`dist/site/lakeshark.html` is a setup page with an
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) installer, for people
who should not have to install a toolchain to try a receiver. ESP32-P4 is a
supported chip family there. It is not deployed — `dist/site/DEPLOY.md` has the
upload steps and the reason the firmware directory is still held back.

## The storage image and the music

`spiffs/` is built into `storage.bin` by `spiffs_create_partition_image()` in
`main/CMakeLists.txt`, and `spiffs/music/` holds five MP3s whose redistribution
rights are unresolved (see [THIRD_PARTY_REVIEW.md](THIRD_PARTY_REVIEW.md)).

The bundle under `dist/lakeshark-lcd43/` therefore ships a storage image
rebuilt from `spiffs/panels/` alone. The board boots and runs normally on it;
MUSIC simply starts empty and reads from the SD card instead. Rebuild that
image with the geometry the firmware expects, or SPIFFS will not mount:

```bash
python $IDF_PATH/components/spiffs/spiffsgen.py 8388608 <input-dir> storage.bin \
  --page-size 256 --obj-name-len 32 --meta-len 4 --use-magic --use-magic-len
```

Those five values come from `CONFIG_SPIFFS_*` in the build's `sdkconfig.h`.
Change the build's SPIFFS settings and this command changes with them.

## First boot

The dongle goes in the **USB-A host** port. Start with ADS-B: it needs no
licence, no frequency to guess and no local system to exist, so aircraft
appearing is a single check that the USB host, dongle, tuning, DSP and display
are all working. Then FM for the audio path. P25 last — it only decodes if a
Phase 1 system is actually on the air near you.
