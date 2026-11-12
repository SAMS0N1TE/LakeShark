# UI usability pass: on the move, on a small touchscreen

A survey of every app as `lssim` renders it on the T-Display-P4 (52x70
portrait, 118x31 landscape, and the TFT with its camera hole), ranked by how
much it hurts in the hand, with the fix. The first section is what was found;
the second is what was done in this pass and what is left.

The yardstick is the house style the main apps already have: HOME's tiles,
FM and P25's big frequency and raised keys, ADS-B's mini map, COMPASS's card,
DRONES' quad whose rotors step once per measurement, SWEEP's rings and big
word, DIGI's Costas array, SUB-GHZ's pulse inspector. Panels, dither fields,
colour by meaning, raised keys with their letter, a header that says what the
app is doing, and one emblem per app that moves only when the app does.

## The survey, ranked

1. **LINK (`scr_wireless.c`)** - text on black with a private button widget.
   Five tabs, one of which (DRONES, `scr_wireless.c:613`) was not a tab but a
   jump to the DRONES app. Wi-Fi's actions were six small keys in two rows at
   the bottom (`:685-699`), REMOTE's eleven (`:277-348`): a wall of same-size
   keys for things done once (passwords, ports, SSH keys) beside things done
   constantly (scan, join). The state that matters - which network, what
   level, is anyone remoted in - was spread over three panels. *Fixed.*
2. **SUB-GHZ BAND chart (`scr_subghz.c`)** - the sweep's default grain was
   SHADE: quarter, half and three-quarter dither blocks per column, a shade
   row along the floor of every column (`:1264`) and a dotted shade gate rail
   (`:1191`). Rendered, that is mottled slabs and a noise strip, not levels.
   *Fixed: bars only (eighth-block tops) or the ASCII ramp; see the rule
   below.*
3. **CELL WATCH (`scr_cell.c`)** - eleven keys in one raised bar on the main
   page (`:723-736`) and six on the other, four pickers reachable only from
   OPTIONS rows (`:192-201`), a DISPLAY picker inside the app that duplicates
   SET (`:247`). The big frequency digits are hand-rolled (`:28-43`) rather
   than the shared wordmark. No emblem; the survey plot should follow the
   bars-or-waterfall rule. The highest-value remaining target; it needs the
   Ninja bench tree (`bench/tests/cell_preview.py`) to render.
4. **LORA LABS (`scr_labs.c`)** - fifteen keys on the face, a nine-key bar
   (`:866-876`) over a six-key switch bar (`:877-886`), plus pickers opened
   from pickers (`:263`, `:290`, `:349`, `:377`). The switches (CRC, IQ, SYNC,
   HOLD, CLEAR, CAL) belong in OPTIONS; DIRECT, MODE, BAND, RECORD, RADIO,
   OPTIONS on the face.
5. **MIX-RF (`scr_mixrf.c`)** - ten keys in two rows (`:160-172`), two of
   them (PROBE, STOP ALL) rare; a key/value panel of text. Face wants
   MONITOR, 2.4 SCAN, NFC, VIEW, MARK, RADIO, OPTIONS, with PROBE and STOP ALL
   as rows. An emblem: the keyboard's three radios.
6. **RF SURVEY (`scr_rfsurvey.c`)** - a root OPTIONS menu of four submenus
   (`:242-249`) with BAND a picker inside a menu inside OPTIONS (`:128`).
   The emblem (the walker, `:255`) is good; BAND should be a key on the face
   and the heat field should be bars or a waterfall, not dots.
7. **NOTES (`scr_notes.c`)** - plain list, four keys with COMPASS APP (a jump
   to another app) taking a quarter of the bar (`:399-400`). *Fixed.*
8. **DIGI (`scr_digi.c`)** - five keys in a fixed five rows (`:516`, `:541`)
   cut every label short in portrait; TEST was both a key and an OPTIONS row.
   *Fixed.*
9. **EXPERIMENTS (`scr_experiments.c`)** - a text list of thirteen trials
   (`:296-343`) where the house already has `ls_tile_grid`; tiles with the
   maturity as hue and the running one lit would be the obvious fix. Needs
   the render-dump tests that drive the list by row re-pointed.
