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

The DMR stack - sync detection, framer, Golay slot type, BPTC, link control -
had no caller outside its own tests. Nothing fed it a symbol.

DMR is 4FSK at 4800 symbols per second, the same rate as C4FM P25, so every
DMR burst on a channel already passes through the P25 frame-sync hunt and is
discarded. `getFrameSync()` now offers each symbol, with the slicer thresholds
it was measured against, to an observer. `dmr_watch` attaches to that and
frames both polarities at once, because which way round the discriminator sits
is not known in advance.

The seam is a function pointer, not a direct call, so the P25 sources do not
gain a link dependency on every decoder that wants a copy of the stream. The
observer steers nothing and touches no P25 state.

### `dmr`

Read-only. Reports symbols seen, bursts framed, Slot Type and BPTC outcomes,
the last burst's class and colour code, and the last link control.

**Only the link control counts as evidence.** A sync match inside its 5-bit
threshold happens on noise; the Slot Type's Golay accepts about a third of
random words inside its correction radius; and BPTC corrects its way to a
clean block often enough to have done so on a P25 channel on this bench. The
RS parity behind the link control is 24 bits checked against a data-type mask,
which noise does not pass. The verdict line says which number to read:

    verdict: no DMR - 35 sync hit(s), 1 through BPTC, none carried a valid
    link control

That is the correct answer for 154.7850, which is P25.

### Not proven

The board has never been pointed at an actual DMR transmitter. Everything
below the symbol stream is tested; the RF path is not.

Framing from IQ is also not proven. Symbol timing recovery lives in
`getSymbol()`, which needs the `dsd_opts`/`dsd_state` the frame-sync loop
owns. `test_dmr_over_iq` subsamples at a fixed phase instead, which leaves
about 7% of bits wrong on a noiseless signal - marginal against a 48-bit sync
match - so it asserts that the burst is recoverable at the right polarity, not
that it frames. `test_dmr_watch` covers framing, from symbols.

Two things that cost time and are worth knowing: the C4FM path needs the gain
the P25 app computes, `-9000`. At unity the discriminator output rounds to
zero and the pipeline returns silence, which reads exactly like a dead
demodulator. And that gain is negative, so C4FM arrives inverted - which is
what the second framer is for.

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
