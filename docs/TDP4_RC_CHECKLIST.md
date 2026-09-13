# 2.1.0-rc1 release checks

Target: LilyGO T-Display P4, 16 MB flash, existing `boards/partitions_16m.csv`.
The release package updates the application at `0x10000` only. It does not
erase NVS, format SD, install C6 firmware or enable the experimental OTA layout.

Required preparation:

- Host gate: `pwsh -File bench/verify.ps1 -Level host` must end `VERIFY OK`.
- ESP-IDF 5.4.3 P4 build using `boards/t_display_p4.defaults`.
- Clean source revision and matching firmware build identity.
- Boot the packaged application and verify the on-device version.
- Check portrait compass, bottom controls, back navigation and landscape layout.
- Include firmware hashes, build configuration, notices and corresponding source.

Hardware evidence already collected during development:

- SX1262 Mesh operation and restoration after leaving direct LoRa use.
- CC1101 energy samples, nRF24 threshold scans and NFC observations concurrent
  with RTL reception; Journal attachments saved.
- Classic 4K: all 256 blocks read; wrong reader keys leave blocks unverified.
- HackRF: 2 MSPS transport, multiple restarts, no dropped input in measured runs.
- Compass cardinal direction check and user confirmation of the rotating dial.

Still not established: long unattended soak, all NFC card families, calibrated
RF sensitivity, precise compass accuracy, HackRF aircraft decode, HackRF
FM/P25 rate conversion, or SX1262 POCSAG equivalence to RTL. These limits remain
explicit in the release notes. Other board configurations are not RC binaries.
