# LakeShark Game

Standalone ESP32-P4 game firmware for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 and Lonely Binary TK21. The two cores and PIE draw 3D into 400 × 240 RGB565 buffers. The PPA rotates and doubles the previous frame into a panel-owned buffer while the CPU draws the next one. Scanout is 480 × 800.

## Play

The game library and all games use landscape orientation. The library shows an animated ASCII scene for the highlighted game. Settings and button capture use an ASCII controller diagram that lights the buttons as you press them. Move through the library with the stick, select with A (bottom, U5), and go back with B (right, U6). The small left Menu button (U11) pauses a game. The small right Alt button (U10) opens Settings from the library and provides an alternate action in games that use it. The GBA entries require the optional emulator.

| Game | Default controls |
| --- | --- |
| Tidal Relay | Steer and change altitude with the stick. Boost with A to get through all 18 gates. |
| Salvage Run | Use the same flight controls to collect gold pods and avoid magenta reefs. |
| Signal Catch and GBA from SD | The stick acts as a D-pad. The defaults map A and B as labeled, stick click to Start, Alt to Select, X to L and Y to R. |

## Settings

Use the stick left or right to adjust brightness, palette, dead zone, and sensitivity. Press A to toggle either inversion setting. The stick settings apply to menus and games. Select **Calibrate Stick**, release the stick to record its center, then sweep its full range and press A to save. **Button Bindings** lets you assign a physical button to flight boost or a GBA action. Menu cancels button capture. The firmware saves settings to NVS.

## Wiring

| TK21 | Header pin | GPIO |
| --- | ---: | ---: |
| X | 15 | 21 |
| Y | 17 | 22 |
| BTNS | 32 | 49 |
| KEY | 11 | 5 |
| 3V3 | 18 | |
| GND | 19 | |

A second TK21 can use the spare pins below. The board can read all three signals on ADC2 with the first TK21 still attached. The game firmware currently polls the first module. [Waveshare header](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3) · [ESP32-P4 pin functions](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/schematic-checklist-esp32p4.html)

| Second TK21 | Header pin | GPIO |
| --- | ---: | ---: |
| X | 34 | 50 |
| Y | 36 | 51 |
| BTNS | 38 | 52 |
| KEY | 12 | 3 |
| 3V3 | 2 | |
| GND | 33 | |

## Build and flash

Use ESP-IDF 5.5.4 from this directory. The defaults target pre-v3 silicon, including the tested v1.3, at 360 MHz with 200 MHz PSRAM. For v3 hardware, use a separate configuration with the matching chip revision.

```powershell
idf.py -B build-v1 -D SDKCONFIG=build-v1/sdkconfig build
idf.py -B build-v1 -D SDKCONFIG=build-v1/sdkconfig -p COM4 flash
```

Use `app-flash` for updates with the same partition layout. The 32 MB flash profile uses 0x2000 for the bootloader, 0x8000 for the partition table and 0x10000 for the app.

The microSD slot accepts FAT32 and exFAT. If an existing game build has short filenames selected in `sdkconfig`, set **FAT Filesystem support → Long filename support** to **Long filename buffer in heap** with `idf.py menuconfig` before rebuilding.

### Optional GBA build

```powershell
git clone https://github.com/midworld/gpsp-esp32.git C:/gpsp-esp32
git -C C:/gpsp-esp32 checkout 0015ff0eea6f8d21973beb7776682277e9ecefe5
idf.py -B build-gba -D SDKCONFIG=build-gba/sdkconfig -D LAKESHARK_GPSP_DIR=C:/gpsp-esp32 build
idf.py -B build-gba -D SDKCONFIG=build-gba/sdkconfig -D LAKESHARK_GPSP_DIR=C:/gpsp-esp32 -p COM4 flash
```

The [Signal Catch source](gba/demo) is included. Run `python gba/demo/build_rom.py` with Clang and LLD on PATH to rebuild the cartridge. The emulator provides video and controls. Audio output and save persistence remain open. SD mode opens a single `game.gba` and mounts with formatting disabled. A compatible `gba_bios.bin` can be supplied on the card if required. The GBA profile allows the recompiler to run its generated code from PSRAM and retains ESP-IDF's internal SRAM protection.

## Diagnostics

On the board, both native games ran at 59.9 FPS. The bundled Signal Catch cartridge sustained 59.9 to 60.0 presented FPS with about 11.8 ms of emulation and frame preparation. The PPA transfer took about 6.2 ms and overlapped preparation of the next frame.

Open the USB-to-UART port at 115200. Use `?` for the current screen, joystick and game telemetry. Use `WASD` for menu navigation, `g` for select or pause, `q` for back, and `m` to return to the library. `1` and `2` open Tidal Relay and Salvage Run. Use lowercase `wasd` to steer. `b` opens Signal Catch and `r` opens the SD ROM.

```powershell
python tools/verify_menu.py COM4 --settings --gba
python tools/capture.py COM4 build-gba/frame.png --keys 1
```

The verifier checks navigation, pause/resume and launches, writes its UART log, and returns to the library. `--settings` uses the board's reset line to check that brightness and remapped buttons survive a reset. Omit `--gba` without the optional emulator. Capture pauses the frame loop during transfer and needs Pillow and pyserial. The verifier needs pyserial.

See the [native SDK guide](../app-sdk/native/README.md) and [porting notes](PORTS.md). The `bsp30` component loads BSP v30 maps you supply; no game data ships with LakeShark.
