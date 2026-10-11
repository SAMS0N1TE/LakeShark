# COMPASS

A tilt-compensated compass that reads true north, guides you to a saved place, finds a transmitter by signal strength, and works as a level.

| | |
|---|---|
| HOME group | FIELD |
| Radio | FIND only: LoRa chip, MESH, LoRa BAND, RTL-SDR, HackRF, CC1101, nRF24, Wi-Fi or Bluetooth |

<img src="../screenshots/tdp4/motion/compass.gif" width="300" alt="COMPASS on the FIND page, scanning two channels with the RTL-SDR">

## What it does

COMPASS has four pages:

- **DIAL**: the compass. It reads true north from the World Magnetic Model once there is a GPS fix, and magnetic north before that.
- **FIND**: direction finding. Turn the board through a circle and FIND shows where the signal was strongest.
- **GO TO**: bearing and distance to a target, such as a note with a place, a mesh node, the map centre, home or a FIND fix. It also follows a route started in MAP.
- **LEVEL**: a bubble level with pitch, roll and tilt.

Bearings and fixes you save go into NOTES as BEARING and FIX lines, with the place they were taken from.

## Radio

Only FIND uses a radio. On the FIND page, press R or tap RADIO to choose one. Each radio is listed as ready or with the reason it cannot be used.

- **MESH** follows MeshCore traffic. MeshCore must be running (open MESH to start it).
- **LoRa BAND** sweeps a band with the LoRa chip. Each transmitter it finds becomes a channel of its own.
- NFC and GPS cannot take bearings.
- Two rows at the end start a beacon calibration: **FLIPPER BEACON 915: CALIBRATE** and **FLIPPER BEACON 433: CALIBRATE**.

A second radio can listen at the same time (OPTIONS > Second radio at once). The two cannot share hardware: only one of the LoRa chip's uses, one USB SDR, and one of the CC1101 and nRF24 at a time.

## How to use it

Open **HOME > FIELD > COMPASS**.

| Tab | Key | Does |
|---|---|---|
| DIAL, FIND, GO TO, LEVEL | 1 to 4 | Opens that page |
| OPTIONS | O | Settings for the page (see below) |
| LOOKS | Y | How the dial is drawn |

Left / Right or Tab move between the buttons. Enter presses the highlighted one. Esc returns to HOME.

### DIAL

| Button | Key | Does |
|---|---|---|
| LOCK (HERE / CLEAR) | L | Locks the current heading so you can steer back to it, or clears it |
| MARK NOTE | M | Saves the heading, GPS line and any locked bearing to NOTES |
| CAL SETUP | K | Opens the calibration guide in LORA LABS |

### FIND

1. Choose a radio with RADIO, and a frequency with TUNE.
2. Hold the board level and turn slowly through a full circle.
3. When the answer shows (SIGNAL AT ...), press SAVE BEARING.
4. Move somewhere else, well to one side, and take another bearing.
5. With two or more bearings, press FIX to cross them.

| Button | Key | Does |
|---|---|---|
| RADIO | R | Chooses the radio |
| TUNE / SCAN / BAND / TARGET | T | Sets the frequency or channel list. For Wi-Fi, Bluetooth and MESH it picks what to follow, or Everything heard |
| METHOD | N | Steps through PEAK, NULL and FIT |
| SAVE BEARING | S | Saves the bearing to NOTES |
| CLEAR SWEEP | C | Starts the circle again and clears the hit log |
| FIX (CROSS / NEED 2) | X | Crosses the saved bearings, saves the FIX to NOTES and sets it as the GO TO target |
| VIEW | V | Steps through DIAL, RADAR and HEAT |
| LOG HITS | G | Opens the hit log |

