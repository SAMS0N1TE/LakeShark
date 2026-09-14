# CELL WATCH handoff — 14 September 2026

Resume the standalone P4 passive cell-site-simulator detector. The user declined
a separate modem/hotspot. Research existing implementations and use the saved
recordings before consuming more hardware-test time. This is an experimental LTE
broadcast receiver; SIB1 and a validated simulator classifier remain unfinished.

## Checkouts on the next PC

```sh
git clone --branch work/p4-cell-detector https://github.com/SAMS0N1TE/LS_Test1.git
git clone --branch work/cell-watch-integrated https://github.com/SAMS0N1TE/LakeSharkGroundStation.git
```

Both repositories are private. The firmware branch contains the MeshCore receiver
patch in `integrations/meshcore`, the research ledger in
`docs/CELL_DETECTOR_FOCUS.md`, and portable recorded-IQ replay in
`integrations/cell-recordings`. The GUI branch preserves its external LTE decoder
patch, licenses and setup instructions in `integrations/cell-watch`.

The source checkouts and recordings are backed up in Git. ESP-IDF/toolchains,
Python environments, WSL/Octave, installed executables and all other SD captures
are not included; recreate the runtimes and keep the P4 SD card with the device.

## Hardware and final test

- LilyGO T-Display-P4, 4.1-inch AMOLED, 568 x 1232, 16 MB flash.
- HackRF/PortaPack attached to the P4 USB host, in HackRF USB mode, larger
  915 MHz antenna. Nooelec is currently detached from the P4.
- T-Beam Supreme SX1262 is the MeshCore USB gateway. Last PC ports were COM9
  for P4 and COM4 for T-Beam; enumerate afresh on the next PC. COM26 was unrelated.
- Final flashed application: 2,549,536 bytes, SHA256
  `f5dc98e3596da12b7bef26d6bf21e736adb19959b2ac6f6baa947fbb10881ba8`.
  The application was flashed at `0x10000`, hash verified. No SD erase.
- Final live capture: 739 MHz, 8 MS/s, 80 ms, 1,280,000 bytes, elapsed 81,365 us,
  zero host-ring drops, CRC32 `16d8f0ac`. PCI 244, 16 sync observations and
  15 consistent pairs. Sync took 3,234 ms; MIB took 2,287 ms, decoded eight
  CRC-valid occasions, 50 RB, two ports, SFN 446. MIB workspace is 33,296 bytes.
- UI remained at 42.5 ms per frame, panel 40 Hz, zero late frames. The final
  test left the P4 on the successful result at 8 MS/s. Serial development access
  was closed; no more test commands are queued.
- Host verification passed 171 test executables and 62 C++ header checks.
  The GUI's last code verification passed 23 parser/analysis tests. The portable
  recording replay passes two MIB decodes and the damaged-timing negative case.

## Unresolved capture problem: start here

10 MS/s has strong LTE reception but timing discontinuities. A downloaded 30 ms
recording proves that complete byte count, zero host-ring drops and plausible
elapsed time can still hide missing samples inside the radio/USB path.

The latest change reposts completed USB IN buffers directly from the client
callback rather than waiting for a lower-priority recovery task. A regression
test verifies repeated reposting with the worker unscheduled. This removes that
scheduling dependency, but did **not** resolve 10 MS/s on hardware: the last
80 ms capture was 1,600,000 bytes in 81,939 us, CRC32 `bd263477`, zero host-ring
drops, and failed synchronization. Do not call 10 MS/s reliable. 19.2 MS/s was
previously too slow and rejected; it was not retested after this change.

Next inspect radio-side overruns using HackRF's GET_M0_STATE request 41 on USB
API >= 0x0106. The official response is 40 bytes; it includes byte counters,
number of shortfalls and longest shortfall. Check the connected PortaPack API
version and support, and validate counter timing/reset semantics. This diagnostic
is researched but **not implemented**. Consider transfer sizing and PSRAM bus
contention after measuring; PSRAM DMA is already enabled. Do not weaken LTE or
capture checks to hide discontinuities. Do not slow the display to obtain an
apparently successful result without measuring usability.

Then implement original bounded SI-RNTI control/PDSCH decoding and SIB1 parsing
against the full-bandwidth PC recording. Keep CRC gates and replay provenance.
Only after network identity and scheduled broadcast configuration are available
should SD baselines and evidence-based anomaly rules drive detector alerts.
8 MS/s recovers central MIB from this 10 MHz LTE carrier but does not preserve
the whole channel for a general SIB decoder.

## Build and operate

Use ESP-IDF **5.4.3**. This PC's working SDK was `C:/esp/v5.4.3/esp-idf`.
Do not reuse a copied CMake cache with another SDK path. In an exported IDF shell:

```sh
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build
```

Follow `docs/TDP4_FIRST_FLASH.md` for first-install offsets. On the already
configured device, use an application-only flash at `0x10000`. Do not erase
the storage partition or SD card merely to resume development.

`cellperf on` reboots into the one-shot focused HackRF session. It retains
display, SD, GPS, nine-axis context and LoRa, while skipping unrelated radio/audio
startup. A normal reset returns to normal mode. In focused CELL WATCH, `S`
captures/analyzes/saves; `L` opens rate selection and `R` capture duration. The
proven setting is 8 MS/s, 80 ms at 739 MHz. Wait until analysis completes before
changing settings. `tui cost`, `display` and `celliq status` give diagnostics.

High-rate MIB results are stored on SD but are not yet automatically forwarded
in CW1 LoRa summaries. Normal CELL WATCH reporting works. Nine-axis data is
receiver-motion context; uncalibrated heading is not a transmitter bearing.
The GUI has an integrated CELL WATCH tab with PC/HackRF and P4 panes. The user's
requested separate NODES dashboard was not located in the fetched branches;
do not represent that repository-identification question as resolved.
