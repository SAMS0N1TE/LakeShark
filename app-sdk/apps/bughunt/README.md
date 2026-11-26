# BUGHUNT

One app for trackers and bugs. A receive-only sweep for hidden trackers and
bugs in a room or a car, a Bluetooth tag watch that warns when a tag is
following you, and a finder to walk you to whatever it heard. Phone apps
only see Bluetooth tags, and cheap RF detectors beep at any burst without
saying what it was. BUGHUNT listens three ways at once and says what it
thinks each thing is.

BUGHUNT 1.2 adds the finder's gauge and dial, AUTO RANGE, distance, BODY
direction, burst timing, HUNT FREQ, the SDR and DUAL receivers, MINE and
WATCH marks, TRACK KINDS, the OPTIONS tree and device names from a card
file (see HUNT, Tracking and Device names).

BUGHUNT 1.1 took in the BLE WATCH app: its tag list, rotating-address
following, "a tag is following you" alert, meter and tone, card log and
settings are all here (TAGS, HUNT and MORE). BLE WATCH is retired.

| Source | What it listens for |
|---|---|
| LoRa radio (SX1262 or LR2021), RSSI | Cellular uplink where GPS trackers transmit, ISM 315 / 433.92 / 902-924 MHz, the UHF bug band 400-470 MHz (ROOM), and 2.4 GHz energy on an LR2021 that reports it can tune there. 868 MHz is left out: in the Americas the 850 downlink (869-894) spills into it |
| Bluetooth (C6) | AirTag / Find My, Samsung SmartTag, Tile, Chipolo, Google Find Hub, DULT separated-mode tags, through the tag engine (`bh_tags.c`, once BLE WATCH's) |
| Wi-Fi capture (C6) | Stations made by camera makers (Hikvision, Dahua, Wyze, Reolink, Axis), and any station uploading steadily like a video stream |

Nothing transmits. Wi-Fi capture stays on the channel the board is on unless
MORE > WI-FI HOP is allowed (asked every time the app opens), because hopping
drops the board's Wi-Fi link.

## Tabs

SWEEP, FINDINGS, TAGS and HUNT: `<` `>` on the strip, a swipe, or LEFT /
RIGHT. The keys along the bottom change with the tab; OPTIONS has the settings.

## Sweep

1. Put your own phone in airplane mode: it transmits in the same bands.
2. **SWEEP ROOM** (2/4/8 min) or **SWEEP CAR** (10/15/30 min). Walk the room
   slowly; in a car sit or drive, since trackers check in every 1-10 minutes.
3. **SWEEP** shows the timeline: a row per band, a column per time slot, max-hold
   per slot (bright cells are bursts over each step's own quiet level, dim
   blue is a tower's steady downlink), the max-hold per step underneath
   (magenta bars are steady carriers), and the Bluetooth and Wi-Fi tallies.
4. **FINDINGS** ranks verdict cards with a confidence and the evidence, e.g.
   "835 MHz burst every ~2 min, strong (-58 dBm), 6 bursts: possible cellular
   tracker close by".
5. **HUNT** a card (see HUNT below).

REGION (US, ARGENTINA, US + AR) picks the uplink bands: US B12 699-716,
B13 777-798, B5/B26 814-849; Argentina adds B28 703-748 and B8 880-915.
In Argentina pick ARGENTINA: 777-798 is B28 downlink there, steady towers
that only cost time per pass (they are drawn dim blue, never reported).

## TAGS: the tag watch

From the moment BUGHUNT opens, every Bluetooth advert goes into one table
that lasts until the app closes, through sweeps and hunts. TAGS shows it:

- The head is a radar: rings, a turning beam, every device heard in the
  last minute a blip at its own bearing and nearer the middle the louder it
  is; a tag that follows you flashes. Beside it: tags and devices, the
  source (CAPTURE or SCAN), GPS and LOG chips, the loudest tag on a meter,
  adverts a second in braille and the alert, pulsing.
- **VIEW TAGS**: trackers first, alerting ones on top. Two lines each on the
  handheld: kind, name, RSSI, trend (+/-), sparkline, last heard; then
  sightings in 5 and 15 minutes, time with you, moved, rotations, AWAY or
  owner near. The pick's detail sits under the list (beside it, in the left
  column, on a wide screen): address and kind, company, advert type, flags,
  TX power, services, owner state, first/last seen, time with you, distance
  moved, sightings, a 15-minute presence strip, a 2-minute RSSI graph and
  the last advert's bytes.
- **VIEW ALL**: every device, strongest first, with advert type and flags.
- **VIEW INFO**: the source, adverts, GPS, the alert settings (taps on
  `[-]` `[+]`, POSSIBLE, LOG), the log file, the logs already on the card
  and the kinds it names.

The list follows a finger. UP / DOWN pick; ENTER, HUNT THIS TAG or a second
tap on the pick hunts it.

### What it names

| Tag | Kind | How it is told | Notes |
|---|---|---|---|
| FINDMY | Apple Find My (AirTag, AirPods, Find My tags) | company 0x004C, continuity type 0x12 | 25 bytes: separated from its owner (AWAY); 2 bytes: owner nearby, never alerts |
| APL-PP | Apple proximity pairing | company 0x004C, type 0x07 | AirPods out of the case, AirTag setup; possible tracker |
| TILE | Tile | service 0xFEED / 0xFEEC | keeps one address |
| SMTAG | Samsung SmartTag | service data 0xFD5A | |
| GFMDN | Google Find My Device | service data 0xFEAA, frame 0x40/0x41 | 0x41 is unwanted-tracking mode (AWAY) |
| DULT | Cross-platform tag, separated | service data 0xFCB2 | uncertain: the draft spec's UUID |
| CHIPLO | Chipolo | service 0xFE33 / 0xFE65 | uncertain |
| FPAIR | Google Fast Pair | service 0xFE2C | headphones mostly; possible tracker |

Also named, never alerted on: iBeacon, Eddystone, Apple continuity, Microsoft
beacons, and the manufacturer of anything with company data (a short table).
Uncertain identifiers are marked in `bh_tags.c`. The Find My kind
("AirTag?", "AirPods?") is read from status bits as AirGuard reads them and
always shows a question mark.

### Following you

A tracker is *with you* from its first sighting until it goes unheard for
5 minutes. The alert fires once per tag when it has been with you for the
set time (10 minutes; 2 to 60 in MORE or on INFO) and was heard in the last
minute. It fires on any tab, sweeping or not.

- With a GPS fix the tag must also have moved 250 m with you, so tags at the
  next table do not alert while you sit still.
- Without a receiver or fix the alert goes by time alone.
- Find My devices beside their owner, beacons and phones never alert.
  Possible trackers (APL-PP, FPAIR) alert only with POSSIBLE TAGS: ALERT.

The alert is a board alert (banner, ring, vibrate), a haptic pulse, a red
line over the title for 30 s (tap it, or press L, to HUNT the tag), a
"! N FOLLOWING" mark in the title bar and on the strip, and a line in the
log. BUGHUNT keeps the screen awake while it sweeps, hunts or shows TAGS.
It watches only while it is open: it has no background service.

### Rotating addresses

Find My, SmartTag, Google and DULT tags change address (every 15 minutes
near the owner, about daily when separated; SmartTag about every 15).
When a new address of the same kind starts within 2 s of an old one going
quiet, at a similar signal (15 dB) and, for Find My, with the same status
byte, they are counted as one tag (`rot` in the list). The check waits 15 s
so a tag that is merely between adverts is not taken for a rotation. HUNT
follows a tag through its address changes.

Limits: two tags of one kind that rotate in the same minute can be swapped;
a tag that rotates while out of range starts again as new; Fast Pair and
proximity pairing are grouped the same way but their payloads change too.

### The log

`/sdcard/bughunt/tags-YYYYMMDD-HHMMSS.csv`, one file per session, on by
default when there is a card (LOG on TAGS, or MORE > TAG LOG TO CARD):

```
uptime_s,event,address,addr_type,class,company,rssi,sightings_5m,span_s,moved_m,rotations,name
```

Events: `new`, `new-tracker`, `seen` (once a minute per tracker still
about), `alert`. Written every 10 s; up to 10 s can be lost when the app is
closed mid-write. The columns are BLE WATCH's, so anything that read its
logs reads these. BLE WATCH's own logs stay where it wrote them,
`/sdcard/ble_watch/watch-*.csv` (removing the app does not touch them);
VIEW INFO counts them and names the newest.

## HUNT

The finder for all three: a card from FINDINGS, a tag from TAGS or the
following alert, or a frequency you name (HUNT FREQ: a list of common ones,
or type it in MHz, no sweep needed). The radio parks on a frequency; a tag is
followed through its address changes; a Wi-Fi station by its address.

- **The gauge**: half a dial, cold on the left and hot on the right, in four
  zones (a tag: FAR, NEARBY, CLOSE, HERE; a frequency: WEAK, FAIR, STRONG,
  HOT). The scale lights up to the level, the needle shows it, a red tick
  holds the most it has been. Beside it: the level, the peak, the range, how
  far a tag is (`~3 m (2-6)`) or how far a frequency is over its quiet level,
  HOTTER / COLDER with arrows, and the sound.
- **The level** is the strongest reading of the last moment (2.5 s for a tag,
  0.4 s for a frequency), not the latest one: Bluetooth fades 10-20 dB from
  one advert to the next as you move, and a fade read as "colder" sends you
  the wrong way.
- **AUTO RANGE**: next to a strong transmitter the meter steps up 20 dB at a
  time (up to 60 dB on a frequency) so it never sits pinned at the top, and
  back down after 3 s low. The Geiger ticks follow the range too.
- **Distance** for a tag: from its level and what a tag of its kind reads at
  1 m (its advertised TX power when it sends one). The span in brackets is a
  6 dB fade either way. **CAL** (K, or OPTIONS > HUNT): hold the tag 1 m away
  and press it; that tag's distances use the level you measured.
- **The dial**: the circle you turned through, heading up (the top is where
  the board points; N E S W turn as you turn). Each 10 degrees is coloured
  by how loud it was there, the loud part drawn bolder; faint dots are the
  part not turned through yet. Each heading keeps a smoothed level (a peak
  pulls it more than a dip) and its neighbours count half, so one lucky
  reading does not win. The arrow is the answer, and beside it in words:
  AHEAD, TURN RIGHT 40 deg, BEHIND YOU, how sharp it is (SHARP, BROAD,
  UNCLEAR, with how many dB it stands out), and how much of the circle is
  done. No arrow until one way stands out by 2 dB. Tap the dial (or D) to
  start the circle again after you move.
- **DIRECTION: PEAK or BODY**. PEAK: hold the board flat in front of you;
  the loudest way is toward it. BODY: hold it flat against your chest and
  turn; your body blocks 10-20 dB at 2.4 GHz, so the quietest way is your
  back to it and the thing is behind you. BODY works better for a tag close
  by, where PEAK's lobe is broad.
- **Bursts** on a frequency: a tracker that checks in every minute or two is
  silent between bursts, so each burst is kept with its peak and the heading
  it came from. The strip says how many, the spacing ("every ~75s"), when the
  next is due, and the last burst against the one before (+6.0 HOTTER).
  Stand still between bursts, then compare. A carrier that never stops says
  STEADY CARRIER instead.
- Sound (OPTIONS > HUNT > HUNT SOUND): AUTO plays BLE WATCH's rising tone on
  a tag and the Geiger tick on a frequency or a station; TICKS or TONE pick
  one for all.
- History: a tag's last two minutes from the tag watch; else the readings
  since HUNT began.
- UP / DOWN: the next or previous tag (from TAGS) or finding.

### Receivers: LORA, SDR, DUAL

A frequency HUNT can use the LoRa radio in the socket, an SDR on USB
(RTL-SDR or HackRF, from its spectrum lines: the strongest bin within
250 kHz of the target, tuned a quarter of its rate off so its centre spike
is not read as the signal), or both (RECEIVER on the HUNT tab, or OPTIONS >
RECEIVERS). The SDR's level is dBFS, relative to its own quiet level.

**DUAL** puts both on one frequency: a directional antenna on one (OPTIONS >
RECEIVERS > DIRECTIONAL ON: SDR or LORA) and the other as the reference.
The POINT meter shows the directional one over its quiet level minus the
reference over its own (-20 to +20 dB): it moves only when the directional
antenna turns toward or away, not when a tracker's bursts come and go, and
the dial turns on it too. DUAL needs firmware that lets a USB SDR receive
beside the LoRa radio (after 3.0.3); older firmware says so and uses the
SDR alone.

## Tracking: MINE and WATCH

MARK on TAGS (or K) steps the pick NONE, MINE, WATCH:

- **MINE**: your own tag. It never raises the following alert, sinks to the
  bottom of the list and shows `M`.
- **WATCH**: a tag you carry (keys, a bag). When it goes unheard for the set
  time (OPTIONS > TAGS AND ALERTS > WATCH: 1 to 30 minutes, 2 by default,
  plus 15 s for a rotation to link), a board alert says it was left behind;
  when it is heard again, "is back in range". Shows `W`, red while gone.

Marks follow a tag through its address changes and are kept on the card by
address (slot `marks`, 24 at most, the oldest goes): a tag that keeps one
address (Tile, many cheap tags) is known again next time; a rotating one
for as long as it keeps that address.

**TRACK KINDS** (OPTIONS > TAGS AND ALERTS): each kind (Find My/AirTag,
Tile, SmartTag, Google Find Hub, DULT, Chipolo) ALERT or LIST ONLY. A kind
on LIST ONLY is still listed and logged; it just never alerts.

## Device names (ids.bin)

The tag engine names trackers itself. For everything else BUGHUNT can read
a name file off the card, built from the public registries by
`tools/build_ids.py`:

| Table | From | Names |
|---|---|---|
| Bluetooth company IDs | Bluetooth SIG assigned numbers | 4082 |
| 16-bit service UUIDs | Bluetooth SIG (SIG and member UUIDs) | 807 |
| Appearance | Bluetooth SIG | 342 |
| MAC makers (MA-L) | IEEE registry | 40535 |

About 640 KB. It goes at `/sdcard/lakeshark/packs/bughunt-ids.bin` (from
PACKS) or `/sdcard/bughunt/ids.bin`. Each name is a block read and a name
read through a small index loaded at start, one card job at a time, and the
answers are kept (96). With it:

- a device with company data the engine's short table lacks gets the
  company's name, one with only a service UUID the service's owner, and one
  with a public address and nothing else its maker; "Samsung Watch" when it
  states its appearance;
- the detail view adds the maker of a public address, the appearance and
  the names of its first three services;
- a Wi-Fi station gets its maker's name, and a maker that makes cameras
  first (Hikvision, Dahua, Wyze, Reolink, Amcrest, Arlo, Ring, Blink, Lorex,
  Swann, Uniview, Hanwha Vision, Vivotek, Ezviz, Zmodo, Mobotix, Avigilon,
  Verkada, GeoVision, Tiandy, Sunell, Raysharp, Milesight, Axis) counts as a
  camera like the built-in list does. Random addresses (most phones, most
  Wi-Fi probes) have no maker.

INFO says what is loaded. Without the file everything works as before.

## OPTIONS

One tree, a level at a time, BACK at the top of each (cancel goes up):
SWEEP (region, room and car length, Wi-Fi hop, sound), TAGS AND ALERTS
(following alert, possible tags, TRACK KINDS, WATCH time, marks, the log,
clear the list), HUNT (hunt a frequency, direction mode, meter range, hunt
sound, CAL), RECEIVERS (LORA / SDR / DUAL, which has the directional
antenna, what was found) and HOW IT WORKS. Kept on the card (`lap_save` slot
`cfg`); 1.0's and 1.1's settings carry over. Saves go one at a time and each
is polled to the end: an app has four card jobs, shared with the tag log,
the marks and the name file. The log setting saved is your choice: a card
that fails stops the log for the session without turning it off for next
time.

## Keys

| Key | Does |
|---|---|
| LEFT / RIGHT / TAB | tabs |
| UP / DOWN | pick (FINDINGS, TAGS); next tag or finding (HUNT) |
| ENTER | HUNT the pick |
| BACK | HUNT back to where it came from; INFO or ALL back to TAGS; then HOME |
| L | HUNT the tag in the alert |
| K | MARK the pick (TAGS); CAL at 1 m (HUNT on a tag) |
| Q | HUNT FREQ |
| E | RECEIVER: LORA, SDR, DUAL |
| B | DIRECTION: PEAK or BODY |
| D | start the dial again |
| + / - | the following alert's minutes (TAGS) |
| P | possible trackers alert too (TAGS) |
| G | the card log on/off (TAGS) |

## How the sweep decides (bh_core.c)

* Each step keeps a quiet level (falls fast, rises slowly). A sample 10 dB over
  it, or more where the level wanders (tower downlink next door), is hot.
  Hot samples in a band within about one pass of each other are one burst.
* A **period** is a spacing 80% of the burst intervals fit, allowing a missed
  check-in and one pass of timing error; it has to be at least a minute and
  five passes long, so an 11 s pass sees periods from 1 min up to 15 min.
  Cellular: possible tracker. ISM: sensor or remote unless it is strong.
* Irregular cellular bursts (5 or more, no period): most likely a phone.
* A step whose quiet level sits 12 dB or more over the plan's median, outside
  the cell bands: **steady carrier** (audio bugs; also some appliances). In the
  cell bands a steady level is tower downlink and is not reported.
* -45 dBm or more anywhere: **very strong**, a transmitter within a metre or two.
* A tracker tag heard for 70% of the sweep and still about: **with you**
  (CAR) / **in range** (ROOM); separated from its owner scores higher, near
  its owner (yours?) lower. Apple devices that say their owner is near
  (AirPods, AirTags at home) are everywhere people are, so two or more of
  them make one low card; a tag away from its owner always gets its own.
  (The sweep keeps its own tag table for this, so "the whole sweep" means
  this sweep; TAGS keeps everything since the app opened.)
* A Wi-Fi station uploading at least 5 kB/s in 70% of 10-second windows for
  30 s or more: **uploading steadily**; a camera maker's address adds to it.

## Measured on the SX1262 board (firmware 2.23.1)

* The scanner stepped about **8 frequencies a second**: every step closed the
  receiver, handed the radio lease back and took it again, then configured
  the part afresh (calibration included), with one RSSI reading a step. The
  92-step room plan was one pass every ~11 s.
* Noise floor -108 to -109 dBm per 500 kHz step (max-hold over minutes about
  -104 to -108). Indoors in Argentina: B28 downlink 758-803 MHz and B5
  downlink 869-894 MHz sit at -65 to -90 dBm; uplink bursts of -83 to -96 dBm
  came and went in 703-749 and 809-853 MHz (phones nearby).
* Firmware 2.23.1 also failed a scan after about 125 retunes; BUGHUNT's
  restart for every list of 64 (the scanner's limit) kept it clear of that.

## Faster stepping and peaks (firmware after 2.23.1)

Scans now retune the receiver in place and keep the lease (handed back once
a pass of the list, at most once a second), and scanner version 2 reads the
RSSI through each dwell and reports every step's peak. BUGHUNT asks for
version 2 with a 3 ms dwell and falls back to version 1 on older firmware.
On the bench (worker passes 40 ms apart, the lease answering EAGAIN twice per
open as on the board): version 1 went from 8.4 to 24.4 steps a second, and
version 2 runs about 80, catching a burst one reading long in every dwell
it falls in. The board's worker runs every 20 ms, so expect about twice
that there; HUNT on a frequency reports the peak of every 40 ms.

## Sources

Bluetooth: the API 2.7 advertising capture (`lap_ble_capture_*`, 256-frame
ring, needs the RADIO_RX grant). If the capture is refused it falls back to
`lap_ble_scan_*` (8-report queue, so busy places lose some reports; there
is no advert type then), and tries the scan again while it is busy. GPS
through `lap_gps_acquire`.

## Simulator

`tools/sim_fixture.py` writes a pcapng (a Hikvision camera streaming, a
separated AirTag, a passing Tile) and prints a LAPSIM_SCRIPT for the RSSI
(a 433.92 MHz carrier, a tracker on 835-839 MHz every 75 s, phone bursts):

    python3 tools/sim_fixture.py /tmp/fix.pcapng 300 > /tmp/fix.txt
    cd bench && LAPSIM=bughunt LAPSIM_CAPTURE=/tmp/fix.pcapng \
      LAPSIM_SCRIPT="$(cat /tmp/fix.txt);20:key=r" lssim lap -A 40 -f 6200 -o out.bmp

`bench/tests/bughunt_tags_fixture.py` writes fourteen minutes of trackers,
phones and beacons, with one AirTag changing address half way; replayed
fast it raises the following alert in a few seconds:

    python3 bench/tests/bughunt_tags_fixture.py /tmp/tags.pcapng
    LAPSIM=bughunt LAPSIM_CAPTURE=/tmp/tags.pcapng LAPSIM_CAPTURE_PACE=0 \
      LAPSIM_SCRIPT="5:key=@right;6:key=@right" lssim lap -A 40 -f 120 -d

Script steps must be in frame order. `iorssif=MHZ,TENTHS`, `ionoise=N` and
`heading=DEG` are the simulator fixtures this uses.

Bench: `test_bughunt_core` feeds synthetic RSSI timelines, adverts and frames
to the core and checks the verdicts; `test_bughunt_tags` the tag engine
(kinds, malformed adverts, rotations, the alert, the CSV line);
`test_ble_apps_sim` runs the app over the tag capture (TAGS, ALL, INFO,
HUNT, the alert on any tab, the log, wide screen, scan fallback).

## Not verified

* 2.4 GHz on an LR2021 (its band is added only when `lap_radio_info2` reports
  a range covering 2402-2482 MHz; a refused frequency drops the band).
* A real GPS tracker, audio bug or hidden camera.
* Scanner version 2 and in-place retuning on either board: the step rate,
  that readings right after a retune are settled (300 us wait at 500 kHz),
  and that MeshCore still gets the radio between passes.
* DUAL with both receivers at once: needs the firmware change after 3.0.3
  (a USB SDR may receive beside a packet or scan session); on 3.0.3 the LR
  board with an RTL-SDR showed the fallback.
* The distance's 1 m references are typical phone-class readings, not
  measured on the board's antenna; CAL replaces them for one tag.
* BODY mode and the dial's answers against a real hidden tag.