- **PEAK** points at the strongest direction. Turn slowly through a full circle.
- **NULL** uses your body as a shield. Hold the board to your chest and turn a full circle.
- **FIT** fits a curve to the whole circle, or matches a pattern learnt for that radio. It warns when front and back look alike.
- TUNE opens a list of up to eight channels per radio. Add a channel, change or remove one, or load a scan preset: ISM remotes, FRS/GMRS, MURS, NOAA weather, marine, LoRa and mesh channels. Channels of the first radio are tagged 1 to 8, the second radio A to H.
- [ and ] step the shown channel through the live ones.
- Without a sweep answer, SAVE BEARING keeps where the board points, with a wide spread.
- In the hit log: LOG DONE, SAVE NOTE, CLEAR LOG and VIEW. Up / Down or a tap picks a row and shows its heading on the dial. Esc leaves the log.

### GO TO

| Button | Key | Does |
|---|---|---|
| SET TARGET | T | Picks the map centre, home, the FIND fix, a note with a place or a mesh node with a position |
| MAP SHOW | M | Opens MAP at the target |
| CLEAR | C | Clears the target |

While a route from MAP is running, the first button reads OPTIONS ROUTE and a guidance line runs along the bottom.

### LEVEL

Lay the board on the surface. The bubble floats to the high side and turns green when level within half a degree. Stood on an edge, the page shows the slope in degrees and percent grade.

### OPTIONS

On DIAL, GO TO and LEVEL:

- **North**: true or magnetic.
- **Style**: 3D (the lit, tilting instrument) or simple (a flat ASCII ring).
- **Calibrate**: the six-position and figure-eight guide, in LORA LABS.
- **Clear the lock**, **Clear the target**, **Clear saved bearings**.
- **Calibrate FIND** and **Clear FIND correction**.
- **Route**: off-route distance, arrival radius, haptics, guidance, reverse and clear.

On FIND, the FIND settings come first, then the rows above:

- Hold each peak, Then let it fall, Forget a direction after.
- A hit stands over noise by, Believe a hit after, Hit blips last.
- Scan dwell per channel, Mark other lobes this tall.
- Second radio at once, Second radio's channels, Clear the hit log.

BACK goes up one level.

### LOOKS

Each row steps to its next choice: signal fill inside the card (full, light, edge only, off), the green trail of directions heard, direction letters (white or orange) and the style.

### Beacon calibration

FIND can correct its bearings against a known transmitter, the Flipper's LakeShark DF Beacon.

1. Start the beacon on the Flipper and set it down 5 to 10 m away, in the open.
2. Choose **Calibrate FIND** in OPTIONS, or a FLIPPER BEACON row in RADIO.
3. Aim the board's top at the beacon and press MARK BEACON.
4. Turn steady circles. Keep under 30 degrees a second.
5. After three circles that agree, press SAVE CAL. CANCEL CAL stops without saving.

The correction is kept for that radio and method, and FIND shows it under the answer.

## What the screen shows

- **DIAL**: the heading in large digits with T or M and the compass point. Lines below show calibration state, TRUE or MAGNETIC NORTH, declination, dip and field strength, the sun's bearing and height, the back bearing and tilt, the lock and the target.
- A yellow **! MAG** lamp lights when the magnetic field looks disturbed.
- **FIND**: the current level and PEAK, a level bar, TURNED n of 360 and CONTRAST, the answer as SIGNAL AT ... +/- spread, other channels' bearings, the last hit, and BEARINGS SAVED. CLIPPING warns when the signal is too close or too loud to give a bearing.
- **RADAR** draws a cone for each channel. **HEAT** shows each channel's circle now, then the shown channel over time.
- Hit log flags: 1x one read, CLIP overloaded, FAST turned fast, WIDE no clear direction.
- **GO TO**: the target name, its bearing in large digits, the distance, and TURN LEFT, TURN RIGHT or STRAIGHT AHEAD. When moving, the speed and minutes to go.

## Tips and limits

- Calibrate first. A bent heading bends every bearing. Recalibrate when the keyboard or nearby metal changes.
- True north needs one GPS fix, or a saved position. Without it the compass reads magnetic north.
- A bearing saved without GPS goes to NOTES but cannot join a fix.
- Close to a transmitter, or among reflections, the circle gives no clear direction. Step back and try again.
- SWEEP can hand a Wi-Fi or Bluetooth device straight to FIND.

## More guides

- [Field and lab guide](../FIELD_LABS.md)
- [NOTES](NOTES.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
