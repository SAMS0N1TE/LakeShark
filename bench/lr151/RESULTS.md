# LR misses the 151.575 pager: step 1, I/Q captures (2026-10-27)

Here is what was set up on this PC:
- The LR T-Display (5B91081095, COM14) with its external antenna (MMCX1) fitted.
- An RTL-SDR (R820T) as the reference, 28 dB fixed gain, centred on 152.0875 MHz at 2.048 MS/s, keeping both channels.
- A HackRF One is attached but wasn't used.

| Script | What it does |
| --- | --- |
| `capture.py` | Runs the RTL continuously while the LR takes untriggered `lora lrprobe <kHz> <bw> 14 -1 auto 250 hex` snapshots: 24,576 IQ16 pairs, 196 ms at 125 kS/s. One snapshot takes about 60 s to hex-dump over the 115200 console. |
| `analyse.py` | LR spectra, plus RTL channel power and spectrograms, matched in time. |
| `tones.py` | Lists LR tones as RF frequencies for each tune, to tell on-air signals from receiver-made ones. |
| `compare.py` | Draws two runs (antenna on and off) together in raw dB on an RF axis. |

The data (~400 MB across both runs) is in the session scratchpad. Only the plots `antenna_on_20261027.png` and `antenna_on_off_20261027.png` are kept here.

## Run `ant`: 12 min, antenna on, LR AGC (step stayed at 13)

There were 13 LR snapshots: 151.575 ×5, 152.600 ×4, and 151.545 at 250 kS/s ×4. The last tune puts 151.575 at +30 kHz, which tests for a spur that's fixed in RF frequency.

- **There's no spur in the 151.575 channel.** Within ±6.25 kHz nothing rises 8 dB over the local floor.
  - DC is −2 to +2 dB relative to the floor, so there's no DC spike.
  - The channel is the receiver's noise hump (+4–6 dB over the median of the whole band).
  - The same holds with the LO 30 kHz below.
- **The tones the LR does show come from the receiver.** They don't keep their RF frequency when the LO moves:
  - +12.6 kHz (+13 dB) and −21.5 kHz (+11 dB) at the 151.575 tune.
  - −13.4 kHz (+17 dB) at the 151.545 tune.
  - +15.656 kHz (+30 dB, about 125 k / 8) at the 152.6 tune.

  All of them are outside the 12.5 k channel. Only +8.6 kHz (+10–14 dB, in single snapshots) falls inside the 11 kHz fm_lr filter, and it's about 30 dB below a real pager.
- **152.600 is on air almost all the time here.** It carries 4-level FSK plus 2-FSK idle at ±4.5 kHz. The LR's snapshots show +28–30 dB in-channel SNR, with tones at +44–50 dB, so the LR's receive path works at 152.6.
- **The RTL heard nothing at all on 151.575 for 13 min** (610 s of samples, peak +1.5 dB over the floor). At this location and time the 151.575 pager isn't on air, so its SNR on the LR can't be measured here.

## Run `noant`: 7 min, LR antenna unplugged, otherwise the same

There were 7 snapshots (151.575 ×3, 152.600 ×2, 151.545 ×2). `compare.py` draws both runs in raw dB on an RF axis, in `antenna_on_off_20261027.png`. The gain step was 13 in both runs, so the levels compare directly.

- **The antenna noise is 15 dB above the board's own noise.** The floor fell 15 dB with the antenna off (at 151.575, median 69.2 → 54.3 dB; rms −50.7 → −66.3 dBFS), so the LR's own noise doesn't limit it when the antenna is fitted.
- **The board makes a 16.0 kHz comb, fixed in RF frequency.** The lines keep their RF frequency at every tune:
  - …151.553, **151.569**, 151.601, 151.617, 151.633 MHz
  - 152.577, 152.593, 152.625, 152.641, 152.657 MHz

  The line nearest the pager is 151.569, 6.0 kHz below 151.575. That's inside the fm_lr 11 kHz filter, at the channel's edge. It's +12–17 dB over the antenna-off floor, about 71 dB raw, which is about 3 dB *below* the antenna-on noise in the channel (74 dB). That's why the antenna-on run didn't show it. 152.6 has comb lines at a similar distance (−7 kHz at 152.593, +9 kHz at 152.609), and the LR decodes 152.6.
