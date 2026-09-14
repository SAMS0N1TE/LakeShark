# Recorded LTE development fixtures

These short receive-only recordings preserve the local development evidence for
the next PC. They are ordinary cellular downlink IQ at 739 MHz, not labeled
simulator captures. Do not use them to claim detector sensitivity or accuracy.
`recordings.json` records the format, rate, length, checksums and expected results.
CU8 is interleaved I/Q, unsigned offset binary; signed IQ is converted with XOR 128.

Run from the firmware checkout with Python, NumPy and host GCC on PATH:

```sh
python integrations/cell-recordings/replay.py
```

The script temporarily compiles this checkout's original C resampler, LTE sync
and MIB decoder, checks fixture hashes and asserts the recorded results. It uses
the same default synchronization search bound as the P4. No SDR is accessed and
no signal is transmitted. The three recordings total 2,680,000 bytes.

- `p4-739-8ms-30ms.cu8`: earlier P4/HackRF recording; PCI 244, 50 RB (10 MHz),
  two ports, SFN 280 and three CRC-valid MIB occasions.
- `pc-739-10ms-80ms.cu8`: separate PC/HackRF recording; PCI 244, 50 RB, two ports,
  SFN 241 and eight CRC-valid MIB occasions. This is the full-bandwidth fixture
  for further control/PDSCH work. The independent external receiver previously
  decoded SIB1 PLMNs 310-410 and 313-100, TAC 1792, ECI 23424527, unbarred.
  The on-P4 implementation does not yet decode those SIB1 fields.
- `p4-739-10ms-30ms-gaps.cu8`: negative timing fixture, CRC32 `7965cb72`.
  Acquisition reported complete, zero host-ring drops and 31,215 us for 30 ms,
  yet independent PSS correlation finds timing discontinuities. Strong PCI 244
  PSS peaks at 1.92 MS/s are 8449, 18049, 27545, 37145, 46603 and 56041;
  their gaps are 9600, 9496, 9600, 9458 and 9438 instead of a stable 9600.
  The default P4 synchronization search rejects it. An experimental full-window
  search can recover the PCI from surviving segments but still fails MIB.
  Recovering a PCI does not certify continuous samples.

The negative recording predates the direct USB completion repost change. A new
80 ms capture with that change also failed synchronization; the change is not
a demonstrated fix for sample loss. Its CRC32 is `bd263477`; that later raw
capture was not downloaded into this fixture set. Check the device SD for its
saved copy; this final test did not read it back.

An independent 20 MHz public fixture is available in LTE-Cell-Scanner commit
`e7f71cbd4fa9f5ee5b97a58b2fb236ac13e4b8b1`, file
`regression_test_signal_file/f1815.3_s19.2_bw20_0.08s_hackrf-1.bin`.
Its SHA256 is `53e45ad837c8bc5a8c5d26554e86c7340be2b9fff73a01d42c474c62552ae13c`.
It is signed IQ at 19.2 MS/s and yields PCI 301, 100 RB and two ports. Small
PBCH/CRS excerpts with provenance are already in `bench/tests/lte_recorded_mib.h`.
