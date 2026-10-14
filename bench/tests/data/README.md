# FSK replay regression capture

`fsk_rec.iq16le` is the user-provided LR2021 recording exported with `rec iq dump`:
131068 bytes (32767 complex samples), signed IQ16 little endian, 125000 samples/s,
433.92 MHz. Source: 4800 baud 2-FSK, +/-20 kHz, alternating preamble and A53C96.
The positive tone is approximately 9 dB stronger. Kept verbatim (128 KiB).

SHA-256: `630a956de5f1da5d922ca913da507d5b43efdb9ac8b85bcc22eb4be7bca583ab`

CRC-32: `F178791A`

`lriq_real_imbalanced_fsk` measures frequency modes independently of the converter,
requires deviation within 10% of 20 kHz and midpoint within 1 kHz of DC, checks
in-place conversion and capacity failure, and prints host conversion time.
The synthetic regression also swaps the stronger tone and varies symbol occupancy
at both 12 and 20 kHz deviation to cover decimation and native-rate output.

Baseline at fedc0deb on this host (MinGW, -O1): input tones -19844.3/+20080.6 Hz;
converted tones -21485.0/+18417.0 Hz, deviation 19951.0 Hz, midpoint -1534.0 Hz,
76800 samples/s, conversion 7.256 ms. The reported HackRF deviation of 10.8 kHz
was not reproduced by this host baseline. P4 timing and RF output need hardware
validation; host timing does not establish the P4 latency.
