# METERS

A receive-only list of utility meters that broadcast their reading: Itron ERT
meters on 902-928 MHz. It shows each meter's ID, its reading and how fast it is
changing.

| | |
|---|---|
| HOME group | RADIO (inside the LABS folder) |
| Radio | RTL-SDR or HackRF; LR2021 |

## What it does

METERS listens for Itron ERT meters of three kinds: SCM, SCM+ and IDM. It decodes
each message and files it by meter type and ID. Each meter keeps its latest
reading, when it was last heard, and a short history. From that history METERS
works out the use per hour and per day, and draws the use as braille bars.

The big panel shows the selected meter: the register in large figures with its
last digit lit, the rate per hour and per day, the ID, how many times it was
heard, and the use over time. The HEARD list has two lines for each meter:
its name, type and ID on the first, a mechanical counter, a small use strip and
the rate on the second. A lamp in the header lights for two seconds when a
reading lands.

Meters heard by SUB-GHZ WATCH or by the LR2021 LR433 session appear here too.
METERS only listens; it never transmits.

On HOME, METERS is a tile inside the LABS folder.

## Radio

R opens the radio picker. METERS starts from the radio SUB-GHZ READ uses, but
your choice here does not change SUB-GHZ.

- **RTL-SDR or HackRF**: METERS uses REC's stream. A capture sees only 0.25 MHz of
  the 902-928 MHz band, so METERS sits on one centre, or hops between five.
- **LR2021**: runs the LR433 ERT session at 912.600 MHz, 2.2 MHz wide.

Any other radio shows "cannot hear ERT; pick an SDR or the LR2021". If SUB-GHZ
WATCH is using the SDR, METERS asks you to stop it first.

Opening METERS starts listening. Leaving it stops the receiver.

## How to use it

| Key | Touch | Action |
|---|---|---|
| R | RADIO | Pick the radio |
| L | LISTEN, PAUSED | Start or pause listening |
| Up, Down, [ ] | Tap a meter in the list | Select a meter |
| P | PIN, UNPIN | Pin or unpin the selected meter |
| N | NAME | Name the selected meter |
| O | OPTIONS | Open OPTIONS |
| Esc | | Back to HOME |

Pinned meters sort first, then the most recently heard.

NAME offers HOUSE, NEIGHBOUR, GARAGE, SHOP, WATER, GAS and POWER. Choose
"(none)" to go back to the ID.

OPTIONS holds:

- **CENTRE MHz** (SDR only): 912.6, 911.0, 914.6, 916.5, 919.5 or SCAN. 912.6 is the default. SCAN visits all five.
- **SCAN DWELL** (SDR only): 10 to 300 seconds at each centre, in steps of 10. The default is 30.
- **SHOW**: ALL or PINNED.
- **CLEAR**: forgets every meter that is not pinned or named.

## What the screen shows

**The header** shows the radio and frequency, LISTENING or STOPPED, the number of
meters and readings, and the receiver state. On an SDR the state reads bursts and
decoded counts. With nothing heard, a drawing of a meter face shows what is
happening: the frequency, and a note that meters send every 30 seconds to a few
minutes.

**The type** has its own colour everywhere: SCM green, SCM+ cyan, IDM magenta.

**Use** needs two readings at least a minute apart. Until then the panel says so.
The rate needs at least a minute of history. A reading that has not moved is
still recorded about every ten minutes, so a quiet stretch counts as zero use.

On a wide screen the list and the detail sit side by side. On a narrow one the
detail sits above the list. If the screen is too small it asks for more space.

## Tips and limits

- METERS shows the meter's own counter. It does not convert to kWh, gallons or cubic feet.
- It holds up to 24 meters. When full, it drops the oldest meter that is not pinned.
- Readings stay in memory only. Pins and names are saved on the card in `/sdcard/lakeshark/meters.json`. GPS is not used.
- A meter only sends every 30 seconds to a few minutes, so a new meter can take a while to appear. On an SDR, hopping between centres makes it slower.
- From TERMINAL: `meters` lists them, `meters pin <id>`, `meters unpin <id>`, `meters name <id> <text>`.

## More guides

- [LORA LABS](LORA_LABS.md)
- [SUB-GHZ](SUBGHZ.md)
- [REC](REC.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
