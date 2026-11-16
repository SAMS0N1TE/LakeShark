# TAP

A packet analyser for the radios the board can listen on: the list, tree
and bytes, display filters, colouring rules, statistics and capture files a
Wireshark user expects, in the hand. Receive only. Version 1.2.0.

| Source | Through | Shows |
|---|---|---|
| Wi-Fi 802.11 | `lap_wifi_capture_*` (API 2.7) | beacons, probes, auth, data headers, EAPOL marked |
| Bluetooth LE | `lap_ble_capture_*` (API 2.7) | AD structures, names, UUIDs, company IDs, iBeacon, Eddystone |
| LoRa | `lap_packet_*` | MeshCore and Meshtastic headers, raw payloads |
| Board decodes | `lap_event_*` | pages, APRS, AIS, ADS-B, P25 and sub-GHz as text records |

Encrypted payloads are named and saved as received. Nothing is decrypted.

Wi-Fi needs coprocessor firmware that forwards monitor frames. Where it does
not, the WIFI source shows `n/a` and the reason. On the T-Display that is
the C6 forwarder, installed once from the console:

```
c6 flash /sdcard/lakeshark/c6/ls_c6_net.bin
```

Bluetooth is the board's passive scanner: advertising channels 37, 38 and
39 only, no connections or data channels. See `docs/CAPTURE.md`.

## Screens

- Wide (a desk window or remote screen, 90 columns or more): menu bar
  (`File` `Edit` `Capture` `View` `Analyze` `Statistics` `Help`), the
  SOURCES panel, packet list, protocol tree, bytes, and a key bar (`PAUSE`
  `FILTER` `FOLLOW` `STATS` `SAVE` `SOURCES` `MENU`). Mouse: click rows and
  menus, wheel scrolls, right-click a packet for its menu.
- Handheld (the T-Display, 52 columns): one pane at a time, the board's
  finger-sized keys along the bottom.
  - The list, the detail and SOURCES AND DROPS open on the SOURCES panel:
    an antenna whose waves ripple while frames come in, and per source a
    lamp, its count, the radio's drops, a braille activity strip and
    packets a second. The title row carries a blinking LIVE lamp (steady
    PAUSED, `REC` beside it while frames go to the card, `FILE` when a
    saved capture is open). Tap the panel for SOURCES AND DROPS.
  - List: drag it with a finger; tap a packet to select it, tap it again
    for its tree and bytes. New packets slide in. Tap the heading to change
    the third column: source, time, destination, length. Keys: `PAUSE`
    `FILTER` `STATS` `MENU` (save, open, find, go to and mark are in the
    menu).
  - Detail: `BACK` `PREV` `NEXT` `MENU` (the packet menu); drag the tree
    and the bytes; swipe left and right for the next and previous packet.
    The tree starts with the frame itself: number, lengths, arrival time,
    time since the first and the previous packet, the colouring rule that
    matched, and `frame.len` as a capture file will have it.
  - The filter row opens the on-screen keyboard.
  - Statistics: a `< NAME n/7 >` strip at the top (or swipe) for the next
    table, drag or `UP` `DOWN` to scroll, `SORT` on the two address tables,
    `SOURCES`, `BACK`.
  - A status note covers the title for four seconds; a tap clears it.

## Sources and drops

`I`, a tap on the source row, or CAPTURE > SOURCES AND DROPS: each source,
running or why not and what to do about it, frames heard and frames the
radio dropped, the card (see below), and the store (packets, kilobytes, how
many aged out).

The source row shows each source's count and, in red, what its firmware
ring dropped (`WIFI 19k -11k`). The title shows how many packets aged out
of the store. TAP reads up to a whole firmware ring (256 Wi-Fi, 128
Bluetooth frames) per screen update, within 12 ms; a burst faster than that
is dropped by the radio and counted, never queued without bound.

## Capture to card

The store holds the newest 4096 packets. For anything longer, CAPTURE >
CAPTURE TO CARD (or `TO CARD` on SOURCES AND DROPS) writes every kept frame
to the card as it arrives, the way Wireshark captures to a ring of files:

| Setting | Choices | Default |
|---|---|---|
| CAPTURE TO CARD | ON / OFF | OFF |
| CARD FILE SIZE | 256 KB, 1 MB, 4 MB, 16 MB, 64 MB | 4 MB |
| CARD FILES KEPT | 2, 4, 8, 16, ALL | 8 |

