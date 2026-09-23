# T-Display-P4 installation

Use the LilyGO T-Display-P4 with the 4.1-inch RM69A10 AMOLED, 568 x 1232 resolution and 16 MB flash.

## Build

Use ESP-IDF 5.4.3 in an exported IDF shell:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build
```

The Wi-Fi and Bluetooth configuration requires matching ESP-Hosted 2.12.9 firmware on the C6. See [Flash the C6 co-processor](#flash-the-c6-co-processor) below: the images ship in `c6_firmware/` and must be written before the P4 wireless features work. C6 firmware uses a separate flash layout.

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

All four images are required for a first installation. Use the generated `build_tdp4/flasher_args.json` to confirm the image layout. Back up existing data before a full installation. For an application-only update on a matching installation, write `lakeshark.bin` at `0x10000`.

## Flash the C6 co-processor

The P4 reaches Wi-Fi and Bluetooth through the on-board ESP32-C6 over SDIO. The host is pinned to **ESP-Hosted 2.12.9** and the C6 must run the matching `network_adapter` slave. A board straight from LilyGO carries the factory image, which does not speak ESP-Hosted; an older slave enumerates but frames packets differently.

Both fail the same way, and it does not look like a Wi-Fi fault — the link times out, the host restarts before the UI starts, and the panel is left on the DSI test pattern. Vertical colour bars plus a reboot loop mean this step is needed:

```
E (7589) transport: Init event not received within timeout, Resetting myself
```

The matching images ship in `c6_firmware/`. The C6 is not on a USB port: the left-hand USB-C is charge only, and the C6 has a dedicated UART header. You need a **3.3 V USB-to-UART adapter**, and the P4 has to be holding the C6 in download mode while you write it.

**1. Put the P4 into co-processor download mode.** Flash LilyGO's `coprocessor_download_mode` program over P4U and watch the serial output until it prints `Coprocessor preparation completed`. Source is in [lilygo_device_driver_example](https://github.com/Xinyuan-LilyGO/lilygo_device_driver_example/tree/main/main/examples/coprocessor_download_mode); prebuilt images are in the [T-Display-P4 repo](https://github.com/Xinyuan-LilyGO/T-Display-P4). This overwrites LakeShark on the P4, which step 4 puts back.

**2. Wire the adapter to the C6 UART header**, RX and TX crossed:

| C6 header | Adapter |
| --- | --- |
| RX | TX |
| TX | RX |
| GND | GND |
| 3.3V | not connected - the board powers the C6 itself |

**3. Hold the C6 `BOOT` button, press and release its `RESET`, then let go of BOOT.** The C6 is now in download mode on the adapter's port:

```sh
esptool.py -c esp32c6 -p YOUR_ADAPTER_PORT -b 460800 write_flash \
  0x0     c6_firmware/bootloader.bin \
  0x8000  c6_firmware/partition-table.bin \
  0xd000  c6_firmware/ota_data_initial.bin \
  0x10000 c6_firmware/network_adapter.bin
```

**4. Flash LakeShark back onto the P4** over P4U, then power-cycle.

| Image | Offset | Partition |
| --- | --- | --- |
| `bootloader.bin` | `0x0` | — |
| `partition-table.bin` | `0x8000` | — |
| `ota_data_initial.bin` | `0xd000` | `otadata` |
| `network_adapter.bin` | `0x10000` | `ota_0` |

These offsets are the C6's: the bootloader sits at `0x0` here and at `0x2000` on the P4. A successful link logs:

```
I (....) headless: ESP-Hosted co-processor link: up (0)
```

## Controls

HOME opens the app list. Touch controls and the detachable keyboard provide navigation. The display rotates automatically; F11 rotates it from the keyboard. SET contains brightness, themes, sounds and Daylight mode.

Open LINK to manage Wi-Fi and the Flipper Bluetooth connection. SCAN lists nearby networks, MANUAL accepts a network name, and SAVED reconnects to the saved network. Password entry starts hidden; SHOW PASSWORD and HIDE PASSWORD switch visibility.

Choosing a receiver in the Flipper app opens its corresponding P4 screen. POCSAG uses the FM pager page.
