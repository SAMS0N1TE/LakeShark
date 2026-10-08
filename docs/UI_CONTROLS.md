# UI controls and portrait review

Radio controls and waterfall controls use separate shortcuts. In FM and P25,
T opens tuning, U/J increases/decreases receiver gain, and +/- changes volume.
On the LR2021, P25 GAIN selects AUTO or MAX and keeps the choice.
R opens RADIO and O opens OPTIONS. Inside scanner lists, R is RANGE and O is
SCAN TYPE instead.

On the waterfall, F changes DETAIL, G changes RANGE, H holds/resumes, S changes
SPLIT, I changes REF, P changes COLOR, A changes AVG, D changes SPEED, K changes
PEAK, and C changes CNTRST. These letter keys accept either case. FALLS uses
RADIO/R to choose its receiver and BAND/N to choose a band. Its FM controls
include MODE/E, TUNE/T and SWEEP/W.

The default spectrum share is one quarter, adjustable with SPLIT. Left/Right
moves a waterfall marker; Space tunes to it. Tap a signal to mark it, then use
TUNE MARK/Y (TUNE in landscape). P25/FM tune the receiver feeding the view.
An FM sweep returns to listening. A LoRa sweep hands the frequency to LORA LABS
direct receive. Changing the source clears the marker.

HOME uses F1-F4 for RADIO, FIELD, SYSTEM and USER. F5/F6 or PREV/NEXT changes
pages. Arrows select tiles; Enter opens one. Digits 1-9 open tiles on the current
page. An app's initial selects its tile. The number of tiles depends on the
screen size. Disabled controls cannot be activated.

Portrait NFC has block paging and a Back control. Tap a block, then VIEW for
its bytes. NOTES supports touch keys and scrolling. SUB-GHZ has pattern paging.
RADIOS shows keyboard radio status and can open MIX-RF.

## P25 and FM radio screens

P25 shows DECODE / SIGNAL in one window. SPECTRUM/2 toggles its plot. SCAN/3,
SETTINGS/4, CALLS/5 and DMR/6 open the corresponding views. P also opens SETTINGS
outside the spectrum view. On the spectrum view, P changes COLOR. 1 returns to
decode. Use RADIO/R to choose the receiver. Without a USB SDR, a fitted LR2021
can receive P25 Phase 1 from 150 MHz.

FM opens on the radio panel. MORE opens its VFO, data and spectrum views.
1 opens VFO, 2 opens the current data view, and 3 opens SPECTRUM. SCANNER/0 or
Escape returns to the radio panel. MODE/E selects NFM, WFM, AM, POCSAG, FLEX,
ACARS, SAME/EAS, APRS or AIS; SWEEP/W switches the band sweep.

- **TUNE** opens frequency entry. A manual tune or mode change stops channel scanning.
- **LISTS > SAVE FREQ** saves the frequency in the selected zone. With all zones selected, new channels go into zone 0. ENABLE includes or excludes the selected channel.
- **SCAN** offers **CHANNEL LIST** and **BAND SCAN**. Select a type, then START. The default band is 150-162.6 MHz at 7.5 kHz. RANGE also offers VHF 136-174, NOAA WX, 2m, 70cm and UHF 450-470. Each preset sets its own step. STEP cycles 5, 6.25, 7.5, 10, 12.5, 15, 20, 25 and 50 kHz.
- **HOLD** pins a channel, including a quiet one. RESUME releases it; NEXT advances. STOP ends scanning. SKIP excludes a saved channel until scanning restarts; in band mode it advances one step.
- FM **A / B** swaps active and standby frequencies on one receiver. It does not receive both at once. **SQUELCH** adjusts the 0-100 threshold. NFM uses noise quieting; the percentage is not calibrated dBm.

CHANNEL LIST supports conventional P25 Phase 1 and NFM, with MIXED AUDIO and
GPS FILTER. Trunked talkgroup following is separate. See [Location scanning](LOCATION_SCAN.md).

P25 SETTINGS exposes demodulation, polarity, AGC, gain, volume, voice gate,
sync beep, trunking auto-follow, encrypted-call skip, CQPSK tuning, scan
threshold and scan hang. Select a row with touch or Up/Down. Left/Right changes
it; CHANGE opens numeric entry where available. Load profiles here or with L.

FM > MORE > MODE selects the newer data receivers. APRS has STATIONS, AIS has
VESSELS, and SAME/EAS has ALERTS. OPTIONS holds settings for the chosen mode.
NFM > OPTIONS > TONE > TONE SQUELCH selects OFF, CTCSS or normal/inverted DCS.
SHOW DETECTED shows the heard tone. A tone gate set for a matching saved NFM
channel is kept with that channel; an unsaved VFO tone is temporary.

CALLS/5 in P25 or FM opens the SD archive. [Updates and calls](TDP4_UPDATES_AND_CALLS.md)
covers recording, playback, retention and file sizes. DMR/6 in P25 shows Tier II
call activity. SLOT/S chooses BOTH, 1 or 2; HOLD/H holds a heard destination.
OPTIONS offers slot, colour-code and talkgroup filters. Voice and DMR recording
are unavailable. Encrypted calls are marked and muted.

REC and SUB-GHZ share captures. RADIO/R chooses the receiver. Stop WATCH before
changing capture source. WATCH/W starts capture; TOOLS/D opens RTL capture
controls in REC. EXPORT and marks keep selected captures or observations.
LoRa FSK captures and CC1101 raw OOK/FSK captures can be replayed where supported.
An OOK24 row identifies a repeated pulse pattern, not a verified manufacturer.

MUSIC is in RADIO. TRACKS/L opens WAV files from SD `/music` or internal
`/music`. Space plays/pauses, I toggles MIC, W toggles microphone recording,
1-4 or V selects LEVEL, BANDS, SCAN or waterfall, C changes colour, and +/-
changes volume. Recordings are `/sdcard/music/REC-001.wav` and subsequent free
numbers. OPTIONS includes player and display settings.

TERMINAL is in SYSTEM. Enter runs a command; Up/Down recalls history; Tab
completes a command; Left/Right scrolls output. KEYS opens touch input. ERRORS
runs `crumb log`, HEAP shows memory, and CLEAR clears output. Type `help` for
the available console commands.

CELL WATCH is in RADIO. BAND/B selects a band, LEARN/L takes three baseline
passes, LOAD/D loads a baseline, and SITE/G chooses GPS or LOCAL. LTE/E searches
for LTE carriers. HIGH RATE/P offers a restart into the high-rate capture mode.
RADIO/R and OPTIONS/O select its receiver and settings. Reports are optional.
It observes broadcast signals and does not read subscriber messages.
