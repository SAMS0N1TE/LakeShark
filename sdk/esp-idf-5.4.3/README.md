# Release SDK

LakeShark uses ESP-IDF 5.4.3 plus three recorded patches:

- `p4-reset.patch`: the ESP32-P4 reset also resets AHB DMA, H264 and SDMMC.
- `usb-hub.patch`: a root port event that arrives after the USB device was freed
  is logged and ignored instead of aborting, so unplugging the RTL-SDR does not
  reboot the board.
- `usb-hcd.patch`: a bulk or control transfer whose DMA descriptor reports an
  error completes as a failed transfer instead of asserting, so a USB error
  while the RTL-SDR streams does not reboot the board.

From a clean ESP-IDF 5.4.3 checkout, or one already prepared for an earlier
LakeShark release:

```sh
python tools/prepare_sdk.py --idf /path/to/esp-idf
```

The script checks the bundle, preserves local changes, and selects the exact SDK
commit. `provenance.json` records the base, commit and hashes; the patch files
provide the readable diffs. Run the usual ESP-IDF export script before
building. The patches retain Espressif's Apache-2.0 license.