- **The other tones stay with the antenna off, and all sit outside the channel.** They are +12.7 kHz at the 151.575 tune, −13.4 kHz at 151.545, and +15.6 kHz at 152.6. I first read them as receiver-made because they follow the LO. The lr2021-sdr run below swept the tune in 5 kHz steps and corrected one of them: +12.7 kHz at the 151.575 tune is a fixed RF line at 151.5876 MHz.

## Same board on the bare lr2021-sdr firmware (no LakeShark), antenna off

The firmware is lr2021-sdr `main` aaf5ae1, and the harness is `bench/lr2021sdr/fwtest.py`. It took six 100 ms captures at tunes from 151.560 to 151.590 MHz.

- **The 16 kHz comb is gone.** There's nothing at 151.569, 151.585 or 151.601 MHz. So LakeShark makes the comb, not the LR2021 or the board. The likely source is something LakeShark clocks at 16 kHz, such as audio or I2S. That's still unconfirmed.
- **Four lines stay, fixed in RF:** 151.5206, 151.5535, 151.5876 and 151.6306 MHz, at +9 to +16 dB. They aren't from LakeShark, and none is within ±6.25 kHz of 151.575.

## What this changes

The leading hypothesis was an in-channel spur on the LR that the FM demodulator locks onto, and these captures don't support it.
- The only board spur near the channel is the comb line at 151.569. With the antenna fitted it sits under the noise.
- On air, the 152.6 pager peaks about 35–40 dB above that noise.
- A 151.575 pager anywhere near as strong would bury that comb line. The comb could only matter for a page within about 10 dB of the noise.
- Nothing about the spurs separates 151.575 from 152.6.

The comb comes from something on the board repeating at 16 kHz. With the HackRF held near the board, it could be traced to a part.

The 151.575 pager needs to be on air before a capture can say anything more. That means the earlier bench location, or a time when the RTL hears it. Then run:

```
python -I bench/lr151/capture.py <outdir> 15 151575 152600
```

and check `analyse.py`'s "RTL same time" column for LR snapshots taken during pages.

## The comb was the audio clocks, and it is gone when they stop

`bench/lr151/comb.py` reads the level of every line on the 16 kHz grid (151.553 + n × 0.016 MHz) against a running-median floor, and the same measure halfway between lines. It was run on `capture.py` runs of 4 minutes, LR antenna off, at 151.575 and 152.6 MHz.

| Firmware | 151.575: comb / between | 152.6: comb / between |
| --- | --- | --- |
| LakeShark as it was (flash backup of 2026-10-27) | +11.3 / +1.6 dB | +10.8 / +1.6 dB |
| I2S channels disabled when idle | +5.4 / +1.4 dB | +6.8 / +1.5 dB |
| ...and the I2S0 module clocks gated (MCLK stops too) | +1.9 / +1.6 dB | +2.2 / +2.2 dB |

- The audio player wrote silence for ever, so the ES8311's clocks never stopped: LRCK 16 kHz, BCLK 512 kHz, MCLK 4.096 MHz.
- Disabling the channels removed three lines in four. The lines left were every fourth, a 64 kHz grid (151.553, 151.617, 152.577, 152.641 MHz at +10 to +17 dB), which is MCLK/64. MCLK keeps running from the I2S module clock until that is gated.
- With both, every comb line sits at the floor, and the total noise at the 152.6 tune fell (rms 22 → 14.8).
- After two seconds of silence the player now parks the clocks (`ls_audio_hw_park`). Any write, read, rate change or teardown restarts them.
- Checked on the board: `snd test noise` after a park came back through the microphone at peak 14932, rms 2368, against peak 4-10, rms 1 when quiet.

## Parking without a snap

