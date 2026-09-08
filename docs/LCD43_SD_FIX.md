# LCD-4.3 SD and Flipper download fix

The LCD-4.3 needs two independent fixes:

- FATFS work buffers stay in internal RAM, avoiding an internal DMA bounce-buffer allocation for each SD transfer under radio load.
- The BLE command task uses a 6 KB internal stack. Its previous 3 KB budget was measured on short commands and did not cover the nested recording load path.

The September 8, 2026 Flipper reproduction captured a stack protection fault in `_svfprintf_r` while REC streamed 524288 bytes/second. Stack bounds were `0x4ff72ca0`�`0x4ff73890`; the failing pointer was `0x4ff72c90`. The BLE command task control block was corrupted. The separate UART command task survived.

## Hardware acceptance

Select a saved capture on the Flipper and download it while the LCD radio stream is active. Confirm the complete file on the Flipper, an unchanged LCD uptime, and remaining `ble_cmd` stack space using the LCD `mem` console command. Screenshot reads/writes alone do not verify this transfer.

This release candidate targets only ESP32-P4-WIFI6-Touch-LCD-4.3 with 32 MB flash. It is not a T-Display-P4 image. Build with ESP-IDF 5.4.3 and `boards/lcd43_gui.defaults`, using a separate generated SDKCONFIG.

## Release hold

Repeated transfers exposed a separate delayed watchdog reset after the BLE stack fix. It also reproduced with the panic handler in IRAM. No stable binary release is authorized as fixed until this is resolved and re-tested. The Flipper app additionally needs the correlated REC LOAD acknowledgment to avoid using the previous file's cached edge count; hardware validation of that change is pending.
