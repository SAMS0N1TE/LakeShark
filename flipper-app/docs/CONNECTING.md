# Connecting the head to the radio

Two ways to link a Flipper Zero to the LakeShark ESP32-P4. **Bluetooth needs no
wires and no pairing code.** Wires are the fallback when Bluetooth misbehaves,
and the better choice when you want to leave it running for hours.

---

## Bluetooth (no wires, no passkey)

The connection runs the opposite way round from what you would expect: the
**Flipper advertises and the radio connects to it**. The Flipper's Bluetooth
stack is peripheral-only - it has no public API for scanning or acting as a GATT
client - so the radio has to be the one that goes looking. Nothing about this is
visible in normal use, but it explains why the steps below are in this order.

### Once, on the Flipper

1. **Settings → Bluetooth → ON.** The app cannot turn the radio on for you; it
   is a system setting. If it is off, the head's **Settings → LINK** page shows
   `BT radio: OFF` and refuses to switch transport.
2. **Settings → Expansion Modules → OFF.** Not optional. The expansion service
   owns the same UART the app needs, and its teardown path can free state an
   interrupt is still using - the symptom is the app crashing on launch with
   `[CRASH][ISR MemMgmt] NULL pointer dereference`.

### Every time

1. Open **LakeShark** on the Flipper.
2. Go to **Settings → LINK** (from the launcher, select `Settings`, then page
   right to `LINK`).
3. Select **Transport** and press OK until it reads `Bluetooth`.
   The screen shows `BLUETOOTH - starting radio...` for a moment: the Flipper is
   restarting its Bluetooth core, which blocks for a few hundred milliseconds.
   That pause is expected.
4. The page now reads `Status: advertising`.
5. **On the radio, run `ble on`** over its USB console. That is the whole
   pairing procedure - there is no passkey, no PIN, and no entry in the
   Flipper's Bluetooth device list.

The radio finds the head by **service UUID, not by name**, so you do not need to
tell it what your Flipper is called. It only matches a Flipper that is actually
running this app with the transport set to Bluetooth, so it will not latch onto
a Flipper sitting at its desktop.

Once the radio has connected once, it reconnects on its own at every boot, and
the head remembers Bluetooth as its transport. In practice that means: power the
radio, open the app, and it links in about a second.

### Reading the LINK page

| Status | What it means | What to do |
|---|---|---|
| `BT off?` | The Flipper's Bluetooth radio is off | Settings → Bluetooth → ON |
| `advertising` | The head is visible; the radio has not connected | Run `ble on` on the radio |
| `connected` | Attached, but no telemetry decoded yet | Wait a second; if it stays here see below |
| `linked` | Working | - |

The header's right-hand pip is the short version: **filled = telemetry is
arriving, hollow = it is not.** It is on every screen.

### If it connects but no data arrives

Check `ble` on the radio's console:

```bash
ble
```

- `drops=` climbing while `tx_frames=` stalls means the radio is refusing to
  transmit because its DMA-capable heap is momentarily starved. This is
  deliberate - the alternative is a failed allocation inside the Bluetooth
  transport, which is a hard assert that reboots the radio. It recovers on its
  own. `heap` shows the `dma` line it is reacting to.
- `rx_lines=0` while `tx_frames` climbs means the radio is talking and the head
  is not. That is head-side: confirm the app is on `Bluetooth` and not `UART`.

---

## Wires (UART)

Three jumpers, 115200 8N1.

```
Flipper pin 13 (TX)  ──>  P4 GPIO33   (link RX)
Flipper pin 14 (RX)  <──  P4 GPIO32   (link TX)
Flipper pin 8/11/18 GND <──> P4 GND
```

Both P4 pins are the **last two signal pins on the LEFT header**, bottom row:
`GPIO32 | GPIO33`, with `GND | GPIO36` directly below. **Count them from the end
of the header, not from the silkscreen.**

Two pins that look right and are not:

- **GPIO45/46** - GPIO46 is the USB-host VBUS enable, and GPIO45 sits directly
  above `C6_IO12`/`C6_IO13`, which belong to the on-board **ESP32-C6, not the
  P4**. A jumper one position off there lands on silicon the P4 physically
  cannot see, so every P4-side diagnostic reports "nothing connected" no matter
  what you do.
- **GPIO37/38** - that is UART0, wired to the on-board USB-C bridge. Driving it
  fights the console.

Set **Settings → LINK → Transport** to `UART`. It is the default.

### If nothing arrives

Run this on the radio, with the Flipper powered and the app open:

```bash
link probe
```

It engages a pull-**down** on each header pin and reports which ones something
external is still holding high - which is what an idle UART transmitter does.
A pin reading ~100% is a wire; 0% is nothing connected.

Use `link probe`, not `link scan`: `scan` engages a pull-**up**, so an
unconnected pin idles high exactly like a connected one and reads clean whether
or not anything is attached. That is why a 17-pin sweep can come back perfect
while the wire is on the wrong pin.

Note that GPIO32 and GPIO36 read 100% on this board even with every jumper off -
they have their own pull-ups. `link probe` labels them and excludes them.

---

## Booting straight into an app

**Settings → DISPLAY → Boot into** picks which app opens at startup: `Launcher`
(the default), `P25`, `FM`, `POCSAG` or `ADS-B`.

This is link-aware, which matters on Bluetooth. The app does not fire the
mode-select command at startup and hope - over Bluetooth the radio is not
listening yet, and the command would go nowhere with nothing to say so. Instead
it holds the command, shows the app name with `waiting for radio`, and sends it
the moment telemetry starts arriving. If the radio never answers within eight
seconds it opens the app anyway and says `No radio - check Link`, rather than
holding the screen indefinitely.

Press **Back** at any point during the wait to cancel and go to the launcher.

---

## Controls

The same five buttons everywhere:

| | |
|---|---|
| **Left / Right** | previous / next page (the dots in the header) |
| **Up / Down** | move the selection, or tune |
| **OK** | act on the selection |
| **OK (long)** | the page's secondary action |
| **Back** | up one level |

**Back only exits the app from the launcher.** Everywhere else it goes up a
level, so a stray press cannot kill a running session.

On **settings** pages, press **OK to open a row for editing**, then **Left/Right
to change the value**, then OK or Back to close it. An open row shows its value
in angle brackets: `<72>`. While a row is open, Left/Right adjusts it instead of
changing page.

On the **VFO** page, Up/Down tunes. The inverted bar below the frequency shows
what Up/Down is currently driving - `STEP`, `VOL`, or `SQL` on FM - and **OK
cycles it**, so volume is one press away without leaving the screen.
