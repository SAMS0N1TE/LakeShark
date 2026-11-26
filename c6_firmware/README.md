# C6 firmware

The T-Display-P4 has a second chip, the ESP32-C6, that handles Wi-Fi and
Bluetooth. This folder holds the firmware it needs: ESP-Hosted 2.12.9.

| File | What it is | Offset |
| --- | --- | --- |
| `bootloader.bin` | C6 bootloader | `0x0` |
| `partition-table.bin` | C6 flash layout | `0x8000` |
| `ota_data_initial.bin` | Which slot the C6 starts from | `0xd000` |
| `network_adapter.bin` | The Wi-Fi and Bluetooth firmware | `0x10000` |

`provenance.json` lists the size and SHA-256 of each file.

## First install

No cable needed. The board installs the monitor image from `net/` by itself a minute after it starts on Wi-Fi; SYSTEM > UPDATE > C6 RADIO FW > UPDATE or `c6 update auto` runs it now. This folder's stock image is kept for recovery. See [The C6 radio firmware](../docs/TDP4_FIRST_FLASH.md#the-c6-radio-firmware). A UART adapter is only for a C6 that no longer boots.

## Update from the SD card

UPDATE uses this file when it is on the card and downloads it when it is not.

1. Copy `network_adapter.bin` to `/lakeshark/c6/network_adapter.bin` on the card.
2. Charge the battery to at least 20%.
3. Open SYSTEM > UPDATE > C6 RADIO FW, choose UPDATE, then CONFIRM.
   Keep the board powered until it finishes.
4. When it reports success, restart the board.

Or from the console: `c6 update`, then `c6 status` to follow progress. `c6`
alone shows the version the C6 is running.

The file is checked before anything is written. A damaged file, a file for a
different chip, or the same or an older version is refused, and Wi-Fi and
Bluetooth keep working. To reinstall the same version, choose OPTIONS >
Version policy > Force same/older version, or use `c6 update force`.

During an update, Wi-Fi and Bluetooth are switched off. They stay off until you
restart the board, even if the update fails.

## If something goes wrong

If an update stops partway or the C6 does not come back after a restart,
reinstall it over the UART header with the recovery steps in the first-flash guide. An update
from the card cannot repair a C6 that no longer responds.
