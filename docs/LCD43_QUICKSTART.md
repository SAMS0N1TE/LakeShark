# LCD4.3 quick start

For the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3, 480×800 display and
32 MB flash only. This experimental candidate is not for Nano, WIFI6 headless,
LCD-4B or T-Display-P4. P25 voice remains unverified for build EF70D1.

## Before upgrading

This is an **application-only upgrade**, not first-time provisioning.

- Confirm the physical board, port and 32 MB flash capacity. The existing
  bootloader and companion firmware must be compatible; see the tested
  companion versions in the release notes. The factory partition must match
  the package manifest, including offset `0x10000` and its required size.
  Stop if compatibility is unknown; this package does not upgrade the C6.
- Stop if secure boot or flash encryption is enabled; this package does not
  provide a secure/encrypted upgrade procedure.
- Back up device settings, SD files and crash evidence. Keep the previous
  compatible application and the ELF matching each application binary.
- Verify all package checksums. Follow its `FLASH_INSTRUCTIONS.txt` exactly;
  do not mix builds, guess offsets, erase the chip or write a partition table.

Use stable power throughout the write. The single factory application slot
does not provide an atomic, power-fail-safe update. If the upgrade fails, see
[Troubleshooting](LCD43_TROUBLESHOOTING.md). The package manifest determines
which exact image is supported; these instructions do not authorize publishing
a package whose distribution review remains incomplete.

## Start with HOME

Connect the RTL-SDR through the board's USB host connection and use a suitable
antenna. Confirm the board's documented power arrangement; battery-powered
operation is not established by this preview.

HOME offers **RECEIVER**, **SYSTEM** and **CLOCK** widget choices. The selection
is saved. RECEIVER shows the last-used receiver and tune, not a live background
receiver; tap its card to reopen it. SYSTEM reports C6, RTL and SD status.
CLOCK displays UTC only after time synchronization.

Use the app tiles to open P25, FM, ADS-B, ACARS, Files, MUSIC, MAP or Settings.
MUSIC opens the file browser. Returning to HOME stops the receiver; do not
expect two receiver apps to run simultaneously. MAP remains experimental.
No HOME mesh-chat connection is implemented.

## Connect Wi-Fi

Open Settings and its Wi-Fi section:

1. Tap **SCAN**, then choose a network from the list, or use NETWORK **EDIT**.
2. Use PASSWORD **EDIT**, then **CONNECT**. Selecting a different network clears
   the entered password; enter it again if required.
3. Read the connection/result status. A button press alone does not prove a
   connection. **SAVED** requests reconnection using saved credentials;
   **DISCONNECT** disconnects; **FORGET** removes the saved configuration.

Do not include passwords or network identifiers in public diagnostic reports.

## Try P25 reception

Use a known active P25 Phase 1 signal and an appropriate antenna. A quiet
channel cannot establish whether decoding works. On DECODE, tap the large
frequency value, or use CONFIG → FREQUENCY → **ENTER**. Enter MHz and confirm.
Check that the requested tune has actually applied.

Use SIGNAL and HEALTH to inspect reception and decoder state. Adjust receiver
GAIN/AGC cautiously; more gain is not always better. Check volume and MUTE
before expecting sound. Control-channel metadata is not voice: valid NIDs,
NAC or TSBKs do not establish intelligible speech or live trunk following.
PROGRAM selects control channels from a loaded profile; it is not an on-device
profile editor.
