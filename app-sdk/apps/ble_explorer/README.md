# BLE EXPLORER

Advertisements in, then a GATT client. Normal client use only: it connects,
reads, and writes only when you press a key for it or start a flow.

## What it does

- **SCAN**: every advertiser heard, strongest first, with its name, address,
  RSSI and when last heard. A device with no name of its own shows a name
  you gave it, one it gave before, or what it is: the service it
  advertises, the maker in its data, the maker of its address.
- **Connections**: up to three at once, in slots `[1] [2] [3]`. The board
  decides how many an app may hold. When the board refuses another
  (`LAP_IO_EBUSY`) the app says how many it allows, keeps the connections it
  has, and shows the limit in the title bar (`1/1 links`).
- **The walk**, started on connect, one request at a time: services, the
  characteristics of each, the descriptors of every characteristic with room
  for them, then a read of every readable characteristic and every
  descriptor. Refused reads are marked and the walk goes on.
- **Names** for services, characteristics, descriptors, companies, address
  makers and appearances come from the shared name library (`lap_ids`):
  your own names first, then the name pack PACKS installs
  (`/sdcard/lakeshark/packs/ble-ids.bin`). While a card lookup is under way
  the app's own short list of SIG names answers, so a row is never blank.
- **NAME** (M): name the picked service, characteristic or descriptor, or
  the picked device in SCAN. The name shows at once and is saved as a line
  in `/sdcard/lakeshark/ble-names.txt` (`service 18f0 Thermal printer`,
  `device aa:bb:cc:dd:ee:ff Desk lamp`), a plain file you can edit.
- **Remembered device names**: a name heard in an advert or read from
  Device Name (2a00) is kept in `/sdcard/lakeshark/ble-devices.txt`, so the
  device is named next time even when it advertises without one. Your own
  names win. Addresses that change every few minutes are not kept.
- **PAIR** (P): pair with the shown device, keeping the keys or for this
  link only, or forget kept keys. A code to type, or to check, comes up on
  screen. Once paired, every value is read again, so reads that needed
  pairing fill in. The link line shows MTU, signal, PHY and security.
- **Notifications**: N subscribes the picked characteristic, U stops. Each
  one lands in the NOTIFY log (time, slot, handle, name, decoded value, hex).
  WeChat AirSync packets (`fe 01 <len> <cmd> <seq>`) are named there, such
  as `WeChat auth request, 26 bytes`, and a packet that comes in parts is
  put back together.
- **FLOWS** (F): short scripted sequences, see below.
- **Save** (S): the shown connection's table as JSON in
  `/sdcard/ble_explorer/gatt-<address>-<date>-<time>.json`.

## Flows

A flow is a short, fixed list of GATT steps you start yourself: subscribe,
write a command, wait for the reply, read a value. The FLOWS page lists the
flows that fit the connected device first (marked `+`). ENTER or a second
tap runs the picked one; each step and its result show as they happen, and
notifications land in NOTIFY as usual. STOP ends a run. A flow that writes
asks before it runs.

Flows come from three places, later ones replacing earlier ones of the same
name:

1. built into the app (the text of `flows/ble-flows.txt`);
2. `/sdcard/lakeshark/packs/ble-flows.bin`, a newer copy from PACKS (plain
   text, many flows in one file);
3. your own files, `/sdcard/lakeshark/ble-flows/*.flow`.

RELOAD reads the card again.

### Format

One step a line. `#` starts a comment. Spaces at the start do not matter.

```
flow Thermal printer: status          # starts a flow; its name
about Asks a receipt printer for its status
match service 18f0                    # fits a device with this service
match name *Printer*                  # or a name like this (* and ?)
confirm Prints a line. Run it?        # the question before it runs

notify 2af0 on
write 2af1 10 04 01                   # hex bytes
write 2af1 "hello\r\n"                # or text
wait notify 2af0 2000                 # a notification within 2000 ms
read 2a19
wait 500
log Status asked
notify 2af0 off
```

