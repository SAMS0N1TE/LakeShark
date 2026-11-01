# NFC

A card workbench for MIFARE Classic cards you own: identify a card, read its
blocks with keys you enter, see what was verified and save the result to the SD
card.

| | |
|---|---|
| HOME group | RADIO |
| Radio | NFC reader on the T-MixRF keyboard board |

## What it does

NFC opens from MIX-RF with CARDS (key C). It needs the keyboard board with its
NFC reader answering. SCAN selects the card in the field and shows its UID,
ATQA and SAK. READ then walks every sector and tries Key A, then Key B, for
each one. The blocks it can verify are drawn on a map, and the bytes of any
block can be opened in a hex view.

Three card types are read, told apart by the SAK byte: Classic Mini (20
blocks), Classic 1K (64 blocks) and Classic 4K (256 blocks). Any other NFC-A
card is identified, and the screen says its memory protocol is not supported.
No blocks are invented for it.

The workbench never writes to a card, changes its keys, emulates a card or
searches for keys. It only tries the keys you give it.

## Radio

NFC uses the NFC reader on the keyboard board at 13.56 MHz. There is no RADIO
picker here. In the workbench, R means READ. Use PROBE in MIX-RF to check that
the reader answers. Stop MONITOR and 2.4 SCAN first: SCAN, READ and SAVE are
refused while the CC1101 or the nRF24 is running, and the status line says so.

## How to use it

| Key | Touch | Action |
|---|---|---|
| I | SCAN | Select the card and read its UID, ATQA and SAK |
| R | READ | Read every sector with Key A, then Key B |
| X | STOP | Stop reading and keep the blocks already verified |
| V | VIEW | Switch between the block map and the hex view |
| A | KEY A | Enter a Key A as 12 hex digits |
| B | KEY B | Enter a Key B as 12 hex digits |
| S | SAVE | Write a text capture to `/sdcard/nfc` |
| J | MARK | Save a card note to NOTES |
| Arrows | Tap a block | Select a block. Up and Down move one row |
| Enter | | Press the focused button. With none focused, switch between map and hex |
| Tab | | Move focus along the buttons |
| , and . | PREV PAGE, NEXT PAGE (portrait) | Page through the map |
| Esc | BACK | Back to MIX-RF |

Keys start as `FFFFFFFFFFFF`. Before a card is identified, KEY A and KEY B set
that default. After SCAN, they set the key for the sector of the selected
block only. Keys live in memory: they are not written to the card, the SD
card or NOTES. Entering a key is refused while a scan or read is running.
Press READ to use a new key. A different card clears the per-sector keys.

SAVE and MARK are greyed until a card is identified. SCAN, READ and the key
buttons are greyed while the reader is busy, and STOP is greyed when it is not.

The workbench has no OPTIONS.

## What the screen shows

**The header** shows the UID, ATQA and SAK, then blocks read out of the card's
total, authentications that worked out of those tried, and the error and NAK
counts. A bar fills as sectors finish.

**The block map** shows each block as `[NN]` in hex, with the selected one as
`>NN<`. Colours:

- **Grey**: not read.
- **Cyan**: being read now.
- **Green**: read and verified.
- **Yellow**: authentication failed. Neither key worked.
- **Red**: the read failed.
- **Purple**: the card answered with a NAK.

The line under the header names the selected block, its sector, whether it is
the manufacturer block, a trailer or data, and whether Key A and Key B were
verified for that sector. On a wide screen a SELECTED BLOCK panel shows its
bytes beside the map.

**The hex view** shows the 16 bytes of the selected block with an ASCII
column. `??` marks a byte that was not read or cannot be read. For a trailer
with valid access bits it also shows the access bit groups and the general
purpose byte.

**The status line** at the bottom says what the reader is doing, or why an
action was refused.

## Saved files

SAVE writes `/sdcard/nfc/card-XXXXXXXX-XXXXXXXX.txt`, with a random name each
time. The file starts with `LakeShark NFC capture v1`, then the UID, ATQA,
SAK, block count and blocks read. After that comes one line per block: the
block number, its state number and 16 bytes. Unread bytes are written as `??`.
A partial read is saved as partial.

If you hold a different card to the reader while a read is not yet saved, the
workbench keeps the earlier data and asks you to SAVE it first.

MARK adds a note to NOTES with the UID, ATQA, SAK, blocks verified and the
authentication counts.

## Tips and limits

- Use cards you own or have permission to read.
- Key A can never be read back from a card. It shows as `??`, and so does
  Key B unless the sector's access bits allow it to be read as data.
- A successful authentication does not mean every block in the sector can be
  read. A block that fails shows red, or purple if the card sends a NAK.
- If the card cannot be selected, the earlier capture is kept and the status
  line says so.
- STOP keeps what was verified. Nothing is written to the card at any point.
- Cards that are not Classic Mini, 1K or 4K are identified only.
- SAVE needs the SD card. If it fails, the data stays in memory and the status
  line says so.

## More guides

- [MIX-RF](MIXRF.md)
- [Keyboard radio monitor](../MIX_RF.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
