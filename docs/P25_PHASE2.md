# P25 Phase II — experimental

Enable **P25 → SETTINGS → Phase II (experimental)**. It defaults OFF after
restart and is not saved. Turning it off restores the selected Phase I demodulator.
Changing the demodulation setting also turns Phase II off.

The first version receives one manually tuned traffic channel and slot at
6000 symbols/s. Scanning stops when enabled. Phase I control-channel decoding
can supply the most recently learned system IDs; the console can set them:

```
p2 config 92715 1f6 01a 1
p2 on
p2 status
p2 off
```

WACN, system and NAC are hexadecimal; slot is 1 or 2. Those example IDs belong
to the public test recording, not necessarily the local system. Open P25 and
tune the desired traffic frequency before enabling. The status shows recovered
voice/audio frame counts and worst decoder block time. Unknown or encrypted
calls stay muted; a receive gap or retune discards clear-call authorization.

Automatic Phase II traffic following and live RF reception are still unverified.
The existing Phase I AUTO/manual demodulation and scanner remain available with
this switch off. This is not a claim of complete Phase II scanner support.

## Public recording

[DSD-FME issue 304, maintainer's test capture](https://github.com/lwvmobile/dsd-fme/issues/304#issuecomment-3247474181)
links `p25p1andp2.zip`. The recording contains recovered dibits from mixed Phase I
and Phase II traffic; it is not raw IQ. The recording is fetched separately and
is not redistributed with the firmware.

After `pwsh -File bench/verify.ps1 -Level host`, run:

```
python bench/tools/check_p25p2_sample.py --cache <directory-outside-repo>
```

The sample SHA-256 is pinned in that script. OP25-derived decoding recovers
1,368 voice codewords; all match the corresponding codewords from DSD-FME
20260715, which recovers two additional frames. The current conservative audio
gate produces 1,214 synthesized frames and mutes 154. PCM is 8 kHz mono.
This validates the symbol-to-voice path; it does not establish live RF or speaker
quality. The host gate separately exercises 6000-baud synthetic IQ and verifies
that the normal Phase I clock remains the default.

The public capture also passed on the P4 through its USB console: 416,245
symbols, 1,368 voice frames, 1,214 synthesized frames and 218,880 PCM samples.
Decoder work took 6.91 seconds total; the worst 208-symbol block took 57.9 ms,
with 12,852 bytes of unused worker stack. The initial unoptimized run took
108.48 seconds. Capture transfer time is separate from decoder work.

The diagnostic decodes locally into PCM counters without transmitting, playing
audio or writing the SD card. Its queue is bounded to 8,192 symbols and it expires
after 30 seconds without input. Install pyserial on the host, then run:

```
python bench/tools/p25p2_device_replay.py --port COM13 --capture <cache>/p25p1andp2.bin --log device-replay.txt
```

Use the actual display port. The supplied script verifies the capture hash,
frame/sample counts, nonzero PCM and stack headroom. `p2 test status` exposes
the result. This validates device decoding, not RF acquisition or speaker output.
Sustained live reception and burst buffering still need a known Phase II call.
The regular OFF/ON/OFF transition and OFF after restart were also checked;
the idle UI frame interval remained about 42.7 ms.

Reference command (DSD-FME portable build):

```
dsd-fme -i p25p1andp2.bin -o null -ft -Z -w reference.wav
```

Upstream provenance and local modifications are recorded in
[the component README](../components/p25_phase2/README.md).
