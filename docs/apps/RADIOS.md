# RADIOS

Every radio on the board and whether it is powered, with one press to switch
it.

| | |
|---|---|
| HOME group | SYSTEM |
| Radio | All of them: USB SDRs, LoRa, GPS, antenna path, Bluetooth and Wi-Fi, and the keyboard radios |

## What it does

RADIOS lists each radio as a box. The top edge of the box shows its name and
state. The middle of the box says what pressing it will do. Turn a radio off
to save current, or to stop it holding the bus while another app needs it.

The header counts how many radios are powered now. RADIOS is also on the tab
strip, so F3 (or the RADIOS tab) opens it from any app.

## Radio

These rows appear on the T-Display-P4:

| Row | State | Press |
|---|---|---|
| RTL-SDR | ACTIVE, parked, starting, failed or unavailable, with the app that owns it | STOP / BLOCK RX or ALLOW RX |
| HackRF | Same as RTL-SDR | STOP / BLOCK RX or ALLOW RX |
| LORA | Chip name and TX, RX, held, armed or parked, with the owner and mode | TURN ON or TURN OFF |
| GPS | off, searching or fix | TURN ON or TURN OFF |
| ANTENNA | internal or MMCX1 | USE MMCX1 or USE INTERNAL |
| BLE | Live Bluetooth status | OPEN LINK |
| WI-FI | connected, disconnected, busy or off | OPEN LINK |
| CC1101, NRF24, NFC | The keyboard board radios | OPEN MIX-RF |

STOP / BLOCK RX parks a USB SDR so no app starts it. ALLOW RX lets its owner
start it again. LORA cannot be switched while another app holds it; the screen
names that app.

## How to use it

| Key | Touch | Action |
|---|---|---|
| Up, Down, Tab | | Move between boxes and OPTIONS |
| Enter | Tap a box | Do what the box says |
| | < PREV, NEXT > | Change page when the boxes do not fit |
| O | OPTIONS | Open OPTIONS |
| Esc | | Back to HOME |

USE MMCX1 switches in one tap and is remembered across reboots. MMCX1 is
for receive unless MMCX1 USE says otherwise: each LoRa transmit goes out the
internal antenna and the switch comes back to MMCX1 when it ends.

OPTIONS holds:

- **MMCX1 USE**: RX ONLY (default) keeps transmits on the internal antenna.
  RX+TX transmits on MMCX1 too; only choose it with an antenna for the
  transmit band on MMCX1, since TX into an open or mismatched port can damage
  the radio. Console: `ant txuse rx|rxtx`.
- **BLE AT BOOT**: start Bluetooth on the Wi-Fi and Bluetooth co-processor at
  boot.
- **WI-FI AT BOOT**: start Wi-Fi at boot.
- **BACK**: close the list.

## What the screen shows

- **N of M powered**: radios drawing power now. The antenna path is not
  counted.
- One box per radio. Green state text means active, yellow means on, dim means
  off or absent.
- A yellow line under the header explains a switch that failed, for example
  "GPS start failed (memory / UART)".

## Tips and limits

- Bluetooth and Wi-Fi run live from LINK. Their boxes here open LINK.
- The keyboard radios are controlled in MIX-RF.
- GPS and LORA can be started again here or from their own apps.
- A USB SDR that shows unavailable on battery may simply have no power on the
  USB-A port. The USB power guide explains why.

## More guides

- [LINK](LINK.md)
- [USB power](../T_DISPLAY_P4_USB_POWER.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
