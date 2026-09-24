# P25 voice: how it works, what must stay true, and how to know

P25 voice broke repeatedly from changes that were not about P25, and every
time the decoder "still decoded": NIDs passed BCH, frames synced, and the
speaker went choppy anyway. This page is what stops that happening again.
Read it before touching anything under `components/lakeshark/apps/p25/`,
`components/lakeshark/audio/`, or the RTL/USB receive path.

Phase II is separate: [P25_PHASE2.md](P25_PHASE2.md).

## The path, end to end

```
RTL-SDR  --USB-->  p25_rx task (core 1)                 dsd_decode task (core 0)
                   ls_radio_iq_read, 16 KiB blocks       getFrameSync   sync hunt, 24 symbols
                   dsp_process_iq  (dsp_pipeline.c)      processFrame   NID, BCH(63,16)
                     LPF + decimate to 48 kHz              processHDU     ESS from the header
                     DC blocker, AGC                       processLDU1/2  9 IMBE frames each
                     FM discriminator, RRC, C4FM DC        process_IMBE   ESS gate: hold or mute
                   -> sample ring (DSD_SAMPLE_RING) ---->  processMbeFrame IMBE FEC, OP25 vocoder
                                                         p25_voice_hold  release / discard
                                                         audio_write_p25_voice  8 -> 16 kHz
                                                           -> audio ring, 1000 ms, PSRAM
                                                         audio_out task -> I2S -> ES8311
```

One LDU is 180 ms of air and nine 20 ms IMBE frames. An LDU1 carries the
talkgroup (link control), an LDU2 the encryption sync (ESS: algorithm, key,
message indicator). A transmission opens with an HDU (82 ms, which also
carries the ESS), alternates LDU1/LDU2, and closes with a TDU or TDULC.

## What must stay true

Each of these is a rule because its opposite shipped and was measured on the
air. The test that holds it is named; a change that breaks one fails the
bench.

1. **Clear voice is never thrown away while the decoder is unsure.** Voice
   decoded before the call's ESS is known is held (`p25_voice_hold.c`) and
   played once the call proves clear, or discarded if it proves encrypted,
   ends, or changes talkgroup. Encrypted voice never reaches the speaker.
   *Tests:* `test_p25_voice_hold`, `a_call_joined_without_its_hdu_still_plays_its_first_ldu1`,
   `an_encrypted_call_never_reaches_the_speaker`.

2. **An LDU2 whose NID passed BCH is decoded, with or without its LDU1.**
   Skipping it left its body unread and cost the next LDU1 too.
   *Test:* `a_lost_ldu1_costs_its_own_frames_and_nothing_more`.

3. **The sync hunt cuts its 24 symbols at their own midpoint and accepts up
   to 3 errors.** The NID's BCH check right after is what rejects a false
   sync. *Tests:* `a_carrier_that_arrives_off_frequency_is_heard_from_its_first_ldu`,
   `the_hdu_proves_the_call_clear_so_the_first_ldu1_plays_at_once`,
   `noise_has_no_valid_frames_...` in `test_p25_acquisition`.

4. **The C4FM symbol clock is eager while acquiring and patient while
   locked.** It moves at every zero crossing until two frames arrive back to
   back, then only once the crossing error adds up to 4 samples.
   *Test:* `voice_quality_under_noise_does_not_fall_below_its_measured_floor`.

5. **Nothing on the decode path allocates, and nothing new lives in internal
   RAM.** The hold, its output and the audio ring are PSRAM. P25 entry has
   been measured leaving under 3 KB of internal RAM.

6. **A sync is signal only once its NID has passed BCH.** The tolerant hunt
   passes about one false sync a second in noise, and `processFrame` rejects
   each one. The SYNC lamp, the NAC on screen, the sync count, the
   demodulator's lock flag, P25QUAL's call window and the scanner's "this
   channel is active" (`P25.dsd_has_sync`) are all set after the check, in
   `app_p25.c`. BCH alone still passes a noise frame every 20-25 minutes, so
   a valid frame must also be corroborated: its NAC matches the one already
   confirmed on this tune, or a second frame with the same NAC follows within
   500 ms (`p25_sync_confirm.c`). A retune forgets the NAC. `p25 acquisition`
   counts the uncorroborated frames. *Tests:* `test_p25_sync_confirm`. The loop
   itself is not compiled on the host bench, so a live soak still guards it:
   noise must not open P25QUAL calls.

