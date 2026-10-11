# SWEEP

A passive scanner for nearby Bluetooth and Wi-Fi broadcasts: trackers, Remote
ID drones and camera or bodycam hints, with a mode that helps you walk toward
one.

| | |
|---|---|
| HOME group | FIELD |
| Radio | Bluetooth and Wi-Fi (the co-processor on the board) |

<img src="../screenshots/tdp4/motion/sweep.gif" width="300" alt="SWEEP listing nearby trackers">

## What it does

SWEEP listens to Bluetooth adverts and the Wi-Fi scan results the board
already has. It matches them against rules and sorts what it hears into five
categories: TRK trackers (such as Apple Find My, Google Find Hub, Samsung
SmartTag and Tile), CAM cameras, BWC bodycams, UAS drones and ATK attack tools.

LIST ranks the contacts. HUNT follows one contact's signal strength so you can
walk closer, and FIND hands it to COMPASS for a bearing. SWEEP only listens;
it never transmits.

## Radio

SWEEP uses Bluetooth on the Wi-Fi and Bluetooth co-processor, and reads cached
Wi-Fi scan results. It does not start its own Wi-Fi scan, so LINK keeps
scanning and joining networks while SWEEP runs. It has no RADIO picker.

## How to use it

| Key | Touch | Action |
|---|---|---|
| S | START, STOP | Start or stop receiving. SWEEP remembers this after a restart |
| Up, Down, [ ] | Tap a contact | Select a contact |
| Enter, H | SELECT | Pick the selected contact |
| V | VIEW | Switch between LIST and HUNT |
| F | FIND (in HUNT) | Hand the contact to COMPASS FIND |
| B | Tap the AVERAGE row | Switch FAST (2 s) and SLOW (8 s) averaging in HUNT |
| M | MUTE | Turn alert sounds on or off. They start muted |
| O | OPTIONS | Open OPTIONS |
| Esc | | Back to HOME |

OPTIONS holds:

- **CATEGORIES / MUTE**: CAMERA, BODYCAM, DRONE and TRACKER can each be OFF,
  SOUND or MUTED. ATTACK (OPT IN) is off until you turn it on.
- **PRIVACY / LOGGING**: LOG ALERTS TO SD is off by default. When on, it writes
  uptime, address, category and signal to `/sdcard/sweep/alerts.csv`, with no
  location and no broadcast payload. RECEIVE ONLY is always on.
- **DISPLAY / FILTER**: show ALL or one category.
- **RELOAD SD RULES**: read `/sdcard/sweep/rules.json` again. The board writes
  the default rules there the first time.

In a list, Esc or BACK goes up one level.

## What the screen shows

**The header** counts each category, shows adverts per second and the GPS
state, the view (LIST or HUNT) with SOUND or MUTED, and the receiver state.
FLOOD appears during an advert flood.

**LIST** groups contacts:

- **WITH YOU**: a tracker reported as separated from its owner that stays
  with you for ten minutes. With a GPS fix it must also move with you at least
  500 m or across three places. A GPS-backed WITH YOU is the one red alarm.
- **NEW**: first heard in the last two minutes.
- **PASSING**: everything else.

Each row shows an ID, the label, owner state (sep or near), a signal meter,
dBm, a trend mark and age, with the last minute as a strip. Trackers near
their owner rotate their address, so SWEEP counts them on one line as
owner-nearby tags.

**HUNT** shows a large dBm readout on a cold-to-hot gauge, CLOSER, FARTHER or
STEADY over the last 10 seconds, the peak held for 15 seconds, and the last 60
seconds with high, low and gaps. TARGET lists vendor, address, owner state,
advert count and when it was last heard. For a drone with a GPS fix, it also
shows range and bearing to the aircraft and operator positions it reports.

## Tips and limits

- Names and signatures are hints, not proof of what a device is.
- Signal strength guides you closer. It is not a distance or a bearing.
- GPS is used for movement counts only. SWEEP keeps no coordinates.
- From TERMINAL: `sweep on|off|status|stats|list|hunt MAC|mute on|off|rules reload`.

## More guides

- [SWEEP guide](../SWEEP.md)
- [DRONES](../DRONES.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
