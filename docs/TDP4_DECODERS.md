# Decoders and radio control, T-Display-P4

What was added, what it is measured against, and what is not proven. Numbers
here come from the bench or the board; where something is untested it says so.

## Sub-GHz: what a capture is, not that one arrived

The screen used to print `OOK24 A3B4C5` - a payload from the one decoder the
board had, which reads 24 bits and nothing else. Every other frame length in
the same family came back as an unidentified capture.

Two decoders now read the signed edge array the recorder already keeps
(microseconds, mark positive and space negative - the Flipper `.sub` RAW
convention). Both are integer work with no radio dependency, so they run on
the host against recorded edges.

**`subghz_pwm`** - pulse-width OOK, where each bit is its own mark-and-space
pair. EV1527, PT2262, HT12E and the remotes, door contacts and PIR sensors
built on them. Reads any length from 12 to 64 bits, the unit, the repeat count
and the preamble length - which is what separates EV1527's 31 units from the
23 that PT2262 modules commonly use. At 24 bits it splits the payload into the
20-bit address and the 4-bit button.

**`subghz_nrz`** - run-length OOK, where the carrier is held on for a run of
identical bits. A mark of four units is four ones. Plain NRZ at a fixed bit
period with a preamble in front of it. Estimates the period, turns runs into
bits, finds the repeated frame and strips the preamble.

Nothing else is named. A bit count is not a device, and a wrong device name is
worse than a payload the operator reads themselves.

### Measured

Against 41 RAW captures taken from a Flipper's own sub-GHz catalogue - real
third-party hardware neither decoder had seen:

    15 of 41 decoded

The decodes are checkable rather than merely plausible:

    Gas_Sign Left    EV1527 id 1F710 btn 12
    Gas_Sign Right   EV1527 id 1F710 btn 15
    Gas_Sign Up      EV1527 id 1F710 btn 3
    Gas_Sign Back    EV1527 id 1F713 btn 12
    Gas_Sign S       EV1527 id 1F713 btn 3

Two device addresses, a distinct button per function. Two Handicap door
remotes decode to the same NRZ payload, which is correct - one is a restack of
the other.

The 26 that do not decode are mostly rolling-code (LiftMaster Security+),
brute-force sweeps rather than single captures, and encodings outside these
two families. That is the honest ceiling for a pulse-width and a run-length
reader; it is not a defect list.

Against a local set of 101 files the number is 85, but that set is a poor
yardstick: 96 unique names, and it includes a folder of generated files and a
folder named `not_working`.

### bench/tools/subdecode

    subdecode <file.sub> [more.sub ...]

Built with the host bench (`bench/tools/subdecode.c`, the `subdecode` target
in `bench/CMakeLists.txt`), so it lands in `bench/build/` after
`bench/verify.ps1`. Runs both decoders over Flipper RAW captures and prints
what each one is. This is how the local set was found to decode 0 of 101
before the NRZ reader existed, and how the 15
of 41 was confirmed. A decoder that has only ever been run against fixtures
written by the same hand that wrote the decoder has not been tested.

## DMR

DMR is 4FSK at 4800 symbols per second, the same rate as C4FM P25, so every
DMR burst on a channel already passes through the P25 receiver and is
discarded. `dmr_watch` reads it on the way past. It never steers the radio
and touches no P25 state.

### Where it gets its symbols

It used to be offered each symbol the P25 frame-sync hunt sliced, through an
observer in `getFrameSync()`. That only ran while P25 was hunting, used a
symbol clock tuned for P25, and could not be tested from IQ, because driving
`getSymbol()` needs the `dsd_opts`/`dsd_state` the frame-sync loop owns.

Now the P25 receive loop hands `dmr_watch_samples()` the C4FM discriminator
output - 48 kHz, after the RRC matched filter, exactly what goes into the
P25 sample ring - and the watcher recovers its own timing (`dmr_symsync.c`):

- a Gardner detector on linearly interpolated strobes, which needs no
  decisions and so works before the levels are known, with a period term held
  within 0.5% of 10 samples a symbol;
