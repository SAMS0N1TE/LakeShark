# LakeShark apps

Write a program for the board in C, copy one file to the SD card, and it shows
up on HOME under USER. One app is one `.lap` file.

## Start

You need Python 3.9+ and Espressif's RISC-V toolchain (an ESP-IDF install has it).

```
python app-sdk/lap.py build app-sdk/apps/snake -o snake.lap
python app-sdk/lap.py info snake.lap
```

Every `.c` file in the folder is compiled into the app. The examples:

| App | Shows |
|---|---|
| `apps/dice` | 3D in dots the way COMPASS draws, the motion sensor (tilt and shake), physics, tones and haptics |
| `apps/snake` | smooth animation between steps, a pad that answers on touch-down, swipes, a best score kept in a file |
| `apps/tcp_probe` | API 1.3 asynchronous outbound TCP using the joined LINK network |
| `apps/meshling` | the radios: MeshCore nodes, packets and messages, Bluetooth and Wi-Fi scans, the clock, a saved pet |

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
network access. It fetches a plain HTTP response from `example.com:80` and
shows the connection state and text. To test a local service, edit `host`, the
port passed to `lap_tcp_connect`, and `request` in
`apps/tcp_probe/main.c`, then rebuild. For a local HTTP test server, run
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

- One app open at a time; it is loaded into PSRAM when opened and freed when closed.
- 512 KB per `.lap`; 4 MB of memory and cached files per app.
- An app can call the exported APIs in `lakeshark_app.h` and `lap3d.h`. It can send on the
  mesh only through MeshCore, with TRANSMIT armed; it cannot tune or transmit
  on the radio directly, and it cannot write over installed apps. It is still
  native code with the board's privileges, so install apps from people you
  trust.
- File changes are written when the app closes.

## 3D (API 1.2)

`include/lap3d.h` is the firmware's 3D engine. It draws in the same dots as
`lap_canvas.h`, lit and dithered in the board's theme colours, and it lives in
the firmware: an app that uses it stays small, and the work runs on both cores
in code built for speed. DICE 3D, ORBIT, CRYPT and MAP 3D in `apps/` use it.

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
- `l3_stats` reports the engine's time and how many dots it drew; ORBIT and
  DICE 3D show it on screen.

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
