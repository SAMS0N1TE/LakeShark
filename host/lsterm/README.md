# lsterm

The LakeShark screen in a terminal on your computer. Every app works: keys, mouse clicks, the map, waterfalls.

The board draws the screen. lsterm only carries the bytes, so it also works inside an SSH session.

## Connect

- **USB:** `python lsterm.py` connects to the one board plugged in, or name the port: `python lsterm.py COM5`.
- **Wi-Fi:** on the board, run `term net on`. It prints a password. Then:
  - `python lsterm.py tcp://<board-ip>`
  - or plain `telnet <board-ip> 2323`
  - The link is not encrypted. Use it on your own network. `term net off` turns it off.
  - **A phone** (Termius or any telnet app) works too. The board asks the app for its window size. When the window is smaller than the grid, you see the part that fits, with a bar on the bottom row: tap **^ UP** or **v DOWN** (and **<** or **>** in a narrow window) to move. A mouse wheel scrolls it too. The middle shows which rows you are looking at. Fine pixel art comes as quadrant blocks, which every font has.
- **No board:** `python lsterm.py --sim`. This runs the firmware's own screens in the simulator, which needs `bench/build/lssim.exe` built.
  - A native app: `python lsterm.py --sim lap:carrier`. Add `--grid 160x45` for a desktop-sized screen, like a Nano's `panel term`.

**Keys:**
- Ctrl-] leaves.
- Ctrl-L repaints.
- Ctrl-P takes a snapshot, Ctrl-R starts and stops a recording.
- Ctrl-T console, Ctrl-F files, Ctrl-G help, Ctrl-K back to the board.
- F1 to F11, the arrows, Enter, Esc and Tab do what they do on the board.
- A mouse click is a tap. Holding the button and moving is a drag, as a finger on the glass: sliders, and the stick in a game.

## The frame and its panel

Over USB the board's screen sits in a border, with a panel beside it (or under it, in a window too narrow for both). The panel is drawn the way the board draws, in the board's own colours, so it follows THEME and DAYLIGHT. Keys light under the mouse and flash when pressed. `--plain` shows the screen alone.

A click on the board's screen gives the board the keyboard; a click in the panel, or its keys, gives it to the panel.

**CONTROL** shows the link, the firmware, updates a second with a graph of the data rate, and the screen, sound and daylight as the board has them.

- **SNAPSHOT** (Ctrl-P): the screen as lsterm has it, as `.png`, `.ans` (24-bit ANSI: `cat` it in a terminal) and `.txt`.
- **DEVICE PNG:** the panel's real pixels, from the board. It takes a few seconds.
- **RECORD** (Ctrl-R): an [asciinema](https://asciinema.org) `.cast` of the board's stream, and an animated `.gif` made from it at 5 frames a second when it stops. A turn of the screen ends a recording.
- **SCREEN OFF:** the board's screen dark with everything still running, to save the battery while you work from here. A touch on the board lights it again.
- **MUTE, VOL - and +, BRIGHT - and +, THEME** (F9), **DAYLIGHT, ROTATE, REDRAW** (Ctrl-L).
- **YOUR BUTTONS:** your own, from `~/.lsterm.json`. ADD BUTTONS writes an example and opens it; RELOAD reads it again. Each button has a `label`, and `send` (a console line, or a list of them) or `keys` (`F1`-`F11`, `ENTER`, `ESC`, `TAB`, the arrows, or text typed on the board). SAVE AS BUTTON on the console line makes one from what you typed.
- **NOW:** what the open app is for and its keys, from the board, changing as you move between apps.

**CONSOLE** (Ctrl-T) is the board's console without leaving the screen. Suggestions appear as you type and Tab completes commands, their words (`display o` offers `on` and `off`), `call` actions and card paths after `sd ls`, `cat`, `get` and `rm`. Up and Down go through what you typed before, Esc gives the keyboard back to the board. The lines lsterm sends for itself stay out of it.

**FILES** (Ctrl-F) is the SD card. Up and Down move, Enter opens a folder or a file (downloaded, then opened with whatever your computer opens it with), Backspace goes up. **G** gets a file into `LakeShark captures/files` (its CRC is checked), **P** puts a file from this computer on the card (to the top or one folder down; Tab completes the path), **N** makes a folder, **Del** twice deletes. A transfer shows its progress. Uploads go about 5 KB a second, so they are for settings and small files, not maps.

**HELP** (Ctrl-G) follows the open app: its own help from the board, then its guide from `docs/apps` (OPEN GUIDE opens the file), then lsterm's keys.

Captures go to `LakeShark captures` in your home folder. The `.png` and `.gif` need `pip install pillow`; without it a snapshot is the `.ans` and `.txt`. CONSOLE, FILES, DEVICE PNG, SCREEN OFF and MUTE need firmware from this change or later.

## Terminal

- **Colour:** it needs 24-bit colour. Windows Terminal, iTerm2 and most Linux terminals have it.
- **Font:** any monospace font works. lsterm draws the board's fine pixel art (map markers, routes, icons) with quadrant blocks, which every font has. If your font has the Unicode sextants, `--sextants` shows the finer 2x3 version; a font without them shows a box for each one. The Cascadia Mono that ships inside Windows Terminal has them, and `python lsterm.py --install-profile` adds a "LakeShark" profile that uses it.
- **Size:** make the window at least as big as the board's grid. That is 118 x 28 on a Nano with no display, and 115 x 27 on a T-Display-P4 in landscape (52 x 70 in portrait; ROTATE or `tui rotate` turns it). The border takes 2 more each way and the panel at least 30 more columns (it uses up to 70); a narrower window puts the panel under the screen if there is height for it, then drops it, then the border.
- **Reset on connect:** opening the port resets a T-Display-P4 (its CH343 does this on Windows). lsterm waits for it to boot, about 10 seconds, then starts.

## A Nano with no display

The ESP32-P4-NANO DSI image runs the full interface even with nothing on the DSI connector. The terminal is its screen.

- `panel set term` chooses this on purpose.
- `panel term 160x40` makes its grid 160 columns by 40 rows (40-200 by 12-60), then reboot. `panel term` shows the size, `panel term default` goes back to 118 x 28.
- `panel none` goes back to the plain headless board.

USB needs `pip install pyserial`.

## In a browser window

`python lsweb.py` (or `python lsweb.py COM5`) serves the board's screen on http://127.0.0.1:8770/ over USB. Nothing listens beyond this computer, so there is no password.

- The page draws the board's own cells with the board's own font (`lsfont.bin.gz`, made by `lssim atlas`), so it shows exactly what the glass would, at every frame the board draws.
- On a Nano with no display the board's grid follows the window: resize it and the screen fits.
- Mouse: click to tap, hold and move to drag, the wheel scrolls. Keys go to the board.
- `?fit=0` on the address keeps the board's grid as it is.

## LakeShark on the desktop (one click)

`python lsdesk.py --install` puts a **LakeShark** shortcut on the desktop and in the Start menu. Clicking it:

- finds the board: the one used last, else the only one plugged in, else the one that answers as a Nano;
- starts the screen bridge and opens it as an app window of its own (Edge or Chrome, no tabs or address bar);
- when the board is unplugged, the window says so and waits; plug it back in, into any USB port, and it carries on;
- closing the window stops the bridge.

`~/.lsdesk.json` remembers the board (`"board"`, its USB serial number) and lists ports never to open (`"skip"`, COM names or serial numbers). A log is kept in `%LOCALAPPDATA%\LakeShark\lsdesk.log`. `http://127.0.0.1:8770/status` says whether a board and a window are connected.