- the +3 and -3 levels tracked from outer-symbol decisions, the DC between
  them, so a carrier offset moves both and is followed. A side of the eye
  that has stopped seeing outer symbols - what noise before a mobile keys up
  leaves behind - snaps to the extreme of the last 16 symbols.

A burst is not found by the bit framer on this path. Every symbol, the last
24 strobe values in the sync slot are correlated against the base station and
mobile sync shapes, normalised so neither levels nor DC matter (the sync is
twelve +3 and twelve -3). A local peak above 0.6 is a candidate: the watcher
lays a straight symbol grid through the sync, tries each half-sample phase
within half a symbol, keeps the one that opens the eye widest across the
sync, takes the levels from it and re-slices the whole 288-bit window. The
candidate is a burst only if the re-sliced sync then matches within 5 of 48
bits - the framer's own test. The grid is straight because a loop still
pulling in from noise wanders by most of a symbol across the 54 symbols ahead
of the sync, where 100 ppm of clock error moves a straight line by a
seventieth of one. What the sync measured is fed back: all of the levels,
half of the phase.

The observer seam in the P25 sources is still there; `lakeshark_backend.c`
now sets it to NULL so the watcher is never fed from two tasks.
`dmr_watch_symbol()` still works for anything that wants to hand it sliced
symbols (the symbol-level tests do).

### Polarity

DMR's voice and data syncs are each other's negation: BS voice through an
inverted discriminator is BS data, symbol for symbol, and the same for MS. So
a sync cannot say which way round the receiver is. Both readings are framed,
counted once, and the first checked block - an LC, CSBK or data header CRC,
or an embedded LC - fixes the polarity. After that the other reading only gets
a quiet look for a checked block of its own.

The second framer this replaced complemented both bits of each dibit, which
turns +3 into -1. No receiver does that, and a burst that really arrived
upside down could never have framed - the old test fed it bits inverted the
same wrong way, so it agreed with itself.

### What is decoded

- **Full LC** (voice header, terminator): RS(12,9) checked under the
  data-type mask, and if that fails, one octet corrected. The two are counted
  apart (`lc`, `rs-fixed`).
- **Voice superframes**: burst A has the sync; B-F do not, so they are read
  576 bits on from A, one frame each. On the sample path they are re-sliced
  too, on a straight line through the loop's strobes across their own window,
  moved by the correction A's sync measured, at A's eye height and the
  tracker's centre. Sliced on the raw strobes and tracked levels, the
  embedded fragments came out wrong on a noiseless signal: an idle burst's
  ninety-odd +1 symbols in the other slot narrowed the tracker's eye. EMB is checked as QR(16,7,6) (the
  (17,9) quadratic residue generator shortened to 15 bits plus even parity;
  only codewords within one bit are followed). B-E's fragments are joined by
  LCSS into the embedded LC: seven Hamming(16,11,4) rows, column parity and
  the 5-bit checksum.
- **Talker alias** (FLCO 4 header, 5-7 blocks; 7-bit, ISO 8859-1, UTF-8,
  UTF-16BE) and **GPS Info** (FLCO 8) from either kind of LC.
- **CSBK**: BS outbound activation, unit-to-unit voice request and answer,
  preamble, with names for channel timing and NACK; others by opcode.
- **Data header**: format, SAP, group/individual, source and destination,
  blocks to follow, and class/type/status for a response. The blocks that
  follow are not reassembled.

### Calls

Per slot: source, destination, group or private, colour code, encrypted,
start, last burst, end and why - terminator, 1.5 s without a burst, or a
different talker taking the slot. A call first heard through its embedded LC
is marked late entry; one whose identity needed RS correction is marked
until an exact LC arrives. Alias and GPS belong to the call. A mobile has no
CACH, so mobile slots are numbered by timing: the first heard is slot 1, one
half a frame off it slot 2.

There is no DMR call archive; the call archive is P25's.

### `dmr`

