# CELL WATCH development loop

The target is a passive, standalone P4 cell-site-simulator detector. The HackRF
receives downlink samples; GPS and the nine-axis sensor add receiver context.
The ground station is optional. Do not treat MIB reception or a new PCI as a
simulator verdict. SIB1 and a validated classifier are still missing.

## One entry point

Run from this checkout in PowerShell:

```powershell
./tools/cell_lab.ps1 doctor
./tools/cell_lab.ps1 prepare
./tools/cell_lab.ps1 verify
./tools/cell_lab.ps1 build
./tools/cell_lab.ps1 archive -Port COM13 -OutputDirectory ../cell-archive
```

`doctor` identifies Python, NumPy, pyserial, GCC and serial devices. The
examples below use COM13 for the P4 and COM12 for the MeshCore USB device;
enumerate on every new PC. Never assume that
the MeshCore port is the P4 console.

The runtime already installed here is ESP-IDF 5.4.3 at
`C:/esp/v5.4.3/esp-idf`, with its Python environment under `C:/Espressif`.
No copied build cache is needed. Builds are incremental after the first build.
Verification uses recorded IQ before any hardware experiment. Do not rerun the
whole suite for documentation-only changes. `verify.ps1 -Quiet` now preserves
failure diagnostics.

`prepare` applies the reviewed ESP32-P4 portion of Espressif commit
[`40dd5e3`](https://github.com/espressif/esp-idf/commit/40dd5e3957fbc7183c6952b165a28beee1704d41)
to the configured SDK (one `system_internal.c` file). ESP-IDF 5.4.3 omitted
AHB DMA and SDMMC reset during software restart; live DMA can overwrite the
next boot's memory. This session reproduced heap, USB semaphore and bootloader
corruption before applying the fix. The patch also carries the upstream H264
reset and ordering changes. It is stored in `bench/patches/`, checks matching
source before application, and is idempotent. Builds refuse a P4 SDK missing
the reset fix. This affects other projects built using that same SDK; no SDK
upgrade or bootloader flash is required. Keep the patch until the SDK contains
the upstream fix. Temporary heap-poisoning diagnostics are disabled again.

A later boot hung in the SDK's unbounded DSI panel-ID read. The native panel
driver now uses a 250 ms deadline for command, response and FIFO waits; timeout
returns through the existing cleanup/error path so startup can reach the
console. This is a separate local driver fix, not another shared-SDK patch.
Host fault injection covers full FIFOs, a busy command, absent data and a FIFO
that never drains. It does not establish why the physical panel missed a reply.

## Field session

Enter CELL WATCH's performance mode (`cellperf on` from the P4 console).
It starts GPS and IMU explicitly, uses large text and a light background, and
keeps normal-mode font preferences separate. VIEW changes theme/text size.

- **AUTO** repeats capture, decode, save and a pause until STOP. STOP finishes
  the current bounded capture/analysis/save, then releases the worker.
- **ONCE** takes one capture. CHANNEL selects a receive preset. OPTIONS keeps
  one-shot experimental rates and durations out of the main controls.
- Auto defaults to the selected channel. OPTIONS can instead cycle the six
  existing presets: 739, 751, 881.5, 1981.25, 1992.5 and 2150 MHz. This is a
  shortlist, not a complete cellular-band search. It may miss carriers in a
  new location. Auto always uses the previously demonstrated 8 MS/s / 80 ms.
- The receiver takes bursts with processing gaps; it does not observe every
  instant. A lack of decoded LTE is an unknown observation, never an all-clear.
- Missing GPS does not stop collection. Records explicitly retain validity,
  satellite/HDOP/speed fields and start/end location and inertial samples.
  End samples bracket RF acquisition, not the later several-second DSP work.
  No-fix observations cannot support a geographic comparison.
- SD failure stops Auto. A session keeps up to 256 MiB of raw IQ, then
  continues small metadata records. Its journal is bounded to approximately
  32 MiB (checked before appending one record). Nothing auto-deletes evidence.
  Starting another session has a new budget; total free SD space still matters.
- Performance mode leaves the unrelated scanners, legacy decoders and console
  event feed inactive; the shared USB service and UI lifecycle worker remain.

## Evidence and integrity

`/sdcard/cell/session_<unix>_<uptime>.jsonl` records every automatic attempt,
including acquisition failures. Raw captures and JSON sidecars retain their
existing `hrf_` prefix. Schema 2 adds the attempt number, timestamps, decoder
version, raw filename, validity, motion and logging context. `raw_saved` is
false after the raw budget is exhausted. MIB evidence still passes the existing
CRC, timing and repeated-frame gates. `complete` describes acquisition checks;
it is not a proof of radio-side sample continuity.

New metadata also records `radio_health` from HackRF GET_M0_STATE after RX stops.
The query uses the leased radio session and waits safely through USB removal;
it is refused during streaming. `result != "ok"` means unknown telemetry, not
zero loss. These counters cover the whole RX session (including startup and
shutdown), not just the saved window. A reported shortfall, device error,
unfinished stop or implausibly small device byte counter makes `complete=false`
and prevents decoding. Failed raw samples remain available with that flag for
negative replay tests. Unsupported firmware keeps the prior host checks and
explicit unknown health; it must not be treated as proven continuity.

The archive command reads metadata first and verifies the device's byte count,
ordered chunks and CRC32 before replacing a local file. It writes a SHA256
manifest. A truncated/corrupt transfer cannot replace a good local copy.
To retrieve one useful raw recording explicitly:

```powershell
python tools/cell_archive.py --port COM13 --out ../cell-archive --raw hrf_NAME.cu8
```

Raw transfer over the 115200-baud console is slow; select recordings after
inspecting metadata. Do not download every capture just to count decodes.
All recordings and GPS data stay local unless explicitly shared.

For a bounded comparison using the same worker and priority as Auto:

```powershell
python tools/cell_experiment.py --port COM13 --out ../rate-comparison --rates 8000000 10000000 --repeats 2
```

Each observation is saved on SD, downloaded with CRC verification, and summarized
with host drops, radio shortfalls, capture timing and MIB outcome. This command
does not enter performance mode or interrupt an existing session. The lower
level command is `celliq once <Hz> <rate> <ms>`.

Console batches use one connection:

```powershell
python tools/cell_console.py --port COM13 --log ../health.json "celliq status" "tui cost" display
python tools/cell_console.py --port COM13 "celliq auto 739000000"
python tools/cell_console.py --port COM13 "celliq stop"
```

`celliq auto list` uses six presets. `celliq read <basename>` reads only regular
files beneath `/sdcard/cell`, bounds file size, and does not modify the SD.

## Decoder and storage optimization (p4-mib-3)

The PSS search reuses input FFTs and energy windows across its 51 templates,
preserving the previous traversal order and acceptance thresholds. Its temporary
workspace is larger but remains in PSRAM. The resampler generates only fractional
phases that the selected rate uses (six at 8 MS/s); output samples are unchanged.

Live MIB decoding uses `lte_mib_confirm`: it returns after two CRC-valid occasions
with matching configuration and progressing SFN. `crc_frames` counts the actual
confirmations, so two is expected and is not evidence of degraded reception.
`lte_mib_find` remains exhaustive for reference replay. Metadata identifies the
mode as `confirm_two_crc`, with decoder version `p4-mib-3`.

Additive metadata fields retain separate acquisition, resampling, synchronization
and raw-write durations, actual receiver gain/bandwidth, and raw ADC statistics.
`ac_power_codes2` is DC-removed mean complex power in sample-code units; it is not
calibrated RF power or RSRP. `rail_fraction` counts I/Q components at 0 or 255.
Invalid captures have `raw_stats.valid=false`. Acquisition timing includes setup,
discard, capture, stop and CRC; it is not just the 80 ms retained window.

Raw buffers are 128-byte aligned, and raw writes go through POSIX `write` to keep
that alignment through VFS/FatFs. ESP-IDF otherwise may fall back to separate
single-sector DMA writes. Full raw retention and the existing budgets remain.
Short writes or close failures still stop Auto.

Verify a saved raw file on the P4 without transferring it over UART:

```powershell
python tools/cell_console.py --port COM13 "celliq check hrf_NAME.cu8"
python tools/cell_experiment.py --port COM13 --out ../comparison --rates 8000000 --repeats 3 --verify-raw
```

`check` returns file length and CRC32; compare both with the capture metadata.
`--verify-raw` performs that comparison and fails on incomplete or mismatching
readback. This verifies storage integrity, not radio sample continuity. Do not
include readback/download time when reporting normal Auto revisit intervals.

## Research decisions and next gates

Display stability: T-Display-P4 performance mode uses a 40 MHz DPI pixel clock
(about 33 Hz). The DSI ISR remains available during flash/cache-disabled work,
and C2M cache writebacks are split into 16 KiB critical sections. Keep the refresh
callback and all its data cache-safe; it must not read a PSRAM-backed task name.
The autorotation setting is loaded at boot and read from RAM each frame.
`display` reports the actual pixel clock, expected frame interval and accumulated
late callbacks; late callbacks alone do not establish an underrun's cause.
The Cell Watch sensor label polls at most 4 Hz and retains a successful reading
for at most 750 ms, then reports STALE. Capture validity is independent.

Additional user-supplied references reviewed:

- [Wooniety/srsLTE-Sniffer](https://github.com/Wooniety/srsLTE-Sniffer): a desktop
  srsLTE-based receiver whose README says it is no longer maintained, with
  MIB/SIB1 and paging processing. Its SIB1
  acquisition and capture-export path are relevant references; its subscriber
  identity collection is outside this detector's objective. It is not evidence
  that full PDSCH decoding fits the P4 or our 8 MS/s capture limits.
- [jaime7981/IMSI-catcher](https://github.com/jaime7981/IMSI-catcher): the README
  only points at cloning UHD; insufficient documentation to adopt it as a
  validated detection implementation. No code from it has been executed.
- [Ettus UHD](https://files.ettus.com/manual/) is the USRP driver/API and FPGA
  tooling ecosystem, not a standalone detector or a replacement HackRF driver.

The relevant display guidance is in the [Espressif LCD FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html)
and the installed IDF 5.4.3 `esp_mm/Kconfig` / `esp_lcd/Kconfig` help for
`ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS` and `LCD_DSI_ISR_IRAM_SAFE`.

1. **Capture continuity:** GET_M0_STATE is implemented. The official host requires USB API
   >= 0x0106 and reads 40 bytes; fields include M0/M4 byte counters, shortfall
   count, longest shortfall, active mode and error. The attached radio advertises
   API `0x0111`. The first guarded probe measured zero shortfalls at 8 MS/s and
   26 at 10 MS/s (longest 1,920 bytes), despite zero host drops at both rates.
   The 8 MS/s capture decoded MIB; the 10 MS/s capture failed synchronization.
   These are individual observations, not a reliability percentage. Compare
   repeat runs before changing transfer sizes or USB scheduling. Probe failures mean unknown
   telemetry, not zero loss. Do not insert an unguarded raw `s_dev` access that
   races USB detach; route diagnostics through the session/broker lifecycle.
   [Official host implementation](https://github.com/greatscottgadgets/hackrf/blob/main/host/libhackrf/src/hackrf.c),
   [API structure](https://github.com/greatscottgadgets/hackrf/blob/main/host/libhackrf/src/hackrf.h).
   The subsequent worker comparison used two captures per rate/transfer size.
   At 16 KiB, 10 MS/s had 34 and 44 shortfalls; at 32 KiB it had 36 and 34.
   All four 8 MS/s comparisons had zero shortfalls. The 32 KiB experiment was
   reverted; it did not fix the full-bandwidth capture problem. Failed 10 MS/s
   raw recordings remain on SD with metadata marking them rejected.
2. **Network identity:** use `cell-recordings/pc-739-10ms-80ms.cu8` as the
   preserved full-bandwidth reference. Add bounded SI-RNTI control/PDSCH
   decoding, transport CRC and SIB1 parsing; compare PLMN/TAC/ECI against the
   independently decoded reference. 8 MS/s MIB success does not demonstrate
   adequate capture of a whole 10 MHz LTE channel. Keep the negative timing
   fixture and strong-signal failures in the gate.
   [srsRAN UE downlink receiver](https://github.com/srsran/srsRAN_4G/blob/master/lib/src/phy/ue/ue_dl.c),
   [HackRF sample-rate/filter guidance](https://hackrf.readthedocs.io/en/latest/sampling_rate.html).
3. **Coverage before anomaly rules:** add full-band channel discovery, decoded
   identity/configuration history and scheduled SIB coverage. SeaGlass uses
   many repeated observations across locations; sparse or moving RF power
   measurements need location/setup qualification. A new location or a new
   PCI alone is not malicious evidence.
   [SeaGlass project and evaluation](https://seaglass.cs.washington.edu/).
4. **Validated alerts:** create labeled benign-change and suspicious-broadcast
   replay scenarios; report rule version, evidence and missing coverage.
   Rayhunter's modem connection/authentication/identity-request checks are not
   supplied by this passive IQ receiver. Its broadcast reselection analysis
   is relevant only after the necessary SIBs are actually decoded.
   [Rayhunter heuristics](https://efforg.github.io/rayhunter/heuristics.html).

For every experiment retain: input SHA256, firmware/source version, rate/gain,
GPS validity, expected/actual CRC-gated decode, duration, dropped/shortfall
telemetry, UI frame cost and stop behavior. Compare the same recordings before
spending hardware time. Host parser tests are not detector accuracy tests.
