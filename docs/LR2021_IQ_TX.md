# LR2021 waveform playback

Upload a waveform to LakeShark and transmit a short burst through the LR2021. The signal preserves amplitude and phase, supporting the P25 test-tone, temperature-packet, and custom waveform examples from [LR2021-SDR](https://github.com/SAMS0N1TE/lr2021-sdr).

Available in LakeShark 2.13.0. It is accessed through the console and uses the LR2021 ROM 1.24 path at 915 MHz, configured -9 dBm.

## Try it

Stop competing radio sessions before sending. From the console:

```text
tui off
mesh stop
```

`tui off` asks the visible screen's receiver to park. That transition is deferred; confirm it completed before sending. For an FM LR2021 session, `fmlr` must report `active=0`. Do not send while `lora` reports that live IQ owns the radio.

Use the generator and uploader in an [LR2021-SDR checkout](https://github.com/SAMS0N1TE/lr2021-sdr). Run these commands from that checkout, replacing the serial number with the board's USB serial:

```sh
python tools/lr2021_make_p25.py research/tx-p25/inputs/voice-tone.p25 p25.iq8
python tools/lr2021_tx_upload.py p25.iq8 --serial YOUR_USB_SERIAL --send 19200 --repeats 2
```

The P25 generator needs NumPy; the uploader needs pyserial. Upload without `--send` to prepare the buffer without transmitting. Once ready, the console can reuse it:

```text
lora lrtx-upload status
lora lrtx-send 19200 2
lora lrtx-upload abort
```

`abort` releases the upload buffer. Uploads are volatile and do not change saved settings.

`lrtx-send` runs synchronously in the console and is limited to a burst of at most 900 ms. A UI receiver release, app park, or app switch requests interruption at the next FIFO or poll boundary and follows normal radio recovery. The same serial console cannot process another command while `lrtx-send` is running, so `lrtx-upload abort` is for releasing a completed upload, not interrupting an active send.

## Memory and performance design

The current P4 build uses ESP-IDF 5.5.4 and the existing `build_tdp4` configuration. Its Wi-Fi Remote environment uses `ESP_IDF_VERSION=5.5`.

- Allocate the upload buffer in PSRAM only when an upload begins; no permanent 16 KiB internal-RAM buffer.
- Reuse the uploaded bytes across repetitions without constructing a larger waveform.
- Keep allocation and diagnostic output outside the FIFO service loop.
- Use the existing owned 16 MHz SPI path, with at most 62 payload bytes plus the two-byte FIFO command per transfer. This fits the current 64-byte CPU transport.
- Keep the integration out of receive processing, waterfall rendering, and screen transitions.
- Hold exclusive radio access during playback and recover the previous radio configuration afterward.

## Waveform contract

| Parameter | Limit |
| --- | --- |
| Input | Signed interleaved I8/Q8, no header |
| Upload | 2–16,384 bytes, even length |
| Amplitude | `I*I + Q*Q <= 56*56` |
| Rates | 4,800 / 9,600 / 19,200 / 38,400 / 76,800 / 125,000 complex samples/s |
| Repetitions | 1–16 |
| Total | At most 32,767 complex samples and 900 ms |

Scale the whole waveform consistently. Normalizing every sample to the same radius removes the amplitude information. RX IQ16 recordings need conversion and resampling before use here.

## REC diagnostics

`rec iq info <path.sub>` queues raw IQ16 statistics: sample count, rate,
frequency, Euclidean magnitude peak/median, and fraction strictly above their
midpoint. It also runs `rec_lr_ook` (60 us glitch filter, 4 ms frame gap),
reporting high pulse count, minimum/maximum high widths, and the first 24 PWM
bits (400 us = 0, 1200 us = 1, +/-100 us tolerance; otherwise `?`). EOF is
flushed with silence; the padding does not enter the magnitude statistics.

`rec iq test 433.92 -9 [rate]` queues five MSB-first `0xA53C96`
repeats: 400/1200 us high in a 1600 us bit, with 10 ms between frames.
Default 19,200 samples/s, I=48/Q=0 on, zero off; absolute edges are rounded to
sample times (232 ms nominal). Test and file replay share all existing bounded
TX, radio ownership, policy, antenna, ROM, FIFO and recovery gates.

Supported rates are 4800, 9600, 19200, 38400, 76800 and 125000.
`rec iq test 433.92 -9 [rate] fsk` defaults to 125000 and sends five
4800-baud, +/-20 kHz continuous-phase FSK frames: 32 alternating bits starting
with 1, MSB-first `0xA53C96`, then `1010`. Each frame is 12.5 ms with a 10 ms
zero-I/Q gap between frames. Explicit low diagnostic rates are accepted but
alias tones outside their Nyquist bandwidth; use 125000 for this RF test.

Recording remains IQ16 at 125 kS/s. ARM measures mean noise power for 20 ms,
then requires three consecutive 1 ms windows at least 10 dB above that floor.
A 10 ms ring retains roughly 7 ms before the confirming rise. No rise within
30 seconds reports "no signal" without saving. A carrier already present
throughout calibration needs a subsequent rise to trigger.

File replay classifies the envelope and pulse widths, requiring on-segment
phase coherence of at least 0.90 even for clustered pulses. OOK is rebuilt
from sliced edges at the requested rate (default 19200), I=48/Q=0 on and zero
off. FSK/other signals retain I/Q, with carrier offset removed using adjacent
on-sample frequency, median on magnitude mapped to 48, and gaps zeroed.
Windowed FFTs measure the centered 99%-energy bandwidth. With two FFT bins
of guard inside the resampler passband, replay chooses 38400 or 76800;
broad or uncertain signals use 125000 without resampling. A requested rate
is a minimum for I/Q replay. Decimation uses a 129-tap Hann-windowed sinc
(cutoff 0.4 times output rate). Magnitudes are limited to 55 before rounding
to keep IQ8 within the driver's radius-56 gate. Status reports the selected
path and actual rate.
The slicer summary is the first status line, so legacy 112-byte readers
retain pulse count, widths and first bits; full console status also includes
raw magnitude statistics.

OOK replay was HackRF-verified with three complete exact `0xA53C96` frames.
The new FSK classification and bandwidth selection have host coverage;
RF verification of the updated FSK replay remains outstanding. The earlier
125 kS/s amplitude/edge-only observation was a carrier-centered HackRF
measurement artifact and is not a reason to avoid native-rate replay.

## Reference

[Upload protocol and examples](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/tx-playback.md) · [P25 RF evidence](https://github.com/SAMS0N1TE/lr2021-sdr/tree/main/research/tx-p25) · [Amplitude and phase payload tests](https://github.com/SAMS0N1TE/lr2021-sdr/tree/main/research/tx-payload)

