# P4 standalone cell-site-simulator detector

## User requirements, 14 September 2026

The primary objective is detecting suspicious cell-site-simulator behavior on
the LakeShark P4. The user explicitly declined a separate hotspot/modem and a
Rayhunter-centered replacement. Use the attached HackRF or Nooelec as the RF
front end; acquisition, decoding, evidence evaluation and alerts must run on the
P4. The PC is optional for development, review and ground-station reporting.
Keep SD storage, GPS, nine-axis context and optional MeshCore/LoRa reports.
Prioritize this detector over additional general-purpose GUI features.

The user also explicitly asked that we always research relevant existing
projects and how they solved the problem before implementing new detector
stages. Keep this source/decision record current. Read implementation and
validation details, check hardware assumptions and licensing, and distinguish
measured results from claims. Do not silently substitute a desktop solution.

## Research and decisions

**Crocodile Hunter:** EFF's passive LTE design examines broadcast observations
and compares cell identity, tracking area, signal level and location. Its
`watchdog.py` has provisional thresholds, including a one-standard-deviation
RSSI rule and an additive score. Reuse the categories of evidence; those weights
are not calibrated probabilities for this device. Its maintained-runtime status
ended in 2022; the archived Linux/srsLTE application is not an MCU port.
Reviewed source commit `4c36359d82605eb5f1927a61f279d8f56b72b1ee`, GPL-3.0.
[Project](https://github.com/EFForg/crocodilehunter),
[rules](https://github.com/EFForg/crocodilehunter/blob/4c36359d82605eb5f1927a61f279d8f56b72b1ee/src/watchdog.py).

**SeaGlass:** Spatial and historical comparison can expose identity/frequency,
location and broadcast-configuration anomalies. The research used many repeated
measurements across vehicles and cities, with modem-based sensors. A handheld
with sparse observations cannot claim that coverage or accuracy. Use SD history
with location and receiver setup checks; a missing map/database entry, stronger
signal or newly observed cell alone is not a simulator verdict. The authors
explicitly distinguished suspicious observations from independently confirmed
IMSI catchers. [University project](https://seaglass.cs.washington.edu/index.html).

**LTE-Cell-Scanner:** Its narrowband cell search is distinct from its wider-band
PDSCH/SIB receiver. The latter uses MATLAB/Octave and additional decoding work.
Use staged acquisition: filtered central-channel PSS/SSS, CRC-checked PBCH/MIB,
then control-channel and broadcast PDSCH decoding at adequate bandwidth. The
new C resampler is original firmware code, not a copy of its AGPL implementation.
Reviewed local reference commit `e7f71cbd4fa9f5ee5b97a58b2fb236ac13e4b8b1`.
[Project](https://github.com/JiaoXianjun/LTE-Cell-Scanner).

**srsRAN and FALCON:** Examine their MIB/PBCH and LTE control-channel pipelines
for architecture, test cases and resource costs. Both are host-side references;
FALCON documents substantial CPU/memory/bus demands. Do not assume a whole-stack
port, real-time operation or a license change is already solved. Profile each
bounded stage on the P4 and preserve the full capture for later stages.
[srsRAN MIB receiver](https://github.com/srsran/srsRAN_4G/blob/master/lib/src/phy/ue/ue_mib.c),
[FALCON](https://github.com/falkenber9/falcon).

**PBCH implementation review:** TS 36.211 sections 6.6/6.10 define PBCH resource
mapping, transmit diversity, scrambling and cell-specific reference signals.
TS 36.212 sections 5.1/5.3.1 define convolutional interleaving, tail-biting coding
and antenna-specific CRC masks. srsRAN's `pbch.c` attempts decoding at individual
10 ms occasions and can combine occasions over 40 ms. Our original bounded C
implementation starts with separate occasions and requires two CRC-valid results
with matching configuration and progressing SFNs. It uses exact tail-biting
maximum-likelihood decoding, retaining MIB extension bits rather than assuming
all spare bits are zero. A zero-soft-input CRC is insufficient without signal
evidence. This is a broadcast decoder, not an authentication check.
[TS 36.211](https://www.etsi.org/deliver/etsi_ts/136200_136299/136211/16.07.00_60/ts_136211v160700p.pdf),
[TS 36.212](https://www.etsi.org/deliver/etsi_ts/136200_136299/136212/16.02.00_60/ts_136212v160200p.pdf),
[srsRAN PBCH](https://github.com/srsran/srsRAN_4G/blob/master/lib/src/phy/phch/pbch.c).

The host replay loop now decodes PBCH from the earlier 30 ms P4/HackRF recording
and the independent 80 ms PC recording: PCI 244, 50 RB, two ports, with three and
eight consistent CRC-valid occasions. The public Belgian recording yields
PCI 301, 100 RB, two ports and eight occasions; this bandwidth/port configuration
matches the independent LTE-Cell-Scanner decode. The committed test fixture
contains four small PBCH/CRS excerpts, its source hash and provenance. Separate
numerical transmit vectors exercise one/two/four ports, nonzero extension bits,
SFN rollover and damaged codewords: `python bench/tools/lte_mib_vectors.py`
(host GCC and NumPy required, no radio transmission). Host tests also reject
noise, DC, the wrong PCI, a single valid occasion, stale repeated SFNs, invalid
bounds and canceled work. These tests do not establish CSS detection accuracy.

**Rayhunter:** Keep its existing optional report-review integration, and learn
from stateful heuristics and explicit evidence coverage. Its identity-request,
authentication, encryption and connection-release checks depend on modem
diagnostic traffic. Those checks are not available merely by passing SDR IQ to
the P4. Broadcast reselection checks may inform independently validated on-device
rules once SIBs are decoded. [Heuristics](https://efforg.github.io/rayhunter/heuristics.html).

## Implementation gates

1. Run HackRF filtering and repeated LTE synchronization on the P4, including
   measured CPU time, sample continuity rejection and SD evidence. A PCI is
   a reusable physical identifier, not a globally unique or authenticated tower.
2. Decode PBCH/MIB with CRC and antenna-port checks on-device. Validate against
   independent recorded broadcasts and corrupted/noise captures. Record bandwidth
   and frame timing, then measure memory and touch/display impact.
3. Decode SI-RNTI control and broadcast transport blocks, enforce CRCs, and parse
   SIB1 operator list/TAC/ECI plus the scheduled SIBs needed for reselection rules.
   The narrowband synchronization filter cannot provide a full-channel SIB path.
   Longer scheduled listening may require repeated capture/analysis cycles.
4. Maintain frozen, setup-qualified identity/configuration baselines on SD.
   Preserve multi-operator cells, repeated identities on legitimate frequencies,
   maintenance, handovers, receiver motion and missing-message coverage in tests.
5. Present specific suspicious evidence with capture provenance and rule version.
   Correlated power measurements are not independent indicators. Unknown evidence
   is not a clean result; ordinary network changes must be included in evaluation.
6. Validate rules with labeled offline scenarios and benign field observations.
   Parser/filter tests do not establish detector sensitivity or false-positive
   rates. No live cellular transmissions are needed for the initial test suite.

## Current boundary

The P4 has continuous-IQ checks, a new filtered HackRF-to-LTE synchronization
path, SD records and sensor context. The flashed firmware independently recovered
PCI 244 from two fresh 739 MHz HackRF captures at 8 MS/s: 16 observations and
15 consistent pairs each, taking 3.906 and 4.469 seconds on the P4. A 10 MS/s
capture passed acquisition checks but did not synchronize. A slow 19.2 MS/s
capture was rejected and could not enter analysis. These few trials demonstrate
the data path, not a measured detection rate. The device is left at 8 MS/s.
That synchronization checkpoint passed 170 test programs and 62 C++ header checks. The filter
also recovered PCI 244 from an independent earlier local recording.
The next firmware build adds on-P4 PBCH/MIB decoding. Two fresh 8 MS/s, 80 ms
captures at 739 MHz each decoded eight consistent CRC-valid MIB occasions:
50 RB (10 MHz), two antenna ports, normal PHICH duration and resource index 2.
MIB processing took 2.298 and 2.294 seconds using 33,296 bytes of workspace;
the UI measured 42.5 ms per frame during the second decode. Total filtering,
synchronization and MIB processing took approximately 6.6 and 6.1 seconds.
Capture CRC32 values were `8d93a99b` and `4838a8aa`. A subsequent complete
10 MS/s capture (`f1a82949`) again failed synchronization; no MIB result was
published for it. Keep 8 MS/s as the proven setting. Timing-based acquisition
checks alone do not establish valid LTE reception at every requested rate.
The updated host suite passed 171 test programs and 62 C++ header checks.
SIB1/network identity decoding and a validated passive CSS classifier remain
incomplete. MIB channel configuration is an input to the next full-bandwidth
control/PDSCH stage, not sufficient information to classify a simulator.
The detector can only evaluate broadcasts it receives; no universal detection
guarantee or protection of the user's phone is implied.
