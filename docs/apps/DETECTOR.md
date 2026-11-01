# DETECTOR

Rules over what the board already sees. When Remote ID drones, SWEEP
categories, aircraft emergencies, pagers, P25 calls, sub-GHz patterns or mesh
events match a rule you choose, DETECTOR raises a banner, a sound, speech, a
vibration, a mesh message or an SD log line.

| | |
|---|---|
| HOME group | USER |
| Radio | None of its own. Uses what SWEEP, Remote ID, ADS-B and the other apps receive |

## What it does

DETECTOR is an SD card app: a `.lap` file that shows up under USER on HOME.
To install it, use the store in the LakeShark hub, or copy the file into
`/apps` on the SD card and restart. See [USER](USER.md) and the
[app guide](../../app-sdk/README.md). The board asks once, the first time you
open it, to allow receiving and background running.

It watches twelve sources. Each rule names the sources it applies to:

| Source | What it is |
|---|---|
| `rid.ble` | Bluetooth Remote ID messages from drones |
| `sweep.ble`, `sweep.wifi` | SWEEP's categories: CAMERA, BODYCAM, DRONE, TRACKER, ATTACK |
| `lora.dronelink` | LoRa drone-link activity on 868 or 915 MHz |
| `drone.watch` | The drone watch in DRONES |
| `adsb` | Aircraft the ADS-B app is tracking |
| `rec.watch`, `rec.catch` | New patterns found by REC's watch and CATCH |
| `pocsag` | Pages from the pager decoder |
| `p25` | P25 calls |
| `mesh.peer` | MeshCore nodes that are new or lost |
| `mesh.alert` | Alerts that other LakeShark boards share on the mesh |

DETECTOR starts no receiver for aircraft, REC, pagers or P25. Run ADS-B, REC,
the pager decoder or P25 in their own apps and DETECTOR sees what they report.
For Remote ID, SWEEP's Bluetooth categories and the LoRa drone link, DETECTOR
asks the board to listen. It only receives and never transmits, apart from the
mesh share described below.

RUN starts a background service. The rules then keep working after you leave
the screen. Without it, DETECTOR watches only while its screen is open. The
service stops if you open another SD app, and only one SD app can keep a
service at a time. RADIOS shows what is running. DETECTOR remembers RUN, and
it starts watching again the next time you open it.

## Radio

DETECTOR has no RADIO picker. What it sees depends on the other apps.
`lora.dronelink` is only started when an enabled rule uses it.

## How to use it

| Key | Touch | Action |
|---|---|---|
| R | RUN | Start or stop watching |
| V | VIEW | Switch ALERTS, AIRCRAFT and RULES |
| T | TEST (TOGGLE in RULES) | Send a test alert to every sink that is on. In RULES, switch the selected rule on or off |
| L | RELOAD | Read the rules file again |
| O | OPTIONS | Open OPTIONS |
| Up, Down | Tap a row | Select a row |
| Enter | Tap the selected rule | In RULES, switch the selected rule on or off |
| Esc | | Back to HOME |

Switching a rule in RULES lasts until the file is read again. It is not
written to the file.

OPTIONS holds:

- **SOURCES**: a switch for each of the twelve sources above. A source that is
  off never matches. All are on at first.
- **SINKS**: BANNER, VIBRATE, SD LOG (NO ADDRESSES), SPEECH, CHIRP and SHARE ON
  MESH. A rule only reaches the sinks that are on here. BANNER, VIBRATE,
  SPEECH and CHIRP are on at first. SD LOG and SHARE ON MESH are off.
- **QUIET HOURS**: QUIET HOURS (off at first), QUIET FROM and QUIET UNTIL as
  local hours 0 to 23 (22 and 7 at first), UTC OFFSET from -12 to 14, and
  SHARE WHILE QUIET.
- **MESH SHARE**: CHANNEL SLOT 1, 2 or 3, and which sinks an alert from
  another board uses: RECEIVED BANNER, VIBRATE, SPEECH and CHIRP. Banner and
  chirp are on at first.
- **RELOAD RULES**: read `/sdcard/detector/rules.json` again.
- **CLEAR ALERTS**: empty the alert list on screen.

OPTIONS choices are remembered. In a list, Esc or BACK goes up one level.

## What the screen shows

**The top row** shows WATCHING (the service is running), ON SCREEN (watching
only while open) or STOPPED, then QUIET when quiet hours are holding sound
back, MESH when SHARE ON MESH is on, and the view. The line under it counts
rules, matches, held alerts, dropped sightings and sightings seen. A line
below that shows a rules file problem or an SD log failure.

