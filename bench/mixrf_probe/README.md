# Keyboard radio isolation

This standalone diagnostic uses the unmodified CC1101, nRF24 and XL95x5
drivers from LILYGO's `cpp_bus_driver`, revision
`ff99314e37f125fe9f18b41340639f000c2f1e83`. It follows the radio power and
pin setup in the T-Display-P4 keyboard examples, without the display, GPS,
MeshCore or RTL tasks. It initializes the radios, reports IDs and powers
their receivers down. It does not transmit or initialize NVS/SD storage.

Build with ESP-IDF 5.5.3, its supported toolchain, and
`-DMIXRF_VENDOR_DRIVER=/path/to/cpp_bus_driver`. The dependency directory
must retain its `cpp_bus_driver` name and upstream license notices.
The defaults select the pre-v3 ESP32-P4 used by the tested board (v1.3).

For the existing LakeShark partition layout, load only `mixrf_probe.bin`
at `0x10000`, then restore the saved LakeShark application at the same address.
Do not use the generated full-flash command or replace the partition table.

USB-powered hardware test on 2026-09-12, without keyboard batteries:
CC1101 identified as `0x14`; nRF24 initialization succeeded. LakeShark's
subsequent probe also identified NFC `0x2A` after freeing GPS stack memory.
