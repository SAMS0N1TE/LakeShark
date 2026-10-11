# MIX-RF

The short-range radios on the T-MixRF keyboard board: a sub-GHz monitor, a
2.4 GHz survey and NFC.

| | |
|---|---|
| HOME group | RADIO |
| Radio | Keyboard board: CC1101, nRF24 and NFC |

## What it does

MIX-RF checks the keyboard board's three radios and runs an observation mode
on each:

- **MONITOR** measures channel energy on the CC1101 at 315, 433.920, 868.350 or 915 MHz.
- **2.4 SCAN** samples the nRF24's received-power detector across 2400 to 2483 MHz.
- **NFC** has two modes. SCAN sends NFC-A card requests at 13.56 MHz and listens for replies. FIELD watches passively for an external reader's field.
- **CARDS** opens the NFC card workbench for reading a card you own.

MARK saves the current observation to NOTES with the board's GPS fix, so a
survey can be placed on the map later. Observation modes keep running when
you leave MIX-RF.

## Radio

RADIO on r picks which of the three keyboard radios the screen shows:
CC1101, nRF24 or NFC. The other radios in the list say "Not on the T-MixRF
keyboard board". VIEW or V steps through the same three views.

PROBE checks the keyboard and its radios. The CC1101 can also be taken by
SUB-GHZ or REC for capture; while it is, MONITOR is greyed and the status
line says who holds it.

## How to use it

| Button | Key | Does |
|---|---|---|
| PROBE | P | Checks the keyboard, CC1101, nRF24 and NFC. Greyed while a mode runs |
| MONITOR | M | Starts or stops CC1101 channel energy |
| CARDS | C | Opens the NFC card workbench |
| MARK | J | Saves the current view's observation to NOTES |
| 2.4 SCAN | S | Starts or stops the 2.4 GHz survey |
| VIEW | V | SUB-GHZ, 2.4 GHz, NFC |
| NFC | N | Picks OFF, SCAN or FIELD. F switches FIELD on and off |
| STOP ALL | X | Stops every mode MIX-RF started |
| RADIO | R | Picks the radio to view |
| OPTIONS | O | In the CC1101 view: RECEIVE BAND (315, 433.920, 868.350 or 915 MHz) |

TAB, LEFT and RIGHT walk the buttons; ENTER presses the focused one. Touch
works on every button.

**Card workbench**

| Button | Key | Does |
|---|---|---|
| SCAN | I | Reads the card's UID, ATQA and SAK |
| READ | R | Reads blocks with Key A and Key B |
| STOP | X | Stops and keeps the blocks already read |
| VIEW | V | Switches between the block map and the hex view |
| KEY A / KEY B | A / B | Sets a 12-digit hex key. The default is FFFFFFFFFFFF |
| SAVE | S | Writes a text capture to `/sdcard/nfc` |
| MARK | J | Saves a card note to NOTES |

Arrows move between blocks. ENTER shows the selected block's bytes. Comma and
period page through the map. Tap a block to select it. ESC returns to MIX-RF.

## What the screen shows

- **KEYBOARD RADIOS / RECEIVE**: whether the keyboard is detected and powered, and each radio's probe result.
- **MONITOR / SURVEY / 13.56 MHz**: the frequency, sweeps and hits, or polls and replies, for the view in front of you.
- **ENERGY**: the CC1101 level in dBm, the 2.4 GHz channel being sampled, or the NFC state.
- **CHANNEL ENERGY**: a trace of recent CC1101 readings.
- **2.4 GHz ACTIVITY**: activity per MHz from 2400 to 2483.
- **NFC history**: the last 32 probes, newest at the bottom. Bar length is the raw level 0 to 15. ! is noise, R a reply, C a confirmed card.
- **CARD WORKBENCH**: UID, ATQA and SAK, blocks read, and authentication counts. Block colours: green read, cyan active, yellow authentication failed, red error, purple NAK. Unread bytes show as ??.

## Tips and limits

- A passing PROBE shows the registers answered. Reception still depends on the antenna.
- CC1101 levels and the nRF24 threshold (about -64 dBm) are estimates, not calibrated readings.
- MONITOR and 2.4 SCAN measure energy only. Packets are not decoded.
- FIELD sees a reader's field, not a passive tag. SCAN and FIELD do not run together.
- The workbench reads MIFARE Classic Mini, 1K and 4K memory. Other NFC-A cards are identified but not read.
- The workbench does not write cards or change their keys.
- Use STOP ALL before unplugging the keyboard board.

## More guides

- [Keyboard radio monitor](../MIX_RF.md)
- [Field and lab guide](../FIELD_LABS.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
