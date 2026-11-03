# @NAME@

Build/run with `lsdk build` / `lsdk sim`, or open in Studio. Held arrows/WASD
or left mouse steer; R centres. Resize/focus changes use the fixed-step loop.

`assets/ship.obj` becomes `ship.l3m` via `obj2l3.py --binary`. Copy the latter
to `/sdcard/apps_data/@ID@/ship.l3m` for a runtime override. Without SD the
embedded mesh runs immediately. Source budget 8192, arena 16384 bytes
including fallback; reserve larger lap3d capacities before drawing larger
models. The starter uses a uniform scene material for every triangle.
See `app-sdk/docs/GAMES_ASSETS.md`. Original demo mesh: CC0.