7. **What is measured is what the listener hears.** Frames played, and the
   share of IMBE frames that reach the vocoder bit-exact. "It decodes" was
   true every time voice was choppy.

## What went wrong, and what it looked like

All on 154.7850 (conventional, C4FM, NAC 527), 2026-09-23 unless dated.

| Symptom | Cause | Measured | Fix |
|---|---|---|---|
| Choppy calls, every NID passing BCH | Unknown ESS muted all voice until an LDU2 proved the call clear. A call joined without a clean HDU lost its first LDU1 and LDU2. | 4905 of 5553 muted frames were "ESS unknown"; a recorded call played 9 of 144 frames | Hold instead of mute (rule 1) |
| One missed sync, 720 ms of silence | An orphaned LDU2 was ignored unread; the hunt ran through its body and missed the next LDU1, orphaning the next LDU2 | Four orphaned LDU2s in a row on one call | Decode it (rule 2) |
| The start of every call missing | The hunt sliced at zero while the discriminator still carried ~1100 of DC (transmitter settling after key-up) against ~2200 outer levels, and demanded an exact match | HDU found 0 of 3; first LDU1 lost on every call | Midpoint slicing, 3-error tolerance (rule 3) |
| Voice garbled even when complete | The symbol clock moved a whole sample at every crossing across a one-sample-wide eye | Clean call 77% bit-exact with no noise; 56% at 16 dB | Two-speed clock (rule 4): 99% and 91% |
| Dark bands on the waterfall, extra chop | A LoRa transmitter beside the board overloaded the tuner at 29.6 dB gain; each packet dropped the whole band ~8 dB | 0 dips at 20.7 dB gain, back at 29.6; gone with the transmitter off | Not firmware - see below |
| SYNC lamp and random NACs in noise; scanner stopping on empty channels | The tolerant sync (rule 3) passes false syncs in noise, and the app counted any sync as signal before the NID check | 97 P25QUAL "calls" in ten minutes, 59 of them noise with no valid NID | Signal only after BCH (rule 6) |
| Possible crash at the end of a call | `read_zeros` (TDU/TDULC) malloc'd and wrote unchecked on the decoder task | By inspection | Stack buffer |
| (2.2.2) 360 ms lost per damaged LDU2 | An LDU2 whose ESS failed Reed-Solomon dropped its nine frames and muted the next LDU1, even in a call already proven clear | A third of one call's voice | Keep a proven-clear call clear |

### Nearby transmitters

The RTL's tuner has almost no filtering ahead of its first amplifier. A
strong transmitter anywhere near its range - a LoRa node at 915 MHz, a pager
transmitter - compresses it while tuned to 154 MHz, and everything in the
window drops for the length of each burst. On the waterfall that is a dark
band across the whole width; in the log it is P25 frames lost in bursts of
0.3-2 s at irregular intervals. Fixes, cheapest first: move the transmitter a
metre or two away, run P25 gain near 20 dB (a strong channel decodes fine
there), or put a VHF band-pass/low-pass filter on the RTL input.

## The tools

### Recording a call on the board

```
python tools/p25_capture.py <name>        # waits for voice, records 8.7 s
```

It runs `p25 capture start`, then `p25 capture save <name>`, which writes
`<name>.iq` and `<name>.txt` to the card's capture folder (30-90 s for 4 MiB).
It then reads the capture over the console into `bench/captures/p25/`, which
takes about 35 minutes for a full capture. Taking the card out and copying
the two files is faster. Boards with a screen do not run the Wi-Fi file
server; headless boards do, and `--host <ip>` fetches over it.

`bench/captures/` is gitignored: it is radio traffic and never published.

### Replaying a capture through the real decoder

```
bench/build/p25_voice_replay <file.iq> <demod_gain> [out.wav] [--frames] [--symbols syms.txt]
```

`demod_gain` is in the `.txt` (`-9000` for the captures so far). It runs the
production DSP, sync hunt, frame processors, ESS gate, hold and the OP25
vocoder, and prints one `P25VOICE` line:

- `hdu ldu1 ldu2 tdu` - frames found; `ldu_missed` - voice slots with no
  frame; `ldu2_orphan` - LDU2s that arrived without their LDU1.
- `imbe_on_air imbe_played` - voice frames on the air, and reaching the
  speaker.
- `muted held released discarded` - the ESS gate and the hold.
- `hdr_fixed hdr_critical imbe_bit_fixes` - FEC work. This is the margin
  left: a receive-path change that raises these is a regression even when
  every frame still plays.

