# LakeShark offline application upgrade

Local preview tooling; no public release or hardware validation is implied.
The current package is not cleared for public distribution. Firmware license,
font and corresponding-source review remain separate release requirements.

This utility upgrades an existing **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3**
with 32MB flash and the exact supported LakeShark factory layout. It does not
provision a blank board, migrate layouts, or support Nano, the square LCD-4B,
LilyGo, other P4 boards, secure boot or flash encryption. A P4 chip ID does
not identify the board model. Inspect the physical model label yourself.

## Check a downloaded package

Extract the complete trusted package ZIP to a new directory. Keep its files
unchanged, including app.elf, notices, manifest and SHA256SUMS.txt. Python3.9+
is sufficient for offline checking; esptool is imported only by explicit flash.

```text
python tools/lakeshark_upgrade.py path/to/extracted-package --board p4-touch-lcd-43
```

This default is check-only. It does not list, open or reset serial ports,
download anything, install dependencies, flash, or erase. It checks the exact
file list and SHA256 values, the application's embedded checksum/SHA256,
matching ELF, board marker, version, project and upgrade manifest. SHA256 is
integrity checking, not a publisher signature: an attacker can replace both
firmware and checksums. Use a trusted package and trusted download source.

## Upgrade an already provisioned LCD4.3

Close other serial applications. Back up important settings separately and
keep stable USB power. Use a dedicated Python environment with esptool4.11.0
installed in advance, or the reviewed ESP-IDF environment using4.12.dev1.
Other esptool versions are rejected rather than silently using a changed API.

```text
python -m pip install esptool==4.11.0
python tools/lakeshark_upgrade.py path/to/extracted-package --board p4-touch-lcd-43 --flash --port COM7 --confirm-board ESP32-P4-WIFI6-Touch-LCD-4.3
```

Replace COM7 with the actual port. The tool never guesses a port or contains
the developer's board serial number. On Linux/macOS use the appropriate device
path. Installing esptool needs network access unless its dependencies were
downloaded beforehand; the upgrade itself is offline.

1. Type `CONNECT` to authorize resetting/opening that explicit port.
2. The tool checks chip/security state, actual flash capacity, partition-table
   SHA256 and MD5, the complete supported partition layout, and the existing
   LakeShark application descriptor. Mismatch or unknown state stops the operation.
3. Read the selected device/version and write range, then type `FLASH`.
   Anything else cancels without a flash write. No unattended `--yes` bypass.
4. Only the packaged application is passed to esptool at0x10000, with image
   revision checks enabled and force/erase-all/encryption disabled. Existing
   flash-mode/frequency/size header fields are kept. The written application
   is read back and SHA256 verified before a reset is requested.
5. Confirm the screen, touch, settings and radio behavior yourself. Successful
   readback is not a hardware acceptance test.

Ordinary application-sector erase is necessary to write flash. No full-chip,
NVS, PHY, storage, coredump, partition-table or bootloader erase/write exists
in this tool. It never reads or exports NVS/settings/RF data.

This single factory partition upgrade is **not atomic or power-fail safe**.
Cancellation before writing preserves flash but may leave the board in download
mode; reset it manually. If power, serial or verification fails after writing
starts, the application may be incomplete. Keep the exact trusted package for
recovery and do not run an erase-all procedure. A recovery after a damaged app
descriptor may require the separately supervised recovery workflow; this tool
intentionally refuses to bypass its existing-application check.

## Reusable package contract for a later website

Input is `package_lcd43_app.py` schema1, kind
`application-only-upgrade-candidate`. Required values are board
`ESP32-P4-WIFI6-Touch-LCD-4.3`, flash_bytes33554432, app_offset`0x10000`,
app_partition_bytes14680064, app/ELF/partition-table SHA256, version, and exact
license/document lists. Its complete extracted file set must equal those lists
plus app.bin, app.elf, manifest.json, FLASH_INSTRUCTIONS.txt and SHA256SUMS.txt.
Partition bytes are read from the device, not supplied for writing by the package.

Supported partition table at0x8000 is3072bytes with its IDF MD5 record:

| Partition | Offset | Bytes |
| --- | --- | --- |
| nvs | 0x9000 | 0x6000 |
| phy_init | 0xF000 | 0x1000 |
| factory | 0x10000 | 0xE00000 |
| storage | 0xE10000 | 0x800000 |
| coredump | 0x1610000 | 0x10000 |

A future web installer must reproduce the preflight and confirmation contract;
it must not treat chip-family selection or an ESP32-P4-compatible library as
proof of board identity. Browser support, secure origin, Web Serial permission,
library/P4 revision support, signing/authenticity and license clearance remain
separate work. This utility neither publishes to a website nor embeds credentials.

## Tests and reviewed API

```text
python -m unittest discover -s bench/tests -p test_lakeshark_upgrade.py -v
```

Tests use synthetic packages and mocked transports only. They cover cancellation,
checksum/image corruption, unsupported board/chip/capacity/security, layout
mismatch, first install, exact app-only write, verification failure and closure.
Installed esptool4.12.dev1 and an isolated public esptool4.11.0 were inspected
without opening hardware. The actual local2563232-byte release package passed
check-only validation, and its3072-byte build partition table matched the
contract offline. Neither check exercised a physical flash. The adapter uses
detect_chip, security eFuse reads, run_stub,
flash_spi_attach, detect_flash_size, read_flash and guarded cmds.write_flash.
Espressif documents the v4 integration API and default write protections:
[Python embedding](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/esptool/scripting.html),
[basic commands and safety checks](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/esptool/basic-commands.html).
