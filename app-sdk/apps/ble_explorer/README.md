# BLE EXPLORER

Advertisements in, then a GATT client. Normal client use only: it connects,
reads, and writes only when you press a key for it.

## What it does

- **SCAN**: every advertiser heard, strongest first, with its name (or the
  service or company it advertises), address, RSSI and when last heard.
- **Connections**: up to three at once, in slots `[1] [2] [3]`. The board
  decides how many an app may hold. Firmware 2.21 gives an app one GATT
  client; when the board refuses another (`LAP_IO_EBUSY`, at once or in the
  connect event) the app says how many it allows, keeps the connections it
  has, and shows the limit in the title bar (`1/1 links`). The firmware's
  app client (`main/ble_lap_client.c`) holds one link today even where
  NimBLE has room for three (the net build); when it grants more, the
  slots fill without a change here.
- **The walk**, started on connect, one request at a time: services, the
  characteristics of each, the descriptors of every characteristic with room
  for them, then a read of every readable characteristic and every
  descriptor. Refused reads (pairing needed) are marked and the walk goes
  on. `walking 60%` shows how far it is.
- **Names and decoding** for common SIG services, characteristics and
  descriptors: Generic Access, Device Information, Battery, Heart Rate,
  Health Thermometer, Environmental Sensing, Current Time, HID, Cycling,
  PnP ID, appearance, connection parameters, CCCD state, presentation
  format, report reference, and the Nordic UART service. Unknown values show
  as text when printable, else hex.
- **Notifications**: N subscribes the picked characteristic, U stops. Each
  one lands in the NOTIFY log (time, slot, handle, name, decoded value, hex)
  and updates the value in the table.
- **Save** (S): the shown connection's table as JSON in
  `/sdcard/ble_explorer/gatt-<address>-<date>-<time>.json`, with every
  handle, property, value, decoded value, error, and the notification log.

## Screens

Handheld: SCAN, GATT and NOTIFY tabs. GATT is the tree (services yellow,
characteristics white with properties `R W w N I B` and `*` when
notifying, descriptors cyan), the picked row's details under it, and on a
tall screen the latest notifications. Wide (90 columns or more): devices
on the left, the table on the right, notifications below it, each in its
own frame.

The table is drawn as a tree (`+-` branches, `` `- `` for the last). The
header shows the device with its antenna: waves run outward while it
connects and walks the table (with a spinner and a progress meter), stay
lit while the link is ready, and breathe slowly while it only listens.
The newest notification lights up as it lands.

Touch: the tab strip's `<` `>` change tab (or swipe left and right); on
GATT, tapping the middle key shows the next slot. Each list follows a
finger. The keys along the bottom: SCAN `CONNECT` `DISCONN` `SAVE`; GATT
`READ` `WRITE01` `NOTIFY` `SAVE`; NOTIFY `CLEAR` `NOTIFY` `SAVE`. Notes
show over the title for a few seconds; a tap there clears one.

## Keys

| Key | Does |
|---|---|
| C, ENTER on SCAN | connect the picked device |
| X | disconnect the shown slot (its table stays to read and save) |
| 1 2 3 | show that slot |
| R, ENTER on GATT | read the picked characteristic or descriptor |
| W | write the one byte 01 to the picked characteristic |
| N / U | notifications on / off |
| D | walk the table again |
| S | save JSON |
| TAB, LEFT/RIGHT | tabs |
| BACK | to SCAN, then HOME |

## Limits

- Values are 20 bytes (MTU 23): longer ones read as `longer than 20 bytes`.
- One request at a time per connection; the board times one out after 12 s
  and then drops that link.
- Pairing is not started by the app; reads that need it are reported.
- Tables hold 24 services, 96 characteristics and 128 descriptors; a
  bigger device is marked `TABLE FULL` and saved with `"complete": false`.

## Simulator

`LAPSIM=ble_explorer lssim lap -A 40 -f 200 -d` connects to the
simulator's synthetic peripheral (one Battery service with a notifying
level). `LAPSIM_SCRIPT=1:blead=-70,<hex AD>` adds an advertiser.
