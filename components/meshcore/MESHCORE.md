# MeshCore on LakeShark

A LoRa mesh node, running in the background on the SX1262, alongside
everything else the radio does.

Upstream MeshCore is at `upstream/`, byte-identical. `compat/` gives it the
four Arduino headers it expects. `ls_mesh.cpp` is the only file that knows
about this board. That separation is the point: following upstream stays a
merge rather than an excavation.

---

## What is actually proven

Measured on 2026-09-09 against a LilyGO T-Beam Supreme running upstream
MeshCore v1.16.0. Full transcript in `bench/HARDWARE_2026-09-09.md`.

| | evidence |
|---|---|
| We receive their traffic | two adverts, −22 dBm, SNR 13.8 dB |
| Their signatures verify | `onAdvertRecv` only runs after `Mesh.cpp:268` verifies ed25519 |
| They receive ours | their packet log shows our node id in a received frame |
| They relay ours | their log shows `RX` then `TX` of the same packet |
| Our airtime maths | they computed 713 ms for our packet, we computed 722 |
| Group text works | typed `hello`, their log shows `type=5` on channel `0x11`, then relayed |

Not proven: direct (addressed) messages, routing beyond a single relay hop,
any range beyond a bench.

---

## Handing this to a user without handing over the radio

This is the part that matters. The device is a receiver that can now
transmit, and a mesh stack is software that *wants* to transmit - on its own
schedule, without asking. Four things keep that in the operator's hands, and
they are arranged so that no single mistake defeats them.

### 1. Transmit is off at every boot, and is never persisted

`ls_mesh_start()` brings the stack up with `_tx_enabled = false`. Nothing
writes that flag to NVS, deliberately: there is no sequence of settings that
leaves a board that comes up transmitting. If you armed it yesterday, it is
disarmed today.

With it off the stack still runs, listens, verifies signatures, and fills the
peer table. A user can run a node forever and never emit.

### 2. Arming is a physical act on the device, or an explicit console command

`mesh tx on`, or `T` on the MESH screen. It is one keystroke because burying
it would be worse - an operator who cannot find the disarm control is not
safer. But it is *never* the default, never remembered, and always visible:
the state word on screen reads `ARMED` in red, and the portrait display grows
a DISARM button that is not there at any other time.

### 3. A user app off the SD card cannot reach it at all

`ls_action_grant_user()` returns `READ | TUNE | UI`. It does not return
`LS_CAP_TX`. `mesh.advert` is registered as `LS_CAP_TX`. The check happens in
`ls_action_call()` before the handler is entered, so this is a property of the
dispatch and not of anything a screen author remembered to write:

```c
if ((e->needs & ~(unsigned)granted) != 0) return LS_ACT_DENIED;
```

A `.lsapp` can build a mesh instrument - show the node, the peers, the
signal, the counters - and cannot make the radio emit. That is the whole
design: **describing is unprivileged, emitting is not.**

### 4. The radio has one owner at a time

While MeshCore is running it owns the SX1262. `lora rx`, `lora tx` and
`lora config` refuse and say why. This is not politeness - two tasks in
`ls_lora_poll()` was measured corrupting each other's IRQ reads ().

### What is deliberately NOT claimed

The action registry gates **who may ask**. It is not a transmit broker and
does not pretend to be. The existing broker - plan, physical confirm,
one-shot token, commit - still sits in front of the paths that use it, and
`mesh.advert` is not yet behind it. That is written down in
`bench/HARDWARE_2026-09-09.md` as an open item rather than glossed.

### What a user needs to be told, in their words

> Your LakeShark can join a LoRa mesh. Out of the box it only listens: it
> will show you who is out there, how strong they are, and what it heard,
> and it will not put anything on the air.
>
> To take part you have to arm the transmitter yourself, on the device. It
> says `ARMED` in red while it is, and it forgets every time you power off.
>
> Apps you load from the SD card can show you the mesh. They cannot transmit
> on it. That is not a setting - they have no way to ask.

---

## How apps use it

Two registries, both bound by **name**, because a file on a card has a string
and not a linker symbol.

### Reading: `ls_value`

| path | type | meaning |
|---|---|---|
| `mesh.up` | bool | the stack is running |
| `mesh.node` | text | this node's id, first 8 bytes of its key |
| `mesh.rx` | int | frames received |
| `mesh.tx` | int | frames sent |
| `mesh.armed` | bool | the transmitter is armed |
| `mesh.rssi` | float | last received signal, dBm |

Every one of these **declines** rather than returning zero when the stack is
not running, so a described screen renders `--`. A zero that really means "no
value" is the class of quiet lie this project keeps finding.

A `.lsapp` on the card, needing no code:

```
panel "MESH"
  value mesh.node   label "node"
  value mesh.rx     label "heard"
  value mesh.rssi   label "signal"  unit "dBm"
```

### Doing: `ls_action`

| action | sig | capability | effect |
|---|---|---|---|
| `mesh.start` | - | `TUNE` | bring the stack up, receive only |
| `mesh.advert` | - | **`TX`** | flood a signed self-advert |
| `mesh.send` | `s` | **`TX`** | send text on the public channel |
| `mesh.name` | `s` | **`STORE`** | set this node's name, persisted |

`mesh.send` and `mesh.advert` need `TX`; `mesh.name` needs `STORE`. A user
app is granted neither, so an SD-card app can render the whole conversation
and cannot join it or rename the node.

From the console, which is the same dispatch:

```
call                 # lists every action, its signature and what it needs
call mesh.start
call mesh.advert     # denied unless transmit is armed
```

A refusal says which refusal it was - `no such action`, `denied`,
`bad argument`, `not fitted`, `busy`, `failed`. An app can tell "you may not"
from "it is not here" from "it broke", which is the difference between a
useful error and a shrug.

