# SDK 2.4–2.6 board size measurements (2026-11-02)

Measured from a clean `t-display-p4` build with both `LS_LAP_3D` and
`LS_MUSIC_COMPRESSED` enabled. No partition changes. The initial build produced
an ELF/map/bin and failed the partition check: **4,876,976 bytes**, versus the
**4,718,592-byte** OTA slot. At least **191,152 bytes** must be saved for 32 KiB
headroom.

Reproduce the linked file/component measurements with the ESP-IDF Python:

```powershell
python -m esp_idf_size --target esp32p4 --files --format json build_tdp4/lakeshark.map
python -m esp_idf_size --target esp32p4 --archives --format json build_tdp4/lakeshark.map
```

Baseline JSON/map and build/test logs are in the ignored `bench/evidence/` and
`bench/*.log` artifacts. Values below are linked flash bytes, including loaded
data and excluding BSS. They are footprints, not independent additive savings:
shared strings/dependencies and linker alignment can move between objects.

Baseline component totals: `apps` **1,800,532**, `lakeshark` **851,989**,
`main` **195,908**, NimBLE `bt` **92,097**, and `espressif__mdns` **37,616**
bytes. These include existing firmware as well as the SDK work.

| SDK feature | Baseline bytes | Measurement boundary |
|---|---:|---|
| BLE peripheral / GATT server / HID device | 8,904 | `ls_lap_ble_peripheral.c` 5,864 + `ble_lap_peripheral.c` 3,040; NimBLE separately 92,097 for all roles |
| BLE GATT client | 8,576 | `ls_lap_ble_client.c` 7,012 + `ble_lap_client.c` 1,564 |
| HackRF TX backend | 2,356 | Retained `ls_hackrf_tx_*`, TX completion/release/step/guard/removal functions; whole RX+TX `hackrf_dev.c` 9,230 |
| Worker DSP | 3,027 | `ls_lap_dsp.c` |
| Multi-service lifecycle/accounting | 1,502 | Retained `lap_service_*` / `ls_lap_instance_*` functions; shared runtime `ls_lap_api.c` 22,834, loader `ls_lap.c` 13,566, service glue `ls_lap_service.c` 2,386 |
| Desktop host broker | 1,407 | `ls_desk_host.c`; desktop SDK functions and broker together 2,042 code bytes |
| Packets / scan / capture / replay / TX adapters | 26,526 | `ls_lap_radio_io.c` 22,788 + hardware adapter 3,190 + channel stores 548; includes NFC SDK functions |
| NFC tag service | 1,640 | Retained `lap_nfc_*` and `ls_nfc_tag_*` functions, excluding the tag worker in mixed `ls_nfc_suite.c` (6,446 total) and NDEF parser |
| Wi-Fi / HTTPS / UDP / time | 13,426 | `ls_lap_wifi_http.c` 8,974 + `ls_lap_udp.c` 3,428 + `ls_time.c` 1,024; shared AV dispatch 480 separately |
| mDNS | 41,552 | `ls_lap_mdns.c` 3,936 + `espressif__mdns` library 37,616 |
| Audio mixer / mic / WAV | 10,978 | `ls_lap_sound.c` 8,474 + `ls_lap_audio.c` 2,504; shared device audio drivers excluded |
| Receive channels / spectrum / decoder attach | 25,684 | Shared `ls_lap_radio.c` 20,888 + hardware adapter 4,796; identifiable API 2.4 channel/decoder/spectrum functions 4,488 code bytes |
| Input / sensors / navigation | 10,200 | Identifiable input functions 2,682 + GPS/motion/navigation functions 7,518 code bytes; shared `ls_lap_sys.c` 18,318 |
| Notifications / picker / streams / IPC | 10,164 | Shared `ls_lap_system.c`; IPC entry points alone 830 code bytes |

The symbol subsets use retained `.text.<function>` entries in the map (nonzero
load addresses), including local helpers where named. Mixed-file totals include
older SDK functionality; the table explicitly identifies those boundaries.

Initially, the five lower-priority service groups (BLE peripheral, HackRF TX, worker DSP,
NFC tag service and mDNS) reduce the non-LTO image to **4,794,752 bytes**: a saving
of **82,224 bytes**. They alone cannot meet the slot limit, even without headroom.
The board retains BLE central/observer, all receive/decoder, packet/scan/capture,
Wi-Fi/HTTPS/UDP/time, audio and input services. NFC tools for an attached MixRF
remain built; only the SDK tag service is omitted.

Each omitted service retains ELF exports and returns unsupported. `lap_caps`
clears peripheral/NFC bits, the HackRF `radio_tx` bit, and the new
`LAP_HW_WORKER_DSP` / `LAP_HW_MDNS` bits. Nano defaults keep every switch enabled.

## Final result

| Build | App bytes | App slot | Free |
|---|---:|---:|---:|
| T-Display-P4 | **4,682,448** | 4,718,592 | **36,144 (35.3 KiB)** |
| Nano v3 DSI | **6,062,112** | 9,437,184 | 3,375,072 |

`bench/size_gate.ps1 t-display-p4` passes, as does an explicit 32 KiB headroom
assertion. Total reduction from the full T-Display baseline is **194,528 bytes**.
Both boards retain `CONFIG_LS_LAP_3D=y` and `CONFIG_LS_MUSIC_COMPRESSED=y`.
The Nano build has all seven optional SDK switches enabled.

Service gating alone was insufficient. The T-Display also uses LTO for the
SDK/UI, task-level decoder/DSP/core code and protocol libraries (hosted RPC,
TLS, HTTP, JSON, NVS, lwIP and NimBLE host). Existing per-source optimization
levels remain in place. Hardware board/radio code, the app IRAM capture source,
hosted wake ISR and NimBLE IRAM sources stay outside LTO. lwIP joins only when
its noflash placement options are disabled; the controller stays outside this
host-only build. The final map check finds **654 retained named IRAM code
symbols, with none moved into flash** (`sdk-final-iram-check.json`).

The NFC switch also removes the SDK tag worker from the shared NFC suite,
while preserving classic NFC tools. Its additional linked saving in the final
LTO configuration was 1,904 bytes.

Validation: **17/17** selected host tests pass normally and **17/17** with LTO.
Coverage includes peripheral/advertiser variants, HackRF TX enabled/disabled,
worker DSP and mDNS on/off, NFC suite/tag-service on/off, capability/error and
loader-export checks, receive/decoder and audio contracts, 3D and music decode.
`app-sdk/tests/test_example_contracts.py` passes **38/38** bundled apps/templates.
Logs: `sdk-size-host-switch-tests.log`, `sdk-size-lto-host-tests.log`,
`sdk-size-example-contracts.log`, `sdk-size-gate-final.log`,
`tdp4-final-protocol-build.log`, `nano-sdk-features-final-build.log`.

No commits, pushes, flashing, desktop windows or partition-table changes.
