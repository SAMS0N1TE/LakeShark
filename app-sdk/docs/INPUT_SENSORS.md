# API 2.5: input devices, GPS, motion and saved navigation

These services expose firmware-owned hardware and saved data. Apps own the UI.
All calls run inside an app callback. Outputs are synchronous copies and may
use the callback stack. No app callback runs on a controller, UART or sensor task.
The RV32 app descriptor remains **228 bytes**. API 1.0–1.3 and 2.0–2.4 binaries
keep their existing structures and exports. Define `LAP_TARGET_API_24` to build
against the preceding API; direct 2.5 imports require firmware 2.5.

## Unified input

`lap_input_next2(out, sizeof(*out))` consumes the same 64-entry queue as
`lap_input_next`. Choose one reader. The new event contains the unchanged
`lap_input_event_t` prefix, device identity, contact identity, gamepad axes,
buttons and d-pad. The old reader skips new event kinds and writes only its
original 22 bytes. `lap_input_state` and `lap_key_down` retain the aggregate
keyboard/mouse view. `lap_input_devices` lists up to eight observed connected
devices; `lap_input_device_state` recovers per-device held state and up to ten
contacts after queue overflow. `lap_input_state.dropped` counts displaced events.

Device IDs are boot-local, not addresses or saved bond IDs. BLE devices use the
existing host's identity, desktop gamepads use a session-local index, and contact
IDs remain stable from down to up. A reconnect or new desktop session can change
identity. Lists do not scan or pair; use firmware LINK to pair devices. Existing
BLE connections are reconciled from the host's published snapshot when an app
starts or returns to focus. No global key capture API is provided.

Focus loss, pause, modal entry and queue reset clear held values and contacts.
Device removal clears that device without releasing another keyboard's keys.
Contact down must begin inside the client. Subsequent moves/releases follow
that contact; a new app cannot acquire an already-held contact by moving over it.
Input state is authoritative even when events overflow. An input-producer queue
overflow still uses the Phase 1 release barrier. A held gamepad is refreshed by
its next report; focus restoration does not synthesize key presses.

Gamepad normalization: LX/LY/RX/RY in `[-32767,32767]`, LT/RT in `[0,32767]`,
positive Y downward; buttons 1–32 become bits 0–31; d-pad X/Y in `[-1,1]`.
The BLE backend supports descriptor-based Generic Desktop joystick/gamepad
application collections, absolute X/Y/Rx/Ry/Z/Rz axes (Z/Rz are triggers),
eight-direction hat switches with a neutral/null value, and button usages.
Multiple input report IDs are merged per peer. Proprietary vendor reports,
console-specific protocols, rumble and nonstandard axis assignments need a
backend profile; they are not advertised as supported protocols. This does not
add BLE HID peripheral/device emulation.

| Board/source | `lap_caps.hardware` and actual support |
|---|---|
| Boards with the configured BLE host and available BLE radio | `LAP_HW_HID_BLE`, `LAP_HW_GAMEPAD_BLE`: existing keyboard/mouse host and generic report gamepads; pairing in LINK |
| Nano with an initialized runtime DSI-panel GT911 | `LAP_HW_MULTITOUCH`: all five GT911 contacts, native tracking IDs, panel swap/mirror and current display rotation |
| T-Display-P4 GT9895 / HI8561 paths | Legacy single contact; their existing readers lack a validated multi-contact ID decoder, so local multitouch is **not** advertised |
| Other boards / no controller | No local multitouch flag; no fabricated contacts |
| Active desktop/remote screen | `LAP_HW_MULTITOUCH`, `LAP_HW_GAMEPAD_DESKTOP`: negotiated browser touch and standard browser gamepads, including controllers attached to the desktop over USB/BLE |
| Native USB on every board | `LAP_HW_GAMEPAD_USB` remains clear: this tree has no USB HID host backend; browser USB controllers are desktop input |

