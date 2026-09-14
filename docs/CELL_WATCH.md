# CELL WATCH: experimental cellular anomaly monitor

Built on private LS_Test1 042923b8. Open HOME > CELL WATCH.

The current product focus is a standalone P4 passive cell-site-simulator
detector with an attached SDR. A PC or cellular hotspot is not a required part
of that target. See `CELL_DETECTOR_FOCUS.md` for the user's requirements,
reviewed projects, implementation gates and remaining detection limits.

The PC page is integrated into the private ground station at `aae2364`
(`integrate-multirx-beta`), preserving its selected theme. Open View > Cell Watch,
or use the supplied launcher. HackRF/PC reception and P4/LoRa reports appear
side by side, with Rayhunter connection evidence below. Splitter widths and
the Animate activity preference persist. Animation stops when hidden or idle.
Spectrum and waterfall data come from captured IQ; the LoRa timeline shows
received reports, not transmitter locations or inferred signal strength.

## P4 RF survey and SD baselines

Select a supported downlink band; START repeats a visible sweep. SITE switches
between GPS AUTO and an explicitly selected LOCAL site (useful indoors).
LEARN takes
three complete stationary passes, retains their per-bin maxima and freezes a
baseline. It saves under `/sdcard/cell/` using a temporary file and previous-file
backup. LOAD reads and validates the SD baseline on demand. GPS baselines are
selected automatically when starting at the corresponding location. Without a
fresh fix, LOAD explicitly selects a manual-site baseline: the operator must
verify the location and antenna setup. Baselines do not modify channel profiles.
LOCAL mode ignores GPS for baseline selection and comparison, while leaving
the GPS receiver available to other apps. It still enforces the IMU gate.

The scanner uses the radio-session broker, 256 ksample/s complex U8 IQ, 25 kHz
peak-held bins, 8 FFTs per tune and fixed 29.7 dB requested gain. It discards the
DC region and settles after each retune. It is a sequential survey, not a
simultaneous wideband capture. The UI reports actual completed tune counts and
last-completed-pass measurements. STOP cancels between bounded radio reads;
leaving waits for the worker to release its session before the next app starts.

A changed bin must rise by at least 12 dB, reach at least -65 dBFS, and persist
for three complete comparable passes. These are initial engineering thresholds,
not validated CSS classifiers. Displayed power is uncalibrated dBFS, not dBm.
Adjacent flagged bins may belong to one signal. The baseline is not silently
updated by monitoring, so a persistent change cannot train itself away.

IMU samples are requested every 250 ms and must remain within the acceleration
and gyro quiet thresholds across a pass. Brief driver contention is tolerated;
sensor data older than one second suppresses comparison. The angular-rate
threshold is 5 degrees/second (allowing for observed uncalibrated gyro bias),
and acceleration magnitude squared must stay between 0.85 and 1.15 g squared.
A three-second settling period precedes scanning.
This is a motion screen, not proof of being stationary: constant-velocity travel
and motion between samples can escape it. GPS uses fresh fixes (<5 seconds,
HDOP <=5) and 0.001 degree tiles. A tile or antenna-route change stops comparison.
Tile boundaries can split nearby locations; changing antennas, gain hardware,
orientation or nearby obstructions can still change RF levels at one location.
Use the same RTL receiver and physical antenna for baseline and monitoring;
changing the dongle or antenna requires learning a new baseline.

Supported presets are 869-894 MHz (B5 / GSM850), 925-960 MHz (B8 / E-GSM900),
729-746 MHz (B12), 746-756 MHz (B13) and 791-821 MHz (B20). These are downlink
frequency windows, not assertions about which radio technology is present.

Published values: `cell.busy`, `cell.changed_bins`, `cell.frequency` (MHz).
Actions: `cell.start` and `cell.stop`, both requiring tuning capability.
Start requires the CELL WATCH screen to be open; no RF transmission is added.

## Display and tuner integration

