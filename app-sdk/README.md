# LakeShark apps

Write a program for the board in C, copy one file to the SD card, and it shows
up on HOME under USER. One app is one `.lap` file.

## Start

You need Python 3.9+ and Espressif's RISC-V toolchain (an ESP-IDF install has it).

```
python app-sdk/lap.py build app-sdk/apps/dice -o dice.lap
python app-sdk/lap.py info dice.lap
```

Every `.c` file in the folder is compiled into the app. The examples:

| App | Shows |
|---|---|
| `apps/dice` | 3D in dots the way COMPASS draws, the motion sensor (tilt and shake), physics, tones and haptics |
| `apps/tcp_probe` | API 1.3 asynchronous outbound TCP using the joined LINK network |
| `apps/spectre` | API 2.1: a spectrum analyser on any radio (RTL-SDR, HackRF, LR2021) on the board's own controls: braille peak-hold trace, the shared waterfall, RADIO list, TUNE keypad, layered OPTIONS |
| `apps/sentry` | API 2.1: a background service: watches a channel for a carrier after you leave it, posts a notice and puts each up and down on the board's timeline |
| `apps/detector` | API 2.2: DETECTOR, rules over what the board's watchers see (`lap_seen_*`), aircraft and the mesh, with banner, sound, speech, vibration, mesh and SD log sinks, in the background |
| `apps/morse` | API 2.0: a decoder of your own: reads I/Q, keys on the envelope, decodes Morse, plays a sidetone |

## An app

```c
#include "lakeshark_app.h"

static void draw(void)
{
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "hello", LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK));
}

LAP_APP(.id = "hello", .name = "HELLO", .sub = "my first app", .version = "1.0",
        .author = "you", .color = LAP_GREEN,
        .icon_art = { "  _____   ", " | o o |  ", " |  >  |  ", " | \\_/ |  ", "  '---'   ", "  HELLO   " },
        .draw = draw);
```

`include/lakeshark_app.h` is the whole interface, with a comment on every call.
Callbacks run on the screen task about 25 times a second; return quickly and
keep local variables small (under 1 KB).

API 1.0 (firmware 2.18): drawing in cells (text, boxes, quadrant and sextant
blocks, braille, big digits, RGB565 pictures), keys and taps, time and random
numbers, read-only radio values, notification sounds, files in the app's own
folder, and `malloc`.

API 1.1 (firmware 2.19) gives an app the board, much as a Flipper app gets its
device:

| Area | Calls |
|---|---|
| Touch | `lap_touch` (the finger while it is down, in pixels), `lap_swipe`, every key including F1-F11 and RECORD |
| Screen | `lap_dots` (COMPASS's 2 x 3 dot packing), `lap_palette`, `lap_put_glass`, `lap_cell_px`, `lap_fullscreen`, `lap_hold_rotation`, `lap_keep_awake`, `lap_brightness`, and `LAP_ICON_PX` for a tile icon in colour dots |
| Motion | `lap_motion`: the 9-axis sensor in screen axes, pitch and roll, tilt-compensated heading |
| Sound and feel | `lap_tone` (sine, square, triangle, saw, noise, queued without gaps), `lap_haptic`, `lap_vibrate` |
| Where and when | `lap_gps`, `lap_time`, `lap_battery` |
| MeshCore | `lap_mesh_self`, `lap_mesh_nodes`, `lap_mesh_messages`, `lap_mesh_send`, `lap_mesh_dm`, `lap_mesh_advert` (sends need TRANSMIT armed in MESH) |
| Wi-Fi, Bluetooth | `lap_wifi_scan`, `lap_wifi_aps`, `lap_ble_scan`, `lap_ble_devices` |
| SD card | `lap_sd_read`, `lap_sd_write`, `lap_sd_list`, `lap_sd_size`, `lap_sd_mkdir`, `lap_sd_remove`: the whole card except `/sdcard/apps`, as jobs that never stall a frame |

Nothing in 1.1 blocks the screen: the mesh and the card are worked by the app
worker and handed over as copies. An app built with this SDK says API 1.1 and
needs firmware 2.19.

## Outbound networking (API 1.3, experimental 2.18.1-rc1)

Declare `.flags = LAP_CAP_NETWORK` in `LAP_APP`. LakeShark asks **on every
launch**, for signed and unsigned apps, before any app code runs. BACK leaves
the app closed. Apps without this capability receive `LAP_NET_EACCES` from
`lap_tcp_connect`, even when they import the networking functions directly.
Permission expires when the app closes. This is a gate on the SDK API;
native machine-code apps run with firmware privilege and are **not sandboxed**.
Only install native apps you trust.

Join Wi-Fi in LINK first. `lap_net_status` returns a worker-refreshed snapshot
of the LINK station's IPv4 address and this launch's permission; it never
returns SSIDs, passwords, or other credentials. These calls do not join a
network or alter LINK settings. The first snapshot may be empty until the worker
runs; requesting status wakes it. AP-only operation reports disconnected.
Losing the station address closes live sockets with `LAP_NET_ENETDOWN`.

| Call | Result |
|---|---|
| `lap_tcp_connect(host, port, timeout_ms)` | Immediate handle >=0, or negative error. Starts DNS/connect in the background; timeout 1–30000 ms covers both. |
| `lap_tcp_state(handle)` | `LAP_TCP_PENDING`, `LAP_TCP_CONNECTED`, or a negative terminal error. |
| `lap_tcp_send(handle, buf, len)` | Bytes copied into the TX queue, possibly fewer than requested; retry remaining bytes later. `LAP_NET_EAGAIN` means pending/full. |
| `lap_tcp_recv(handle, buf, len)` | Bytes copied from the RX queue; `LAP_NET_EAGAIN` means pending/empty; zero means peer EOF after buffered data drains. |
| `lap_tcp_close(handle)` | Immediately invalidates this handle and schedules socket cleanup. |

All calls return without waiting for network I/O. Poll from draw/key callbacks
and return each frame. DNS runs asynchronously on the lwIP task; connect,
send, receive, and close run on the app IO worker. There are two outbound IPv4
TCP sockets and a 4096-byte queue in each direction per socket. Each send/recv
accepts at most 4096 bytes. Host names are at most 253 ASCII characters; numeric
IPv4 also works. There is no listening, UDP, IPv6, TLS or SSH implementation.

Use **app globals or app malloc buffers** for host names, status and data;
stack addresses are rejected by ownership checks. A send result acknowledges
queue acceptance, not delivery. Drain receive data even if state reports EOF
or an error. Close every handle you no longer need; terminal states retain
the slot until close. App exit closes all sockets and drops queued sends.
A canceled/timed-out DNS request may retain a slot until its late callback
returns; repeated immediate reconnects can temporarily return `LAP_NET_EBUSY`.

Build the working example:

```
python app-sdk/lap.py build app-sdk/apps/tcp_probe -o tcp_probe.lap
python app-sdk/lap.py info tcp_probe.lap
```

Copy it to `/apps` on the SD card, join Wi-Fi in LINK, open TCP PROBE and allow
network access. It sends an HTTP request to `example.com:80` and shows each
step and PASS or FAIL. To test a local service, edit `host`, `port` and
`request` in `apps/tcp_probe/main.c`, then rebuild. For a local HTTP test server, run
`python -m http.server 8080 --bind 0.0.0.0` on a reachable PC, use that PC's LAN
IPv4 and port 8080, and retain an HTTP GET request. This is an HTTP test server,
not an encrypted channel.

An app-supplied SSH library can adapt its nonblocking transport to these calls;
it must handle EAGAIN and partial writes and implement its own cryptography,
host-key verification and authentication. Whether it fits the 512 KiB ELF /
4 MiB app allocation limit depends on the library and enabled algorithms.
The firmware's existing inbound screen SSH service is separate from this API.
Apps built with this SDK advertise API 1.3; older firmware rejects them clearly.
Existing API 1.0–1.2 apps keep their descriptor layout and remain compatible.

## Radios (API 2.0)

API 2.0 makes an app a radio app: it can list the board's radios, open a
receive session, tune, set gain and rate, read I/Q and spectrum lines, hear
the receiver's audio and play its own, take what the board's decoders hear,
call the board's tuning actions and post notices. `include/lakeshark_app.h`
documents every call; this is the map.

| Area | Calls | Needs |
|---|---|---|
| Radios | `lap_radio_list`: name, kind, tuning and rate ranges, gain range, features, present, busy, and why not | `LAP_CAP_RADIO_RX` |
| Receive | `lap_rx_open`, `lap_rx_state`, `lap_rx_status`, `lap_rx_tune`, `lap_rx_gain`, `lap_rx_rate`, `lap_rx_close` | `LAP_CAP_RADIO_RX` |
| I/Q | `lap_rx_read`: int16 I/Q pairs with the block's first sample number, centre, rate and GAP / RETUNED flags | `LAP_CAP_RADIO_RX` |
| Spectrum | `lap_rx_spectrum`: averaged dBFS lines, about 10 a second, squeezed to the bins you ask for | `LAP_CAP_RADIO_RX` |
| Audio | `lap_audio_listen`, `lap_audio_read`: the receiver's demodulated audio, 16 kHz | `LAP_CAP_RADIO_RX` |
| | `lap_audio_write`, `lap_audio_status`: play PCM at the board's volume | none |
| Decoded | `lap_event_subscribe`, `lap_event_next`, `lap_event_dropped`: pages, APRS, AIS, ADS-B, mesh messages | `LAP_CAP_RADIO_RX` |
| Actions | `lap_action("fm.freq", "162.4", out, cap)`, `lap_action_count`, `lap_action_info` | read-only: none; tuning: `RADIO_RX`; mesh sends: `RADIO_TX` |
| Notices | `lap_notify` (a banner), `lap_alert` (detector-style: steady, with the board's ring and vibrate) | none |

Which radios: the RTL-SDR and the HackRF (I/Q at their own rates, gain,
rate), the LR2021 in the LoRa socket (125 kHz I/Q; `LAP_RX_WIDE` uses its
576 kHz front end with the channel moved off the LO), and the SX1262, which is
listed but has no I/Q (`why` says so). On the PC simulator there is one radio,
SIM: a noise floor, a steady carrier and a Morse-keyed one.

How it stays safe:

- **Asked once per install.** Declare `.flags = LAP_CAP_RADIO_RX` (and
  `LAP_CAP_RADIO_TX` for mesh sends). The board asks before the app is first
  opened and remembers the answer for that id, version and file size; an
  update asks again. BACK denies and the app stays closed. `LAP_CAP_NETWORK`
  is still asked on every launch.
- **The firmware owns the radio.** The lap_io worker opens, tunes and closes
  radios; capture tasks push into a bounded queue in PSRAM (32 blocks of 2048
  pairs). App code never runs in a capture task, and no `lap_*` call waits.
  When the app falls behind, the newest samples are dropped and counted
  (`lap_rx_status`), and the next block says `LAP_IQ_GAP`; `first_sample`
  says how much.
- **The board's ownership rules hold.** Opening an app already parks the
  receiver HOME was keeping. A dongle another job still holds answers
  `LAP_RX_EBUSY`; the LR2021 is taken from MeshCore the way FALLS takes it and
  given back. One session per app.
- **Everything is given back.** Closing the app, `lap_exit` or a crash closes
  the session, stops the capture, hands the radio back, drops queued audio and
  ends subscriptions. Transmitting stays with the board: there is no raw
  transmit call; `mesh.send` through `lap_action` still needs TRANSMIT armed.

### Write your first radio app

A spectrum line on any radio, in about thirty lines:

```c
#include "lakeshark_app.h"

static lap_radio_info_t radios[LAP_RADIOS_MAX];
static lap_rx_config_t cfg;
static float db[128];
static int h;

static void draw(void)
{
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    if (h <= 0) {                                  /* find a radio with a spectrum and open it */
        int n = lap_radio_list(radios, LAP_RADIOS_MAX);
        for (int i = 0; i < n && h <= 0; i++)
            if (radios[i].present && !radios[i].busy && (radios[i].features & LAP_RXF_SPECTRUM)) {
                cfg = (lap_rx_config_t){ 433920000, 0, LAP_GAIN_AUTO, LAP_RX_SPECTRUM_ONLY };
                h = lap_rx_open(i, &cfg);
            }
        lap_put_str(1, 1, "looking for a radio", LAP_ATTR(LAP_CYAN, LAP_BLACK));
        return;
    }
    if (lap_rx_spectrum(h, db, 128, NULL) != 128) return;   /* opening, or no line yet */
    for (int x = 0; x < lap_cols() && x < 128; x++) {
        int y = (int)(-db[x] / 5);                 /* 5 dB a row, 0 dBFS at the top */
        lap_put_cell(x, y < lap_rows() ? y : lap_rows() - 1, '*', LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK));
    }
}

LAP_APP(.id = "firstrx", .name = "FIRST RX", .sub = "my first radio app", .version = "1.0",
        .author = "you", .color = LAP_GREEN, .flags = LAP_CAP_RADIO_RX, .draw = draw);
```

Then, step by step:

1. **Build it and look at it on the PC.** `python app-sdk/lap.py build myapp -o myapp.lap`.
   With the folder in `app-sdk/apps/`, the bench's `lssim` runs it against the
   SIM radio: `LAPSIM=firstrx lssim lap -f 60` (add `-l` for landscape).
2. **Read I/Q yourself** when the board's spectrum is not enough: open without
   `LAP_RX_SPECTRUM_ONLY`, then each frame
   `while ((n = lap_rx_read(h, iq, 2048, &meta)) > 0)` and process `n` pairs.
   Pick the lowest rate that holds your signal: an app runs on the screen task,
   about 25 frames a second. Reset your filters when `meta.flags` has
   `LAP_IQ_GAP` or `LAP_IQ_RETUNED`. `apps/morse` is a whole decoder built
   this way.
3. **Use what the board already decodes** instead of redoing it: name the
   receiver in the manifest (`.radio = LAP_RADIO_FM`), call
   `lap_event_subscribe(LAP_EV_PAGE)` and read pages with `lap_event_next`;
   `lap_audio_listen(true)` gives the same receiver's audio.
4. **Tune the board's receivers** with the console's own verbs:
   `lap_action("fm.freq", "162.400", NULL, 0)`; list them with `lap_action_info`.
5. **Tell the user** with `lap_notify` for news and `lap_alert` for something
   that must not be missed.

Old apps keep working: firmware with API 2.2 loads every 1.0-1.3 app and
every 2.0 and 2.1 app. An app built with this SDK says API 2.2; to keep one
running on older firmware put `#define LAP_TARGET_API_1` (2.18 / 2.19),
`#define LAP_TARGET_API_20` (2.20) or `#define LAP_TARGET_API_21` (2.21)
before the include. The examples that use
nothing newer do (MORSE is a 2.0 app).

## The board's own controls (API 2.1)

`include/lakeshark_ui.h` hands an app the building blocks the board's screens
are made of. They are the board's code, not copies: an app that uses them
looks and behaves like FM, P25 or RADIOS, and follows them when the board's
look changes.

| Control | Calls | What it is on the board |
|---|---|---|
| Tap keys | `lap_ui_keys`, `lap_ui_keys_rows`, `lap_ui_key_hit`, `lap_ui_key_find` | the filled keys along the bottom of every screen, label, value and letter |
| OPTIONS | `lap_options_open` with a `lap_opt_list_t`, `lap_options_close`, `lap_options_showing` | the one layered settings list: choices step in place, numbers open the keypad, MENU rows open a level below with BACK, `LAP_OPT_DISPLAY` opens the waterfall's own settings |
| Lists, keypads | `lap_picker_open` / `_add` / `_select`, `lap_numpad_open`, `lap_ui_freq_pad`, `lap_ui_freq_hit` | the RADIO-style list, the number keypad, FM's inline frequency keypad |
| Radio glass | `lap_ui_bezel`, `lap_ui_chip`, `lap_ui_rail`, `lap_ui_frequency`, `lap_ui_bargraph`, `lap_ui_vfo`, `lap_ui_vfo_hit` | FM and P25's receiver face: ruled bezel, annunciator chips, big rolling figures, VFD bargraph, VFO card with its slider and +/- keys |
| Meters, history | `lap_ui_meter`, `lap_ui_sparkline`, `lap_ui_history` | the ASCII meter `[===|==>--+--]`, line-character sparklines, SWEEP's braille history strips |
| Waterfall | `lap_wf_push`, `lap_wf_draw`, `lap_wf_key`, `lap_wf_touch`, `lap_wf_marker_hz` | the shared waterfall instrument, its palettes, contrast, speed and pixel rendering |
| Colours | `LAP_UI_TEXT`, `_DIM`, `_FAINT`, `_FRAME`, `_LIVE`, `_VALUE`, `_WARN`, `_ALARM` | the meanings the board's screens give their colours |
| Notices | `lap_notify`, `lap_alert` (API 2.0) | the board's banners |

A screen in a few calls:

```c
static lap_btn_t keys[3];
static const lap_opt_t ROWS[] = {
    { .label = "PEAK HOLD", .kind = LAP_OPT_TOGGLE, .id = 0 },
    { .label = "DISPLAY",   .kind = LAP_OPT_DISPLAY },
};
static double get(int id) { return hold; }
static void set(int id, double v) { hold = v > 0.5; }
static const lap_opt_list_t OPTIONS = { "MYAPP", ROWS, 2, get, set };

static void draw(void)
{
    const int kh = lap_ui_keys_rows(3), ky = lap_rows() - kh;
    lap_wf_draw(0, 2, lap_cols(), ky - 2, LAP_WF_MINI);
    keys[0] = (lap_btn_t){ "RADIO", radio_name, 'r' };
    keys[1] = (lap_btn_t){ "HOLD", hold ? "on" : "off", 'h', hold };
    keys[2] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, ky, lap_cols(), kh, keys, 3, -1);
}
static bool touch(int col, int row)
{
    const int k = lap_ui_key_hit(col, row);
    if (k == 2) return lap_options_open(&OPTIONS);
    ...
}
```

How they stay safe:

- **Copied in.** Labels, tables and values are copied when the call is made
  (an OPTIONS tree when it opens: up to 6 lists, 48 rows, 8 names a row).
  Hand the board **app globals, string literals or `malloc` memory, never a
  local array**: memory that is not the app's is refused (a key with a
  foreign label is drawn blank, an OPTIONS tree with one is not opened).
- **Clipped.** Every rectangle is the app's own cells and is held inside its
  area; nothing an app passes draws outside it.
- **Called back safely.** A list's choice, a keypad's number, an OPTIONS
  row's get and set reach the app on the screen task, inside the app's
  context, only if the pointer is the app's code. Closing the app closes
  anything of its still open, gives the waterfall back, and no callback
  reaches it afterwards.

## Background services (API 2.1)

An app may keep a small tick running after its screen closes: a decoder that
keeps listening, a watcher that posts a notice. Declare `LAP_CAP_SERVICE`
(asked once per install) and start it from any callback:

```c
static lap_service_t svc = { tick, stopped, 200 };   /* every 200 ms */
static void enter(void)
{
    if (ready) return;            /* back from the background: all still here */
    ...                           /* open the radio, allocate */
    lap_service_start(&svc);
}
static void leave(void)
{
    if (lap_service_running()) return;   /* keep the session for the tick */
    ...                                  /* otherwise close as usual */
}
```

- While the screen is closed **the app stays loaded**: its memory, its radio
  session and its subscriptions stay. Closing the screen calls `leave()`;
  opening the tile again calls `enter()` again with everything still there.
- The tick runs on the screen task every `period_ms` (50 ms to 10 s), open
  or not. It cannot draw and gets no keys; the radio, events, `lap_emit`,
  notices and the clock all work. `lap_screen_open()` says whether anyone is
  looking.
- **Budget:** a tick returns within `LAP_SERVICE_BUDGET_US` (4 ms). Three in
  a row over it, or one over ten times it, stop the service with a notice. A
  tick that never returns stops the board: it restarts, names the app and
  asks before running it again. No service starts by itself at boot.
- **The owner is in charge.** The first time the screen closes a notice says
  the app keeps running. RADIOS lists it under APP SERVICE with its rate,
  tick count and worst tick, and a STOP key; the console has `app svc` and
  `app stop <id>`. Opening another native app stops it (one service at a time,
  `LAP_SERVICES_MAX`). When it stops with the screen closed, `stopped` is
  called, then everything is freed and given back as when an app closes.

`apps/sentry` is a whole one in about four hundred lines.

## Emitting decodes (API 2.1)

An app that decodes something puts it on the board's decoded timeline, the
same one the board's pagers, APRS, AIS, ADS-B and mesh decoders publish to,
and the one the radio head's decoded list reads:

```c
static lap_decoded_t d;                 /* app memory, not the stack */
d.kind = LAP_EV_TEXT;                   /* or PAGE, APRS ... SUBGHZ, LORA */
snprintf(d.from, sizeof(d.from), "SENTRY");
snprintf(d.text, sizeof(d.text), "carrier up 433.920 MHz +23 dB");
lap_emit(&d);
```

- Needs `LAP_CAP_RADIO_RX`. One `LAP_EV_*` kind a decode; `from` and `text`
  are cut at their size.
- Each decode is tagged with the app's id. A reader gets it from
  `lap_event_next` and asks `lap_event_source` who decoded it ("" for the
  board's own). 2.1 adds the kinds `LAP_EV_SAME`, `_ACARS`, `_SUBGHZ`,
  `_LORA` and `_TEXT`; `lap_event_t.kind16` holds the whole kind. A 2.0 app is
  never handed a kind it cannot hold.
- Up to 4 a second in bursts of 8; more answers `LAP_RX_EAGAIN` and is
  counted in `lap_emit_dropped`, so one app cannot push the board's own
  decodes out of the 256-entry ring.

`lap_decoder_t` stays reserved: it is the shape the radio head will load
decoders in (input, rate, offset, bandwidth), and their output is exactly
what `lap_emit` does today.

## What the board's watchers see (API 2.2)

2.2 adds three timeline kinds, `LAP_EV_P25` (a call starting: talkgroup,
unit), `LAP_EV_REC` (something new on REC's sub-GHz watch) and
`LAP_EV_DETECT` (a detector's alert), and one feed for a detector app:

```c
static lap_seen_t seen;                 /* app memory, not the stack */
lap_seen_watch(LAP_SEEN_BLE | LAP_SEEN_RID | LAP_SEEN_P25 | LAP_SEEN_PAGER);
while (lap_seen_next(&seen) == 1) {
    if (seen.source == LAP_SEEN_P25 && (seen.flags & LAP_SEEN_F_EMERGENCY)) ...
}
```

- Needs `LAP_CAP_RADIO_RX`. The sources: SWEEP's Bluetooth and Wi-Fi
  categories (`LAP_CAT_TRACKER` ...) and list sections, Remote ID, the LoRa
  drone-link listener, DRONES' drone watch, REC's watch and CATCH, the pager
  decoder and P25. Watching Bluetooth, Remote ID or LoRa starts the scan or
  listener the board runs for it; the decoders report while their own app
  runs them.
- No address is handed over: `entity` is a pseudonym that holds for one
  boot. 64 sightings wait; past that the newest are dropped and counted in
  `lap_seen_dropped`.
- While an app watches `LAP_SEEN_DRONE`, drone watch's alerts are its to
  raise; with none watching, the board raises them itself.
- `lap_mesh_channel_private(slot, name, cap)` says whether a mesh channel
  slot is keyed (not the public channel, not a hashtag channel), for an app
  that shares alerts.

`apps/detector` is DETECTOR on these, with the engine the board used to
carry (`det_engine.c`, tested on the bench as `test_detector_app`).

## The LakeShark look

An app on HOME sits next to the board's own screens, and it should look like
one of them. The two radio examples show how.

- **Characters, not widgets.** The screen is a character grid: ASCII art,
  box-drawing lines (`lap_box`, `lap_hline`), braille for graphs and traces
  (`LAP_BRAILLE`), `lap_dots` for colour pictures and waterfalls, line
  characters for meters (`[=====-----]`). Solid blocks only as small accents
  or one big headline value (`lap_big_char`). Nothing that looks like a stock
  widget kit: no grey boxes with labels, no plain list dumped on the screen.
- **A face of its own.** Give the app its own character art (a header emblem,
  `icon_art`, `LAP_ICON_PX`) and several theme colours, so it is known at a
  glance.
- **Motion from real data.** Move only when something happened: a new
  spectrum line, a decoded letter, a level. No spinners or decorative loops;
  a quiet radio is a still screen.
- **The first screen answers "what is happening now"**, with the next useful
  action one tap away. Detail goes behind a tap, not onto the first screen.
- **Touch first, controls the board's way.** Every action is a tap key at
  least 3 rows by 8 columns with its keyboard letter on it (`RADIO r`), along
  the bottom as the board's own screens have them: use `lap_ui_keys`. Settings
  go in one OPTIONS list on `o` (`lap_options_open`), deeper ones a level
  below; choices from a list (`lap_picker_open`), numbers on the keypad.
  Shared controls stay shared: do not redraw your own look-alike of a key, a
  list or a meter, and do not restyle them per app.
- **The board's colours mean things.** Words in `LAP_UI_TEXT` or `_DIM`,
  never in `_FAINT` (rules, grids and unlit scales only); live values
  `_LIVE`, settings `_VALUE`, trouble `_WARN` and `_ALARM`; frames `_FRAME`.
  Several colours, never one or two.
- **A service is a guest.** Keep the tick short, say what it is doing on its
  screen, and post a notice only for something the owner wants to know.
- **Check it** in `lssim`, portrait and landscape (`-l`), before a board runs
  it; `LAPSIM_SCRIPT` can press keys (`60:key=o` opens OPTIONS).

## Drawing in dots

`include/lap_canvas.h` is a header-only canvas on top of `lap_dots`: pixels,
lines, circles, lit balls, dithered ramps, ASCII-art sprites and a 3 x 5 dot
font. Two colours per cell survive, so keep text on a plain background and let
dither do the shading.

## Looking at an app without a board

The host bench compiles every app in `apps/` into `lssim`, the TUI simulator,
with the real app API and renderer:

```
LAPSIM=dice LSSIM_FRAMES=frames lssim lap -A 40 -f 60
```

writes every frame as a picture. `LAPSIM_SCRIPT` plays the sensors by frame
(`5:tilt=0.3,-0.1;20:swipe=L;30:mesh;31:rx=4;40:down=120,300;44:up`), and
lssim's own `-x` taps cells. See `bench/shims/ls_lap_screen.c`.

## Installing

- **From the store:** terminalbay.com, LakeShark hub, APPS. Connect the board
  with USB in Chrome or Edge and press INSTALL.
- **By hand:** copy the `.lap` into `/apps` on the SD card and restart the board.
- **Over USB from your own tools:** the board's console command `app`
  (`app put`, `app data`, `app end`, `app list`, `app info`, `app rm`).

Store apps are signed by the store. An app from anywhere else runs too; the
board asks once before it opens one the store did not sign.

## Limits

- One app open at a time; it is loaded into PSRAM when opened and freed when
  closed, or when its background service ends. One service at a time.
- 512 KB per `.lap`; 4 MB of memory and cached files per app.
- An app can call the exported APIs in `lakeshark_app.h`, `lakeshark_ui.h` and `lap3d.h`. With
  `LAP_CAP_RADIO_RX` it can receive and tune (API 2.0). It can send on the
  mesh only through MeshCore, with TRANSMIT armed, and has no raw transmit
  call; it cannot write over installed apps. It is still native code with the
  board's privileges, so install apps from people you trust.
- One radio session per app; an I/Q queue of 32 x 2048 pairs; the last 256
  decodes kept for every reader; audio rings of 1 s (heard) and 1/2 s (played).
- File changes are written when the app closes.

## 3D (API 1.2)

`include/lap3d.h` is the firmware's 3D engine. It draws in the same dots as
`lap_canvas.h`, lit and dithered in the board's theme colours, and it lives in
the firmware: an app that uses it stays small, and the work runs on both cores
in code built for speed.

```c
#include "lakeshark_app.h"
#include "lap3d.h"

static l3_scene *sc;
static int steel;

static void draw(void)
{
    if (!sc) {
        sc = l3_create(lap_cols(), lap_rows());
        steel = l3_material(sc, L3_RAMP_METAL, 4, 0.6f, 0);
        l3_shadows(sc, true);
    }
    l3_begin(sc);
    l3_camera_orbit(sc, l3v(0, 0.5f, 0), 5, lap_millis() * 0.02f, 25, 40);
    l3_floor(sc, 0, steel);
    l3_flags(sc, L3_CAST);
    l3_sphere(sc, l3v(0, 0.8f, 0), 0.8f, steel);
    l3_end(sc);
    l3_show(sc, 0, 0);
}
static void leave(void) { l3_destroy(sc); sc = NULL; }
```

What there is:

| | |
|---|---|
| Shapes | sphere, ellipsoid, box, rounded box, cylinder, capsule, cone, torus, disc, quad, endless floor |
| Meshes | your own triangles, or made for you: lathe, extrude, height map, icosphere, 3D text |
| Models | `tools/obj2l3.py` turns an OBJ from Blender into C (below) |
| Worlds | `l3_terrain` (height-map landscapes, Voxel Space style), `l3_walls` (grid mazes, Wolfenstein style), `l3_particles` |
| Look | materials are ramps of theme colours; spec highlights, grain, checks, textures, round-dot textures (`L3_ROUND`), glow, wireframe (`L3_WIRE`), outline, fog, sky |
| Light | one sun with shadows, four lamps |
| Cameras | perspective or orthographic; `l3_camera_orbit`, `l3_camera_look`, `l3_drag_turn` for a finger |
| Asking | `l3_pick` (what is under a dot: taps), `l3_depth`, `l3_project`, `l3_ray`, `l3_raycast` (what a ray hits: aiming, collisions) |

### Keeping it fast

- Mark what does not move with `l3_flags(sc, L3_STATIC)`. When nothing has
  moved, a frame costs almost nothing; when a few things move, only the dots
  they (and their shadows) cover are drawn again.
- Set the camera, sun and materials only when they change. Moving the camera
  redraws everything.
- Shadows cost one more pass; turn them on only if the scene needs them, and
  give `L3_CAST` only to what should cast one.
- Curved shapes are traced exactly per dot; boxes, quads and meshes go
  through the faster triangle path. Prefer them for big flat things.
- `l3_stats` reports the engine's time and how many dots it drew.

### Memory

A scene keeps its buffers in PSRAM (about 1-2 MB at full screen) and a few KB
of internal RAM while it is open. Nothing is allocated once frames are
running. The default capacities are 256 draw calls, 1,024 vertices a mesh and
2,048 triangles; raise them with `l3_reserve` between frames, and check
`dropped` in `l3_stats`. Text, sprite rows, mesh and terrain arrays are read
at `l3_end`, so they must live until then; textures are copied when made.

### Models

```
python app-sdk/tools/obj2l3.py ship.obj -o app-sdk/apps/myapp/ship_model.c --name ship --size 2 --center
```

writes a `const l3_mesh ship` and lists the OBJ's materials in order. Make
your scene's materials in that same order before anything else, so their
numbers match, then `l3_draw_mesh(sc, &ship, &pose, 0)`.