### For richer apps: the published tables

`ls_mesh_peers()` and `ls_mesh_events()` give the peer list and the event
ring. These are C API, so they are for built-in screens rather than `.lsapp`
files - the MESH screen uses nothing else, which is the test that the seam is
real: the interface cannot see anything an app could not be given.

### Adding an action

One `ls_action_register()` call, in `ls_action_builtin.c`, with an honest
capability. The rule that matters: **register it only when there is a real
implementation behind it today.** An action that returns OK and does nothing
is the `TRUNK MONITOR` failure with a shorter feedback loop.

---

## The interface

Two postures, genuinely different, because they are used differently.

### Landscape - three pages, and CHAT is the one it opens on

The first version of this screen was three columns of counters. That was the
wrong thing: a mesh is for talking on, and counters belong in the corner of a
chat window rather than instead of one.

```
 SHARK  62E29FE6                        LISTENING  ▁▃▅▇
+- PUBLIC CHANNEL ------------------+  +- HEARD ------+
| 12s < 97B5C3C7: hello from the be |  | 97B5C3C7  4s |
|  4s > SHARK: reading you five by  |  |        ▁▃▅▇  |
+-----------------------------------+  |              |
| > _                               |  |              |
+-----------------------------------+  +--------------+
 [F2 CHAT] [F3 NODES] [F4 SETUP]
```

| page | what it is |
|---|---|
| **CHAT** | the feed and a compose line. Ours and theirs are told apart by colour and a gutter marker, not indentation - at 48 columns there is no room to indent and still read. |
| **NODES** | peer list: age, RSSI, SNR, advert count, signal meter |
| **SETUP** | name, frequency, spreading factor, bandwidth, power - applied immediately, persisted |

| key | does |
|---|---|
| `F2` `F3` `F4` | change page, from anywhere, so a stuck edit cannot trap you |
| typing | goes into the compose line |
| `ENTER` | send (on SETUP: open / apply a field) |
| `ESC` | clear the compose line, or cancel an edit |
| `F5` `F6` `F7` | start-stop, arm transmit, advert |

**On CHAT, letters are text, not shortcuts.** A chat window where typing `t`
arms a transmitter is a trap. The controls move to function keys; that is the
trade every messaging app makes and it is the right one.

On NODES and SETUP, where nothing is being typed, `S` and `T` still work -
stopping the stack should never require changing page first.

### Portrait - a passive node display

In the spirit of a Meshtastic OLED. Large type, fixed layout, no paging, no
navigation: identity, state on a shaded band, counters, a signal meter, the
message feed, and who has been heard. The feed gets the space left over,
because it is the reason to look at the screen.

**The one control, and only when it is needed.** Portrait grows a single
large `DISARM TX` button when the transmitter is armed, and shows nothing
otherwise. A passive display you cannot stop is the wrong kind of passive -
it would mean the only way to stop emitting is to rotate the screen or find a
serial cable. The rest of the time, which is the normal state, it is exactly
as passive as it looks.

This is a deliberate, documented exception to "everything is reachable in
both postures". The rule exists so controls are not lost; here, one control
appears precisely when losing it would matter.

### Animation and theme

- **Opening** - `constellation` (): five nodes pop in, links grow from
  the centre, a pulse runs each link. Every other opening in the shell is one
  radio doing something; a mesh is a *topology*, and what is worth drawing is
  that separate things become connected.
- **Live pulse** - a ring expands from the node panel each time a frame
  arrives, driven by `ls_mesh_event_seq()`. A mesh node is silent most of the
  time and the one thing a glance should answer is "is this alive". A counter
  does not answer it; a ring does. One integer of state.
- **Sub-cell graphics, not just letters.** Signal meters are `LS_TUI_TRACE`
  eighth-height bars that rise left to right; the header and the portrait
  state band are `LS_TUI_SHADE_25` fills; selected rows are solid blocks;
  the compose cursor is a blinking `LS_TUI_BLOCK_FULL`. A character grid can
  draw more than characters and this screen is where that starts paying.
- **Colour is state, not decoration.** `LISTENING` green, `ARMED` red, `OFF`
  grey. Events are tinted by kind: adverts cyan, receives green, transmits
  yellow, errors red. Nothing here picks a literal colour - it goes through
  the theme, so all themes in `ls_theme.c` work.
- **The icon** (`LS_ICON_MESH`) is four nodes with the corners joined.
  Deliberately not a mast and not a wave: those are already taken, and they
  say the wrong thing about what this is.

---

## Cost, measured

| | |
|---|---|
| flash | +111 KB, dominated by ed25519's precomputed tables |
| internal RAM | ~13.9 KB (102,483 → 88,551 free) |
| task stack | 4,368 used of 6,144 allocated |
| peer + event tables | ~1.2 KB |

---

## Known gaps

- **Our advert carries no app data.** `createAdvert(self_id)` with no
  `app_data`, so the advert is signature-valid but nameless and typeless.
  Peers verify and relay it - both observed - but cannot classify us, and
  type-filtered tables ignore us. The T-Beam's `neighbors` list stayed empty
  for exactly this reason while it was demonstrably relaying our packets.
  `AdvertDataBuilder` is the fix.
- **`mesh.advert` is not behind the transmit broker.**
- **DIO1 is on the I²C expander**, so the radio is polled at a ~10 ms floor.
  Fine for a mesh; fatal for anything with a turnaround deadline. See
  `ls_lora.h`.
- **Only the public channel.** Direct messages need a contact exchange
  (advert → contact → shared secret) and are not implemented. Additional
  channels are not implemented.
- **The public channel is public.** It uses MeshCore's well-known PSK, so
  anything sent on it is readable by every MeshCore node and phone app in
  range. The compose line says so; there is no privacy here and none is
  claimed.