**ALERTS** lists the last 24 alerts, newest first, with their age and a line
such as the source label, the facts and the rule that matched. The selected
alert opens a box with the sinks that took it. It also says if quiet hours
held some back, what happened to a mesh share, how many more matches the
cooldowns held back, and for a mesh alert the distance and direction of the
sender when both ends have a position. Emergencies are red.

**AIRCRAFT** lists up to 16 aircraft from the ADS-B app, nearest first, with
the rules each one matches. Squawk 7500, 7600, 7700 and any emergency status
show in red.

**RULES** lists every rule with `[x]` when it is on, its id and its sources.

## The rules file

The rules are in `/sdcard/detector/rules.json`. The app reads it when it opens
and when you press RELOAD. DETECTOR does not create it. With no file, it uses
a built-in set: Remote ID, trackers that are with you, cameras and aircraft
emergencies are on, and a few examples are off. A copy with more examples is
in the repository as `data/detector/rules.json`.

The file is read whole or not at all. One mistake rejects it, and the previous
rules stay in use. The screen says the rules were rejected but not which line.
The file can be up to 32768 bytes with up to 64 rules. Names are case
sensitive, and an unknown name or a name given twice is refused. Numbers are
whole and have no leading zeros. There are no comments.

```
{
  "version": 1,
  "defaults": {"cooldown_s": 60, "rule_cooldown_s": 120},
  "quiet_hours": {"start": 22, "end": 7, "utc_offset": -5},
  "mesh": {"channel": 1, "share": false},
  "rules": [
    {
      "id": "cameras",
      "enabled": true,
      "sources": ["sweep.ble", "sweep.wifi"],
      "match": {"category_in": ["CAMERA"]},
      "sinks": ["banner", "sd"]
    },
    {
      "id": "low-and-close",
      "enabled": true,
      "sources": ["adsb"],
      "match": {"distance_km_within": 5, "altitude_baro_ft_below": 2000},
      "sinks": ["banner", "vibrate"]
    }
  ]
}
```

**Top level.** `version` must be 1 and `rules` is required. `defaults`,
`quiet_hours`, `mesh` and `sweep_rules` are optional. `sweep_rules`, if given,
must be `"/sdcard/sweep/rules.json"`.

- `defaults` can set `enabled`, `cooldown_s`, `rule_cooldown_s` and
  `dedupe` (only `"rule_entity"`). Rules start from these. Without them a
  rule is off, with `cooldown_s` 60 and `rule_cooldown_s` 120.
- `quiet_hours` takes `start` and `end` (0 to 23) and `utc_offset` (-12 to
  14). Having it turns quiet hours on.
- `mesh` takes `channel` (1 to 3) and `share`. `"share": true` turns SHARE ON
  MESH on.
- When the file has `quiet_hours` or `mesh`, they replace the OPTIONS values
  each time the file is read.

**Each rule** needs `id`, `sources`, `match` and `sinks`. `enabled`,
`cooldown_s` and `rule_cooldown_s` are optional.

- `id`: up to 31 letters, digits, `-` or `_`. Each id is used once.
- `sources`: names from the table above, each at most once.
- `sinks`: any of `banner`, `vibrate`, `sd`, `tts`, `tone`, `mesh`. `tts` is
  speech and `tone` is the chirp. `flipper` is accepted and ignored.
- `cooldown_s`: seconds before the same device, aircraft or call can raise
  this rule again. 0 to 86400.
- `rule_cooldown_s`: after the rule raises an alert, further matches are held
  for this many seconds and counted. The next alert says how many were
  grouped. 0 to 86400.
- `match`: one or more of the tests below. All of them must be true.

**Tests for `rid.ble`, `sweep.ble`, `sweep.wifi`, `lora.dronelink` and
`drone.watch`:**

| Test | Value | Meaning |
|---|---|---|
| `category_in` | `"CAMERA"`, `"BODYCAM"`, `"DRONE"`, `"TRACKER"`, `"ATTACK"` | SWEEP category |
| `section_in` | `"with_you"`, `"new"`, `"passing"` | Where SWEEP lists it |
| `min_rssi_dbm` | 0 or a negative whole number, down to -127 | Signal at least this strong |
| `validated_message` | `true` or `false` | `rid.ble` only: a validated Remote ID message |
| `band_in` | `"868"`, `"915"` | `lora.dronelink` only |
| `min_hits_per_min` | whole number | `lora.dronelink` only |

A rule with sources `["drone.watch"]` only takes `category_in` and
`min_rssi_dbm`, and `min_rssi_dbm` counts only for calibrated readings. Such a
rule replaces the built-in drone alert, which raises a banner and a
vibration when a drone link is near, with a note when its signal is rising.

**Tests for `adsb`.** The rule's sources must be exactly `["adsb"]` and it
needs at least one of these:

| Test | Value | Meaning |
|---|---|---|
| `squawk_in` | up to 8 strings of 4 octal digits, such as `"7700"` | Squawk code |
| `emergency_status` | `true` | The aircraft reports an emergency |
| `callsign_prefix_in` | up to 8 strings of capital letters and digits, 1 to 8 long | Callsign starts with one of them |
| `icao_in` | up to 8 strings of 6 hex digits | ICAO address |
| `icao_ranges` | pairs such as `["A00000","A0FFFF"]` | ICAO address range. Shares the 8 slots with `icao_in` |
| `adsb_category_in` | `light`, `small`, `large`, `high_vortex`, `heavy`, `high_performance`, `rotorcraft`, `glider`, `lighter_than_air`, `parachutist`, `ultralight`, `uav`, `space`, `emergency_vehicle`, `service_vehicle`, `obstacle` | Emitter category |
| `distance_km_within` | 1 to 1000 | Within this range of you |
| `altitude_baro_ft_below` | 0 to 60000 | Barometric altitude below this |

An empty list is accepted and matches nothing. Each aircraft is checked once a
second. One that has not been heard for 60 seconds does not alert. A change in
squawk or emergency state counts as a new alert.

**Tests for `rec.watch`, `rec.catch`, `pocsag`, `p25`, `mesh.peer` and
`mesh.alert`.** These rules can only name sources from this group:

| Test | Value | Meaning |
|---|---|---|
| `capcode_in` | up to 8 whole numbers | `pocsag` capcode |
| `talkgroup_in`, `unit_in` | up to 8 whole numbers | `p25` talkgroup or unit |
| `text_contains_any_ci` | up to 4 words, 15 characters each | `pocsag` page or `mesh.alert` text contains a word, in any case |
| `frequency_khz` | `[low, high]` | `rec.watch`, `rec.catch` frequency in kHz |
| `novel_pattern` | `true` | `rec.watch`, `rec.catch`: a pattern not heard before |
| `emergency` | `true` | `p25` or `mesh.alert` emergency flag |
| `event_in` | `"new"`, `"lost"` | `mesh.peer`: node first heard, or not heard for 30 minutes |

For a pager match only the word that matched is kept, never the page.

**Limits on alerts.** At most four alerts go out in a burst, then one every two
seconds. The rest are counted as held. Quiet hours hold back the chirp, speech,
vibration and the mesh share, unless SHARE WHILE QUIET is on. The banner and
the SD log carry on. If the board has no time yet, quiet hours count as
active.

## Sinks

- **Banner**: a notice with the source label and the facts. An emergency is
  raised as an alarm.
- **Chirp**: one sound.
- **Vibration**: 400 ms.
- **Speech**: a short phrase such as "P25 emergency." or "AIRCRAFT alert." It
  names the kind of alert and never reads out what was received.
- **SD log**: one JSON line per alert in `/sdcard/detector/events`, in files
  `detector-0.jsonl` to `detector-7.jsonl`. Each file takes 4 MB, then the
  next one is started and the oldest is overwritten. A line holds the time,
  source, rule ids, category, signal, counts and what each sink did. It holds
  no device address, no position and no pager or message text. For an
  aircraft it holds the ICAO address, callsign, squawk, emergency state,
  altitude and category it broadcast.
- **Mesh share**: sends a one-line alert on a private MeshCore channel slot.
  It needs MESH running with TRANSMIT armed and a private channel in the slot
  chosen under MESH SHARE. It sends at most one a minute and not the same
  summary again within ten minutes. While the minute runs it keeps the newest
  one, for up to five minutes, with a count of the others. Alerts carry no
  position. Alerts received from other boards are never sent on again.

Every alert except a test is also put on the board's event timeline.

## Alerts from other boards

DETECTOR reads group messages on the private channel slots. A message that
begins `#LSA/1|` is an alert from another LakeShark board running DETECTOR.
Your own alerts and repeats are ignored. A mesh alert that no rule takes is
raised on its own, using the RECEIVED sinks in OPTIONS. If DETECTOR can place
the sender, from a position in the alert or from your MESH node list, and you
have a GPS fix, the alert shows distance and compass direction.

## Tips and limits

- Names and signatures are hints, not proof of what a device is.
- A sink only works if it is on in OPTIONS as well as in the rule.
- It needs firmware that supports apps with SDK 2.2.
- With no SD card or no rules file, the built-in rules are used and the
  screen says so. If an SD log write fails, the screen says so too.

## More guides

- [USER](USER.md)
- [SWEEP](SWEEP.md)
- [DRONES](DRONES.md)
- [ADSB](ADSB.md)
- [MESH](MESH.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
