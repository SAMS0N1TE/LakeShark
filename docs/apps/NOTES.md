# NOTES

A field notebook: write notes on the SD card and drop in the time, a GPS fix, the heading, a map picture or what a radio hears.

| | |
|---|---|
| HOME group | FIELD |
| Radio | None of its own. INSERT can add what a running radio app reports |

## What it does

- Keeps notes as Markdown files in `/sdcard/notes`.
- Edits with a real cursor. Touch keys appear when no keyboard is attached.
- INSERT writes live readings into the note at the cursor.
- Shows data lines as data: coloured tags, tick boxes for checklist items and a live map for a place.
- Saves map pictures beside the note as PNG files.
- Opens a note's place in MAP, or follows it with COMPASS.
- TRASH moves a note to `notes/trash`. Nothing is deleted.

Other apps save into NOTES too: COMPASS marks, FIND bearings, fixes and hit logs, map markers and lines from MAP, and EXPERIMENTS readouts.

## Radio

NOTES does not use a radio. **INSERT > Radio** adds one line with what a radio app is reporting at that moment. The choices are the ones reporting now: P25, FM, ADS-B, MESH, CELL and RTL-SDR. When none is, the row says so.

## How to use it

Open **HOME > FIELD > NOTES**.

### The list

| Button | Key | Does |
|---|---|---|
| NEW NOTE | N | Starts a note. Type the title first |
| FIND | F | Shows only notes whose title contains the text. Press again to show ALL |
| OPEN | O or Enter | Opens the selected note |
| COMPASS APP | C | Opens COMPASS |

- Up / Down select a note. Tap a note to select it, tap it again to open it.
- Tap the `^` or `v` at the right edge to page through the list.
- Left / Right or Tab move between the buttons. Enter presses the highlighted one.

### Reading a note

| Button | Key | Does |
|---|---|---|
| EDIT | E | Opens the editor |
| MAP PLACE | M | Opens MAP at the note's place |
| GO TO COMPASS | G | Sets the place as the COMPASS target and opens COMPASS |
| TRASH | T | Press twice (SURE? TAP AGAIN) within four seconds to move the note to trash |
| BACK LIST | B, Backspace or Esc | Back to the list |

- Up / Down scroll. Tap the top or bottom half of the page to page up or down.
- Checklists: tap the box to tick it. On the keyboard, Space selects the next item and Enter ticks it.
- MAP and GO TO work when the note has a place: a GPS, MAP or FIX line, or a BEARING line that says where it was taken from.

### Editing

| Button | Key | Does |
|---|---|---|
| DONE (SAVE) | Esc | Saves and returns to the reader |
| INSERT DATA | F1 | Opens the INSERT AT CURSOR list |
| KEYS (ON / OFF) | | Shows or hides the touch keys. Reads KB ON while a keyboard is attached |

- Arrows move the cursor. F5 / F6 page up and down. Tab adds two spaces. F2 saves without leaving.
- Tap the text to move the cursor there.
- The note also saves itself after 20 seconds without an edit.

### INSERT AT CURSOR

| Row | Adds |
|---|---|
| Time | Now, in UTC when the clock is set |
| GPS position | Fix, altitude, HDOP and satellites |
| Heading | Compass heading, magnetic and true, with tilt |
| Map picture | Opens a map to frame, then saves it beside the note |
| Sensors | Motion, magnetic field and temperature |
| Checklist item | `- [ ] ` |
| Heading line | `## ` for a section title |
| Divider | `---` |
| Radio: ... | What that radio is reporting now |

A GPS line is written only from a fix under three seconds old. Otherwise it says there was no fix.

### Map picture

| Button | Key | Does |
|---|---|---|
| SAVE PICTURE | S | Saves the map as PNG beside the note and adds a MAP line |
| ZOOM - / ZOOM + | - and + | Zooms out or in |
| HERE GPS | H | Centres on the current GPS fix |
| CANCEL | X, Backspace or Esc | Back to the editor |

Arrows pan the map. A tap on the map pans toward that side. When there is no map frame to save, the place is still added as a MAP line.

## What the screen shows

- **List**: NOTES and the note count. Each row shows the title, the date, and badges for what it holds: GPS, MAP, RADIO, BEARING, SENSORS. A checklist count such as `[2/5]` sits at the right.
- **Reader**: `#` titles in colour, data lines as coloured tags (TIME, GPS, MAP, HEADING, SENSORS, RADIO, BEARING, FIX), checklist items as `[ ]` and `[x]`. The first line with a place gets a live map under it, with the place marked.
- **Editor**: NEW NOTE or EDIT, the text with a blinking cursor, and a footer with the save state and the character count.

## Tips and limits

- Keep an SD card fitted. Notes and pictures are saved there.
- A note holds up to 16 KiB of text. The list shows up to 200 notes.
- An inserted reading is a record of that moment. It does not update later.
- If a save fails, the edits are kept on screen. Press DONE to try again.
- Notes are plain text and can be read in FILES or on a computer.

## More guides

- [Field and lab guide](../FIELD_LABS.md)
- [COMPASS](COMPASS.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
