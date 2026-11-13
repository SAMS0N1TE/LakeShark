# ESP32-C5 coprocessor firmware (ESP32-P4-NANO)

ESP-Hosted 2.12.9 slave for the Nano's ESP32-C5, plus a Wi-Fi monitor forwarder
(`ls_promisc.c`) so capture apps such as TAP can receive 802.11 frames.
Receive only. Same SDIO settings as the stock image.

## Install

1. Copy `network_adapter.bin` to the SD card, for example `/sdcard/ls_c5.bin`.
2. On the console: `c6 flash /sdcard/ls_c5.bin`. The board restarts when it is done.
3. Check with `c6 mon 8`: it counts the frames the C5 forwards.

The running C5 image is kept until the new one is written and checked.

## Build

- ESP-IDF 5.5.4, target `esp32c5`.
- Start from `managed_components/espressif__esp_hosted/slave`, add `ls_promisc.c`
  to `main/`, apply `slave.patch`, then `idf.py -D IDF_TARGET=esp32c5 build`.
