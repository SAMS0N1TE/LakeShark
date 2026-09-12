# Radio source provenance

RTL-SDR and tuner files derive from [Osmocom rtl-sdr](https://github.com/osmocom/rtl-sdr) through [xtrsdr](https://github.com/XTR1984/xtrsdr). The original import revisions are unrecorded.

License: GPL-2.0-or-later. See `COPYRIGHT.librtlsdr` and per-file notices.

| File | Upstream notice | Modified here |
|---|---|---|
| `librtlsdr.c`      | present | yes; ESP-IDF USB host, ESP_LOG, FreeRTOS transfers |
| `rtl-sdr.h`        | present | yes - declarations trimmed to what this firmware builds |
| `rtl-sdr_export.h` | present | no |
| `tuner_e4k.c/.h`   | present | `.c` only - logging |
| `tuner_fc0012.c/.h`| present | `.c` only - logging |
| `tuner_fc0013.c/.h`| present | `.c` only - logging |
| `tuner_fc2580.c`   | present | no |
| `tuner_fc2580.h`   | none upstream either | no |
| `tuner_r82xx.c/.h` | present | `.c` only - logging |
| `reg_field.h`      | none upstream either | no |
| `rtlsdr_i2c.h`     | none upstream either | no |

LakeShark radio adapters use GPL-3.0-or-later, as stated in their source headers. Modified upstream files retain change notices.
