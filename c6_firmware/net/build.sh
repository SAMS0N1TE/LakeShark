#!/bin/bash
# Build the T-Display-P4's C6 image with the Wi-Fi monitor forwarder.
# ESP-Hosted 2.12.9 slave (from managed_components) + c5_firmware/ls_promisc.c
# and slave.patch, peer data on, target esp32c6, ESP-IDF 5.5.4.
# Output: c6_firmware/net/network_adapter.bin
set -e
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${WORK:-$HOME/ls_logs/c6_promisc}
rm -rf "$work"; mkdir -p "$work"
cp -r "$repo/managed_components/espressif__esp_hosted/." "$work/"
cd "$work/slave"
cp "$repo/c5_firmware/ls_promisc.c" main/
patch -p1 < "$repo/c5_firmware/slave.patch"
printf '\n# LakeShark: Wi-Fi monitor forwarder\nCONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y\n' >> sdkconfig.defaults.esp32c6
idf.py -D IDF_TARGET=esp32c6 build
cp build/network_adapter.bin "$repo/c6_firmware/net/network_adapter.bin"
sha256sum "$repo/c6_firmware/net/network_adapter.bin"
