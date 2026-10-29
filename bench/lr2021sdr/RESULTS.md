# lr2021-sdr firmware on the LR T-Display: bench results

Board: LilyGO T-Display-P4 LR2021 (USB serial 5B91081095, CH343 on COM14), LR2021 ROM 1.24. The antenna was off for these runs.

LakeShark's full 16 MB flash was saved first to a local file kept out of the repo, read at 460800 baud (the 921600 read failed at 38%). Restore it with:

```
python -m esptool --chip esp32p4 --port COM14 -b 460800 write-flash 0 <that backup>.bin
```

## Baseline: stock firmware, lr2021-sdr `main` aaf5ae1 (2026-10-27)

This was built with ESP-IDF 5.5.4 from a clean `git archive`; nothing changed in the repo.

| Test | Result |
| --- | --- |
| Boot | `LR2021 ROM 1.24 frequency=154785000 bandwidth=31250 configured=1 errors=0000` |
| Continuity: `check 3000` ×10 (1 KiB FIFO, checked stream) | 10/10 PASS, flags 0x00, 1,500,000 ±60 bytes per run (125,000 pairs/s by the host clock), FIFO peak 124–128 B, worst service gap 246–249 µs |
| Orientation, from fixed RF lines while retuning 151.560–151.590 MHz in 5 kHz steps | Each line moves by exactly −Δtune: I + jQ as stored, so RF above the tune is a positive frequency |
| Sample rate, from the 151.5535 MHz line across 30 kHz of retuning | 125,004 pairs/s (nominal 125,000; one FFT bin is ±30 Hz) |
| Fixed RF lines from the board, with no LakeShark running | 151.5206, 151.5535, 151.5876 and 151.6306 MHz, at +9 to +16 dB over the local floor |
| 16 kHz comb seen under LakeShark | Absent: LakeShark makes it |

The stock firmware does only finite exports (`capture` up to 100 ms, `dump` as hex at 115200). It can't stream continuously to a host, and it has one rate (125 kS/s). Streaming and the SoapySDR module were written on lr2021-sdr's `feature/host-sdr` branch. This file collects the board results for those builds.

## Bridge firmware (lr2021-sdr `feature/fw-bridge`, on `feature/host-sdr` 9e15fc6), 2026-10-27

The firmware is the serial and TCP bridge, with the radio worker on core 1, SPI at 26.67 MHz, and an 8 MiB PSRAM output spool. The host side is the bench clients here (`bridge_test.py`, `tx_test.py`) plus `soapy_bench.cpp`, which runs in WSL against the other session's SoapySDR module.

### Receive

| Path | Run | Result |
| --- | --- | --- |
| CH343 serial at 4 Mbaud, CS12 | 125k for 60 s | Radio side lossless (0 dropped, flags 0x00); host got 99.8% (25 CRC-bad frames) |
| CH343 serial at 4 Mbaud, CS16 | 125k for 30 s | 77% delivered; the link carries 388 kB/s of the 511 kB/s needed, and dropped blocks are reported to the host |
| Wi-Fi TCP (ESP32-C6), CS16 | 250k for 10 min | 150,003,050 pairs, 100% delivered, no gaps, worst FIFO service gap 720 us |
| SoapySDR over TCP | 125k and 250k, 60 s each | 0 overflows, timestamps exact (step error 0 ns) |
| SoapySDR live gain step | During a 250k stream | No overflow; step 13 to 3 moves the level from -41.7 to -79.5 dBFS |
| SoapySDR retune | During a stream | One OVERFLOW, then data |

### Transmit

All bursts were at -9 dBm. The receivers were a HackRF One with a 433 antenna and an RTL-SDR, and the LR board had a 433 antenna on MMCX1.

| Run | Result |
| --- | --- |
| Bridge client, 915.0 MHz, 125k, 8000 and 32767 samples | error 0, flags 0x07, irq 0x0008000A; RTL sees 915.010 MHz at +52 dB for the burst length |
| Bridge client, 433.92 MHz, 125k, 16000 samples | HackRF sees 433.930 MHz at +44.6 dB for 129.0 ms (128 nominal) |
| SoapySDR writeStream, CF32, END_BURST, 433.92 MHz | Status 0; HackRF burst 129.0 ms at +44.7 dB |
| SoapySDR timed burst (HAS_TIME, +2.000 s) | Status time +11 us from the requested time; HackRF burst 129 ms |
| SoapySDR RX at 250k, TX burst mid-stream on the same device | One RX overflow, RX data again 0.40 s after the burst |

### Bugs found on hardware and fixed

| Bug | Fix |
| --- | --- |
| Task watchdog at 5 s: the busy-polling radio loop starved IDLE0, and the watchdog's log text corrupted the stream and overflowed the FIFO | The radio worker runs on core 1, and IDLE1 is not watched |
| PSRAM slot queue: copies into it stretched the FIFO service gap to 761 us and overflowed it at 250k | Slots stay internal; the PSRAM spool sits after the sender |
| Live gain change: SetRxPath during RX ended the session | SetRxPath is sent only when the boost changes |
| RF switch default had no TX LF state | DIO8 0x06, DIO6/DIO10 0x08 |
| Antenna switch fixed on MMCX1 | `antenna internal|external` command |
| TX FIFO writes over 62 bytes per SPI frame lose data: no TX_DONE, and the burst ends in an underflow | TX_WRITE 62 |

## Scripts added at the end of the 2026-10-27 session

- `headroom_run.sh <tag> [ip]`: 60 s of 500k CS16 and 60 s of 1M IQ8 over Wi-Fi; prints RX_END (FIFO peak, worst gap, flags).
- `txchirp.py <ip> <hz> <rate> <pairs> [sign]`: one TX burst of a -20..+20 kHz chirp (sign -1 reverses it).
- `rtlrec.py <out.npy> <centre_hz> <seconds> [gain]`: gap-free RTL-SDR recording through the async reader. The sync `read_samples` loop drops samples between reads, which looks like time steps in a chirp.
- `chirp_step.py <capture.npy> <offset_hz> <rate:pairs:sign> ...`: de-chirps each burst, then reports its length and the largest frequency step (a skipped or repeated stretch of samples).
- `bridge_console.py <cmd> ...`: run bridge-firmware console commands over the CH343 (board profile, `board rfsw`, `reboot`).

The TX integrity check of feature/fw-bridge 09f4f06 and 7272d49 was `rtlrec.py` at 914.9 MHz, then `txchirp.py` at 125k, 500k and 1M, both signs, then `chirp_step.py`. All bursts were full length and showed no steps above about 70 samples.