Remote `term caps` retains its exact legacy `term: caps ev1` line and additionally
advertises `ev2`. The browser only sends ev2 after negotiation. ev1 stays 24 bytes.
ev2 is 52 little-endian bytes: the ev1 prefix with version 2, device u32 at 24,
contact u16 at 28, classes/transport at 30/31, six i16 axes at 32, buttons u32 at
44, signed d-pad X/Y at 48/49, zero at 50/51. Up to four records fit the existing
512-byte command limit. The entire batch is validated before posting. Desktop
IDs occupy `0x30000..0x3ffff`; they cannot impersonate BLE/local identities.
The browser preserves PointerEvent contact IDs through a bounded local mapping
and polls standard gamepads at 20 Hz while the page has focus.

## GPS

`lap_gps_acquire(0)` returns an app-generation lease; repeated acquire returns the
same active handle. `lap_gps_release(handle)` releases it; stale handles have no
effect. The worker uses `ls_gps_app_demand`, independently of firmware
`ls_gps_start/stop` demand. It starts/retries acquisition while leased, and drops
only app demand at release/unload. No UART start or stop runs in an app callback.
Firmware demand keeps the receiver running after the app closes; a firmware stop
does not defeat an active app lease. Acquisition has no raw-NMEA/reset controls.

`lap_gps_status` always fills a status, including ABSENT. OFF means the reader is
not running, ACQUIRING means no fix yet, FIX means a valid position and sentence
within three seconds, and STALE preserves last-known coordinates while clearing
`position.fix`. Fix/sentence/sky ages are independent; `UINT32_MAX` means never
seen. GGA quality, antenna TXT state, visible/used/tracked counts and asynchronous
start errors remain distinguishable. `lap_gps_satellites` copies up to 48 entries
with constellation, PRN, elevation, azimuth, C/N0 (SNR) and GSA-used flag. Sky data
expires after three seconds independently of other GPS traffic. Existing
`lap_gps` semantics are unchanged.

GPS/IMU hardware flags retain the Phase 1 meaning: board support/presence as the
firmware can discover it, independently of a live fix or valid magnetic sample.
An optional receiver can have a supported UART but no sentences or antenna.

## Motion

`lap_motion_subscribe(fields, period_ms)` selects latest-only delivery at
50–10000 ms. `lap_motion_next` returns 1 when due, 0 between periods, ENODEV on
hardware loss, or EAGAIN before a cached sample exists. Slow apps skip samples;
this is not a historical sample FIFO. `lap_motion_subscribe(0,0)` stops delivery.
The firmware's cached field worker remains at its existing rate; the app cannot
raise I2C load above the driver's 20 Hz limit.

Samples include the existing screen-axis motion view plus uncalibrated board-axis
acceleration/gyro/magnetometer in g, degrees/s and microtesla. These are physical
units, not ADC counts. Field masks identify unavailable magnetometer/heading
data. `lap_motion_status` reports available/subscribed fields, period, age,
sequence and calibrated/disturbed/low-confidence/true-north flags. The compass
learner's heading sigma is in degrees, **-1 when unknown**. A calibrated flag does
not imply a numeric accuracy. Calibration remains in the firmware compass UI.
The host raw axes are a synthetic approximation of its screen-axis fixture.

## Saved navigation and the map boundary

`lap_navigation_open(kind)` queues one read-only snapshot. All three kinds read
SD on the firmware worker; apps poll `lap_navigation_read(handle, first, out,
max)` (1–64 points per page), and close with `lap_navigation_close`. Pending reads
return EAGAIN. Closing/unloading cancels without retaining app buffers; BUSY can
persist until the worker acknowledges cancellation. Missing SD/file returns
ENODEV, invalid files EIO. These calls neither mutate the map nor start recording.

* TRACK: the firmware's `/lakeshark/track.log` fixed-record ring, oldest first,
  at most 4096 points. Validates geometry, coordinates, truncation and header
  changes during capture. A changed ring produces EBUSY: close/reopen to retry.
  Raw records take at most 64 KiB; page conversion avoids a full expanded copy.
