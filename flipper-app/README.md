![lakeshark_banner](https://github.com/user-attachments/assets/34b12b2c-fd64-4fdc-850c-e9c93d7aede7#gh-light-mode-only)
![lakeshark_banner_dark](https://github.com/user-attachments/assets/657f79dc-afd4-4943-89b3-d9b215a7cb09#gh-dark-mode-only)

This is LakeShark Flipper Edition, which is basically a control head for the LakeShark SDR receiver on a Waveshare ESP32-P4-NANO. The radio does receiving and decoding and the Flipper acts as a remote control. It supports connections over the GPIO pins and Bluetooth.

## What it do

I've added four receivers and all of them are a work in progress. I will try to rate them below for your expectations. I will continue to refine these and will update this table. 
 - 8/8/2026

| Receivers | Status |
| --- | --- |
| P25 | B- |
| FM | C- |
| POCSAG | Untested |
| ADS-B | F |

P25 shows talkgroup, NAC, modulation and the decoded voice status, with an S meter that updates every IQ buffer. FM covers narrowband listening, wideband, and *a scan mode that sweeps a range and lets you jump to the strongest signal*(Not working currently). POCSAG keeps a log of received pages. ADS-B lists traffic and lets you open a single aircraft for detail.

Frequencies can be typed in directly, stepped with the tuning control, or recalled from memories. Each receiver keeps its own memory page, and presets can be loaded from a file on the SD card.

Settings are split into levels, link, device, display and about. From the device page you can reboot the radio, reset the coprocessor, restart or power cycle the SDR, and turn the radio's Bluetooth off.

The head alerts you when the receiver drops off or goes silent, and again when it comes back. This is mainly a workaround due to certain ESP32-P4 boards behaving differently with USB connections. Hoping I can get this more reliable in the future.

## Connection

**UART.** Wire the Flipper to the radio's header, then set the link to UART in settings.

## Do not power the ESP32-P4 from the Flipper 5V pin. 
| Flipper | Radio |
| --- | --- |
| Pin 13 (TX) | RX |
| Pin 14 (RX) | TX |
| Pin 11 (GND) | GND |

115200 baud, 8N1.

**Bluetooth.** Set the link to Bluetooth in settings and the radio connects to the head on its own. The radio needs `ble on` once. Pairing is automatic and there is no code to enter.

Full setup notes are in [docs/CONNECTING.md](docs/CONNECTING.md).

## Power

## Do not power the ESP32-P4 from the Flipper 5V pin. 
It will most likely result in nothing working and could result in a broken flipper. Don't hurt the lil-guy.

## Building

The app builds with ufbt.

```
ufbt launch
```

The radio firmware is a separate project and lives in the LakeShark repository.

## Requirements

A Flipper Zero on firmware with API 87.1 or later. A Waveshare ESP32-P4-NANO running the LakeShark firmware, with an RTL-SDR receiver attached.
