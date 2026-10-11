# Apps

Every app on the T-Display-P4 HOME screen, grouped as on HOME. Tap a name to
open its page: what it does, which radio it needs, the keys and touch
controls, and its limits.

On HOME, F1 to F4 (or the tabs at the top) choose RADIO, FIELD, SYSTEM or
USER. Arrows and Enter open a tile, or tap it. In an app, R opens RADIO to
pick the receiver, O opens OPTIONS, and Esc or BACK goes back. See
[Controls and navigation](../UI_CONTROLS.md) for the shared controls.

## RADIO

| App | What it does | Radio |
|---|---|---|
| [P25](P25.md) | Follow a P25 trunked system, decode Phase 1 voice and see active talkgroups | RTL-SDR or HackRF; LR2021 for Phase 1 voice |
| [FM](FM.md) | Listen to NFM, WFM or AM, decode pagers, ACARS, APRS, AIS and SAME, or sweep a band | RTL-SDR or HackRF; LoRa chip for POCSAG |
| [ADSB](ADSB.md) | Aircraft on 1090 MHz as a list and an offline mini map | RTL-SDR, HackRF or LR2021 |
| [FALLS](FALLS.md) | A waterfall of the last minute on the receiver you choose | RTL-SDR, HackRF or LoRa chip |
| [CELL WATCH](CELL_WATCH.md) | Compare cellular downlink power with a learned site baseline | RTL-SDR; HackRF for HIGH RATE |
| [MESH](MESH.md) | A MeshCore node: hear, relay and message over LoRa | LoRa chip (SX1262 or LR2021) |
| [LORA LABS](LORA_LABS.md) | Packet capture, spectrum, bearing plot, FSK and pager receive | LoRa chip (SX1262 or LR2021) |
| [EXPERIMENTS](EXPERIMENTS.md) | Radio trials that may grow into apps of their own | LoRa chip (SX1262 or LR2021) |
| [SUB-GHZ](SUBGHZ.md) | Watch the sub-GHz bands, keep repeating patterns and decode known ones | RTL-SDR, HackRF, CC1101 or LoRa chip |
| [FILES](FILES.md) | Browse the SD card, preview captures and text, send, rename or delete | None |
| [MUSIC](MUSIC.md) | Play WAV files, watch levels and spectra, record the microphone | None |
| [MIX-RF](MIXRF.md) | The keyboard board radios: sub-GHz monitor, 2.4 GHz survey and NFC | CC1101, nRF24 and NFC (keyboard board) |
| [DRONES](DRONES.md) | Bluetooth Remote ID drones and their operators | Bluetooth |

## FIELD

| App | What it does | Radio |
|---|---|---|
| [NOTES](NOTES.md) | Field notes on the SD card with the time, GPS, heading, map or radio inserted | None; GPS optional |
| [COMPASS](COMPASS.md) | True-north compass, guidance to a saved place, FIND by signal strength, level | GPS optional; FIND uses the receiver you pick |
| [SWEEP](SWEEP.md) | Passive scanner for nearby trackers, drones and camera hints | Bluetooth and Wi-Fi |
| [REC](REC.md) | Capture what a radio receives to the SD card, and replay captures | RTL-SDR, CC1101, LoRa chip, GPS and others |
| [MAP](MAP.md) | Offline map with your position, aircraft, mesh nodes and marks | GPS optional |
| [TILES](TILES.md) | Install and manage offline map regions for MAP | Wi-Fi |
| [GPS](GPS.md) | Fix state, satellites in view and a GPX track recorder | GPS |

## SYSTEM

| App | What it does | Radio |
|---|---|---|
| [DIAG](DIAG.md) | Memory, temperature, SD card, radio health and the error log | None |
| [TERMINAL](TERMINAL.md) | The serial console commands, typed on the board | None |
| [UPDATE](UPDATE.md) | Install signed LakeShark builds over WiFi | Wi-Fi |
| [SET](SET.md) | Volume, brightness, mute, screen lock, display, sounds and keyboard | None |
| [RADIOS](RADIOS.md) | Every radio on the board, and power for each one | All |
| [LINK](LINK.md) | Join Wi-Fi, connect a Flipper, record a Wi-Fi and Bluetooth survey | Wi-Fi and Bluetooth |

## USER

| App | What it does | Radio |
|---|---|---|
| [USER](USER.md) | Your own screens, written as `.lsapp` files on the SD card | None |

## More guides

- [T-Display-P4 screens](../TDP4_SCREENS.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [Updates and calls](../TDP4_UPDATES_AND_CALLS.md)
