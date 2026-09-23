# Release SDK

LakeShark 2.2.2 uses ESP-IDF 5.4.3 plus the recorded ESP32-P4 reset patch.
It resets AHB DMA, H264 and SDMMC alongside the existing peripheral reset sequence.
This is the same patch used during development, now versioned rather than left dirty.

From a clean ESP-IDF 5.4.3 checkout:

```sh
python tools/prepare_sdk.py --idf /path/to/esp-idf
```

The script checks the bundle, preserves local changes, and selects the exact SDK
commit. `provenance.json` records the base, commit and hashes; `p4-reset.patch`
provides the readable diff. Run the usual ESP-IDF export script before building.
The patch retains Espressif's Apache-2.0 license.
