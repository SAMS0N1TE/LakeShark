# PINBALL 2.1

Two machines, with the original 2D renderer and two lap3d views. Requires API 1.2 firmware
(the same engine used by ORBIT and DICE 3D). It is fast on firmware whose
lap3d redraws only what moved and whose screen pushes only changed cells; older firmware still
runs it, redrawing the whole table every frame.

- **REEF CLUB:** wood cabinet, warm lamps, green playfield and three pop
  bumpers. Clear the three drop targets for a 5,000-point jackpot.
- **ION STORM:** dark cabinet, cyan light strips, violet playfield, four
  pop bumpers with domed crowns and a swinging spinner. Each spinner hit scores
  150; every fifth hit awards another 2,500 before the multiplier.

Both machines retain the MESH target bank, rollover lanes, ball save,
CHILL/NORMAL/ARCADE pace settings, tilt and physical plunger controls.
The MESH bank awards 3,000, raises the multiplier and restores the save.

Choose a machine and pace on the title screen, then tap the glass to play.
Hold the left/right half for that flipper. With the ball in the shooter lane,
hold right to pull the plunger and release to shoot. Lean to steer; shake to
nudge. Three nudges within four seconds tilt the machine. Tap **[3D]**,
**[TOP]** or **[2D]** in the top left to cycle through perspective, the steeper
aiming view and the original 2D style. Changing the view keeps the current game.
The selection is remembered. Tap **EXIT** at the top right to return home.

The app uses the full display. In 3D, scores, ball count and multiplier live
in the cabinet's backbox; instructions and awards scroll across its marquee.
The 3D view uses an RGB565 image to preserve the renderer's individual colours
instead of reducing each six-dot terminal cell to two colours. The 2D view
retains the original dot packing and artwork.

Keyboard: 1/2 or up/down selects the machine; left/right selects pace.
Enter/space starts and launches. Left/Z/A and right/M/L operate flippers.
V changes camera; P shows the frame interval, the 3D engine's time and the dots it redrew. After a game, tap to return to the machine selector.

Records are separate for each machine and pace. `machines.txt` remembers
them and the last selection. Old `best.txt` records migrate to REEF CLUB;
the original file is retained.

Build from the repository root:

```
python app-sdk/lap.py build app-sdk/apps/pinball -o pinball.lap
cmake --build bench/build --target test_pinball lssim
ctest --test-dir bench/build -R "^test_(pinball|lap3d.*)$" --output-on-failure
```

Copy the app to `/apps/pinball.lap` on the SD card. An unsigned local build
uses the firmware's normal first-open trust prompt. This app is not a
firmware update.

`test_pinball` runs the production game and real renderer with deterministic
input, clock and card stubs. It checks selection, charging and release,
spinner scoring, tilt scoring suppression, new-game resets, score migration,
record isolation, failed-write retry and 2,700 physics frames. It renders both
machines and all three views at three screen sizes, checking that the shooter
lane stays visible and that geometry capacity does not overflow. Physical
touch, motion, audio, haptics and device frame rate require board testing.
