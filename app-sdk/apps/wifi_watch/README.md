# WIFI WATCH

An 802.11 intrusion watch. It listens to management and data frames through
`lap_wifi_capture_*` (API 2.7) and raises an alert when something looks like
an attack. Receive only on Wi-Fi: it never transmits, joins, deauthenticates
or decrypts there. An alert can sound, vibrate, speak, raise the board's
banner and go out over the LoRa mesh (see [Alert delivery](#alert-delivery));
the mesh is off until the owner turns it on.

| Alert | When |
|---|---|
| DEAUTH, DISASSOC | 10 or more from one source to one target within 2 s (once per 10 s per pair); rate, reason and target are shown |
| FLOOD | 30 or more new BSSIDs within 3 s (once per 30 s) |
| TWIN | the same SSID from another BSSID with other protection ("open here, WPA2 there"), or one BSSID changing its protection |
| HIDDEN | a hidden network named by its own probe response or by a station's association request |
| HANDSHAKE | messages of a 4-way handshake seen between an AP and a station |
| PMKID | message 1 of a handshake carries a PMKID |

Handshakes are a seen/not-seen mark. Nonces, MICs, key data and PMKIDs are
not copied out of the frame, kept, shown or written.

## Screens

Six tabs: ALERTS, COUNTERS, DEAUTH (source, target, frames, rate, reason),
NETWORKS (SSID, BSSID, channel, security, hidden/named/twin notes), PROBES
(each client, the names it asks for, `R` for a randomised address) and
HANDSHAKES (which of messages 1-4 went past, PMKID present).

- Header: a radar of the networks heard (nearer the middle the louder; a twin
  or a changed network flashes) with its beam turning while listening, the
  alert count, the channel's activity as a VFD bar, frames a second in
  braille and the newest alert. Each kind of alert has its own mark
  (`-x>` deauth, `-/>` disassoc, `)))` flood, `<=>` twin, `(?)` hidden,
  `o~o` handshake, `[k]` PMKID); a new one flashes for six seconds. At the
  right of the second row, what an alert will do: `<))` sound (`<x` the
  board is muted), `{v}` vibrate (`{x}` no motor), `"A"` voice, `(Y)` LoRa
  mesh (flashes `)Y(` as a notice goes; yellow while alerts are held back,
  red when nothing can go), `zz` quiet hours. Lit when on, faint when off;
  a tap there opens the alert options.
- Handheld: one tab at a time. Swipe left and right, or tap `<` `>` on the
  tab strip. Drag the list with a finger; `UP` `DOWN` page it, `PAUSE`
  pauses, `MORE` opens the options. Notes show over the title for a few
  seconds; a tap there clears one.
- Desk window (90 columns or more): all tab names across the top; click one.
- Keys: LEFT/RIGHT tabs, 1-6 a tab, UP/DOWN/PGUP/PGDN scroll, ENTER or M
  MORE, O alert options, T test alert, SPACE pause, H channel plan, L log
  on/off.

MORE: pause, alert options, channel plan (hop 1/6/11, hop 1-13, stay),
allow link drop, log to card, test alert, clear, restart capture, close.

COUNTERS ends with DELIVERY: where notices go and why none can, the last
notice and what the mesh did with it (`lap_mesh_send_state`: waiting,
handed to the mesh, or refused and why), alerts held back and when the next
notice may go, the sound/vibrate choices, the motor, mute, voice, banner,
quiet hours and the last test.

## Alert delivery

MORE > ALERT OPTIONS (or `O`, or a tap on the header marks) opens the
board's OPTIONS list. The choices are kept in the app's own file
(`/sdcard/apps_data/wifi_watch/alerts.cfg`) and come back on the next start.

| List | Row | Choices (default first) |
|---|---|---|
| LORA MESH | SEND TO | **OFF**, CH0 PUBLIC, CH1-CH3 (named from MESH), DM NODE |
| | PICK NODE | the board's list of nodes heard (`lap_mesh_nodes`); picking one selects DM NODE |
| | MIN SEVERITY | **ALERT ONLY**, WARN AND UP, INFO AND UP |
| | EACH KIND EVERY | **10**, 1-120 minutes: one notice per kind of alert |
| | MAX PER HOUR | **6**, 1-30 notices an hour, all kinds together |
| SOUND AND VIBRATE | ALERT (KICK,FLOOD,TWIN) | **BOTH**, NONE, SOUND, VIBRATE |
| | WARN (HANDSHAKE,PMKID) | **NONE**, SOUND, VIBRATE, BOTH |
| | INFO (HIDDEN NAMED) | **NONE**, SOUND, VIBRATE, BOTH |
| | SPEAK ALERTS | **OFF**, ON |
| | BANNER | **OFF**, QUIET (`lap_notify`), BOARD ALERT (`lap_alert`) |
| QUIET HOURS | QUIET HOURS | **OFF**, ON |
| | QUIET FROM / UNTIL | **22** / **7** (local hour; equal means all day) |
| | LORA IN QUIET HOURS | **ON**, OFF (OFF holds notices until the hours end) |
| (top) | TEST ALERT | runs the ALERT row's outputs, the voice, the banner and the mesh once |

**LoRa mesh.** A notice is one plain line, at most about 100 bytes:

```
WIFI WATCH DEAUTH x12 02:aa:00:00:00:01>bcast ch6 14:03
WIFI WATCH TWIN "TEST-LakeNet" 02:ee:00:00:00:66>..00:00:01 ch6 14:03
```

kind, how many alerts it stands for (`x12`, none for one), the SSID if the
alert names one (cut at 20 characters), the source and short target, the
channel and the local time of the newest. The first alert of a kind goes at
once; more of that kind within EACH KIND EVERY are counted and go as one
notice when the gap ends, and no more than MAX PER HOUR go in any hour, so a
deauthentication flood cannot flood the mesh. Nothing is sent while the mesh
is not running or TRANSMIT is off in MESH; the alerts wait and COUNTERS says
why ("no LoRa mesh: the board has no LoRa radio, or MESH is not running",
"TRANSMIT is off in MESH"). Channel 0 is the public channel and a channel
not keyed in MESH shows `(open)`: anyone in range can read those notices.

**Sound, vibrate, voice.** Per severity: NONE, SOUND (`lap_sound`: the
alert tone, a chirp for WARN, roger for INFO), VIBRATE (`lap_vibrate`: 600,
300 or 120 ms) or BOTH. At most one sound or buzz every 3 s and one spoken
alert every 5 s; a burst of alerts sounds once, for the most severe. The
board's mute silences the sound, WIFI WATCH does not speak while the board
is muted, and VIBRATE still buzzes; the options then show `SOUND:MUTE`. Without a vibration motor (`lap_haptic` false) the
options show `NO MOTOR` and COUNTERS says so. Quiet hours silence all of
these and the banner.

**Banner.** QUIET puts up the board's silent banner (at most one a second).
BOARD ALERT uses the detector-style alert, which rings and vibrates with the
board's own ring and vibrate settings (at most one every 5 s); when it goes
up, WIFI WATCH does not sound or buzz a second time. The app runs only while
it is open, so banners are mostly for when another overlay is up.

There is no "only while the screen is off" choice: the SDK has no call that
says whether the panel is dimmed.

## Firmware and permissions

Built for API 2.8 (the SDK's default target: firmware with API 2.8). The
calls it uses need: raw Wi-Fi capture 2.7, `lap_mesh_channel_private` 2.2,
OPTIONS, the node list and the board's keys 2.1, `lap_alert` / `lap_notify`
2.0, `lap_say` 1.2, `lap_mesh_*` and `lap_vibrate` 1.1, so no newer target
was needed for 1.2.0.

1.2.0 declares RADIO_TX next to RADIO_RX: `lap_mesh_send` and `lap_mesh_dm`
need it. The board asks once for the new version. RADIO_TX is used for the
mesh notice only, and only after SEND TO is set; Wi-Fi stays receive only.

## Channels and the board's own link

Hopping retunes the radio, which drops the board's Wi-Fi network (LINK,
remote screens, network apps). While the board is joined, WIFI WATCH watches
the link's channel only and says so; MORE > ALLOW LINK DROP asks before it
hops.

## T-Display: the C6 forwarder

The T-Display's Wi-Fi radio is an ESP32-C6 coprocessor. Its stock firmware
does not forward monitor frames, so capture starts with
`LAP_CAPTURE_NO_TRANSPORT` and the app shows NOT LISTENING with the fix:

```
c6 flash /sdcard/lakeshark/c6/ls_c6_net.bin
```

## Log

Each session writes `/sdcard/wifi_watch/alerts-YYYYMMDD-HHMMSS.csv` on its
first alert:

```
time,severity,kind,channel,source,target,detail
```

## Limits

- Tables are fixed: 64 networks, 64 probing clients, 32 deauthentication
  pairs, 24 handshakes; the least recently heard entry makes room. 128 alerts
  stay on screen; all of them go to the log.
- A full firmware ring drops frames; the count shows in the title bar and
  under COUNTERS.
- Protected management frames (802.11w) are counted; their reason codes are
  sealed and show as `sealed`.

## Simulator

```
LAPSIM_CAPTURE=capture.pcapng lsdk sim wifi_watch
```

`LAPSIM_CAPTURE_NO_TRANSPORT=1` shows the C6 message, `LAPSIM_CAPTURE_JOINED=1`
the link guard. `LAPSIM_SCRIPT=0:mesh` brings up a mesh with TRANSMIT armed
and three nodes; each notice handed over is logged as
`[app wifi_watch] mesh <where>: <text>`. The simulator has no vibration
motor, so it shows `{x}` and NO MOTOR. `app-sdk/tests/test_wifi_apps.py` builds its captures with
`app-sdk/tests/pcapng_gen.py`.