The original 60 MHz pixel clock produced user-observed whole-screen blue
flashes and repeated 36.8 ms refresh gaps on a 20.1 ms schedule. At 40 MHz the
user confirmed that flashing stopped, while noting slightly slower-looking
updates. Twelve injected touch-route screen changes took 21.6-26.7 ms and the
UI frame interval stayed 42.6 ms; the panel recorded zero late frames across
more than four minutes with GPS and autorotation enabled. These timings do
not measure the entire physical finger-to-photon path.

The current setting is 48 MHz, targeting approximately 40 Hz panel refresh,
as an intermediate setting for on-device validation. UI sleep cadence is
unchanged. Refresh counters are available with `display`. The legacy `disp`
diagnostic concerns the retired LVGL path and is not the right measurement
for this TUI panel.

An explicit park request now rechecks the actual receiver even if the UI's
previous request was also parked: a console `mode p25` can otherwise start RX
without updating the UI's cached request. Opening CELL WATCH parks the
previous decoder, and leaving it cancels the sweep and releases the tuner.

### Hardware validation, 14 September 2026

* At 48 MHz, the board delivered approximately 40 refreshes/second with zero
  late frames across 6,204 refreshes during baseline learning. After a restart,
  another 4,249 refreshes including live comparison also had zero late frames.
  The user confirmed no blue flashing at this setting. This is an observed
  improvement under tested loads, not proof against every possible load.
* Earlier live scanning measured 42.1-42.6 ms UI frames; a later RF scan measured
  62.9 ms, mostly input/sensor time and the existing sleep interval. The latest
  post-flash idle CELL check measured 42.6 ms with 40 Hz panel refresh and zero
  late frames across 12,980 refreshes. These are load-dependent measurements.
  GPS and autorotation
  were enabled. Touch-route navigation measured 21.6-26.7 ms at the earlier
  40 MHz test setting; post-restart opening at 48 MHz measured 25.3 ms.
* Three local-site B13 (746-756 MHz) passes completed at about 36.2 seconds per
  pass and saved a 1,452-byte `local_0_0_b3_a0.bin` baseline on SD.
* Restarted the board, selected B13 and LOCAL, and used LOAD. The app read the
  SD baseline as READY before new samples were taken. A subsequent full pass
  compared real RF readings against it; observed changes were below the
  persistent-rise threshold. No simulator detection is claimed.
* HOME navigation during an active sweep cancelled it and released the tuner.
  The test also exposed and fixed the cached-park issue described above.
* Original Franklin PD / Fire channel entries and the active, franklin-nh and
  new-england SD profiles remained present with unchanged listed sizes.
* Latest host verification passed 169 test executables, 62 C++ header checks, board
  rules and module-shadow checks. The four added cases exercise persistence
  thresholds, invalid data/motion, baseline checksum/context rejection and
  coverage of every planned bin across all five presets.

The early B13 baseline checks used the earlier antenna setup. Both SDRs now
have larger 915 MHz antennas. Later local B5 learning and comparisons used the
larger antenna, and that baseline was successfully loaded after a reset. Learn
again after any antenna or receiver change. Learning
refuses moving or stale IMU data and leaves the old SD file intact on failure.

## Corrections to the supplied proposal

