# TAP

A packet analyser for the radios the board can listen on. Receive only.

| Source | Through | Shows |
|---|---|---|
| Wi-Fi 802.11 | `lap_wifi_capture_*` (API 2.7) | beacons, probes, auth, data headers, EAPOL marked |
| Bluetooth LE | `lap_ble_capture_*` (API 2.7) | AD structures, names, UUIDs, company IDs, iBeacon, Eddystone |
| LoRa | `lap_packet_*` | MeshCore and Meshtastic headers, raw payloads |
| Board decodes | `lap_event_*` | pages, APRS, AIS, ADS-B, P25 and sub-GHz as text records |

Encrypted payloads are named and saved as received. Nothing is decrypted.

Wi-Fi needs coprocessor firmware that forwards monitor frames. Where it does
not, the WIFI source shows `n/a` and the reason. See `docs/CAPTURE.md`.

## Screens

- Wide (a desk window or remote screen, 90 columns or more): menu bar,
  packet list, protocol tree, bytes. Mouse: click rows and menus, wheel
  scrolls, right-click a packet for its menu.
- Handheld: the list, then ENTER for the tree and bytes. Soft buttons on
  the bottom row.

## Keys

| Key | Does |
|---|---|
| SPACE, P | pause / resume |
| / | type a display filter |
| X | clear the filter |
| F, V | follow the device / the conversation |
| S | statistics |
| W | save pcapng to `/sdcard/captures` |
| C | clear packets |
| O | options |
| M | packet menu |
| TAB | next pane |
| BACK | up a level, then HOME |

## Display filters

```
wifi  ble  lora  decode  beacon  probe  mgmt  ctrl  data  eapol  deauth
auth  ibeacon  eddystone  scanrsp  rid  meshcore  meshtastic  protected
addr == aa:bb:cc     src == ..     dst == ..
ssid == name         name contains tag
rssi > -70    len >= 100    ch == 6    no > 500
contains "text"
and  or  not  ( )
```

An address matches by prefix. `contains` looks in the bytes, the info
column and the name.

## Capture filters

OPTIONS > CAPTURE, per radio: on/off, minimum RSSI, Wi-Fi frame types and
channel plan, LoRa preset. Hopping asks before it drops the board's Wi-Fi.

## Files

pcapng with three interfaces, readable by Wireshark:

| Interface | LINKTYPE |
|---|---|
| 802.11 + radiotap | 127 |
| Bluetooth LE LL + PHDR | 256 |
| LoRaTap | 270 |

- Bluetooth: the controller reports no channel or CRC. Records use
  advertising channel 37 and a zero CRC flagged as not checked.
- Board decodes are text, not frames, and are not written.
- The store keeps the newest 4096 packets or 1.5 MB.

## Simulator

```
LAPSIM_CAPTURE=capture.pcapng lsdk sim tap
```

lssim replays the file. It never invents Wi-Fi or Bluetooth frames.
