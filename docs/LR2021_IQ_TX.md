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

## Reference

[Upload protocol and examples](https://github.com/SAMS0N1TE/lr2021-sdr/blob/main/docs/tx-playback.md) · [P25 RF evidence](https://github.com/SAMS0N1TE/lr2021-sdr/tree/main/research/tx-p25) · [Amplitude and phase payload tests](https://github.com/SAMS0N1TE/lr2021-sdr/tree/main/research/tx-payload)

