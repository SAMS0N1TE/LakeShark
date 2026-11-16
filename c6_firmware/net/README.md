# C6 firmware with the Wi-Fi monitor forwarder

The stock C6 image (one folder up) gives the T-Display-P4 Wi-Fi and Bluetooth
but cannot hand the P4 raw 802.11 frames, so TAP and the other capture apps
report that the coprocessor does not forward monitor frames. This image is the
same ESP-Hosted 2.12.9 with the forwarder the P4-NANO's C5 uses
(`../../c5_firmware/ls_promisc.c`): receive only, frames copied in the
Wi-Fi task and sent over SDIO from a task of its own, so a busy link drops
frames (counted) instead of holding up the radio.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `network_adapter.bin` | 1212672 | `69e0a6d9bd5f0913c545b7eade65e9d114535291a01720728286c39ec8e0e57f` |

Built with ESP-IDF 5.5.4 (the stock image was 5.4.3). Same partition layout,
same SDIO settings.

## Install

The C6 must already run ESP-Hosted (any board that has Wi-Fi works).

1. Copy `network_adapter.bin` to the card as `/lakeshark/c6/ls_c6_net.bin`.
2. On the console: `c6 update /sdcard/lakeshark/c6/ls_c6_net.bin force`
   (`force` because the version, 2.12.9, is the same as the stock image), or
   `c6 flash /sdcard/lakeshark/c6/ls_c6_net.bin` on 2.22 and later.
3. Restart the board, then `c6 mon 8` counts the frames the C6 forwards.

To go back, install the stock `../network_adapter.bin` the same way.

## Build

`bash c6_firmware/net/build.sh` with ESP-IDF 5.5.4 in the environment.
