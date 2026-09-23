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

Live RF reception is still unverified, though the receive front end is now
tested from IQ against real Phase II traffic across +-1.6 kHz of carrier
offset (below). The existing Phase I AUTO/manual demodulation and scanner
remain available with this switch off. This is not a claim of complete Phase
II scanner support.

## Automatic following (experimental)

Enable **P25 → SETTINGS → Phase II follow (exp)**, or `p2 follow on` on the
console. Like the manual switch, it is OFF after a restart and not saved.
With it on, a Phase I control channel's grant onto a two-slot TDMA carrier
(IDEN_UP_TDMA channel types 3 and 5) is followed like a Phase I grant: the
same talkgroup filter, hold, lockout and encrypted-skip policy decide whether
to take it, the radio retunes to the carrier, the decoder is configured for
the granted slot with the system's WACN, SYSID and control-channel NAC, and
the receiver returns to the control channel when the call ends. Four-slot
type 4 stays unsupported. A grant is not followed until NET_STS_BCST has
given the WACN and SYSID, because the slot's scrambling is keyed on them.
With the switch off, a TDMA grant is observed and counted, and never tuned.

A call's end is read from the slot's own MAC PDUs, not from voice or sync: a
two-slot carrier stays up while the other slot talks. On the public
recording every transmission carries MAC_ACTIVE (opcode 4) every 360 ms and
closes with two MAC_END_PTT (2); the idle slot sends only MAC_IDLE (3). The
policy (`p25_p2_follow.c`, host tested with a fake clock) leaves on:

| Reason | When |
|---|---|
| `ended` | 500 ms after END_PTT with no HANGTIME or new PTT |
| `idle` | MAC_IDLE on the granted slot |
| `hang` | 2 s after the first HANGTIME, if no one keys |
| `no-sync` | no sync 1.5 s after the grant, retune included |
| `lost` | sync gone for 1 s |
| `quiet` | synchronised, no PTT/ACTIVE/voice for 2 s (3.5 s before the first) |
| `encrypted` | the PTT or ESS names an algorithm other than clear; stamps the talkgroup's encrypted skip, as Phase I does |
| `other-tg` | the slot's PTT names another talkgroup |

Voice counts as activity only once a MAC PDU on the slot has passed its CRC,
because a voice burst is recognised before it is descrambled: configured
with the wrong system, the slot "has voice" and nothing else.

`test_p25_p2_follow` follows the recording's six calls the way the firmware
does - a grant, a fresh decoder, a tick every 10 ms of air - and requires all
1,368 voice frames to fall inside a followed window and every call to be
left as `ended`, 560-690 ms after its last voice. `p2 status` prints the
follower's phase and a count of every way a call has ended.

While a Phase II call is followed, the Phase I decoder is idle, so no
control-channel grants are heard: a higher-priority grant cannot preempt it,
exactly as on a Phase I traffic channel. Only the granted slot is decoded.

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

The same script then checks two things the symbol replay cannot:

- **Reversed polarity.** The capture with every dibit's polarity bit flipped,
  as a receiver of the other sign convention delivers it, must decode to the
  same 1,368 voice frames and byte-identical PCM. OP25's framer recognised
  the reversed sync, but the adapter ignored it, so an inverted stream framed
  its bursts and decoded no voice at all. It now inverts the stream from that
  burst on, as OP25's `rx_sync` does (`p2 status` counts `polarity_flips`).
- **From IQ.** `bench/tests/test_p25_phase2_iq` takes 20 s of the capture,
  decodes its dibits directly as the reference, then modulates the same
  dibits as 6000-baud pi/4-DQPSK IQ. It runs them through `dsp_pipeline` in
  Phase II mode, `p25_p2_slice` and the decoder. It asks for the same voice
  under carrier offset, noise and multipath echo, and for sample-identical
  PCM on a clean channel. This is the half of Phase II that had never seen a
  signal.

That second test found the front end's real limit. The differential
detector's AFC corrects at most pi/8 a symbol, 375 Hz at 6000 baud. So the
capture decoded whole at +-300 Hz and not one voice frame at +-400, while an
RTL-SDR's 1 ppm alone is 850 Hz at the 700/800 MHz where Phase II lives. A
frequency-locked loop now steers the pre-demodulator NCO from the signal's
mean instantaneous frequency (pi/4-DQPSK averages to zero, so what is left is
the carrier offset). It has a 150 Hz deadband, so the existing AFC still owns
small offsets and Phase I CQPSK behaves as before. The capture now decodes
fully from -1600 to +1600 Hz.

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
