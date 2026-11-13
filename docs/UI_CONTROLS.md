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
its bytes. NOTES supports touch keys and scrolling. SUB-GHZ opens on a menu of large items.
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

REC and SUB-GHZ share captures and the same menu: READ, RECORD RAW, SAVED,
FREQUENCY ANALYZER and SETTINGS (1 to 5). Each page has BACK, the frequency
as a key, and one main key: START/W listens on READ, RECORD/D keeps one raw
capture on RECORD RAW, START/N sweeps on the analyzer. A capture opens to
SEND and SAVE, a file to SEND, RENAME and DELETE. On the analyzer, LISTEN
HERE tunes READ to a detection and starts it. The radio (R), TX power,
LEARN/L and the rest are on SETTINGS. Leaving a page stops what it started.
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

LINK is in SYSTEM. Its header says the network, address and level, the
Bluetooth head and which remote doors are open before anything is tapped.
Tabs are WI-FI/W, BLUETOOTH/B, SURVEY/V and REMOTE/E; TAB cycles them. On
WI-FI the keys are SCAN/S, JOIN/J, LEAVE/D and OPTIONS/O, where CONNECT SAVED
(R), HIDDEN NETWORK (M), FORGET SAVED (F) and DRONE WATCH (G) live; a second
tap on a saved network connects to it. On REMOTE the keys are WEB/M,
TERMINAL/T and SSH/S switches and OPTIONS/O, with the password (H show, N new,
P set), SIGN OUT BROWSERS (X), KICK TERMINAL (K), SSH PORT (R), LEARN (L) and
FORGET SSH KEYS (F) as rows. DRONES is its own app on HOME.

NOTES opens on NEW/N, FIND/F and OPEN/O over the list; the header shows the
count on the card and the note under the cursor. C opens COMPASS; a note with
a place has GO TO COMPASS in its reader. DIGI's bar is START/S, BAND/B, MODE/M
and OPTIONS/O; TEST SIGNAL (T) is a row in OPTIONS.

FREQS is in RADIO, the frequency list every app tunes from. The bar is
TUNE/T (or Enter, or a second tap on a row), SAVE/S, BANK/K and OPTIONS/O; the
filters above the list are BAND/B, GROUP/G, NEAR/N and FIND/F, and Esc clears
FIND. The TUNE key names the app it will open: FM in the entry's mode, P25,
SUB-GHZ (LISTEN HERE on READ), LORA LABS, ADS-B, GPS or CELL WATCH, or NOT
HERE when nothing can hear the mode. NEAR without a GPS fix says NO FIX and
hides nothing. DELETE and LOAD INTO SCANNER in OPTIONS ask twice. FM (NFM,
AM, HF, WFM, POCSAG, ACARS), P25, LORA LABS, CELL WATCH and SUB-GHZ (a
detection's menu on BAND, OPTIONS on READ) have a SAVE TO FREQS row; the
banner that follows says whether it was written. See [FREQS](apps/FREQS.md).

The SUB-GHZ analyzer's sweep is drawn as bars (eighth-block tops), an ASCII ramp or
a pixel waterfall of the last passes (newest at the top, in FALLS's colours,
dimmed below the gate and full strength from it up); U on the analyzer switches bars and waterfall,
Analyzer chart in SETTINGS lists them all and Chart colour the palette. The frame's title names the chart drawn.
The DETECT track, its number, the column taps and the band edges are the same
in both. MUSIC's views are one VIEW key between < and >: tap VIEW to list the
ten, 1-0 and V still select them. JOURNAL shows chips for recording, entry
count, GPS, IMU and the attached radio over its list.

CELL WATCH is in RADIO. The bar is START/S, BAND/B, LEARN/L (three baseline
passes) and OPTIONS/O; the header shows the band, the frequency, chips for
the state, site and baseline, and the pass meter. OPTIONS holds LOAD BASELINE
(D), LTE SEARCH (E), SITE (G), LORA REPORTS (T), HIGH RATE (P) and RADIO (R);
N lists the cells, H goes home. HIGH RATE's bar is AUTO/S, ONCE/C, CHANNEL/B
and OPTIONS, with DISPLAY (V) and EXIT (H) as rows. Reports are optional.

LORA LABS' bar is DIRECT/D, MODE/M, BAND/B, REC/E and OPTIONS/O; SETUP (S),
SEND (T), MARK (J), RADIO (R), CRC (C), IQ (I), SYNC (P), HOLD (H), CLEAR (X)
and CALIBRATE (K) are OPTIONS rows that still answer their letters. MIX-RF's
bar is MONITOR/M, 2.4 SCAN/S, NFC/N, VIEW/V, MARK/J and OPTIONS/O, with PROBE
(P), NFC CARDS (C), STOP ALL (X) and RADIO (R) as rows. RF SURVEY's OPTIONS is
one list. EXPERIMENTS shows its trials as tiles: tap one to open it; LEFT and
RIGHT walk them, UP and DOWN the rows.
It observes broadcast signals and does not read subscriber messages.