10. **MUSIC (`scr_music.c`)** - fourteen one-row tabs in two strips (MIC REC
    TRACKS OPTIONS / LEVEL BANDS SCAN WFALL WAVE SPECTRO MELODIC SPECTRUM
    SCOPE TOOLS); the nine views want to be one VIEW key that cycles, or a
    picker, with LEVEL and the transport on the face.
11. **JOURNAL, DIAG, TILES, MESH's chat** - key/value text on black. JOURNAL
    and DIAG are viewers and read fine; TILES could carry a map-sheet emblem
    and MESH's header a node emblem whose pip steps per packet.
12. **SD apps (`app-sdk/apps/`: detector, burst, level, iridium, probe,
    tcp_probe, rflab, meshtastic)** - outside the firmware screens; they draw
    through the lap API. Their level charts fall under the bars-or-waterfall
    rule and want a pass of their own with the app SDK's simulator.
13. **PINS (`scr_pins.c`)** - Nano only, not in the simulator's app table;
    text with arrow stepping. Low traffic.

### The chart rule

Every spectrum or level chart draws bars (eighth-block partial tops, colour
by dB over the floor) or a proper waterfall (scrolling rows on the shared
palettes, `ls_wf_level_colour`). No dither-block slabs, no mottled strips,
no dotted rails. SUB-GHZ BAND follows it now; CELL WATCH's survey, RF
SURVEY's heat field, FM's sweep and the SD apps' level plots are the ones to
check next.

## What changed in this pass

### SUB-GHZ BAND
SHADE is gone; BARS is the default grain and ASCII the alternative. A board
with SHADE stored gets BARS. The floor is one thin line, the gate rail a thin
line with a solid marker, the legend swatches and the level and progress
meters solid blocks. Heights (two noise spreads over the floor), colour, the
gate, detections and hold caps are unchanged.

### LINK
- Header with the app's emblem: the board under its fan of arcs, one arc lit
  per bar of the network it is on; the arcs step outward only while a scan
  or a join runs. The network, address and level, the Bluetooth head and the
  remote doors (WEB/TERM/SSH, watched or not) read before anything is tapped.
- Tabs WI-FI, BLUETOOTH, SURVEY, REMOTE. The DRONES tab is gone: DRONES is on
  HOME, on G here, and the DRONE WATCH row in Wi-Fi's OPTIONS.
- Wi-Fi: SCAN, JOIN, LEAVE, OPTIONS in a raised bar at the bottom. Moved into
  OPTIONS: AUTO (now CONNECT SAVED, R), MANUAL (HIDDEN NETWORK, M), FORGET
  (FORGET SAVED, F). A second tap on a saved network connects to it.
- REMOTE: WEB, TERMINAL, SSH switches and OPTIONS. Moved into OPTIONS: SHOW/
  HIDE password (H), NEW password (N), SET password (P), SIGN OUT browsers
  (X), KICK terminal (K), SSH PORT (R), LEARN keys (L), FORGET keys (F). The
  web screen's letter is M (it was O, which is OPTIONS everywhere).
- Bluetooth: SCAN, STOP, OPTIONS; unchanged otherwise.
- Keyboard: LEFT/RIGHT walk the bar, ENTER presses, TAB cycles the tabs,
  every old letter still answers.

### NOTES
- Header with the emblem: a page whose ruled lines are lit for the notes on
  the card (up to three) and whose pen walks the lines only while the card is
  read or written. Beside it: the count, the note under the cursor with its
  time and badges.
- NEW, FIND, OPEN. COMPASS APP left the bar: COMPASS is on HOME, still on C
  here, and GO TO COMPASS stays in the reader for a note with a place.

### DIGI
- START, BAND, MODE, OPTIONS at the house bar height; TEST SIGNAL is the
  OPTIONS row it already was, still on T.

## Round 2

### CELL WATCH
- Header band in the mode's hue: band and EARFCN range, the mast whose arcs
  light as cells are heard (as the pass goes, in a survey), the frequency in
  the house block digits with the window's EARFCN, chips for RECEIVING/HELD/
  READY, the LTE phase, GPS FIX/NO FIX/LOCAL, the baseline or MIB, and the
  pass meter. The hand-rolled digit table is gone.
