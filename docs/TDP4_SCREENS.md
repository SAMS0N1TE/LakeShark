# T-Display-P4 screens

LakeShark 2.9.1 on the LilyGO T-Display-P4.

HOME groups apps under RADIO, FIELD, SYSTEM and USER. Use F1-F4 to choose a
group, arrows and Enter to open an app, and F5/F6 or PREV/NEXT to change pages.
The number of tiles per page depends on the screen size.

- RADIO: P25, FM, ADSB, FALLS, CELL WATCH, MESH, LORA LABS, EXPERIMENTS,
  SUB-GHZ, FILES, MUSIC and MIX-RF.
- FIELD: NOTES, COMPASS, REC, MAP and GPS.
- SYSTEM: DIAG, TERMINAL, UPDATE, SET, RADIOS and LINK.
- USER: apps loaded from the SD card.

P25 has a DECODE / SIGNAL window. SPECTRUM toggles the plot; SCAN, SETTINGS,
CALLS and DMR open their views. CALLS is also available in FM.
See [UI controls](UI_CONTROLS.md) and [Updates and calls](TDP4_UPDATES_AND_CALLS.md).

FM > MORE > MODE includes APRS, AIS and SAME/EAS. Their detail pages are STATIONS,
VESSELS and ALERTS. NFM > OPTIONS > TONE provides CTCSS and DCS tone squelch.

MUSIC plays WAV files, shows levels and spectra, and records microphone input.
TERMINAL runs console commands with the keyboard or touch keys. CELL WATCH
surveys cellular signals; it does not read subscriber messages.

EXPERIMENTS includes RS41 SONDES, LR433 and PAGER RECON. NOTE or N saves the
readout to NOTES. [Field and lab guide](FIELD_LABS.md) covers these receivers.
MESH node details offer INSPECT for route tracing, login, status and telemetry.
LINK > SURVEY records a walk's Wi-Fi and BLE observations. MAP > ROUTE follows
a GPX file or backtracks a recorded walk. See [Field map](map-field.md).

## Scanning, replay and aircraft

FILES opens captures in REC for replay. SUB-GHZ watches raw signals. ADSB has a
mini map with FOLLOW choices for all aircraft, the selected one or the nearest.
The choice is kept. P25 and FM scan saved channel lists or stepped bands.
See [Location scanning](LOCATION_SCAN.md).

## Portrait

The app pages stack panels and use several rows of touch controls. HOME pages
its tiles. P25 keeps its DECODE / SIGNAL readout together, with SPECTRUM, SCAN,
SETTINGS, CALLS and DMR tiles below. CALLS has PLAY, STOP, DELETE, OPTIONS and
BACK. NOTES opens touch keys for editing when no keyboard is attached.

## Landscape

Pages use the wider view for plots, lists and side-by-side panels where space
allows. Controls adapt to whether a keyboard is attached. The displayed key
hints show the current bindings. F10 opens help; F11 rotates the screen.

## Daylight

SET > DISPLAY > Daylight uses black text on a white background. The underlying
theme choice is kept. SET > Rotate lock holds the current orientation.