The first park order snapped on every P25 transmission: mute the DAC, XL9535 IO6 off, stop the clocks; on resume, start the clocks, settle 150 ms, IO6 on, unmute.

**IO6 is not just the amplifier.** LilyGO's driver (`lilygo_device_driver`, `t_display_p4` `SetEs8311OperatingMode`) says it switches the OUT_5V audio rail. That rail feeds the NS4150 amplifier and the RT9080 that makes the ES8311's analog 3.3 V. Their sleep order powers the codec's output stage down by register (0x13 = 0x40, 0x12 = 0x02, then 0x0D) before it drops IO6.

**The webcam microphone was not good enough.** It measured "leave IO6 on across the park" at the room floor, but the user still heard a snap on every transmission. Turning parking off at runtime (`audio idlepark off`) stopped it, so the park was the cause.

**The SX board's microphone is.** It sits beside the LR board's speaker, and the console `mic <ms>` command reports a peak. `audio silence` queues 1 s of zeros, which wakes the clocks with nothing to hear; they park again about 3.7 s after the command. Timing the mic window around one edge at a time separates the edges. Rest is a peak of 12-31, depending on the room.

| What happens in the window | Mic peak |
| --- | --- |
| Rest, no park | 12-31 |
| Resume after a long park, IO6 left on | 44-137 |
| Park, IO6 left on (clocks stop under a muted DAC) | 13-17 |
| IO6 off right as the clocks stop | 45-64, every take |
| IO6 off 0.5 s after the clocks stop | 21-41 |
| IO6 off 1, 2 or 4 s after | 17-31 |
| Resume 0.3-0.5 s after a park, IO6 still on | 21-36 |
| Resume 1.5 s after a park, IO6 still on | 44-74 |
| Resume with IO6 off, then IO6 on 150 ms later | 10-21 |

- The codec's output steps when its clocks restart, muted or not, once they have been stopped for more than about a second.
- Dropping IO6 while the codec is still settling from the clock stop snaps too.

**The firmware order now:**
1. Park: mute the DAC and stop the clocks.
2. 600 ms later, IO6 goes off. The player's idle loop calls `ls_audio_hw_park` again while parked, and that call checks the time.
3. Resume: start the clocks and wait 150 ms. If IO6 was off, turn it on and wait 20 ms. Then unmute.

A resume inside the 600 ms keeps IO6 up.

**Measured on that build** (2026-10-27, LR volume 57):

| Edge | Mic peak |
| --- | --- |
| Park | 17-26 |
| Resume 0.3 s after a park | 21-27 |
| Resume 5 s after (8 takes) | 10-12 |
| Resume 30 s after | 11-14 |
| Rest | 12-17 |

- A sweep after a long park reached the SX mic at the same level as with parking off (32 against 28-30), so the codec comes back from the rail cycle playing.
- The webcam numbers in the earlier commit are not to be trusted for this.
- While parked for more than 600 ms, the amplifier and the codec's analog rail are off. So that state also leaves the amplifier out of the LR's receive band.
- `audio idlepark off` stays on the console to rule the park in or out.

## 2026-10-28: a slight snap a couple of seconds after P25 calls

By ear, no microphone (the SX board was not attached), volume 29-50:

- At AMP_OFF_AFTER_MS 600 there was a snap a couple of seconds after each P25
  call; `audio idlepark off` removed it, so it was the park.
- AMP_OFF_AFTER_MS 2000, with an unpark later than 1 s into a park turning
  IO6 off before the clocks (ff1b278d): a slight snap remains, about 3 s
  after the voice by ear, so the IO6 drop rather than the clock stop.
- Powering the ES8311's output stage down by register before IO6 (0x13 =
  0x40, 0x12 = 0x02, 0x0D = 0xFA, read first and written back before the
  rail returned) made it much worse: snaps before and after every sound.
  The writes happened 2 s after the clocks stopped; LilyGO's order probably
  assumes a clocked codec. Reverted, not committed.

Next: measure the clock-stop and IO6 edges separately with the SX board's
`mic` as above before changing the order again.