- Bar: START/STOP, BAND, LEARN, OPTIONS (was eleven keys in three rows).
  Moved into OPTIONS: LOAD BASELINE (D), LTE SEARCH (E), SITE (G), LORA
  REPORTS (T), HIGH RATE (P), RADIO (R); HOME is H/ESC. HIGH RATE: AUTO,
  ONCE, CHANNEL, OPTIONS; DISPLAY (V) and EXIT (H) are rows.
- Chart: eighth-block bars from a thin floor line instead of braille dots;
  scale, colours and the baseline dashes unchanged. The one-row bar leaves
  room for all fourteen cells of the review fixture in portrait.

### LORA LABS
- Bar: DIRECT, MODE, BAND, REC, OPTIONS (was nine keys over six switches).
  Moved into OPTIONS on every mode's list: SYNC PRESET (P), HOLD PLOT (H),
  CLEAR PLOT (X), SEND ONE PACKET (T), MARK JOURNAL (J), CALIBRATE COMPASS
  (K), RADIO (R); CRC (C) and INVERT IQ (I) were rows already. SETUP stays
  on S. The RSSI/spectrum trace is bars coloured by level.

### MIX-RF
- Bar: MONITOR, 2.4 SCAN, NFC, VIEW, MARK, OPTIONS (was ten). One OPTIONS
  list for every view: RECEIVE BAND (says when it does not apply), PROBE
  RADIOS (P), NFC CARDS (C), STOP ALL (X), RADIO (R). A chip rail under the
  title shows which radios were identified and which is listening.

### RF SURVEY
- OPTIONS is one list (17 rows) instead of a menu of four menus; the heat
  rows first, then files, survey numbers, custom band. The channel strip is
  bars from a thin floor line. The heat field (already a braille heat map
  with the walker emblem) is unchanged.

### EXPERIMENTS
- Tiles, like HOME: a glyph per trial by what it listens to, maturity as
  hue, the running one lit. Tap opens; LEFT/RIGHT walk, UP/DOWN rows.

## Round 3

### SUB-GHZ BAND waterfall
- CHART (U) on BAND switches bars and a waterfall: one row a pass, newest
  at the top, every cell the bin's dB over the floor in the sweep's own
  colours (SPECTRUM COLOUR), bright where it clears the gate. WATERFALL is
  the third SPECTRUM STYLE (stored as 3, so an old BARS value stays bars).
  A pass is taken as complete when the sweep's progress wraps, or every
  400 ms when it does not move; 48 rows of 64 bins in PSRAM. The DETECT
  track and its number, the column taps (LISTEN HERE), the peak reading,
  the marked detection and the band edges are the same in both views.

### MUSIC
- The ten view chips in two rows became one row: < , VIEW <name> (tap to
  list the ten with what each shows), >. Keys 1-0 and V unchanged.

### JOURNAL
- A chip rail under the title (RECORDING/IDLE/WRITE FAILED, entry count,
  GPS, IMU, the attached radio); the panel turns green while recording.

## Round 4

- **DIAG**: a rail of chips on the frame's first row (radio stream state,
  internal memory, alerts on/off); section titles bright. **TILES**: chips
  under the title (SD free space, region count, MAP OPEN, a transfer while
  one runs, BUSY). Nothing below either rail moved.
- **FM**: checked. The shared waterfall (`ls_waterfall.c`) draws its history
  with the pixel renderer for the default PIXEL grain and for SMOOTH
  (`draw_pixels`), falling back to cells only for the ASCII, DENSE and BARS
  grains the operator picks; no shade slabs by default. The spectrum half is
  bars. Nothing to change.
- **Tiles**: the grid's pitch follows the longest name where the rows allow,
  and a name that still does not fit folds at its last space onto the
  subtitle row (or shows its first word where a short tile has none), so
  EXPERIMENTS reads AVIATION / PULSES rather than AVIATION PULS.
- Fixture tests: test_lssim_rs41's EXPERIMENTS case follows the tiles (every
  trial one tap away, ESC back to the grid); test_lssim_notes' failed-save
  case keeps its blocking directory non-empty, because the simulator's
  fixture copy removes empty entries since the FREQS change.

## Not done

PINS (13) is Nano-only and not in the simulator's app table, so it was left
untouched rather than restyled blind. The SD apps (12) draw through the app
SDK's own canvas API and want a pass with that SDK's simulator. The SD apps'
level plots remain to check against the bars-or-pixel-waterfall rule.
