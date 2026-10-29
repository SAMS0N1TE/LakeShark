# TIME MACHINE

Keeps the last N seconds of whatever receiver runs, saves "WHAT WAS THAT?" clips,
replays a clip through the pager decoders, and shows one timeline of what every
decoder reported. Branch `feature/ovh-time-machine-20261028`.

## Pieces

| File | What |
|---|---|
| `components/lakeshark/core/ls_tmach_ring.[ch]` | stage (SPSC copy queue), CIC2 decimating PSRAM ring, tape ticks, freeze windows |
| `components/lakeshark/core/ls_tmach_clip.[ch]` | clip sidecar (REC's keys + extension), upsampling playback |
| `components/lakeshark/core/ls_tmach_timeline.[ch]` | timeline: an `ls_decoded` reader, merge, clip links |
| `components/lakeshark/core/ls_tmach.[ch]` | taps, worker task `tmach` (core 0, prio 2), settings, console `tm` |
| `components/apps/tui/ls_tmach_decode.[ch]` | replay into fm_dsp+pocsag (CU8 256k) and pager_iq+pocsag (CS16 125k) |
| `components/apps/tui/screens/ext/scr_tmach.c` | the TIME MACHINE app |

Taps: `ls_radio_endpoint_set_capture_tap` (every RTL/HackRF `ls_radio_iq_read`) and
`lr_wf_set_capture_tap` (every block `lr_wf_session` hands its consumer, narrow or wide).
Both copy only. Above `max_rate` (default 1.3 MS/s, so not ADS-B) nothing is kept.

## Clip format (version 1)

`/sdcard/lakeshark/tmach/tmNNNN.cs8|cs16` raw interleaved I/Q, plus
`tmNNNN.json`: REC's sidecar keys first (`rec_sidecar_parse` reads them;
`sample_rate` is the stored rate), then `format "lakeshark-tmclip"`, `version`,
`iq`, `iq_format`, `center_hz`, `pairs`, `source_rate`, `source_format`, `decim`,
`start_us`, `end_us`, `saved_us`, `fix`, `lat`, `lon`, `alt_m`, `decoder`, `gaps`.
CS8 is CU8 minus 128, so an undecimated clip plays back byte for byte.

## Settings

`/sdcard/lakeshark/tmach/config.json`, defaults in `data/tmach/config.json`
(the bench checks they match the firmware). OPTIONS on the screen or `tm` below.

## Board checks (not run here)

1. `tm status` at boot: `on`, "Waiting for a receiver to run".
2. Open FM, POCSAG, on a pager channel (152.6 here). `tm status`: `recording RTL-SDR
   152.6000 MHz 64000 S/s cs8 (from 256000 cu8, /4) held ... of ~32 s`, ticks rising,
   stage drops 0. FM keeps decoding as before (`status` pocsag pages).
3. TIME MACHINE app: reels turn, tape scrolls; bursts rise on it.
4. Wait for a page, then tap WHAT WAS THAT? (or `tm what`). `tm status` shows
   `saving tmNNNN` then `tmNNNN saved, 30.0 s`; `tm clips` lists it; the card has
   `tmNNNN.cs8` and `.json`.
5. `tm replay` (or REPLAY): `Replay: 1 page, <ric>: ...`; `tm events` shows the page.
6. LR board: FM on the LR2021, both `lr wide off` and on: `tm status` shows
   `LR2021 ... 62500 S/s cs16 (from 125000 cs16, /2)`; repeat 4 and 5.
7. `tm off`: ring freed (`tm status` ring 0 KB); `tm on` comes back on the next read
   (ring 4096 KB plus 512 KB of stage, PSRAM).
8. Stop every receiver for a minute: the log says `tmach: idle 60 s: ring 4096 KB + stage freed`,
   `tm status` shows ring 0 KB and "Idle: memory freed"; start FM again and it comes back
   (`tmach: ring 4096 KB + stage 512 KB allocated`). 4096 KB stays the default: at the LR rate
   (62500 S/s cs16, 250 KB/s) 2048 KB would hold only 8 s.

## Merge notes

- `core/ls_decoded.c` is a byte-identical copy of `feature/ovh-sdk-radio-20261028`'s.
- `core/ls_decoded.h` adds kinds `P25 1024`, `REC 2048`, `DETECT 4096` and the fields
  `freq_hz`, `radio` at the end of `ls_decoded_t`; take this version (a superset).
- Both branches bridge ADS-B from the event bus into `ls_decoded` (theirs only while
  a native app listens); keep one, at boot. The timeline merges duplicates per ICAO.
- MeshCore messages reach `ls_decoded` only through their `ls_lap_events_service`
  poll; the timeline shows them once that runs always.