- Files are `/sdcard/captures/tap-YYYYMMDD-HHMMSS_00001.pcapng`, `_00002`
  and so on. Each is a whole pcapng file with its own header and opens on
  its own. A file is closed at the first frame that would pass its size.
- With a number of files kept, the oldest is removed as a new one starts.
- Frames wait in a 192 KB buffer and go out through the board's SD jobs,
  two at most at a time; the screen never waits for the card. A frame that
  finds the buffer full is not written and is counted: SOURCES AND DROPS
  shows packets and bytes written, files made and removed, how many wait,
  and `n dropped: the card was slower than the air`. A card error stops the
  capture and says so.
- What the store ages out is already on the card. Paused frames and frames
  under a capture filter are in neither. Board decodes are text and are not
  written.
- Leaving TAP sends what was still waiting to the card.

## Open a capture

FILE > OPEN A CAPTURE (`L`) lists `/sdcard/captures` in the board's picker,
newest name first (up to 48), and reads the chosen file: pcapng or classic
pcap, little-endian, link types 127 (802.11 radiotap), 105 (802.11), 256
(Bluetooth LE LL with PHDR, advertising PDUs) and 270 (LoRaTap).

- The radios stop while a file is open, so its frames never mix with live
  ones. The lamp says `FILE`, the panel says `FILE  NOT LIVE` with the
  file's name, size and counts, and the first key is `LIVE`: it closes the
  file and the radios listen again.
- The list, tree, bytes, filters, find, marks, colouring and statistics
  work as they do live. Times are the file's.
- A file with more frames than the store holds is shown a part at a time
  (`part 1-4096 of 30000`): FILE > NEXT PART and PREVIOUS PART move through
  it, GO TO PACKET jumps to any frame number. The statistics always cover
  the whole file.
- Frames of other link types, and Bluetooth scan and connect requests, are
  counted (`n of kinds TAP does not read`) and not shown.
- SAVE writes what is open (all, displayed or marked) to a new file.

## Keys

| Key | Does |
|---|---|
| SPACE, P | pause / resume |
| / | type a display filter |
| X | clear the filter |
| R | recent filters |
| E | find: a filter, or text in quotes |
| N, B | find next, find back |
| G | go to packet number |
| K | mark / unmark the packet |
| T | time display: since first, since previous, time of day |
| F, V | follow the device / the conversation |
| S | statistics (`E` there: expert information, `Z`: sort) |
| I | sources and drops (`D` there: capture to card) |
| W | save pcapng to `/sdcard/captures` |
| L | open a capture; with one open, back to live |
| C | clear packets |
| O | options |
| M | packet menu |
| TAB | next pane |
| BACK | up a level, then HOME |

## Display filters

Wireshark's field names and operators:

```
frame.number  frame.len  frame.cap_len  frame.time_relative  frame.marked
wlan  wlan.addr  wlan.sa  wlan.da  wlan.ta  wlan.ra  wlan.bssid  wlan.ssid
wlan.fc.type  wlan.fc.subtype  wlan.fc.type_subtype  wlan.fc.protected
wlan.fc.retry  wlan.fc.tods  wlan.fc.fromds  wlan.fixed.reason_code
wlan_radio.channel  wlan_radio.signal_dbm  radiotap.dbm_antsignal
radiotap.channel.freq  radiotap.datarate  wlan_rsna_eapol.keydes.msgnr
eapol  llc  arp  ip  ipv6  radiotap  wlan_radio  wlan.mgt  _ws.malformed
btle  btle.advertising_address  btle.advertising_header.pdu_type
btle_rf.signal_dbm  btle_rf.channel  btcommon
btcommon.eir_ad.entry.device_name  btcommon.eir_ad.entry.company_id
loratap  loratap.channel.sf  _ws.col.info  _ws.col.protocol

==  !=  >  <  >=  <=   (eq ne gt lt ge le)
contains   matches (~)   in {a, b}
&&  ||  !   and  or  not   ( )

wlan.fc.type_subtype == 0x08 && wlan.ssid contains "Lake"
wlan.fc.type_subtype in {10, 12} || eapol
wlan.addr == 02:aa:00:00:00:01 && radiotap.dbm_antsignal > -60
frame contains "LakeNet"      wlan contains 88:8e
```

