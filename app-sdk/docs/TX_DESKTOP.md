# API 2.6: HackRF transmit and desktop host services

The app descriptor remains 228 bytes. API 1.0–1.3 and 2.0–2.5 binaries still
load; define `LAP_TARGET_API_25` to build against the previous target. New
exports are grouped as **HackRF TX / desktop host** in the public header,
allowlist and firmware resolver. The generated registry/coverage matrix comes
from `python app-sdk/apidoc.py --generate` using `api_registry.py`.

## HackRF TX

```c
int lap_tx_hackrf_config(int radio, const lap_hackrf_tx_config_t *cfg);
```

`cfg` has `size`, `version=1`, `vga_db` (0–47), `amp` (0/1), and two zero reserved
bytes. Inputs are copied synchronously, including active callback-stack inputs.
Settings are copied into each queued TX and reset on app load. The existing
`lap_tx_iq` and `lap_tx_replay` select the USB backend when the selected radio
is a HackRF. `lap_tx_status` and `lap_tx_cancel` retain their existing contracts.
Only `LAP_CAP_RADIO_TX` is required to transmit. No band, region, licence or
confirmation policy is applied.

TX tunes 1 MHz–6 GHz with 64-bit frequency, uses a hardware sample rate of
2–20 MS/s (no RX decimator), programs filter bandwidth, request 21 TX VGA and
request 17 amp, enables transceiver mode 2, and sends signed interleaved I8/Q8
to USB endpoint 0x02. The finite waveform has at most 32,767 pairs/65,534 bytes.
Replay reads the existing app-local LSR2 recording on the worker, converts
IQ16 blocks to I8/Q8, and uses the same bounded waveform. Larger recordings
return ESIZE; there is no unlimited file loop. LSR2 source frequency is
informational; the explicit TX configuration chooses the transmitted frequency.
The legacy RX discovery/control path retains its existing 32-bit tune limit;
TX accepts the full HackRF range independently.

`power_dbm` remains the shared conservative hardware ceiling (0–15); it is
**not a calibration of HackRF output power**. VGA and amp are explicit settings.
The owner console command `hackrf txlimits <0..47> <0|1>` persists maximum VGA
and whether amp use is allowed, and applies reductions to active sends.
Defaults allow the device's full 47 dB stage and amp; app defaults are 0/off.
Only the owner settings/console can change these ceilings; there is no app
export for them. Duty protection admits at most 10 s per rolling minute and
thermal protection uses the existing 5 s heat budget/cooling model plus the
firmware hot-fault latch. Each finite send reserves its sample duration plus
500 ms for startup/drain and stop-control latency; a dedicated firmware TX guard task stops at a 200 ms transport deadline,
independent of SD/app-worker latency. It is admitted before enabling RF.
Failed and cancelled operations retain the charge. This is deliberately
conservative for small USB waveforms.

The radio registry lease refuses an RX owner (including a paused/reserved RX
session). TX never stops or steals a receiver. Receive-only SDK ports are
rejected before queuing. Front-end writes are refused during TX. Stop/unload,
USB error, timeout, owner-limit reduction and thermal faults turn mode/amp off
and flush pending output. DMA transfers and removed-device context are retained
until USB returns ownership; no callback enters an app. If cleanup-task creation
fails during unplug, the device context is retained rather than freed unsafely.

`hackrf_signal` is the example: T sends a 4,096-pair quadrature tone at 2 MS/s,
R replays `signal.lsr` from `/sdcard/apps_data/hackrf_signal/`, C cancels,
+/- changes VGA, A toggles amp, and F increments centre frequency. Supply an IQ
LSR2 recording with a 2–20 MS/s rate for replay. It uses `lap_caps` and radio
modes/list discovery, showing an attach message on boards without a HackRF.

## Desktop transactions

```c
uint32_t lap_desktop_features(void);             /* bit 0 URL, bit 1 save */
int lap_desktop_request_status(int request);     /* 0 pending, 1 success */
int lap_desktop_request_cancel(int request);     /* terminal status is ECANCEL */
```

The existing `lap_desktop_open_url(url)` and
`lap_desktop_save_file(scope, path, suggested_name)` now return a positive
request ID after admission, or a negative `LAP_SYS_E*` error. Polling consumes a
terminal result. One request is outstanding globally; IDs belong to the app
that requested them. Completion is cancelled on disconnect/unload or after
120 s. The scoped file is copied on the worker with the existing scope, path,
symlink and app-generation checks. Maximum export size is 65,536 bytes, including
zero-length files. No PC path is supplied by the app or returned to it.

The viewer displays the HTTPS URL and requires an Open action before asking
the PC browser to open it. Plain HTTP, controls, whitespace, backslashes and
URL credentials are rejected. The native host validates HTTPS independently.
Save displays the suggested filename and a Save action, then invokes the PC's
save dialog. The native host writes a temporary file in the chosen directory
and atomically replaces the chosen destination. Standalone secure-context
browsers use `showSaveFilePicker`; viewers without that API or a native host
advertise URL only. There is no silent download fallback. A success means the
host accepted the operation; it does not mean a web page loaded successfully.

