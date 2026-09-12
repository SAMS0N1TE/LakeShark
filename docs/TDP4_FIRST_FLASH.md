# T-Display-P4 installation

Use the LilyGO T-Display-P4 with the 4.1-inch RM69A10 AMOLED, 568 x 1232 resolution and 16 MB flash.

## Build

Use ESP-IDF 5.4.3 in an exported IDF shell:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build
```

The Wi-Fi and Bluetooth configuration requires matching ESP-Hosted 2.12.9 firmware on the C6. Install the C6 firmware identified by the release before enabling the P4 wireless features. C6 firmware uses a separate flash layout.

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

## Controls

HOME opens the app list. Touch controls and the detachable keyboard provide navigation. The display rotates automatically; F11 rotates it from the keyboard. SET contains brightness, themes, sounds and Daylight mode.

Open LINK to manage Wi-Fi and the Flipper Bluetooth connection. SCAN lists nearby networks, MANUAL accepts a network name, and SAVED reconnects to the saved network. Password entry starts hidden; SHOW PASSWORD and HIDE PASSWORD switch visibility.

Choosing a receiver in the Flipper app opens its corresponding P4 screen. POCSAG uses the FM pager page.
