# Small native apps, shared framework

How a built-in app is put together from the shared pieces, with an example app
that is off by default. Paths below are relative to the repository root unless
linked.

## Framework map

| Concern | Existing code and contract |
| --- | --- |
| Registration | [ls_app.h](../components/apps/tui/ls_app.h), `ls_app.c`: `ls_app_register` registers both directory entry and screen; rejects incomplete `ls_app_doc_t`. Descriptors must live for the program. `LS_TUI_MAX_SCREENS` is 32. Firmware assembly is `main/compact_ui.cpp`; simulator assembly is `bench/tools/lssim.c`. Check registration failures and leave space for SD apps. |
| Lifecycle | [ls_tui_screen.h](../components/apps/tui/ls_tui_screen.h), `ls_tui_screen.c`: the actual names are `enter/draw/key/touch/leave`, corresponding to open/draw/key/touch/close. Router switches screens, delivers cell coordinates, owns global navigation and overlays. Return false for unconsumed keys. Use the same rectangles for drawing and hit testing; do not rely on the list-style fallback for controls. |
| OPTIONS | [ls_options.h](../components/apps/tui/ls_options.h), `ls_options.c`; reference `screens/main/scr_p25.c` (`CTX_P25`, `CTX_P25_POLICY`). One root per app; `LS_OPT_MENU` children, BACK, `LS_OPT_LEVEL` arrows and number entry. Use `LS_OPT_ROWS`, `ls_opt_button`, `ls_opt_key`, `ls_opt_open`; let the router poll overlays. Callbacks read cached state and request changes. |
| VFO/keypad/RADIO | [ls_radio_panel.h](../components/apps/tui/ls_radio_panel.h), `ls_radio_panel.c` render the shared receiver face and dispatch key/touch operations from `ls_radio_view_t`. `ls_radio_select.h/.c` and `ls_radio_select_hw.c` implement RADIO, backed by `components/lakeshark/core/radio_choice.h`. Selection is not a radio lease. |
| Input widgets | [ls_picker.h](../components/apps/tui/ls_picker.h), `ls_numpad.h`, `ls_keyboard.h`, `ls_keydock.h`; `ls_tui_ui.h/.c` provides filled button bars, slots, hit testing, tiles, panels and meters. Picker storage is bounded: check its API rather than building another modal list. |
| Notifications/chrome | [ls_notify.h](../components/apps/tui/ls_notify.h): probes must be cheap, since the router polls them during drawing. `ls_tui_screen.c` owns status/hints; `ls_tui_chrome.h/.c`, `ls_tui_inset.h/.c` and `ls_tui_touch.h/.c` handle layout, cutout and touch geometry. Preserve the full grid; route around the hole. |
| Colour/art/shell | [tui_core.h](../components/apps/tui/tuilib/tui_core.h) supplies `TUI_*`/`TUI_ATTR`; `ls_theme.h` supplies themes. `ls_icons.h`, `ls_glyph.h`, `ls_braille.h`, `ls_stroke.h`, `ls_compass_art.h` provide cell art. `screens/ext/scr_rec.c` and `scr_subghz.c` show top bars and filled tabs; `ls_field.h`, `ls_map_ink.h`, `ls_map_marks.h` and `screens/ext/scr_map.c` show FIELD/map presentation. Compose those primitives instead of adding bitmap assets or copied art tables. |
| Radio ownership | [radio_endpoint.h](../components/lakeshark/radio/radio_endpoint.h) and `radio_endpoint.c`: broker sessions, endpoint capabilities, claim/release and cancellation. `core/app_registry.h` provides receiver switching/parking; `apps/fm/fm_receive.h` shows session cleanup. `ls_tui_radio_want` expresses screen demand, not permission to steal an active receiver. Stop/join producers before releasing their session or storage. |
| Observers/SWEEP | [ble_link_observer.h](../main/ble_link_observer.h) bounds advertisement queuing and dispatch; `main/ble_link.c` fans out to SWEEP, RID and heard-device consumers. `main/ls_sweep_runtime.c` owns observer enable/retry, `tui/ls_sweep_app.h` and `ls_sweep.c` own rule matching, snapshots and notices. This passive nearby-device SWEEP differs from the tuned spectrum sweep in `lakeshark/dsp/ls_sweep.h/.c`. `core/event_bus.h/.c` is another publish/subscribe path; callbacks are not automatically workers. |
| Sources/sinks/detector | [ls_lora_lr20xx.h](../components/lakeshark/board/ls_lora_lr20xx.h): `lr20xx_iq_sink_t` and `lr20xx_stream_source2_live` accept bounded blocks; stop the owner before retuning/freeing sink memory. `apps/rec/rec_catch_core.h/.c` separates detector state and IQ/spectrum inputs from `rec_catch.h/.c` runtime. `tui/ls_wf_source.h/.c` adapts radio data for shared waterfall consumers. There is no single universal source/sink ABI: use the owning subsystem's contract. |
| REC WATCH | [rec_watch.h](../components/lakeshark/apps/rec/rec_watch.h), `rec_watch.c`: source-tagged `rec_watch_observe_from`/`observe_mod`; `rec_watch_runtime.c` queues captures using a bounded free/pending pool and runs storage work on `subghz_io`; `rec_watch_store.c` owns versioned CRC-checked catalog persistence. Producers submit; UI reads snapshots and requests work. Preserve modulation and source identity. |
| Persistence | [settings.h](../components/lakeshark/core/settings.h), `settings.c`, `settings_schema.h/.c`: cached settings and deferred persistence for supported setters (check return values). `ls_nvs_safe.h/.c` dispatches flash work onto a DRAM-stack worker, but `ls_nvs_call` waits: it is not a render-safe asynchronous API. `tui/ls_p25_settings.h` shows the UI-facing boundary. |
| SD/custom apps | [ls_userapp.h](../components/apps/tui/ls_userapp.h), `ls_userapp.c`, `USER_APPS.md`, `lsapp.example`: bounded declarative `.lsapp` loader backed by `ls_value.h`/`ls_action.h`; useful when native C is unnecessary. `apps/p25/p25_profile.h/.c` is a validated versioned **key=value** parser. `ls_sweep.c` loads bounded JSON rules from `/sdcard/sweep/rules.json` with defaults in `data/sweep/rules.json`; `ls_tiles_backend.c` parses bounded catalog JSON. These JSON formats are not a general versioned schema service. |
| Workers | [radio_decode_worker.h](../components/lakeshark/core/radio_decode_worker.h), `apps/fm/fm_rx_worker.h` own decoder task lifetimes. Use `rec_watch_runtime.c` for capture storage, `settings.c` for deferred settings, `main/ls_sweep_runtime.c` for SWEEP, `ls_tiles_backend.c` for map storage/network work. Do not add a task to paint a screen. |