* WAYPOINT: saved `/maps/marks.txt` markers (up to 48), including labels and
  creation time. Sketch geometry and malformed coordinates are omitted; IDs
  are local to the snapshot. Unsaved live edits are intentionally absent.
* PLACE: the installed `/maps/installed/*.ccpi` library (up to 32), validating
  CCPLACE2 headers and bitset CRC. Files store **tile covers**, not exact named
  points. Output flag 2 labels the circular-mean/Mercator cover centre; flag 4
  labels a queued installation. Saved library keys and labels are retained.

CartoCore feature-near-point queries and tile-region rendering stay firmware-only
in this increment. The inspected `ls_cartocore_map_impl.h` uses singleton
request/style/source state, shared frame locks, and two firmware pixel buffers;
it has no app-generation ownership or independently bounded query mailbox.
`ls_carto_budget.h` budgets **5.25 MiB scene + 4.5 MiB decoded** for cell sources,
plus output/cache overhead and a 2 MiB firmware reserve. That exceeds the app's
4 MiB allocation/cache allowance before app UI buffers. A small output region
does not bound source decode memory. Wrapping MAP's globals would let app close,
style changes or concurrent MAP use invalidate firmware resources. A future map
service needs explicit shared-renderer admission, cancellation and ownership;
no map capability or success-returning stub is advertised here.

## Examples and deterministic simulation

`apps/gamepad_tester` uses all input/device and motion calls.
`apps/gps_sky` acquires/releases GPS, shows freshness/quality and plots the sky.
`apps/track_viewer` pages tracks, waypoints and places into an app canvas (T/W/P);
the track display samples every eighth point to bound app rendering memory.
All three query `lap_caps` and handle absent hardware/data.

Add `gps,imu,sd,desktop` to `LAPSIM_HW` as needed. Input backend flags can be
selected explicitly with `hidble,gamepadble,gamepadusb,multitouch`; these are host
fixtures, not declarations that a real board has a USB host. `LAPSIM_SCRIPT`
uses the existing `frame:command;...` format:

```
1:device=100,4,1;2:gamepad=100,-32767,32767,0,0,32767,0,1,-1,0;8:device=100,4,0
1:contact=200,17,0,10,20;2:contact=200,42,0,30,40;3:contact=200,17,1,15,25;8:contact=200,17,2,15,25
1:gpsfix=42.3,-71.2,18;1:satellite=3,1,45,180,38,1;5:gpsantenna=2;10:gpslost
1:tilt=0.2,0.1,1;2:motionquality=6,-1;8:caps=sd,desktop
```

`device=id,classes,connected`; classes are keyboard=1,mouse=2,gamepad=4,touch=8.
`gamepad=id,LX,LY,RX,RY,LT,RT,buttons,dpadX,dpadY`.
`contact=device,id,phase,x,y`; phase 0 down,1 move,2 up, in client pixels.
`satellite=prn,constellation,elevation,azimuth,snr,used` updates that satellite;
no refresh makes the sky expire naturally. `motionquality=flags,sigma` injects
calibration/interference/unknown accuracy. Scripts can remove hardware with
`caps=` and use the existing focus/pause/dialog/unload scenarios. Navigation
uses real host files beneath the simulator's private `sdcard` directory.

Board verification remains necessary for BLE pairing/reconnect/coexistence,
actual controller descriptors/axis assignments, GT911 IDs under rotation and
multi-finger release, UART acquisition/power sharing, sensor calibration/heading
accuracy, SD latency and concurrent recorder snapshots. No firmware image build,
flash or RF transmission is required by these host tests.

Validation on this worktree: 17 targeted CTest entries pass (including six
headless example journeys), six `test_phase1.py` checks pass, and all 24 bundled
apps/templates pass `test_example_contracts.py --frames 40 --build-dir bench/build`
with RV32 `.lap` builds. The registry and simulator matrix were regenerated via
`apidoc.py --generate` / `api_registry.py`; old export fixtures still resolve.
`test_screen_resolution` additionally reproduces an existing base-commit failure:
its expected default is 7×13, while both base and current page select 10×17.
No resolution behavior was changed for that test.