* R820T coverage ending at 1766 MHz excludes DCS1800 downlink (1805-1880 MHz),
  PCS1900 downlink, LTE B2/B4/B25 downlink. Do not confuse uplink with downlink.
  See [3GPP TS 45.005, band definitions](https://www.etsi.org/deliver/etsi_ts/145000_145099/145005/18.00.00_60/ts_145005v180000p.pdf)
  and [TS 36.101, table 5.5-1](https://www.etsi.org/deliver/etsi_ts/136100_136199/136101/12.24.00_60/ts_136101v122400p.pdf).
* GSM shutdown does not make all energy in its former spectrum suspicious.
  Cellular allocations overlap across technologies; power alone cannot identify
  GSM, a cell identity, an operator, or a simulator. The supplied T-Mobile 2024
  shutdown date is not a sound basis for a detector rule. Consult the operator's
  [network evolution page](https://www.t-mobile.com/support/coverage/t-mobile-network-evolution).
* T3212 = 0 disables periodic updating; it does not force constant updates.
  [TS 24.008, MM timer information](https://www.etsi.org/deliver/etsi_ts/124000_124099/124008/14.08.00_60/ts_124008v140800p.pdf).
* Seeing the same cell while a receiver changes position does not prove a moving
  tower. A real fixed tower can cover a large area.
* Carrier offset inferred from arbitrary undecoded energy is not a reliable
  transmitter fingerprint. Receiver oscillator error and modulation must be
  separated first. This app does not pretend to measure that fingerprint.
* A nominal 200 kHz GSM channel does not establish that continuous GSM decoding
  works through this USB path. Symbol timing, filtering and dropped samples
  require a separate measured receiver experiment.

[SeaGlass](https://techpolicylab.uw.edu/wp-content/uploads/2018/07/SeaGlass-Enabling-City-Wide-IMSI-Catcher-Detection.pdf)
supports investigating several independent anomalies with historical context;
its observations do not turn a generic spectral change into confirmed CSS
detection. P4 RF observations leave identity unknown; separate, successfully
decoded PC broadcast observations can include identity. Neither issues a
"safe area" verdict when no anomalies are found.


## LTE physical-cell search on the P4

Press LTE to scan one selected downlink band. The screen displays the actual
frequency and completed/total frequency count. This is a finite pass; START is
the separate repeating RF survey. STOP and HOME cancel the current task and
release the RTL receiver. A completed pass does not mean the area is safe.

The LTE detector searches FDD, normal cyclic prefix, at 1.92 MS/s. It requires
repeated PSS/SSS matches at the expected five-millisecond spacing and checks
both half-frame sequences. It reports physical cell identity (PCI), frequency,
carrier offset and correlation quality. PCI is reused across networks and is
not a globally unique network/cell identity. P4 does not decode PLMN/TAC/ECI.
TDD, extended CP, GSM and 5G NR are not supported by this search.

The worker uses 30 ms captures, rejects incomplete or reported-loss captures,
and releases the USB stream between frequencies. A negative search takes
about 1.4 seconds per tune on the P4; the 237-frequency B5 pass takes roughly
seven minutes including tuning and transfer overhead. Only the latest eight
cells are shown. Detections are journaled at `/sdcard/cell/lte.jsonl`, rotating
at 1 MB to `lte.previous.jsonl`.

A public recorded signal was independently decoded by LTE-Cell-Scanner's C++
CellSearch as PCI 301, 100 resource blocks, normal CP, two antenna ports. The
same signal replayed through the P4 detector produced PCI 301, six hits, five
consistent pairs, and completed in 1.976 seconds. This is a replay validation,
not a locally received tower. Replay diagnostics cannot emit live reports or
train RF baselines. Host regression also rejects noise, cancelled analysis,
and broken repeated synchronization sequences.

The local 739 MHz signal has also been received directly on the P4/Nooelec:
an 80 ms capture at 1.92 MS/s completed with zero reported drops, PCI 244,
eight synchronization hits and seven consistent pairs. The latest post-flash
repeat produced PSS 0.555, SSS 0.729 and took 2.567 seconds to analyze. An
automatic B12 scan previously found the same PCI and delivered it over LoRa.

## LoRa remote reporting

The onboard LoRa radio carries reports; it does not decode cellular signals.
The T-Beam Supreme on COM4 runs a MeshCore USB companion and feeds the PC
P4/LoRa panel inside CELL WATCH. The P4 REPORT picker selects a known MeshCore destination;
Reports OFF disables these automatic addressed messages. Destination choice
is persisted at `/sdcard/cell/report.txt` and loaded at startup.

Current pair: P4 SHARK -> LS Cell Gateway. Both use 910.525 MHz, SF7,
62.5 kHz bandwidth and coding rate 4/5. P4 transmit power remains 22 dBm and
receiver power 10 dBm. This preserves the configured local link; it does not
claim compatibility with every public MeshCore preset. Receiver firmware is
MeshCore companion-v1.17.1, source commit
`d92964352441e53b93e8667b802e04f6e072b39e`, built for
`T_Beam_S3_Supreme_SX1262`. Its private identity backup stays in the work folder.

CW1 messages carry a boot identifier, sequence, observation type, current
frequency, optional PCI, quality flags and fresh GPS when available. Types
include RF unverified/compared/rise, LTE progress/completion/failure, receiver
loss, stop and explicit test. LTE progress is sent every two minutes; new
physical-cell observations and terminal state changes are sent promptly.
The recipient ACK is shown separately from merely queuing a transmission.
The authenticated MeshCore return-path ACK handling was corrected and actual
recipient ACKs have been observed. The ground station rejects malformed
reports, deduplicates them and marks stale/disconnected reception clearly.
It does not infer a simulator from a report or from an absent report.

PC LoRa logs are under `%LOCALAPPDATA%/LakeShark/cell-watch/`. There is no
persistent P4 outbound report queue yet; a disconnected gateway can miss data.

## HackRF broadcast identities on the PC

The normal arrangement is HackRF on the PC and Nooelec on the P4. Direct
HackRF-to-P4 support is undergoing a separate USB test. The PC page displays
LoRa reports and HackRF broadcasts side by side. Select Lower cellular
bands, PCS 1900, AWS 2100, or a custom center and press Search. Capture and
analysis run in a worker so the window remains responsive. Search is one pass.
Stop cancels the receiver/decoder and preserves completed observations.

Native `hackrf_transfer` captures at 19.2 MS/s with RF amplifier and bias power
off, LNA/VGA gain 32/32. It requires the full sample count and rejects captures
with more than 1% saturated sample components. Initial settling samples are
discarded. The same C synchronization detector searches each capture on a
100 kHz raster; candidates get a new centered recording for full decoding.
This limited search can miss weak cells, other duplex modes and signals with
larger receiver offset. Antenna coverage does not expand decoder support.

An external AGPL-3.0 LTE-Cell-Scanner Octave process decodes broadcast system
information from SI-RNTI allocations. Only CRC-verified BCCH blocks accepted
by its ASN.1 decoder enter the table. SIB1 supplies advertised PLMN, tracking
area code, cell identity, barred status and receive-selection threshold.
SIB presence and reselection priorities are extracted. A legacy-network
priority review requires LTE priorities from the same capture, frequency,
PCI and SIB1 identity. If SIB1 schedules SIB5 and it was not decoded, the
comparison stays incomplete. A short SIB1 schedule is informational.
These rules are not proof of a downgrade attack. No subscriber traffic,
paging identities, attach procedure or
cellular transmission is used by this adapter. Broadcast identities are not
authenticated and can be spoofed.

Replay is explicitly labeled and excluded from live baseline learning. A manual area
baseline requires the same advertised cell identity in three distinct live
captures in that area, followed by Store area baseline. Later identities,
TAC changes and access/selection changes are marked for review. The baseline
is frozen until explicitly replaced; three sightings do not establish trust.
Keep location, antenna, receiver settings and orientation comparable.

These PC broadcast baselines and observations are stored under
`%LOCALAPPDATA%/LakeShark/cell-broadcast/`, not on the P4 SD card. P4 RF baselines,
LTE journal and report destination remain on SD. Captures are retained locally
for diagnosis; there is no automatic capture-retention limit yet. Under the
packaged desktop host, Windows may redirect LOCALAPPDATA into its package
LocalCache; the application uses the environment's actual path.

### Decoder setup and source

The configured PC already has native HackRF tools at `C:/HackRF/bin`, Python
3.12 with PySide6/pyserial/numpy, and Ubuntu under WSL with Octave, signal,
communications and statistics packages. The external decoder is checked out at
`work/lte-reference`, source commit
`e7f71cbd4fa9f5ee5b97a58b2fb236ac13e4b8b1` from
[JiaoXianjun/LTE-Cell-Scanner](https://github.com/JiaoXianjun/LTE-Cell-Scanner).
The supplied patch includes the adapted receiver, incremental JSONL BCCH
output, SI-RNTI-only PDCCH selection, and corrected control-region mapping.
The reference code assumed fixed pilot positions; using the actual PCI modulo
three resolved the local PCI 244 SIB1 decoding failure. See
[TS 36.211](https://www.etsi.org/deliver/etsi_ts/136200_136299/136211/16.07.00_60/ts_136211v160700p.pdf).
Decode always reads the supplied
IQ; it does not reuse upstream filename-only MAT caches. Temporary ASN.1
payload names are unique per decoded block.

For another installation:

1. Install Python and the ground station `requirements.txt` dependencies.
2. Clone the external decoder at the commit above and apply
   `LTE-Cell-Scanner-cell-broadcast.patch` at that repository root.
3. In Ubuntu WSL install `octave octave-signal octave-communications
   octave-statistics gnuplot-nox build-essential`. Build its ASN.1 decoder:
   `cd asn1_test/LTE-BCCH-DL-SCH-decode && make -f Makefile.am.sample -j6`.
4. Set `LAKESHARK_HACKRF_TRANSFER` to the native Windows `hackrf_transfer.exe`,
   `LAKESHARK_LTE_DECODER` to that repository, and `LAKESHARK_LTE_SYNC_DLL` to
   `lte_sync.dll`. The supplied DLL is compiled from the included P4 C source
   (`lte_sync.c`, `-shared -O3 -DLTE_SYNC_SEARCH_SAMPLES=57600`, with its
   include directory, linking libm). The PC DLL searches the full capture;
   the P4 retains a shorter search window to control processing time.
5. Run `python run.py --cell-watch --cell-connect` from the patched ground
   station. Select the receiver's actual COM port if Windows renumbers it.

The supplied PowerShell launcher uses this PC's existing paths. The DLL,
firmware and patches have SHA-256 values in the build manifest. The external
AGPL license is included separately; original attribution is retained.

### Additional hardware validation, 14 September 2026

* A P4 panic was decoded from its saved core dump: an assertion in USB host
  bulk descriptor completion, rather than a display or LTE DSP stack overflow.
  The old stop/re-prime path relied on delays before freeing/reusing transfers.
  It now tracks pending callbacks and waits for ownership to return; on timeout
  it keeps those buffers and refuses a new stream. USB errors mark sample loss.
* The rebuilt application was flashed at 0x10000 and verified by esptool.
  One complete 237-frequency B5 scan then finished without restart. At 28,817
  panel refreshes it reported 40 Hz and zero late frames, reset reason power-on.
  LoRa delivered periodic progress and the end-of-pass report.
* Switching into P25 started a live RTL stream; returning to CELL WATCH parked
  it. Cancelling an LTE search through HOME released the receiver. Route timing
  was about 26-41 ms in those checks, not a finger-to-photon measurement.
  One 45.9 ms panel gap occurred during the later P25 test. This is not a claim
  of zero late frames under every load, and local P25 speech quality was not
  established by that idle-channel test.
* Full firmware host verification passed, including the new delayed-USB-callback
  ownership regression, LTE recorded-signal cases, C++ header checks, board
  rules and module-shadow checks. Twenty-three ground-station parser/baseline/analysis tests
  passed.
* A known public 1815.3 MHz recording decoded PCI 301, PLMN 206-01, TAC 9340,
  ECI 46136578, plus SIB2/SIB3. The GUI labeled all these observations REPLAY.
  These identities are not evidence of a local Belgian operator or local tower.
* Native HackRF captures completed across lower cellular, PCS and AWS centers.
  A local 739 MHz recording decoded PCI 244, 50 resource blocks (10 MHz),
  PLMNs 310-410 and 313-100, TAC 1792, ECI 23424527 and unbarred SIB1.
  TAC and ECI match the nearby reference supplied by the user. Reanalysis
  in the integrated GUI repeatedly recovered CRC-verified SIB1 records.
  This successful file was originally captured at 10 MS/s and resampled to
  19.2 MS/s. Fresh native 19.2 MS/s tests have recovered MIB but have not yet
  reliably recovered SIB1. Three distinct live SIB1 captures have not been
  established, so no validated local identity baseline is claimed.
* Native Qt verification passed embedded tabs, side-by-side layout, theme use,
  motion controls, hidden-animation suspension, bounded external logs and
  asynchronous process cancellation on close.

## Rayhunter connection evidence

Rayhunter complements the SDR observations by analyzing a modem's connection
messages. The integrated panel imports desktop JSON or daemon NDJSON, preserves
severity/timestamps/version metadata and runs the official Windows checker on
QMDL or compatible cellular PCAP/PCAPNG recordings. It cannot consume raw
HackRF IQ. See [EFF's reanalysis guide](https://efforg.github.io/rayhunter/reanalyzing.html).

The configured checker is v0.12.0. Its official Windows release ZIP was verified
against published SHA-256
`30acca2c5d639cd55d32e0df9b4744565a901cc7c3745e47ee5b19680a469d2a`.
Set `LAKESHARK_RAYHUNTER_CHECK` to `rayhunter-check.exe`. Download and licensing
details are in the [official release](https://github.com/EFForg/rayhunter/releases/tag/v0.12.0).
It runs as a separate executable; the supplied source patches do not embed it.

Each reanalysis writes a separate report and checker log under the configured
ground-station directory, normally `%APPDATA%/LakeShark/rayhunter-analysis/`.
Original inputs remain unchanged. Stop and a ten-minute timeout cancel the
owned process. Failed runs retain the previous report. Imports are historical
evidence and never train the SDR baseline or generate LoRa observations.

The official checker passed an end-to-end empty-QMDL GUI test, correctly showing
"No analyzable messages". Parser fixtures test formats and malformed inputs.
No real modem capture or controlled simulator dataset has been tested here.
Live Rayhunter operation requires a supported modem diagnostic interface;
the P4, HackRF and LoRa radio do not supply it. See
[supported devices](https://efforg.github.io/rayhunter/supported-devices.html) and
[porting requirements](https://efforg.github.io/rayhunter/porting.html).

## HackRF directly on P4: bounded diagnostic

The CELL WATCH screen now has **HIGH RATE**. Its picker explains the restart;
choose **Restart into HackRF** to start a focused capture session. C6/Bluetooth
and audio startup are skipped. The display, touch, keyboard, 9-axis IMU, GPS,
LoRa and SD remain available. The normal OS's saved receiver and audio settings
are not replaced. **NORMAL OS** opens the return/restart choice. The boot button
also returns to normal once a capture has finished. The mode is a one-session
software-restart request: an ordinary reset or power cycle returns to normal.

The focused screen provides CHECK CELL, frequency, sample rate, capture length,
LoRa destination and NORMAL OS controls. Complete captures save CU8 IQ and
matching JSON metadata under `/sdcard/cell/hrf_*.cu8`. Metadata includes CRC,
frequency, sample rate, dropped bytes, GPS validity and the accelerometer,
gyroscope, magnetometer and heading at capture start. The nine axes provide
orientation/motion context, not proof of a simulator. A single start snapshot
cannot exclude movement later during capture. The amplitude plot is measured
IQ amplitude across capture time, not a calibrated frequency spectrum.

With CELL WATCH open and scanning stopped, the new command
`celliq hackrf 739000000 2000000 80` requests an 80 ms HackRF capture.
Permitted values are 600-2690 MHz, 2-20 MS/s and 20-1000 ms, limited to 4 MB.
`celliq status` reports the actual source, bytes, duration, reported drops and
completion; the diagnostic explicitly rejects an unavailable HackRF endpoint.
RF amplifier is off. The existing adapter converts signed HackRF IQ to the
unsigned format used by LakeShark.

CHECK CELL now performs filtering and repeated LTE FDD normal-CP PSS/SSS
analysis on the P4 after a valid capture. It displays PCI, repeat/pair counts
and processing time, then saves that evidence in the IQ sidecar. USB reception,
filtering, synchronization and SD writes have separate progress labels.
`celliq sync` also accepts complete captures at 1.92 MS/s or 2-20 MS/s; it
examines at most 80 ms. The 129-tap, 96-phase filter keeps the central LTE
channel and rejects aliases before conversion to 1.92 MS/s. It does not replace
the separate full-bandwidth path needed for SIB decoding. Raw IQ is preserved.

This is physical-cell synchronization, not decoded operator/TAC/ECI or a CSS
classification. The normal automatic survey still uses RTL-specific rates;
the focused HackRF screen currently checks the selected center frequency.
Noise, missing sync or an incomplete capture never mean the area is safe.

Before the focused profile, ordinary-OS 2 and 4 MS/s 80 ms captures completed
without reported drops. At 8 MS/s, an 80 ms capture overflowed the ring and was
rejected; two 30 ms captures completed without reported drops. Moving a console
log before RX activation fixed an additional startup loss. All losses, including
startup/discard failures, are now included in diagnostic output.

The second 8 MS/s short capture was exported and verified against its CRC32
`db1ef573` (480,000 bytes). Offline filtering/resampling and C synchronization
analysis on the PC found local PCI 244, six hits, five pairs, PSS 0.930 and
SSS 0.978. This earlier check established usable signal reception through
HackRF directly on P4; that particular analysis ran on the PC. HackRF's
manufacturer recommends sampling at least 8 MS/s and filtering/decimating for
lower-bandwidth work; the lower-rate tests are transport diagnostics.
[HackRF sample-rate guidance](https://hackrf.readthedocs.io/en/latest/sampling_rate.html)

With the focused profile and dedicated capture worker, 8 MS/s and 10 MS/s
80 ms captures completed without reported drops and were saved with GPS/IMU
metadata. The measured acquisition times were 80.7 ms and 82.0 ms. At 19.2 MS/s,
the same requested duration took 151.9 ms despite a zero ring-drop counter.
That rate is not validated: USB backpressure can lose samples inside the radio.
The new timing check rejects captures taking more than 125% of nominal duration
plus 5 ms. Successful short captures are not proof of sustained operation.

The high-rate boot showed about 71 KB of DMA-capable free memory after backend
startup versus about 2.7 KB in the earlier normal boot. Display checks during
capture showed approximately 40 Hz, zero late frames and about 42.5 ms UI frames.
Both IMU chips initialized, and actual accelerometer, gyro, magnetic and GPS
values appeared in the screen and SD sidecars. Heading is an uncalibrated,
level-board magnetic estimate; nearby metal and tilt can distort it.

The on-device restart picker was tested both by cancelling and by entering the
focused profile. NORMAL OS returned through its picker, restored the ordinary
P25 preference and restarted the C6/BLE link successfully. No persistent mode
preference was overwritten.

The earlier high-rate build checks repeated 10 MS/s at 80 ms: 1,600,000 bytes in
82.0 ms, zero reported drops, CRC32 `1b3421f3`. A 19.2 MS/s attempt took
151.7 ms and was correctly rejected rather than saved as a complete capture.
The earlier over-slow test file and sidecar were renamed with `.rejected` on
SD. A real MeshCore link test from the focused profile reached the PC receiver.
The normal RF/LTE scan reports remain automatic; the focused raw-IQ capture
screen does not yet generate automatic capture summaries over LoRa.

The subsequent on-P4 analysis build (SHA-256
`b45f1119b8c0c8df757eafa30af364d3b39482de15a96ef8911a290f6bd52f69`)
found PCI 244 in two fresh 739 MHz, 8 MS/s, 80 ms captures. Both had 16 matched
observations and 15 timing/half-frame pairs. Analysis took 3.906 and 4.469 seconds;
the IQ and result metadata were saved on SD. A 10 MS/s capture completed but
did not synchronize, so the board is left at 8 MS/s. A slow 19.2 MS/s capture
was rejected, and its result screen and console correctly withheld LTE evidence.
UI frames measured 47.2 ms during filtering and 42.5 ms while idle. This moves
LTE synchronization onto the device. The following build adds the next stage.

The PBCH/MIB build (SHA-256
`e5caf9725f065b2737d4ea0127bbaec0ed387fd521228b33ea270f559b0bdb6c`)
decodes bandwidth, antenna ports, PHICH configuration and frame number on the
P4. It requires separate CRC-valid occasions with matching configuration and
progressing frame numbers. Two fresh 739 MHz, 8 MS/s captures each yielded
eight such occasions: 10 MHz bandwidth and two ports. The MIB stage took
2.298 and 2.294 seconds with 33,296 bytes of workspace; total analysis took
about 6.6 and 6.1 seconds. A UI timing check during decoding measured 42.5 ms
per frame. IQ and the nested `lte.mib` result are saved with GPS/9-axis context.
A complete 10 MS/s capture again failed synchronization, so 8 MS/s remains the
proven setting. No network identity or simulator verdict is inferred from MIB.

Recorded-data validation recovered consistent MIBs from an earlier P4 capture,
a separate local PC recording, and the independent public Belgian fixture.
Host validation passed 171 test programs and 62 C++ header checks. Numerical
vectors additionally exercise 1/2/4 antenna ports, frame-number rollover and
nonzero MIB extension bits. Tests reject noise, DC, wrong-cell observations,
a lone CRC, replayed unchanged frame numbers, damaged codewords and cancellation.
See `docs/CELL_DETECTOR_FOCUS.md` for the primary research and implementation
gates. SIB1/network identity and a validated passive CSS classifier remain work
to do on the P4; wider-channel reception still needs its own validation.

## Antennas for this setup

Both SDRs now use larger 915 MHz antennas. The 739 MHz signal was received
with this setup, but 1900/2100 MHz performance is not established. A
2.4 GHz Wi-Fi antenna is an experiment there, with unknown off-band response.
Check center contacts: RP-SMA Wi-Fi antennas need a matching adapter to make
contact with HackRF's standard SMA socket.
[HackRF connector documentation](https://hackrf.readthedocs.io/en/latest/hackrf_connectors.html).

Recommended first purchase: two Proxicast ANT-120-008 standard SMA male paddle
antennas, one per SDR. Manufacturer specifications cover 600-6000 MHz with an
integrated ground plane; peak gain is not constant gain across that range.
[Amazon two-pack](https://www.amazon.com/dp/B09K63LQX3) /
[single](https://www.amazon.com/dp/B09K627KGQ) /
[manufacturer specifications](https://www.proxicast.com/shopping/ANT-120-008).
Optional PC positioning base: Proxicast ANT-120-MB5, SMA female top/SMA male
lead, 6.5 ft CFD240 cable:
[Amazon](https://www.amazon.com/dp/B09SYD86QC) /
[specifications](https://www.proxicast.com/shopping/ANT-120-MB5).
Amazon checkout price/availability was not verified. Keep the existing
915 MHz LoRa antennas while the reporting link works; they do not need to
cover cellular spectrum. Supplied T-shaped antennas cannot be identified by
shape alone; check their labels, dimensions and connectors before assigning
them to a receiver. Learn a new RF baseline after any antenna swap.

## What is still needed for a credible detector

The app is an experimental measurement and anomaly-review tool, not a validated
Stingray detector. The next evidence needed is repeatable live broadcast
decoding with suitable antennas, repeated stationary observations, controlled
replay/anomaly tests, and measured false-positive/false-negative performance.
LTE broadcast changes can result from ordinary network maintenance or reception
conditions. New identities are not automatically malicious, and a simulator
can copy familiar identities. NR/GSM decoding, neighbor-list consistency,
protocol/security downgrade evidence and multi-sensor corroboration remain
future work; LoRa is transport rather than an extra cellular sensor.

The user-supplied map screenshots are reference material, not a trusted cell
inventory. Frequency-only labels such as “Lower Cellular Spectrum” also group
non-cellular services: 946/950 MHz can be broadcast studio-transmitter links,
462 MHz entries are not cellular base stations, and a licensed receiver record
is not a transmitting tower. Do not whitelist those records as known cells.
See [FCC Part 74](https://www.govinfo.gov/content/pkg/CFR-2023-title47-vol4/pdf/CFR-2023-title47-vol4-part74.pdf).

Further primary references:
[3GPP TS 36.211 synchronization signals, section 6.11](https://www.etsi.org/deliver/etsi_ts/136200_136299/136211/16.07.00_60/ts_136211v160700p.pdf),
[HackRF One bandwidth and tuning specifications](https://greatscottgadgets.com/hackrf/one/),
and [SeaGlass research](https://techpolicylab.uw.edu/wp-content/uploads/2018/07/SeaGlass-Enabling-City-Wide-IMSI-Catcher-Detection.pdf).
