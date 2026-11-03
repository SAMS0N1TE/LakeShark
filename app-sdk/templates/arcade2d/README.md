# @NAME@

Build/run with `lsdk build` / `lsdk sim`, or open in Studio. Arrows/WASD or
held touch move. Collect stars. P saves; N changes score slot and demo tune.
Without SD or speaker it uses embedded art and session scores and runs silently.

`assets/sheet.png` contains three 8×8 frames. An override may be copied to
`/sdcard/apps_data/@ID@/sheet.png`; source budget 4096 bytes, decode arena
16384 bytes including the fallback. `assets/arcade.ltm` and `night.ltm`
are hand-written tracker tunes embedded in `assets.inc`.
See `app-sdk/docs/GAMES_ASSETS.md` for formats, budgets and scripts.
Original demo assets/music: CC0. Replace them with your own game assets.
