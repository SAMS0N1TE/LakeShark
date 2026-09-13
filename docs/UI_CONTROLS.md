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