They mean what they mean in Wireshark 4: numbers are decimal or `0x` hex
(`true`/`false` for flags), a bare field is true when the frame has that
field (`wlan.fc.protected` is every 802.11 frame; `wlan.fc.protected == 1`
the protected ones), a comparison is false on a frame without the field,
`==` is true when any of a field's values match and `!=` when none do,
`==` and `contains` keep case, `matches` ignores it. `matches` takes a
small pattern: `.` `*` `+` `?` `^` `$` `[a-z]` `\d` `\w` `\s`; no groups
or `|`. The tests run forty of these filters through TAP and through
tshark on the same capture and compare the counts.

`frame.len` is the length the frame has in a capture file (the 802.11 frame
plus the 15-byte radiotap header TAP writes), as Wireshark shows it;
`len` below is the frame on air.

TAP's short forms work beside them, ignore case, and match an address by
its start:

```
wifi  ble  lora  decode  beacon  probe  mgmt  ctrl  data  eapol  deauth
auth  ibeacon  eddystone  scanrsp  rid  meshcore  meshtastic  protected
bad  retry  cut  marked
addr == aa:bb:cc     src == ..     dst == ..
ssid == name         name contains tag
rssi > -70    len >= 100    ch == 6    no > 500
contains "text"
```

`contains "text"` on its own looks in the bytes, the info column and the
name.

A filter that is wrong is not applied: the filter row turns red, shows the
text and why by turns (`! unknown field wlan.fc.typ`), and the list keeps
the last good filter. Tap the row to mend it. On a wide screen the row is
red while the text being typed does not compile, with the reason beside
it. A good filter is green.

The last eight filters are kept between visits: ANALYZE > RECENT FILTERS
(`R`), or UP and DOWN while typing on a wide screen.

## The packet list

- **Colouring rules**, first match wins, after Wireshark's defaults. VIEW >
  COLOUR RULES turns them off; the detail names the rule that matched; the
  help lists them in their colours.

  | Rule | Matches | Colour |
  |---|---|---|
  | Bad frame | `bad`: too short for its header, an element past the end | red on black |
  | Deauth / disassoc | `deauth` | red on black |
  | EAPOL | `eapol` | bright yellow |
  | Auth / assoc | `auth` | yellow |
  | Beacon | `beacon` | cyan |
  | Probe | `probe` | green |
  | Management | other `mgmt` | bright cyan |
  | Control, null data | `ctrl`, null frames | grey |
  | Data | `data` | bright white |
  | BLE beacon | `ibeacon or eddystone` | bright magenta |
  | BLE advertising, scan response | `btle`, `scanrsp` | bright blue, blue |
  | MeshCore, Meshtastic, LoRa | | bright green, yellow, white |
  | Board decode | `decode` | magenta |

- **Time** (VIEW > TIME, `T`): seconds since the first packet, since the
  previous displayed packet, or the time of day.
- **Mark** (`K`, the packet menu): a marked packet keeps a magenta bar and
  a `*` whatever the rules say. EDIT AND GO has MARK ALL DISPLAYED, UNMARK
  ALL, NEXT MARK and PREVIOUS MARK; the filter `marked` (or `frame.marked
  == 1`) shows them.
- **Go to packet** (`G`): the board's keypad. In a file, a number outside
  the part in the store loads the part that has it.
- **Find** (`E`, then `N` and `B`): a display filter when the text is one,
  plain text otherwise or in quotes (bytes, info, names and addresses, any
  case). The search wraps and says so.
- **Save** (FILE): ALL, DISPLAYED (what the filter shows) or MARKED, to
  `tap-YYYYMMDD-HHMMSS[-shown|-marked].pcapng`.

## Statistics

`S`, then the strip, a swipe, or LEFT and RIGHT. Everything is counted as
packets arrive, so the tables cover the whole capture (or the whole file),
not only what the store still holds.