## Make a small instrument app

The [example implementation](../components/apps/tui/apps/example_app.c) remains
one short teaching file (about 180 lines), with the existing public header and
registration entry. It has no persistent preferences, tunable policy or radio.
COUNT and a bounded 24-action history are session state, reset on entry.

- **Emblem:** shared `ls_icon_draw` chip pins surround a block `C` from
  `ls_glyph_draw`. Cyan pins and a blue core become amber after a real action.
  Existing glyph/icon tables hold the static art; no new bitmap/art table.
- **Hierarchy:** cyan instrument title, green three-digit minimum count, dim
  session labels, blue history frame and amber latest sample. The same shared
  glyph helper used by instrument-style UI handles all ten unsigned digits.
- **Meter:** `ls_bar` displays count magnitude: bit length divided by the
  unsigned integer width. LOG2 is labelled explicitly; zero is empty and
  UINT_MAX fills the meter. It never wraps or implies a configurable goal.
- **Pulse:** COUNT and RESET call `ls_fresh_bump`; `ls_fresh_level` highlights
  the core, EVENT lamp and the actual pressed key for about 300 ms (the high
  half of a 600 ms freshness window). Nothing restarts it while idle.
- **History:** one count sample per COUNT/RESET action, including zero on reset;
  the last 24 samples appear oldest to newest, with their scale shown. The
  bounded ring lives in PSRAM via `EXT_RAM_BSS_ATTR`. There is no shared general
  event-chart API, so this small cell trace stays local to the example rather
  than coupling it to radio acquisition or creating another chart framework.
