# components/lakeshark/radio - provenance

This directory mixes third-party librtlsdr with LakeShark original code. A
reader could not previously tell which was which, and several upstream notices
had been lost. This file records what came from where.

Licence text: `COPYRIGHT.librtlsdr` (GPL-2.0-or-later). Root: `LICENSE` (GPLv3).

## From librtlsdr - GPL-2.0-or-later

Upstream: <https://github.com/osmocom/rtl-sdr>. Reached here by way of
[xtrsdr](https://github.com/XTR1984/xtrsdr), which trimmed librtlsdr for
ESP32-class targets. **The exact upstream revisions are not recorded** and
should be pinned before release; the table says what was verified, not what was
assumed.

| File | Upstream notice | Modified here |
|---|---|---|
| `librtlsdr.c`      | present | yes - heavy; ESP-IDF USB host, ESP_LOG, FreeRTOS transfers |
| `rtl-sdr.h`        | present | yes - declarations trimmed to what this firmware builds |
| `rtl-sdr_export.h` | present | no |
| `tuner_e4k.c/.h`   | present | `.c` only - logging |
| `tuner_fc0012.c/.h`| present | `.c` only - logging |
| `tuner_fc0013.c/.h`| present | `.c` only - logging |
| `tuner_fc2580.c`   | **was missing - restored** | no |
| `tuner_fc2580.h`   | none upstream either | no |
| `tuner_r82xx.c/.h` | present | `.c` only - logging |
| `reg_field.h`      | none upstream either | no |
| `rtlsdr_i2c.h`     | none upstream either | no |

`tuner_fc2580.c` had lost its leading comment block. It was restored verbatim
from `osmocom/rtl-sdr` master (fetched 2026-09-07 and compared character for
character), not reconstructed from memory.

`tuner_fc2580.h`, `reg_field.h` and `rtlsdr_i2c.h` carry no leading comment
**upstream either** - checked at the same time against
`include/reg_field.h`, `include/rtlsdr_i2c.h` and `include/tuner_fc2580.h`.
They are not missing a notice, so do not invent one for them; they are covered
by the project licence in `COPYRIGHT.librtlsdr`.

Files this project changed carry a "Notice of change" comment under the
upstream header, which GPL-2.0-or-later section 2(a) requires of modified
files. Add one to any upstream file you touch.

## LakeShark original - GPL-3.0-or-later

These are not librtlsdr and were written for this project. They carry an SPDX
header saying so.

    esp_libusb.c / esp_libusb_private.h     ESP-IDF USB host shim standing in
                                            for libusb
    rtlsdr_dev.c / .h                       radio-endpoint adapter for the RTL
    rtl_adapter_private.h
    rtl_sdr_private.h
    hackrf_dev.c / hackrf_radio.c / .h      HackRF adapter
    hackrf_adapter_private.h
    radio_endpoint.c / .h                   device-independent radio interface
    radio_health.c / .h
    receiver_diagnostics.c / .h
    iq_app_control.c / .h
    stream.c / .h
    tx_broker.c / tx_policy.c / tx_*.h      transmit gating
    usb_host.c / .h

`esp_libusb.c` is a reimplementation against the ESP-IDF USB host API, not a
copy of libusb; libusb itself (LGPL-2.1) is not vendored here.

## Updating

Replace files only from a reviewed upstream revision, record its commit hash in
this file, keep every upstream notice, add or update the change notice on
anything you modify, and re-run `pwsh -File bench/verify.ps1 -Level smoke`.
