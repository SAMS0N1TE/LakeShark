# 2.1.0-rc2 release checks

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
- Include the P25 settings UI and its build/test inputs in the release commit,
  including currently untracked files. Verify all 16 settings are reachable in
  portrait and landscape: demodulation, polarity, AGC, gain, volume, voice gate,
  sync beep, auto-follow, encrypted-call handling, CQPSK tuning/reset and scan
  threshold/hang and the experimental Phase II switch. Check persistence where supported; do not assume every
  setting survives restart.
- Retain P25 AUTO demodulation, manual overrides, early protocol lock and
  loss-of-lock reacquisition. Current host coverage includes 16 demod-control
  cases and 18 P25 screen cases. Validate C4FM/CQPSK acquisition on known RF
  separately; AUTO demodulation does not mean automatic Phase II voice support.
- Preserve Phase II TDMA channel/grant/slot recognition. The experimental manual
  voice decoder must default OFF, mute unknown/encrypted calls and restore Phase I
  when disabled. Public-symbol replay on the host and P4, plus synthetic-IQ tests,
  pass; live RF and automatic trunk following remain pending. Recheck the
  [Phase II validation notes](P25_PHASE2.md) before publishing.
- ADS-B Scope by John Stockdale (@jstockdale) and Off by One, Inc. is credited
  in the README and release notes, consistent with commit 4874ce04. This credit
  does not claim that the offered AMOLED/TFT detection code was imported.

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

## Remaining integration work

- CC1101: validate the new bounded RMT OOK capture and repeated OOK24 decoder
  with known RF transmissions. REC/Sub-GHz now select RTL or CC1101. MIX-RF
  MONITOR remains RSSI-only. General sensor/FSK decoding remains unfinished.
- Scanner: validate channel-list and stepped-band scanning with known live
  signals, including hold, skip, receiver removal and return. The engine's twelve
  host cases pass; that does not establish field reception across a scan list.
- Passive operation: run an unattended recorder/Journal soak alongside Mesh and
  radio reception. Duplicate grouping, pinned retention, interrupted writes and
  low-space refusal have host coverage. Verify real alert delivery separately;
  the queued-message counter is not a delivery acknowledgement.
- Keyboard development: Wi-Fi diagnostics work, but supported OTA updating and
  interrupted-update recovery are unfinished. Do not apply the proposed OTA
  partition table as if it were an implemented updater.
- NFC: expand real-card coverage beyond the previously tested Classic 4K.
  Host coverage includes NFC-A, Classic, Type 2 and NDEF; it does not replace
  antenna/coupling and known-card tests.
- Keep HackRF FM/P25 and SX1262 POCSAG marked experimental/unverified. Leave
  compass tilt compensation and precision calibration for a separate pass.

The 2026-09-13 follow-up passed 117 targeted host cases covering the scanner,
passive recorder, field/Journal, LoRa scan and NFC components. The connected P4
had mounted SD and valid GPS sentences without a fix. The keyboard probe
reported absent, so no new CC1101/nRF24/NFC hardware result was established.

The recorder integration passed the host gate (`VERIFY OK`) and the P4 build.
The flashed board passed RTL WATCH start, background operation and stop;
selecting CC1101 parked RTL, refused WATCH with the keyboard absent, and
switching back restarted RTL. Portrait and landscape controls were checked.
The idle landscape recorder measured a 42.4 ms frame interval with 557 us draw
and 367 us presentation time. These checks do not establish sustained capture
throughput or CC1101 RF decoding. General status now reports REC frequency and
gain, and console tuning respects WATCH and the selected source's bands.

## RC2 device and UI review

The public Phase II symbol capture passed on the connected P4: 416,245 symbols,
1,368 voice frames and 218,880 PCM samples. Decoder work fell from 108.48 seconds
to 6.91 seconds after optimizing voice synthesis; stack headroom was 12,852 bytes.
This is a USB replay into the real decoder, not a live RF or speaker test.

Portrait and landscape simulator renders cover the launcher, P25, FM, waterfall,
ADS-B, Mesh, Labs, Journal, recorder, Sub-GHz, MIX-RF/NFC, diagnostics, settings,
map, GPS and radio list. P25 settings were checked through the last row in both
orientations. Physical display console checks exercised app navigation and
captured the rendered grids and timing counters in both orientations. Typical
idle frame intervals were 41.5–42.8 ms; the settled waterfall measured 43.3–43.6 ms.
Bottom button bars now respect rounded corners, including their touch regions.
Normal P25 scan startup disables experimental Phase II mode.

A fresh boot with RTL attached exposed a transient DMA allocation failure that
left Mesh stopped. LoRa now reserves its SPI/device buffers before USB startup.
The repeat boot brought up SX1262 and Mesh successfully, retained RTL streaming
and showed no DMA-drop warnings in the captured startup window.

No new card or CC1101 RF test is claimed with the keyboard absent. Long-running
mixed radio/SD operation, live Phase II acquisition and audible quality remain
field validation work before a stable release.

The P4 packaging and release-surface Python checks pass (seven cases). Broader
Python discovery also finds two pre-existing legacy test imports whose helper
scripts are absent: `tools/lakeshark_upgrade.py` and `tools/package_lcd43_app.py`.
Those legacy update/LCD-4.3 paths are not part of this P4 app-only package.

## GPS and mixed scanning follow-up

The host gate and P4 build pass with GPS coverage filtering, mixed conventional
Phase I/FM scanning and transactional USB/SD imports. Device checks passed
decoder handoffs, rejected-upload preservation and channel add/clear saves without
a restart. The temporary test list was removed. Scanner stack headroom measured
3,484 bytes after moving GPS working storage into PSRAM. Portrait and landscape
simulator checks cover the new controls.

Outdoor GPS movement, sustained mixed-mode audio and live RadioReference login
remain untested. Automatic trunked-site roaming and tone/NAC filtering are not
implemented. RadioReference is optional; local CSV/JSON imports need no account.
See [GPS scanning and imports](LOCATION_SCAN.md).

## Bluetooth startup recovery

Discovery now filters duplicate reports. If the receive event pool fills, the
transport drops advertisements for 100 ms while continuing to deliver connection
and control packets. A host burst regression covers backoff, recovery and control
delivery. The P4 reproduced buffer pressure and recovered with one warning and
23 dropped discovery reports, then connected to the Flipper and completed startup.
The `ble` console command reports these drops separately from application data.
Three subsequent reset/connect checks passed; two exercised buffer-pressure
recovery without stalling startup or flooding the log.