- **Layout:** REC/MUSIC-style stacked panels in portrait, side-by-side in
  landscape; the right glass stops before the landscape camera hole. Shared
  router chrome retains its original position. Empty history explains how to
  start instead of inventing samples.
- **Controls:** shared filled raised keys show C COUNT, R RESET and H HOME,
  their effects and pressed feedback. Shared hit testing and keyboard lookup
  reach the same dispatcher; `.hint` also names all shortcuts.

1. Define static-lifetime screen and documentation descriptors. Say what the
   app does, records and does with GPS. Keep a stable lowercase ID.
2. Draw only cached state into the supplied area. Compose shared art and
   `ls_btn_bar_raised_row`; delegate touch to `ls_btn_hit` and keys to
   `ls_btn_key`, then call one action dispatcher. Give every action a filled
   target at least 3 rows by 8 columns in both orientations and name its key
   in `.hint`. Reset session state in `enter`; release owned resources in
   `leave` (the example owns none, so its leave callback is NULL).
3. Add the descriptor to the registration assembly, not a private screen list.
   The example's `LS_EXAMPLE_APP_ENTRY` is shared by firmware and simulator.
   `components/apps/CMakeLists.txt` already discovers C sources recursively.
4. Enable firmware `CONFIG_LS_EXAMPLE_APP=y` through Kconfig. It defaults off;
   both the implementation and directory entry compile out. For preview use
   `cmake -S bench -B bench/build-framework -G "MinGW Makefiles" -DLS_EXAMPLE_APP=ON`,
   then `cmake --build bench/build-framework --target lssim -j 8`.
5. Run `ctest --test-dir bench/build-framework -R '^test_example_app$' --output-on-failure`.
   The test renders TFT portrait/landscape, checks measured target sizes and
   exercises COUNT/RESET/HOME by both tap and key. Manual renders:
   `lssim example --tft -o example-portrait.bmp` and
   `lssim example --tft -l -o example-landscape.bmp`.

For a receiver app, use RADIO plus the shared radio panel rather than extending
this counter. For settings, add one OPTIONS tree; use MENU/LEVEL and the shared
picker/numpad. For messages, use `ls_notify`. None of those facilities is needed
just to count taps, and the example introduces no replacement for them.

## Add a customizable data schema

New user-editable policy belongs in JSON on SD with defaults under `data/`.
There is currently no universal loader to call. Reuse a subsystem loader if its
format fits; otherwise add a small public parser module plus an existing worker
adapter, not file operations in `draw`, key or touch callbacks.

Example **design**, not a supported file for the counter:
`{"version":1,"label":"FIELD","threshold_db":12}` at
`/sdcard/apps/myapp/config.json`, with `data/apps/myapp/config.json` as default.
Define byte/depth/row/string limits, required fields, integer version, finite
numeric ranges, duplicate/unknown-field policy and migration behavior in the
module header. Reject future versions, duplicate keys, invalid types, non-finite
numbers, overflow, path traversal and oversized/truncated input. A JSON schema
document alone is insufficient: enforce these checks in the C parser.

Read on the owning worker into bounded PSRAM staging, validate the **entire**
document, then publish a complete snapshot under that subsystem's lock/version
protocol. On failed reload retain the last valid snapshot and report the error;
on missing first-run data use validated defaults. Do not silently reset an
unsupported version. Bound cJSON input/depth before parsing as TILE does; do not
assume cJSON rejects duplicate keys. Save via a temporary sibling and the SD
worker's replacement/recovery policy. Test missing/default, malformed, boundary,
unknown-version, duplicate-key, truncated and failed-reload retention cases.
