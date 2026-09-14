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
Host validation passed 170 test programs and 62 C++ header checks. The filter
also recovered PCI 244 from an independent earlier local recording.
P4 MIB/SIB1 decoding and a validated passive CSS classifier remain incomplete.
The detector can only evaluate broadcasts it receives; no universal detection
guarantee or protection of the user's phone is implied.
