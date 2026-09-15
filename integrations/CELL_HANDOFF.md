# CELL WATCH handoff — 14 September 2026

## Current PC: continuous sessions, radio loss gates and restart fix

Read `integrations/cell-lab/README.md` first for the repeatable doctor → replay /
host verification → build → archive loop and current detector gaps. This update
adds Auto repeated captures, a six-preset option, larger 15x26 text, a direct
light/dark toggle, sensor startup and bounded SD session journals. Normal mode
retains its font preference. Auto is a burst receiver with processing gaps;
the shortlist is not a full-band search or a validated simulator classifier.

The P4 is currently CH343 COM13, serial `5C84301528`; the MeshCore device is
COM12. Identify afresh on another PC. `cellperf on` enters performance mode;
AUTO starts the selected channel at 8 MS/s / 80 ms, STOP finishes the current
save. OPTIONS switches to six presets. VIEW switches theme and text size.
`celliq auto <Hz|list>` and `celliq stop` expose the same session engine.

The new radio-health operation reads HackRF GET_M0_STATE only after stopping RX,
under the session lifetime/control locks. The connected radio reports API
`0x0111`. Any reported shortfall/error, unfinished stop or device counter shorter
than the capture rejects the capture before decoding. Failed raw IQ is retained
with `complete=false` for review. Missing/unsupported health is explicitly
unknown. These counters cover the entire RX session, including startup/stop.
`tools/cell_experiment.py` and `celliq once <Hz> <rate> <ms>` compare rates using
the same worker as Auto and retrieve verified metadata.

Four live 8 MS/s comparison captures reported no radio shortfalls. Four 10 MS/s
captures reported 34, 44, 36 and 34 shortfalls, all with zero host drops; the
new gate rejected all four without calling the decoder. Doubling USB transfers
from 16 to 32 KiB did not remove the loss and was reverted. Auto remains at
8 MS/s / 80 ms; full-bandwidth reception and SIB1 remain open work.

Software-restart corruption was traced to a missing SDK DMA reset, matching
Espressif commit `40dd5e3957fbc7183c6952b165a28beee1704d41`. Its P4-only backport
is in `bench/patches/`; run `tools/cell_lab.ps1 prepare` on a new SDK. It modifies
one shared SDK source file and is idempotent. P4 builds check for the reset fix.
The patch passed three normal-to-performance transitions and two returns to
normal, with heap poisoning disabled. Do not remove the patch when reproducing
this build. Performance startup also avoids unrelated legacy radio engines;
the common USB service and UI lifecycle worker remain active.

A later restart exposed a separate indefinite wait in the SDK DSI panel-ID
read. `ls_panel_dsi_id.h` bounds that boot-only operation to a 250 ms deadline
and lets failed display initialization return to startup/console. It uses the
same DCS ID command through IDF's low-level register API; it does not guess an
ID or hide a missing response. Host tests inject five stalled/no-response cases.
The cause of the panel's missed response remains unproven.

The archived SD metadata includes six post-checkpoint field captures: two
739 MHz / PCI 244 MIB successes at 20:40–20:41 UTC, followed by four no-sync
captures roughly one kilometre farther from the previous checkpoint. All six
have valid GPS and nine-axis flags. Their actual rates were 8 MS/s; the last
used 100 ms. This PC's first Auto session at the new location recovered PCI 44,
50 RB and seven CRC-valid MIB occasions. Its next capture synchronized to PCI
475 without a valid MIB. GPS was receiving sentences but had no current fix.
None of these observations establishes simulator activity.

The first implementation passed 172 host executables / 62 C++ header checks,
the existing recorded-IQ regressions and archive corruption/truncation tests.
Hardware confirmed startup of GPS, ICM20948 and AK09916; an automatic session
with two ordered SD records; clean Stop; 41.6 ms UI frames and 40 Hz panel with
no late frames. The enlarged portrait controls received a further layout fix
after inspecting the real device. Check the current task's output report for
final firmware hashes and final hardware checks.

The preceding application was read back before flashing and its SHA256 matches
the historical checkpoint below. Local backup: the task's `work/p4-previous-app.bin`.
SD files and the storage partition were preserved. Restart failures generated
new core dumps; two dumps and matching ELFs were archived under the task's
`work/crash-check*`. The firmware's crash handler overwrote the earlier on-device
dump during these failures; it was not manually erased.

## Previous checkpoint (historical)

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