`desktop_export` writes an app-scoped CSV asynchronously, then offers U for the
website, S for save, and C for cancellation. It checks SD and viewer features,
and displays `lap_desktop_info().scale_milli`. The native Desktop wrapper also
routes requests from its board iframe to its host bridge with frame/source and
origin checks.

### desk1 wire contract

The terminal advertises `desk1` in its existing `term: caps` reply. A new viewer
sends `term desk hello FLAGS SCALE_MILLI`. Only that serial/web viewer receives
requests. `term desk features FLAGS SCALE` updates native-bridge availability;
`term desk scale SCALE` updates DPI without cancelling a request. No handshake
means no features; old viewers receive their unchanged screen stream.

Requests are separate bounded text records between serial frames, or WebSocket
text messages, independent of cell/VT screen encoding:

```
term: desk1 ID OP OFFSET TOTAL NAME_HEX DATA_HEX
term: deskcancel ID
term desk result ID RESULT
```

OP 1 is HTTPS URL (`NAME_HEX`, zero payload); OP 2 is save (basename and raw
bytes). Payload chunks have at most 96 bytes, offsets must be contiguous, total
must be at most 65,536, and empty data is `-`. Result 1 is success; negative
values are `LAP_SYS_E*`. Unknown/stale IDs are ignored. Records never contain
terminal escape sequences from app strings. DPI is `devicePixelRatio * 1000`,
clamped to 250–8000; 0 means unknown on old viewers. DPI changes use the existing
`LAP_DESKTOP_SCALE` event and leave the event structure unchanged.

## Simulator and verification

Use `LAPSIM_RADIO=hackrf` or `LAPSIM_SCRIPT` steps `hackrf=1`,
`hackrflimits=12,0`, `hackrfrx=1`, `hackrftemp=1`; ordinary `radio=none`,
`ioerror` and cancellation still work. Desktop fixtures are
`desktopcaps=3,1500`, `desktopresult=1` (or `-8` to cancel), and `desktop=0`.
No simulator fixture opens a browser, save dialog, desktop window or RF device.

Examples:

```
LAPSIM=hackrf_signal LAPSIM_RADIO=hackrf LAPSIM_SCRIPT=5:key=t;12:key=c
LAPSIM=desktop_export LAPSIM_SCRIPT=1:desktopcaps=3,1500;8:key=s;15:desktopresult=1
```

Bench tests execute shared runtime admission/replay/storage contracts, the
production USB TX state machine with deferred callbacks, native save/browser
operations with OS calls mocked, and actual screen.js request handlers in a
Node VM. `test_example_contracts.py` builds every bundled app/template with
`lap.py` and runs headless launch/lifecycle scenarios. Simulator wrapper flags
use a response file so the expanded API fits Windows command-line limits.

Board checks still required: RF amplitude/amp/filter accuracy, USB OUT throughput
and FIFO drain timing across HackRF variants/ROMs, real unplug/cancel/control
failure timing, persisted owner limits, PA temperature/fault integration, and
real serial/web/native save dialogs and DPI changes between monitors. No firmware
images or flashing are part of this change.

## Verification for this worktree

- `cmake` bench build with MinGW/Ninja, sanitizers off; no firmware image build.
- 11/11 targeted CTest targets passed: VT/HackRF profile, manifest/API/SDK,
  radio I/O, Phase 1, system broker, production HackRF TX state-machine harness,
  native desktop host and screen.js handlers. The SDK signing-verifier subcase
  was skipped because `LAP_MBEDTLS_DIR` is not configured; export/manifest,
  legacy import and descriptor checks ran.
- `test_example_contracts.py --frames 40 --build-dir bench/build`: 32/32 apps
  and templates passed RV32 builds plus both launch and hide/show/stop scenarios.
- Scripted `test_tx_desktop26.py`: 4 passed; Phase 1, system and games/assets
  Python suites: 6, 5 and 6 passed. Existing radio-I/O scenarios: 5 passed.
  USB attach lifecycle harness: 2 passed. JS/Python syntax and diff checks passed.
- Logs/results are under `bench/build/`, including `targeted-tests.log`,
  `example-contracts.log`, `sdk-contracts/results.json` and `scripted-tests.log`.
- Tests requiring signing dependencies used the existing IDF Python 3.11
  environment. `LAP_CC` pointed to the existing Espressif 14.2.0 compiler;
  no packages were installed and no desktop windows were opened.

Firmware paths changed: `components/lakeshark/radio/{hackrf_dev.c,
hackrf_radio.h,lr_only_stubs.c}`, `components/lakeshark/core/settings.{c,h}`,
`main/headless_main.c`, and `components/apps/tui/{ls_lap_radio_io.c,
ls_lap_radio_io.h,ls_lap_radio_io_hw.c,ls_lap_radio_sim.c,ls_lap_system.c,
ls_desk_host.c,desk/ls_desk_host.h,ls_rterm.c,ls_vt.h,web/screen.js,
ls_lap_manifest.h,ls_lap_exports.inc}`. Host paths changed are
`host/lsterm/{lsweb.py,lsapp.py}` and `app-sdk/studio/web/desktop.html`.
