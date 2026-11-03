# Pure app libraries

Include `lap_loop.h`, `lap_json.h` or `lap_csv.h`. `lap.py`, `lsdk` and both
simulator builds compile `app-sdk/lib/*.c` alongside app sources; section GC
discards unused functions. These helpers add **no firmware exports** and use
**no dynamic allocation**. Project sources may be top-level C or under `src/`.

SDK 2.5 also supplies `lap_assets.h` (budgeted PNG/L3M1 decoding and async SD
loads), `lap_game.h` (sprites/maps/collision/particles/shake/fades) and
`lap_music.h` (four-channel LTM1 tracker on the Phase 2 PCM mixer). These use
caller-owned memory; the asset file and music adapters import the existing
SD/mixer services. See [games/assets contracts](../docs/GAMES_ASSETS.md) and
the `arcade2d` / `flight3d` templates. No new firmware exports are added.

`lap_loop_init(&loop, 60, 8, lap_millis())` sets 60 updates/second and at most
eight catch-up updates per draw. `lap_loop_advance(&loop, lap_millis(), &alpha)`
returns how many fixed updates to run (each with `dt = 1.0f / loop.hz`); draw
between the previous/current states using `alpha`. Catch-up beyond the budget
is discarded. The rational millisecond accumulator supports rates such as
60 Hz without rounding the step to 16 ms. Millisecond wrap is supported.

Feed focus changes from API 2.3 system events to
`lap_loop_focus(&loop, focused, now_ms, api_major, api_minor)`. The helper
pauses and clears accumulated time on focus loss and resets time on regain.
For targets without API 2.3 events, guard the event adapter with the header's
API version macro; the helper itself ignores focus notifications for versions
before 2.3. Include `lakeshark_app.h` before `lap_loop.h` to enable the
`lap_loop_event(&loop, &event, now_ms)` adapter under `LAP_API_23`; it handles
FOCUS_LOST/GAINED and PAUSE/RESUME without taking events from your queue. Explicit `lap_loop_pause` also supports menus and app hide/show.
There is no implicit event polling or newer runtime import in this library.

`lap_json_read` validates one complete document into caller-provided tokens.
Containers and keys have parent indices; string spans exclude quotes and
preserve escapes. Token exhaustion returns -2; invalid syntax or nesting over
32 returns -1. Duplicate keys remain separate tokens: schema code chooses
whether to reject them. The reader preserves UTF-8/Unicode escape bytes and
does not normalize or decode strings, or validate UTF-8 encoding. `lap_json_string` escapes a string into
a caller-provided writer; `lap_json_raw` appends caller-controlled JSON syntax
or literals. Check every result or the sticky `writer.failed` flag. Validate
the completed document before using it when assembling untrusted fragments.

`lap_csv_next` reads one field at a time, decoding doubled quotes and keeping
commas/newlines inside quoted fields. `end_record` distinguishes a comma from
a record boundary. Empty and trailing fields, CRLF and LF are supported;
unterminated quotes, quotes inside unquoted fields and buffer overflow fail.
The reader position is unchanged on failure. `lap_csv_write` quotes fields as
needed, doubles quotes and appends a comma or CRLF. It leaves the writer
unchanged on capacity failure. Both APIs terminate successful output with NUL.

Run host boundaries/round trips with:

```text
python -m unittest discover -s app-sdk/tests -p test_phase1.py
```
