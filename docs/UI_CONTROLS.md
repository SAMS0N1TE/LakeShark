# UI controls and portrait review

Radio controls and waterfall controls use separate shortcuts. In FM and P25,
T opens tuning, U/J increases/decreases receiver gain, and +/- changes volume.
On the waterfall, F changes detail, G changes display range, H holds/resumes,
S changes split, R changes reference, P changes palette, A changes averaging,
D changes scroll speed, K changes peak hold, and C changes contrast.
These keys accept either case. The standalone Falls source/band controls use
V/N; its FM mode, tuning and sweep controls use E/T/W.

The default spectrum share is one quarter, adjustable with SPLIT. P25 audio
status occupies a single row below the plot. Waiting for a voice header remains
a decoder state and does not reserve a panel above the spectrum.

Home group keys R/F/S/U work in either case and take priority over app initials.
Use arrows and Enter for apps whose initial is a group key. ] selects more apps.
Quick controls and ordinary buttons display their actual shortcut bindings.
Disabled buttons reject taps. Shared hit areas are cleared when the screen
changes and before a new frame so hidden controls cannot retain old targets.

Portrait NFC uses larger block targets, Previous/Next pages, and a Back button.
Tap a block, then VIEW for its bytes. Journal has touch scrolling and Back;
Sub-GHz has Previous/Next pattern controls. The Radios page on T-Display-P4
reports CC1101, nRF24 and NFC status from MIX-RF and opens its workbench.

Animation is bounded and event-driven: button press/state feedback, live app
markers, actual packet/save highlights, busy operation indicators, and a brief
highlight as NFC blocks become verified. The NFC progress bar uses verified
block counts. Busy indicators represent activity, not percentage completion.
Animation state uses PSRAM where appropriate; no new task, polling loop, radio
measurement, automatic transmission or SD logging was added for animation.

Validation covered portrait and landscape renders for all 16 simulator apps,
37 screen tests, the render-cost tests and the complete host verification gate.
On-device checks exercised P25 waterfall shortcuts without changing receiver
gain/tuning, NFC portrait paging to blocks 96 and 192, block selection and hex
view, and live keyboard-radio status. Simulated RF fixtures are layout checks,
not reception evidence. A P25 hardware check measured about 3.5 ms UI draw time,
56 ms frame interval, 16 active USB transfer slots and zero dropped input bytes.

## P25 and FM radio screens

The radio view shows the receiver's effective frequency when available. Pending or failed tunes remain visible. MORE opens the existing decode, waterfall, mode and audio controls; RADIO or 0 returns.

- **TUNE** opens frequency entry. A manual tune or mode change stops channel scanning.
- **LISTS > SAVE FREQ** saves the current frequency in the selected zone. With all zones selected, new channels go into zone 0. ENABLE includes or excludes the selected channel. Use arrows or the touch page controls for longer lists.
- **SCAN** opens two choices: **CHANNEL LIST** visits saved frequencies; **BAND SCAN** visits every frequency step in a range. Select the type, then press START. STEP cycles 5, 6.25, 10, 12.5, 15, 20, 25 and 50 kHz. RANGE cycles 150–162 MHz, 144–148 MHz and 420–450 MHz, with the chosen step size (12.5 kHz by default). Existing console band settings are also honored.
- **START** begins the selected scan type. HOLD pins the channel, including a quiet channel. RESUME/NEXT advances. STOP ends scanning. SKIP excludes a saved channel until scanning restarts; in band mode it advances one step.
- FM **A/B** swaps the active and standby frequency on one receiver. It does not provide simultaneous reception. **SQUELCH** edits the 0–100 IQ-level threshold; it is a percentage, not dB.

Channel scanning supports P25 Phase 1 and NFM. WFM, AM and data modes remain available under MORE. Saved-channel scanning and P25 trunked talkgroup following are separate functions.

The simulator uses host fixtures, not RF measurements. Host tests cover tuning, sync dwell, hold, next, session skip, receiver loss and tune failure. On-device reception and scan timing still need a connected receiver check.

P25 has a dedicated **SETTINGS (P)** button, also available as page 3 under MORE. It exposes demodulation, polarity, AGC, receiver gain, volume, voice gate, sync beep, trunking auto-follow, encrypted-call skip, CQPSK loop tuning/reset, scan threshold and scan hang. Select a row with touch or UP/DOWN, adjust with the left/right controls, or use CHANGE for numeric entry.

On a waterfall, tap a signal to mark it, then press **TUNE MARK (Y)**. The marked frequency is shown on the button. P25/FM tune the receiver feeding the view; FM band sweeps switch back to listening. LoRa sweeps hand the marked frequency to LoRa Labs direct receive. A missing marker disables tuning, and changing the source clears the old marker.

REC and SUB-GHZ use the same capture list. **SOURCE/R** switches RTL/CC1101 while
WATCH is stopped; **WATCH/W** starts passive capture. **TOOLS/D** opens the old
RTL capture diagnostics, and **BACK/B** returns. PIN, EXPORT and JOURNAL work
with either source. OOK24 rows show a repeated decoded payload; RAW rows remain
unclassified pulse captures. Landscape shows the waveform alongside the list.