Read-only. The text comes from `dmr_watch_format()`, so the bench tests it.
From `test_dmr_calls`, which feeds symbols with the clock frozen - on the
board `timing=own` and the call has a duration:

    dmr symbols=9216 bursts=39 slot_type=34 bptc ok=1 fail=0 lc=1
    checked: lc rs-fixed=0 embedded=5 (fail 0) csbk=0 data hdr=0 voice B-F=25
    last burst: BS data errors=0 cc=5 polarity=normal timing=P25
    last LC: flco=0 fid=0 src=2621441 dst=9 opts=0x00
    slot 1: LIVE group 2621441 -> TG 9 cc=5 0.0s
            alias "N0CALL Bob"
            gps 52.52000 13.40500 err=1
    slot 2: no call
    verdict: DMR - 1 link control block(s) decoded

**Only link control counts as evidence.** A sync match inside its 5-bit
threshold happens on noise; the Slot Type's Golay accepts about a third of
random words inside its correction radius; and BPTC corrects its way to a
clean block often enough to have done so on a P25 channel on this bench. The
RS parity behind a full link control is 24 bits checked against a data-type
mask, which noise does not pass, so one exact LC is enough. A corrected LC
passes about one random word in 5500 and an embedded LC rests on a 5-bit
checksum behind its Hamming rows, so one of those is not - two are. CSBK and
data header CRCs are 16 bits; they are shown and set the polarity, but are
not counted. With nothing checked the line is unchanged:

    verdict: no DMR - 35 sync hit(s), 1 through BPTC, none carried a valid
    link control

That is the correct answer for 154.7850, which is P25.

### Measured

`test_dmr_sweep` renders 40 voice LC headers as RRC-shaped 4FSK IQ (alpha
0.2, as ETSI specifies) with Gaussian noise, runs them through the board's
C4FM demodulator and the watcher, and prints raw BER and LC success against
C/N in 12.5 kHz. LC decode holds at 90% or better from:

| Stream | 0 Hz | +1 kHz | -1 kHz |
|---|---|---|---|
| repeater (BS sync, CACH, continuous) | 8 dB | 8 dB | 8 dB |
| mobile (one slot keyed, no CACH) | 7 dB | | |

A call joined late, identified only by its embedded LC (six superframes a
point, five of six or better): 11 dB. Four bursts' fragments have to come
through Hamming(16,11) a row at a time with no BPTC behind them, so it sits
above the full LC.

`test_dmr_over_iq` frames and decodes from IQ, asserting exact LCs and the
right identity, clean; inverted; at +-1 kHz with 20 dB C/N; inverted at
-1 kHz, 18 dB, 100 ppm clock error and a fractional start together; a
mobile burst out of 400 symbols of no carrier; a call joined mid-superframe,
either way up, identified from all three of its embedded LCs; and the older
ramp-shaped unsigned 8-bit fixture this test used to sample at one fixed
phase. An idle carrier and dead air produce nothing.

At 0 Hz the raw BER floors near 1e-3 where at +-1 kHz it reaches zero. That
is upstream of DMR - most likely the pipeline's IQ DC blocker taking part of
an FM carrier that sits on DC - and BPTC absorbs it, but it would cost
anything uncoded.

### Not proven

The board has never been pointed at an actual DMR transmitter. Every number
above comes from IQ this bench generated, through the real demodulator; the
RF path, a real transmitter's deviation and filtering, and the LR2021's own
front end are not in it.

The vectors for RS, BPTC and Slot Type come from MMDVM-Host's encoders (see
`bench/fixtures/dmr_reference_vectors.md`). Those for CSBK, data header,
talker alias, GPS and the embedded LC were built from the ETSI field layouts
in a separate script, with Python's own CRC-CCITT - independent of these
decoders' code, not of this reading of the standard. Talker alias's octet
formats starting on the octet after the length field is that reading; so is
the checksum's bit order in the embedded LC.

Not decoded: AMBE+2 voice (patent-encumbered, deliberately absent), the
data blocks after a data header, CACH short LC, Tier III and manufacturer
CSBKs beyond their opcode, and privacy beyond the service-options bit.