| Step | Does |
|---|---|
| `read <uuid>` | read the characteristic, show its value |
| `write <uuid> <data>` | write; a write request, or a write without response when the characteristic takes only that |
| `notify <uuid> on` / `off` | turn notifications or indications on or off |
| `wait <ms>` | pause |
| `wait notify <uuid> <ms>` | wait for a notification on it |
| `log <text>` | a line in the run |
| `? <step>` | the step may fail: noted, and the flow goes on |

- `<uuid>` is `2a19`, `0x2A19` or the whole 128-bit form. `1802/2a06`
  means 2a06 in service 1802, for devices with the same characteristic in
  two services.
- `<data>` is hex bytes (`10 04 01` or `100401`), `"text"` with `\n` `\r`
  `\t` `\\` `\"` `\xNN`, or `$time` (the board's date and time as Current
  Time, 10 bytes), mixed as you like.
- A flow stops at the first step that fails, unless the step starts with `?`.
  A request with no reply in 5 s fails.
- Limits: 32 steps, one value per write (128 bytes on current firmware),
  waits up to 30000 ms, 6 `match` lines. There are no loops: every step
  runs once.
- A flow with a bad line is left out; FLOWS shows the first bad line.
- Flows run only once the table walk is done.

### Built-in flows

Bluetooth SIG services (written from the public specifications): Device
information, Device name and look, Battery level, Find me (beep and quiet),
Current time (read and set), Heart rate, Health thermometer, Environment,
Cycling, Running, Fitness machine, Tx power and link loss, Keyboard or mouse
info. Serial bridges: Nordic UART, HM-10 style and Microchip Transparent
UART pings. Printers: ESC/POS receipt printer status and test print, cat
printer status, paper feed and test print (from rbaron/catprinter, MIT).
Others: WeChat AirSync probe (reads only), Xiaomi LYWSD03MMC thermometer,
Mi Flora (from open-homeautomation/miflora, MIT), Govee lights (from
Beshelmek/govee_ble_lights, MIT), Triones and ELK-BLEDOM LED strips,
SwitchBot info and press. Each flow names its source in a comment.

## Screens

Handheld: SCAN, GATT, NOTIFY and FLOWS tabs. Wide (90 columns or more):
devices on the left, the table on the right (FLOWS takes its place while
that tab is picked), notifications below it.

Keys along the bottom: SCAN `CONNECT` `NAME` `DISCONN` `SAVE`; GATT `READ`
`NOTIFY` `NAME` `MORE` (write 01, pair, save, walk again, disconnect);
NOTIFY `CLEAR` `NOTIFY` `SAVE`; FLOWS `RUN` `STOP` `RELOAD`.

## Keys

| Key | Does |
|---|---|
| C, ENTER on SCAN | connect the picked device |
| X | disconnect the shown slot (its table stays to read and save) |
| 1 2 3 | show that slot |
| R, ENTER on GATT | read the picked characteristic or descriptor |
| W | write the one byte 01 to the picked characteristic |
| N / U | notifications on / off |
| M | name the picked row or device |
| P | pair |
| F | FLOWS; ENTER runs, Q stops, O reloads |
| D | walk the table again |
| S | save JSON |
| TAB, LEFT/RIGHT | tabs |
| BACK | from a run to the list, then to SCAN, then HOME |

## Limits

- A value keeps its first 128 bytes. On firmware before API 2.9 values are
  20 bytes and there is no pairing or write without response.
- One request at a time per connection.
- Tables hold 24 services, 96 characteristics and 128 descriptors; a
  bigger device is marked `TABLE FULL` and saved with `"complete": false`.

## Simulator

`LAPSIM=ble_explorer lssim lap -A 40 -f 200 -d` connects to the
simulator's synthetic peripheral (one Battery service with a notifying
level). `LAPSIM_SCRIPT=1:blead=-70,<hex AD>` adds an advertiser.
