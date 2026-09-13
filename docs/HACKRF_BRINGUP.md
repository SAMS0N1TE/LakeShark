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

## Current limits

- The receive-only driver advertises 2–20 MSPS. Only 2 MSPS has been measured
  on this board; the advertised upper bound is not a sustained-throughput claim.
- ADS-B requests 2 MSPS. FM/POCSAG requests 256 kSPS and P25 requests 240 kSPS,
  so they need filtered rate conversion before using this driver.
- No HackRF transmission was implemented or tested.
- Startup with one connected USB radio is the validation target. Hot swapping
  is not yet a verified feature.

## Earlier private research

[LS_Test1's HackRF task](https://github.com/SAMS0N1TE/LS_Test1/blob/HEAD/bench/done/340-hackrf-endpoint.md)
explicitly separated writing/compiling the driver from hardware bring-up.
[TT_Flipper](https://github.com/SAMS0N1TE/TT_Flipper) contains `iqdecode.py`,
`rxframes.py` and `HANDOFF.md`: earlier PC-side HackRF capture and FSK analysis
at 2 MSPS. Those scripts are useful measurement references, rather than proof
of ESP32 USB host operation.