| Table | Shows |
|---|---|
| HIERARCHY | packets, share and bytes per radio and protocol |
| ENDPOINTS | per address: frames sent and received, bytes, RSSI, first and last seen, name; up to 160 |
| CONVERSATIONS | per pair: packets, bytes, each direction, RSSI, first and last seen; up to 128 |
| I/O GRAPH | packets a second per source in braille; a long capture folds the graph and doubles the seconds per dot |
| EXPERT | errors, warnings, notes and chats, with a count and the last packet |
| CHANNELS | frames per Wi-Fi and Bluetooth channel |
| SIGNAL | RSSI histogram per radio |

`SORT` (`Z`) orders ENDPOINTS and CONVERSATIONS by packets, bytes, signal,
last seen, first seen or address.

Expert information: malformed or short frames; a deauth or disassoc flood
(ten or more in a second, with the peak); deauthentications; an access
point with the name of another but open where that is protected, or the
other way; a BSSID whose beacons change SSID, security or channel;
retransmissions (the retry bit) as a share of 802.11 frames; frames cut to
the snap length; EAPOL key messages and complete 4-way handshakes (messages
1 to 4 between one access point and one station). Live, it also lists what
the radios dropped, what did not reach the card and what aged out. Tap a
row to filter the list to its packets.

## Capture filters

OPTIONS > CAPTURE, per radio: on/off, minimum RSSI, Wi-Fi frame types and
channel plan, LoRa preset. Hopping asks before it drops the board's Wi-Fi.

## Files

pcapng with three interfaces. The tests open TAP's files in tshark
(Wireshark 4.6) and require every frame to dissect with no malformed
packet, no error and no warning:

| Interface | Name | LINKTYPE |
|---|---|---|
| 802.11 + radiotap | `wifi0` | 127 |
| Bluetooth LE LL + PHDR | `ble0` | 256 |
| LoRaTap | `lora0` | 270 |

- Times are microseconds of the board's clock (uptime added to the wall
  clock TAP started at).
- Wi-Fi: radiotap carries flags, rate (when the radio gave a legacy rate),
  channel and antenna signal. Frames have no FCS. A frame cut to the snap
  length keeps its length on air.
- Bluetooth: the controller reports no channel, no PDU header and no CRC,
  and only frames whose CRC it checked. Records use advertising channel 37
  when the channel is unknown; the PDU header is rebuilt from the report
  type and address type, and the CRC-24 is worked out over that PDU, so
  Wireshark finds it correct. A directed advertisement's target is not
  reported either and is written as 00:00:00:00:00:00.
- LoRa: LoRaTap version 0. Its bandwidth field counts 125 kHz steps, so a
  62.5 kHz preset is written as 125 kHz.
- Board decodes are text, not frames, and are not written.
- The store keeps the newest 4096 packets or 1.5 MB in PSRAM, whichever
  fills first; older packets age out and are counted. CAPTURE TO CARD
  keeps them.

### Getting a capture to Wireshark on a PC

1. **lsterm over USB** (any size). On the PC, with the board plugged in:
   `python host/lsterm/lsterm.py` (add the port if it is not found). Press
   Ctrl-F for the FILES panel, open `captures`, select the file and press
   `G`. It arrives, CRC-checked, in `~/LakeShark captures/files/`. Then
   `wireshark "~/LakeShark captures/files/tap-....pcapng"`.
2. **The card.** Power off, put the microSD in a reader, copy
   `captures/*.pcapng`.
3. **The serial console** (any terminal, no lsterm): `sd ls captures`, then
   `sd get captures/NAME.pcapng`. The board prints `sd: get begin PATH
   SIZE`, the file as base64 lines that start with `~`, and `sd: get end
   PATH N bytes crc XXXXXXXX` (CRC-32). Strip the `~`, decode the lines in
   order, check the size and CRC. This is what lsterm does for you.

SAVE TO PC as BOOT LOG has it (the desktop's save dialog) carries at most
64 KB per file, about 300 frames, so TAP does not offer it; the board's web
file server is off on boards with a screen. Over Wi-Fi alone there is no
route for a capture today.

## Simulator

```
LAPSIM_CAPTURE=capture.pcapng lsdk sim tap
```

lssim replays the file. It never invents Wi-Fi or Bluetooth frames. Captures
put in `sdcard/captures` under the simulator's working folder are there for
FILE > OPEN.
`LAPSIM_CAPTURE_NO_TRANSPORT=1` shows the C6 message, `LAPSIM_CAPTURE_DROP=1`
lets a burst overflow the firmware ring as a radio would.
