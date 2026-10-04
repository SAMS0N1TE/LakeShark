# HackRF USB bring-up

The T-Display-P4 has received HackRF IQ at 2 MSPS (4 MB/s) with zero dropped
input bytes and no read errors. This establishes USB transport; it does not
by itself establish successful RF decoding or support for every app.

## Startup failures found

The RTL and HackRF probes shared a USB client and could open the same device
concurrently. USB enumeration now reads the descriptor, closes that temporary
handle, and dispatches one adapter. This prevents the second open from failing
with `ESP_ERR_INVALID_STATE`.

HackRF interface claims then exposed `ESP_ERR_NO_MEM`. Allocation diagnostics
identified a 16-byte, 512-byte-aligned request with capabilities `0x80808`
(internal, DMA, cache-aligned). Free PSRAM could not satisfy it: the existing
`ls_usb_descriptors.h` workaround intentionally keeps these descriptors internal
to avoid an interrupt watchdog failure in PSRAM cache synchronization.

The workaround now reserves eight 512-byte internal slots before startup heap
fragmentation. Allocation and release use an atomic bitmap, and reused storage
is cleared. Larger requests or additional endpoints retain the aligned internal
heap fallback. The IQ ring remains in PSRAM. Host tests force heap allocation
failure and verify alignment, unique slots, exhaustion, reuse and cleanup.

ADS-B gain/status telemetry also read NVS from the console's TCM stack and
triggered a cache-disabled assertion. It now reads the runtime gain request.
The startup absence watchdog now recognizes HackRF as well as RTL, avoiding
a false "SDR not enumerated" report when HackRF is connected.

The descriptor-pool build passed its initial boot and two software restarts
with HackRF connected throughout. Each boot registered the endpoint, restored
1090 MHz and streamed at approximately 4 MB/s with no input drops. The full
host verification gate ended `VERIFY OK`. Aircraft decoding was not observed
in these tests, including after fitting an ADS-B antenna.

## Rates below 2 MSPS

FM and pagers ask for 256 kSPS and P25 for 240 kSPS, below the HackRF's 2 MSPS
floor. The radio runs at the smallest whole multiple of the asked rate (P25 at
240 kSPS x9, FM and pagers at 256 kSPS x8, broadcast FM x4) and a third-order
CIC filter brings it down. A decimated stream is tuned two output rates aside
and mixed back, so the radio's own carrier leak lands on a filter null instead
of the channel. A slow AGC keeps a weak carrier filling the 8-bit output.

POCSAG at 152.6 MHz decoded the same pages through the HackRF as through an
RTL-SDR in the same window. FM, pagers and ACARS stream with no ring overflows.
P25 streams at its full 240 kSPS, but the decimator adds about a quarter of a
core to P25's receive task, which then keeps its core about 90% busy; an IQ
ring overflow comes every few seconds. Each one is reported as a single gap and
the stream carries on.

Gain reaches 113 dB, split about evenly between the LNA and VGA stages. AUTO is
a fixed 48 dB. `hackrf ppm <n>` corrects the reference; 0 has measured right.

## Console

- `hackrf`: radio and output rates, decimation, gain stages, ring overflows,
  time per read, and levels either side of the decimator.
- `hackrf dump`: one second of decimated output, sent as checked base64.
- `hackrf snap`: the last four seconds the app was given, the same way.

## Current limits

- The receive-only driver advertises 2 to 20 MSPS. Only 2 MSPS has been
  measured at full rate on this board.
- P25 decoding, ACARS, FLEX, REC and SUB-GHZ capture have not been confirmed
  on live signals through the HackRF.
- P25 gaps every few seconds on the HackRF (above) until the decimator runs on
  the other core.
- ADS-B uses the radio picked in its RADIO list. With nothing picked it takes
  an RTL-SDR first, then the HackRF, then the LR2021.
- Gains saved for an RTL-SDR carry over and read low on the HackRF's scale.
- CELL survey uses the RTL-SDR only.
- No HackRF transmission was implemented or tested.
- Startup with one connected USB radio is the validation target. Hot swapping
  is not yet a verified feature.