Two things that cost time and are worth knowing: the C4FM path needs the gain
the P25 app computes, `-9000`. At unity the discriminator output rounds to
zero and the pipeline returns silence, which reads exactly like a dead
demodulator. And that gain is negative. Through it, the bench's +1944 Hz
symbol comes out positive - normal polarity - but which way round a real
transmitter arrives through the LR2021 has not been seen, which is one more
reason polarity is settled by content rather than assumed.

## Radios at boot

BLE scans continuously from boot on every unit, whether or not a control head
has ever been paired, and there was no way to stop it short of a rebuild.

    radios                 # what starts at boot
    radios ble off         # persisted, applies next reboot
    radios wifi off

Default is on, so a board nobody configures behaves exactly as before.

BLE is decided before `settings_init()` runs - the C6 and its stack come up
around 3.6 s, settings load with the backend around 4.0 s - so the ordinary
getter reads its default and the radio starts anyway.
`settings_peek_ble_at_boot()` reads the one byte straight from NVS at that
point, rather than reordering init and moving every other preference's load
with it.

Wi-Fi's autojoin is decided at the same point, and it first shipped through
the ordinary getter: `radios wifi off` was stored and reported back as off,
and every boot still rejoined. It reads `settings_peek_wifi_at_boot()` now,
logs `WiFi autojoin held off at boot by preference` when it is off, and
`bench/quality.py` fails if `app_main` calls any `settings_get_*` before
`lakeshark_backend_start()`.

Confirmed on the board: `BLE held off at boot by preference`, `ble state=off`,
surviving a reboot.

Power saving is not quantified. The fuel gauge reports 0 mA on USB, so any
figure has to be measured on battery.

## P25 signal panel

How Phase 1 voice is decoded, what must stay true, the failures behind
each rule and the tools that measure them: [P25_VOICE.md](P25_VOICE.md).

The level bar cannot separate a strong signal from a saturated front end -
both read near 1.0, and the fix for one is the opposite of the fix for the
other. The share of IQ components pinned at 0 or 255 does separate them, so it
sits on the level line once it passes a fifth.

Measured on 154.7850 at 49.6 dB: 5792 of 16384 components clipped during a
call. **The working gain on this board is 26 dB.** 49.6 is too hot. The
comment on `P25_CLIP_SHOW_PCT` in `scr_p25.c` records the 49.6 dB
measurement and the one-fifth threshold; the 26 dB figure is from the bench
session and is not written down anywhere else.

The clipping indicator is covered by the bench and has not yet been seen on
the glass, because that needs a transmission while someone is watching.

## Bench

`bench/verify.ps1` runs on Windows and Linux. Directory calls went three
different ways across the suites - POSIX `mkdir` with a mode, mingw `_mkdir`
from `<direct.h>`, and a two-arm `#ifdef` - and each form broke the other
host. `ls_test_mkdir()`, `ls_test_rmdir()`, `ls_test_sleep_ms()` and
`ls_test_fixture_dir()` put the split in the harness.

A machine with both a mingw and an ESP-IDF toolchain carries several
`libstdc++-6.dll`, and Git for Windows ships one that sits ahead of
`C:\mingw64` on a git-bash PATH. The seven `.cpp` suites then load a runtime
they were not linked against and die with `0xC0000139` before `main()`,
printing nothing, while the `.c` suites pass. The test run now puts the
directory of `CMAKE_C_COMPILER` first, and a launch that prints nothing
reports its exit code instead of a bare FAIL.

**A silent bench failure is not a race.** Get the exit code first: a suite
that runs always prints something.

## tools/ls_console.py

Finds the board by serial number on Windows as well as Linux - pyserial
reports the USB serial number on both, with the `/dev/serial/by-id` symlinks
kept as the fallback.

`--png FILE` saves the panel's own pixels, checking the byte count and CRC-32
the board reports before anything reaches the file. The image arrives as one
45 KB burst which overruns the default driver receive buffer and silently
loses whole lines, so the port asks for a larger one.
