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
| RTL-SDR | ACTIVE, parked, starting, failed or unavailable, with the app that owns it | STOP <app>, STOP / BLOCK RX or ALLOW RX |
| HackRF | Same as RTL-SDR | Same as RTL-SDR |
| LORA | Chip name and TX, RX, held, armed or parked, with the owner and mode | STOP <app>, TURN ON or TURN OFF |
| GPS | off, searching or fix | TURN ON or TURN OFF |
| ANTENNA | internal or MMCX1 | USE MMCX1 or USE INTERNAL |
| BLE | Live Bluetooth status | OPEN LINK |
| WI-FI | connected, disconnected, busy or off | OPEN LINK |
| CC1101, NRF24, NFC | The keyboard board radios | OPEN MIX-RF |

STOP <app> turns off the app using the radio: FM, P25, ADS-B and REC park
their receiver, SUB-GHZ turns WATCH off, CATCH, DRONES and CELL WATCH stop,
and an app service is asked to end. The radio is then free (the LoRa chip goes
back to MeshCore) and nothing takes it back until you open that app again.
An owner STOP does not know is blocked instead: STOP / BLOCK RX or STOP /
BLOCK LORA, then ALLOW to undo it.

## How to use it

| Key | Touch | Action |
|---|---|---|
| Up, Down, Tab | | Move between boxes and OPTIONS |
| Enter | Tap a box | Do what the box says |
| | < PREV, NEXT > | Change page when the boxes do not fit |
| O | OPTIONS | Open OPTIONS |
| Esc | | Back to HOME |

USE MMCX1 switches in one tap and is remembered across reboots. Transmit
goes out whichever antenna is selected, on the LR2021 and the SX1262 alike;
MMCX1 USE can keep transmits on the internal antenna instead.

OPTIONS holds:

- **MMCX1 USE**: RX+TX (default) transmits on MMCX1 when it is selected, so
  keep an antenna for the transmit band on it: TX into an open or mismatched
  port can damage the radio. RX ONLY moves each LoRa transmit to the internal
  antenna and back. Console: `ant txuse rx|rxtx`.
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
