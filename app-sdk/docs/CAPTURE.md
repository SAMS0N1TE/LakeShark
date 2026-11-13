# Raw capture (API 2.7)

Frames as the radios heard them, for analysers. Receive only: nothing in
this API transmits, injects, deauthenticates or decrypts.

```c
int  lap_wifi_capture_start(const lap_wifi_capture_config_t *config);
int  lap_ble_capture_start(const lap_ble_capture_config_t *config);
int  lap_capture_read(int capture, void *data, size_t cap, lap_capture_meta_t *meta);
int  lap_capture_status(int capture, lap_capture_status_t *out);
void lap_capture_stop(int capture);
```

- Needs `LAP_CAP_RADIO_RX`.
- One capture per source for the whole board. The handle is the source:
  `LAP_CAPTURE_WIFI` or `LAP_CAPTURE_BLE`.
- Frames wait in a PSRAM ring of 256. Poll from `draw()`. A full ring drops
  the newest frame and counts it in `dropped`.
- `lap_capture_status` works before a start and after a refused one. Show
  `why` to the user as it is.
- Closing the app stops its captures.

## Wi-Fi

802.11 MAC frames, header first, no FCS, no radiotap. Per frame: channel,
RSSI, timestamp, length on air, length kept, legacy rate when known.

| Config | Meaning |
|---|---|
| `types` | `LAP_WIFI_MGMT`, `_CTRL`, `_DATA`; 0 is all |
| `channel_count` 0 | listen where the board already is; never retunes |
| `channel_count` 1 | stay on `channels[0]` |
| `channel_count` 2..16 | hop, `dwell_ms` each (50..5000, default 250) |
| `snap_bytes` | bytes kept per frame, 64..2346, default 512 |
| `LAP_CAPTURE_DROP_LINK` | the user agreed to lose the Wi-Fi link |

**Coprocessor firmware.** The Wi-Fi radio is an ESP32-C5/C6 behind
esp_hosted. Capture needs coprocessor firmware that forwards monitor frames.
The firmware shipped with LakeShark 2.21 (esp_hosted 2.12.9) does not: its
host API leaves `esp_wifi_remote_set_promiscuous*` unimplemented and the
slave has no handler for it. On those boards `lap_wifi_capture_start`
returns `LAP_IO_ENOTSUP` and the status reason is `LAP_CAPTURE_NO_TRANSPORT`.
Handle that case in every app. Bluetooth capture is not affected.

**The board's own link.** Tuning away from the access point drops LINK,
remote screens and `lap_tcp`. A config that names channels is refused with
`LAP_IO_EBUSY` / `LAP_CAPTURE_LINK_IN_USE` while the board is joined to a
network. Ask the user, then set `LAP_CAPTURE_DROP_LINK`. `channel_count` 0
is always allowed. Hops happen on the app's own polls: an app that stops
polling stops hopping.

## Bluetooth

One record per advertising report from the board's passive scanner (shared
with LINK and `lap_ble_scan`): advertiser address and type, PDU type
(`LAP_BLE_ADV_IND` .. `LAP_BLE_SCAN_RSP`), RSSI, and the AD structures as
data (0..255 bytes).

- The controller strips the PDU header and CRC, and does not say which of
  channels 37/38/39 it heard: `channel` is 0 on the board.
- The scanner is passive, so scan responses appear only when another
  feature scans actively.
- Extended advertising is not delivered yet.

## Reasons

| `reason` | Start returns | Meaning |
|---|---|---|
| `LAP_CAPTURE_RUNNING` | handle | capturing |
| `LAP_CAPTURE_STOPPED` | | never started, or stopped |
| `LAP_CAPTURE_NO_RADIO` | `LAP_IO_ENOTSUP` | radio absent or off |
| `LAP_CAPTURE_NO_TRANSPORT` | `LAP_IO_ENOTSUP` | coprocessor firmware cannot forward frames |
| `LAP_CAPTURE_LINK_IN_USE` | `LAP_IO_EBUSY` | hopping would drop the joined network |
| `LAP_CAPTURE_BUSY` | `LAP_IO_EBUSY` | another app holds this capture |
| `LAP_CAPTURE_NO_MEMORY` | `LAP_IO_ELIMIT` | no PSRAM for the ring |
| `LAP_CAPTURE_NO_FILE` | `LAP_IO_ENOTSUP` | simulator: no file to replay |
| `LAP_CAPTURE_REPLAY_END` | | simulator: the file has been replayed |

## Simulator

lssim **replays a capture file. It never invents frames.**

| Variable | Meaning |
|---|---|
| `LAPSIM_CAPTURE=file.pcapng` | the file to replay, once, in file order |
| `LAPSIM_CAPTURE_PACE=0` | deliver as fast as the app polls (default: the file's own pace) |
| `LAPSIM_CAPTURE_JOINED=1` | the board is on a network, to test the link guard |

- LINKTYPE 127 (802.11 radiotap) records feed Wi-Fi; channel, signal and
  rate come from the radiotap header, an FCS flagged there is removed.
- LINKTYPE 256 (Bluetooth LE LL with PHDR) records on advertising channels
  feed Bluetooth. Scan requests and connect requests are skipped: the
  board's scanner never reports them.
- Timestamps are the file's.
- Without `LAPSIM_CAPTURE` both starts return `LAP_IO_ENOTSUP` with
  `LAP_CAPTURE_NO_FILE`.

Files written by the TAP example app (`app-sdk/apps/tap`) replay here.

## Example

```c
lap_ble_capture_config_t c = {.size = sizeof(c), .version = 1};
int h = lap_ble_capture_start(&c);
if (h < 0) {
    lap_capture_status_t st;
    lap_capture_status(LAP_CAPTURE_BLE, &st);
    /* show st.why */
}
/* in draw() */
uint8_t ad[255];
lap_capture_meta_t m;
int n;
while ((n = lap_capture_read(h, ad, sizeof(ad), &m)) >= 0) {
    /* m.addr, m.kind, m.rssi; n bytes of AD structures */
}
```
