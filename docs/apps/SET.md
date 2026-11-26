# SET

Volume, brightness, mute and screen lock, with pages for the display, sound,
power, radio, network and system.

| | |
|---|---|
| HOME group | SYSTEM |
| Radio | None |

## What it does

SET keeps the settings you change often on its first page. The rest sit one
level down in DISPLAY, SOUND, POWER, RADIO, NETWORK and SYSTEM. Every setting
is a box: press it to change it. Changes apply at once and are kept after a
restart.

SET is on the tab strip, so F4 (or the SET tab) opens it from any app. It
always opens on its first page.

## Radio

None.

## How to use it

| Key | Touch | Action |
|---|---|---|
| Up, Down | | Select a box |
| Enter | Tap a box | Change the setting, or open a page |
| Left, Right | Tap the left or right half | Step a level down or up |
| Esc, Backspace | BACK | Return to the first page |

Esc on the first page goes back to HOME.

### First page

- **Volume**: 0 to 100 percent in steps of 5.
- **Brightness**: 5 to 100 percent. Left and Right step by 10. It never goes
  fully dark.
- **Mute**: sound on or MUTED.
- **Screen lock**: locks the screen now. Radios keep running. With no PIN,
  tap UNLOCK twice. With a PIN, type it.
- **Rotate lock**: on holds the current orientation. Attaching the keyboard
  still turns the screen.
- **Display**, **Sound**, **Power**, **Radio**, **Network**, **System**: open
  their pages.

### DISPLAY

- **Theme**: Terminal Bay, Amber, Phosphor, Ice, VFD, Synthwave, Arcade and
  Night. F9 also steps the theme from any app.
- **Daylight**: black text on a white background for bright sun. Your theme
  choice is kept and returns when Daylight is off.
- **Font**: 10x17, 9x16 or 15x26. A larger font fits fewer characters on the
  screen.
- **Auto dim**: on or off.
- **Dim after**: 15, 30, 60, 120 or 240 seconds.
- **Screen off**: never, or 15 s to 10 min after the screen dims.
- **Lock PIN**: set or change a PIN of 4 to 8 digits.
- **Lock when**: with a PIN, lock when the screen goes off and at boot, only
  one of them, or by hand only.
- **Remove PIN**: asks for the PIN first.

### SOUND

- **Boot sound**: off, chirp or spoken. Spoken is offered when the build
  has speech.
- **Voice**: opens VOICE options: the voice, VOICE LEVEL, SPEAKER, TEST,
  ADS-B CALLOUTS and MESH MESSAGES.
- **Alert sound**: on or off.
- **Notify sound**: the sound for ordinary notices. A tap plays the next one.
- **Vibrate**: all vibration, taps, alerts and detections.

### POWER

The top of the page shows the battery and its current. It reads BATTERY, ON
USB or CHARGING with the percentage, or ON USB, FULL. When plugged in, the
status bar shows USB, CHG or FULL in place of the percentage.

- **Power mode**: FULL, SAVER (half brightness, dims at 30 s) or WATCH (also
  dark after 20 s).
- **Watch keeps**: what runs while WATCH has the screen dark: EVERYTHING,
  MESH, P25 or GPS LOG. A touch brings the rest back.
- **Keyboard light** and **Keyboard dim** ("with screen" dims the keyboard
  light with the screen).
- **Service 1 to 4**: an app's background service, shown only while one
  runs. A tap stops it.

### RADIO

- **LoRa radio**: the board's own socket, or a module on the header.
- **LoRa module**: which T-LR2021 is on the header.

A change to either waits for a restart.

### NETWORK

- **Hotspot**: the board's own Wi-Fi name and password. Tap twice for a new
  password.
- **Update**: shows the update state and opens UPDATE.
- **Check for updates**: Daily or Off. Off stops background checks; CHECK in
  UPDATE still works.
- **GPS**: on keeps the GPS running from power-up; off leaves it to the apps
  that need it.
- **Data packs**: opens PACKS. **Update packs** refreshes the packs you
  installed when a newer list is out.

### SYSTEM

- **Language**: English or Ukrainian.
- **Time zone**: AUTO, UTC or a named zone. AUTO picks the zone from where
  the GPS last had a fix, and the box shows the zone it picked. Left and
  Right step through the list.
- **Clock**: 12 h or 24 h.
- **USB autoreboot**: restart when a PC flashes the board over USB.
- **Bug report**: opens the report sheet, as in DIAG.

The clock keeps time across a restart, and GPS sets it when there is no
network.

## What the screen shows

The title shows the page. Each box has its name on the top edge and its value
in the middle. Levels have < and > keys at either end. A page shows a live
panel above its boxes. In landscape the boxes sit in two columns.

## Tips and limits

- F11 rotates the screen from any app. F10 opens help.
- Under Daylight the Theme box reads "after Daylight", meaning the theme
  returns when Daylight is turned off.
- MAP follows the theme, Daylight included.

## More guides

- [T-Display-P4 screens](../TDP4_SCREENS.md)
- [Updates and calls](../TDP4_UPDATES_AND_CALLS.md)
- [DIAG](DIAG.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