The WAV is laid out on the air's own timeline, so choppiness is audible.
`--frames` lists every frame with its ESS state and header errors, and
`--symbols` dumps every hunted symbol with its slicer thresholds.

### The bench gates

`pwsh -File bench/verify.ps1 -Level host` runs both of these.

- **`bench/p25_voice_check.py`** replays every capture in `bench/captures/p25/`
  and fails if any plays fewer frames than its `.expect` baseline. After a
  change that plays *more*, `--update` records the new baseline. `--wav <dir>`
  writes what each capture sounds like.
- **`test_p25_voice_call`** generates calls from nothing
  (`bench/fixtures/p25_call_gen.c`): an HDU, LDUs carrying known IMBE frames,
  and a TDU, encoded with the firmware's own FEC encoders and rendered as
  standard C4FM IQ. Impairments: noise, frequency offset, key-up drift,
  clock error, and gain bursts. With the vocoder replaced by a recorder, it
  checks which frames reach the speaker bit for bit.

  To add a case for a new failure, reproduce it with the channel model
  first: it has to fail on the old code and pass on the fix. The key-up case
  only failed the old sync code once the generator modelled the transmitter
  settling after PTT. A plain frequency offset did not reproduce it.

### On the board

- **`P25QUAL`** is one line per call. `bchOK/bchFAIL` are NIDs; `vox` is
  voice frames delivered; `under` is audio underruns (about 19 per call is
  the normal tail); `rel/disc/muted` are IMBE frames released from the
  hold, dropped from it, and muted as encrypted. A choppy call with `muted=0`
  and `disc=0` lost its voice before the decoder: sync, RF, or overload.
- **`p25enc`** gives session totals: muted, unknown-ESS, ESS Reed-Solomon
  failures, and the hold.
- **`p25 acquisition`** shows hunt statistics, the tune fence, and
  `ring_drops` (DSP to decoder).
- **`top` / `top irq`** show per-task CPU and interrupt load. **`heap`**
  shows internal/DMA/PSRAM headroom and task stacks.
- To bring the firmware's quiet defaults back after changing log levels,
  reset the board.

## Before you change P25 code

1. Run `bench/verify.ps1 -Level host`. The voice-call suite and the capture
   check must pass.
2. If the change touches the DSP, sync, timing or slicing, replay every
   capture before and after. `imbe_played` must not fall, and
   `hdr_critical` and `imbe_bit_fixes` must not rise.
3. Run `bench/build.ps1 t-display-p4`, flash app-only, and run
   `python tools/p25_soak.py 10 --open --save after.log` on a busy channel. Then
   `--summarise before.log after.log` against a run of the previous firmware
   taken *at the same time of day*. The air
   changes: 2.2.2 measured 4.35 gaps a second one morning and 6.05 that
   afternoon.
4. The operator listens. "Written, gated and flashed" and "it sounds right"
   are different claims.

## Known limits, and what to try next

- **The HDU's ESS still fails at key-up about half the time on the real
  channel.** The HDU sits in the first 82 ms, while the carrier's DC is
  still settling, and DSD's thresholds follow a 256-symbol average. The hold
  covers it (the first LDU1 plays 180 ms late), but a faster centre during
  the HDU would let it play at once. The demodulator's DC filter slows ten
  times at the first sync (`c4fm_dc_avg`), and that is where to look.
- **A carrier that opens directly on an LDU1 loses it about half the time.**
  Its sync is the first 24 symbols of carrier. Real transmissions open with
  an HDU, so this only matters when a call is joined mid-transmission.
- **The receive filter is an RRC, which is not the matched pair for C4FM's
  shaping filter.** Integrate-and-dump did better on the real capture
  (8 header words lost against 14) and worse on generated calls. Deciding
  needs more real captures than the one there is.
- **`FSK4_TRACKING`** (a profile option) had excellent steady-state timing
  before the two-speed clock existed: 99% / 95% bit-exact against C4FM's
  77% / 57% at the time. It loses frames at call starts on the real
  capture. Its tracker is worth studying, not switching to.
- **The first call on a fresh tune counts as signal one frame late.** Rule 6
  needs a second frame to corroborate a NAC nobody has confirmed yet, so the
  SYNC lamp and P25QUAL start about 180 ms after the first frame. Voice is not
  delayed: every valid frame is decoded either way.
- **Only one real call is in the corpus.** Every capture added makes every
  decision above better informed. Weak, fading and trunked-voice captures
  would be the most useful additions.
