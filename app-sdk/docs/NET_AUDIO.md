# API 2.4: networking, Bluetooth and audio

The app descriptor remains 228 bytes on RV32. All old imports remain available; `LAP_TARGET_API_23` keeps an app at API 2.3. New declarations/exports are in one named area block in each shared file. The resolver accepts area blocks without requiring a globally sorted table.

`lap_caps` reports fitted Wi-Fi/BLE/speaker/microphone/SD hardware; calls return `LAP_IO_ENOTSUP` for missing hardware. Declare `LAP_CAP_NETWORK` for network changes, HTTP, UDP, DNS-SD and SNTP. Declare `LAP_CAP_MIC` to capture; the existing loader grant flow names microphone access and firmware chrome shows MIC ON, including hidden apps and desktop windows. No transmit legal, region, licence or confirmation policy was added.

All new calls execute in app callbacks and copy inputs synchronously. Stack buffers are valid. Workers never retain app memory or run app callbacks. Structures have size/version headers; initialize input headers to `sizeof(value), 1`. Poll asynchronous operations; close every handle, including failed ones. `EBUSY` also means the worker currently owns a snapshot: retry on another frame. Close uses atomic handle tickets and does not wait for worker locks. Unload invalidates handles and workers release resources; BLE holds its scanner lease through disconnect acknowledgement.

| Audit item | Service and bounds |
|---|---|
| N1 Wi-Fi | Cached SSID/IP/RSSI/channel/AP state; four operations, scan up to 16 APs. Password=NULL joins firmware-saved SSID; supplied password is copied, erased after dispatch, and saved by the existing firmware join path only on success. Join completion waits for association/IP or failure (30 s); close does not undo a global join/AP change already handed to firmware. SoftAP uses firmware identity and secret. |
| N2 HTTP(S) | Two requests; methods GET/POST/PUT/DELETE/HEAD/PATCH, URL 255 bytes, supplied headers 511 bytes, copied upload body <=4096 bytes, response headers <2048 bytes, stream queue 8192 bytes, total response <=1 MiB. Firmware CA bundle and hostname verification are mandatory. 3xx replies are delivered without following redirects. Read drains queued bytes before terminal error/EOF. 100..30000 ms total deadline. TLS admission checks contiguous internal memory. |
| N3 TCP/UDP | Existing TCP API now has six connections; legacy signatures unchanged. Four IPv4 UDP sockets, four packets each direction, <=1472-byte payload; broadcast enabled, multicast joins/leaves with TTL 1. Datagrams remain queued on undersized read; overflow counts drops. |
| N4 DNS-SD | One two-second browse snapshot (<=8 records) and one advertisement lease. Uses bundled Espressif mDNS, including probing/renaming; duplicate firmware service types return conflict. TXT <=255 bytes/8 entries. Stop unregisters only this app's service; query memory and the service worker are released after leases finish. |
| N5 Time | RTC/local/SNTP source, last monotonic sync timestamp and unknown uncertainty explicitly marked UINT32_MAX. Resync completion waits for a successful new clock update; global wall time is never set by apps. |
| B1 BLE reports | Independent scanner lease; 8 complete AD/scan-response reports, <=1650 bytes each, address/type/RSSI/timestamp. Raw TLVs retain manufacturer/services and duplicates. Too-small app buffers preserve the record; overlong/full reports count drops. Uses existing observer assembly/scan arbitration. |
| B2 GATT | One additional client, one in-flight procedure, default MTU=23 and values <=20 bytes, 16 events. Service/characteristic/descriptor discovery, read/write requests, CCCD subscriptions and notifications. Runs on existing NimBLE host timer; no extra BLE task. Pairing-required ATT failures return errors; this API does not add new pairing or expose bond keys. A characteristic event's end_handle carries its definition handle; service events carry the service end. UUID length distinguishes 2/4/16-byte UUIDs; short UUIDs occupy low bytes with other bytes zero. All bytes are Bluetooth little endian. |
| A1 Mixer | Three opt-in mono 16 kHz signed-16 channels: music/effects/voice, each 2048 samples. Per-channel volume and voice-triggered duck percentage, saturated sum. Legacy tone/PCM queues preserved. Mixer yields to mute/live radio/media; status counts consumed and dropped samples. |
| A2 Mic | One lease, mono signed-16 16 kHz, 4096 samples, drop newest/count. Codec capture ownership excludes media format changes/reinit. Device stereo ADC frames are copied to mono; visible firmware MIC ON indicator. Nano boards without a microphone return unavailable. |
| A3 WAV | One file stream and one recorder in apps_data/<app-id>/, plain basename only. PCM16 mono/16 kHz WAV, bounded 30 seconds, RIFF chunk/padding validation. SD IO is worker-owned. record_stop finalizes asynchronously and preserves the status handle; close/unload also finalizes WAV. No power-loss durability guarantee. |

## Signatures

Supporting types and return codes are documented in `include/lakeshark_app.h`. Every function below is covered by the generated registry and used by the bundled examples.

```c
int lap_operation_status(int operation, lap_operation_t *out);
void lap_operation_close(int operation);
int lap_wifi_status(lap_wifi_status_t *out);
int lap_wifi_scan_start(void);
int lap_wifi_scan_result(int operation, int first, lap_wifi_ap_t *out, int max);
int lap_wifi_connect(const char *ssid, const char *password);
int lap_wifi_disconnect(void);
int lap_wifi_ap_request(bool on);
int lap_http_open(const lap_http_request_t *request);
int lap_http_status(int h, lap_http_status_t *out);
int lap_http_header(int h, const char *name, char *out, size_t cap);
int lap_http_read(int h, void *out, size_t cap);
void lap_http_close(int h);
int lap_udp_open(const lap_socket_address_t *local);
int lap_udp_send(int h, const lap_socket_address_t *to, const void *data, size_t len);
int lap_udp_recv(int h, lap_socket_address_t *from, void *data, size_t cap);
int lap_udp_join(int h, const char *group, bool on);
int lap_udp_status(int h, lap_operation_t *out);
void lap_udp_close(int h);
int lap_mdns_browse(const char *service, const char *protocol);
int lap_mdns_next(int browse, lap_mdns_service_t *out);
int lap_mdns_register(const lap_mdns_registration_t *service);
void lap_mdns_stop(int h);
int lap_time_status(lap_time_status_t *out);
int lap_time_sync_request(void);
int lap_ble_scan_start(void);
int lap_ble_advert_next(int scan, lap_ble_advert_t *out, void *payload, size_t cap);
int lap_ble_scan_status(int scan, lap_operation_t *out);
void lap_ble_scan_stop(int scan);
int lap_ble_connect(const uint8_t addr[6], uint8_t addr_type);
int lap_gatt_discover(int connection, uint16_t start, uint16_t end);
int lap_gatt_descriptors(int connection, uint16_t start, uint16_t end);
int lap_gatt_read(int connection, uint16_t attribute);
int lap_gatt_write(int connection, uint16_t attribute, const void *data, size_t len);
int lap_gatt_subscribe(int connection, uint16_t cccd, bool on);
int lap_ble_event_next(lap_ble_event_t *out, void *data, size_t cap);
void lap_ble_disconnect(int connection);
int lap_mixer_config(int channel, int volume, int duck);
int lap_mixer_write(int channel, const int16_t *pcm, int samples);
void lap_mixer_clear(int channel);
int lap_mixer_status(int channel, lap_operation_t *out);
int lap_mic_open(void);
int lap_mic_read(int h, int16_t *pcm, int max, lap_operation_t *status);
void lap_mic_close(int h);
int lap_sample_play(int channel, const char *name);
int lap_audio_record_start(int mic, const char *name, uint32_t max_ms);
int lap_audio_record_stop(int h);
int lap_audio_operation(int h, lap_operation_t *out);
void lap_audio_close(int h);
```

## Examples and deterministic simulator controls

- `tcp_probe`: HTTPS streaming, UDP loopback/multicast, Wi-Fi joins/scans/AP, DNS-SD and time sync. Keys are shown in the app. W asks for SSID/password; K joins the entered SSID with firmware credentials.
- `ble_explorer`: full advertisements, manufacturer/service summary, RSSI timestamps, GATT discovery/read/write/subscriptions and value bytes. W explicitly writes 01; no automatic peripheral writes.
- `sound_board`: channels 1/2/3, M microphone, R five-second recording, S stop/finalize, P playback. One binary handles missing mic/speaker/SD.

Use LAPSIM_HW=wifi,ble,mic,speaker,sd,desktop to fit synthetic hardware, or none to remove it. The normal host starts on SIM WLAN; HTTPS uses deterministic fixtures and never contacts the URL. UDP uses actual host sockets; BLE has a synthetic battery-service peripheral.

LAPSIM_SCRIPT steps use N:command separated by semicolons:

| Commands | Effect |
|---|---|
| wifierror=code; wifidown | Inject join/scan/AP failure or disconnection. Reset code to 0. |
| httperror=code; httpstatus=200; httpbody=text | TLS/IO failure, HTTP status, copied streamed body fixture. Negative codes are LAP_IO_E*. |
| udpdrop=0/1 | Drop outbound datagrams and increment drop counter. |
| mdnserror=code; mdnsempty=0/1 | Duplicate/unavailable services or empty browse snapshot. |
| blesilence=0/1; blead=RSSI,HEX | Stop default repeated adverts or inject raw AD, with rotating synthetic address. |
| gatterror=code; blenotify=HEX; bledisconnect | Procedure failure, notification or lost link. |
| micpcm=amplitude; micwav=path | Synthetic square/silent PCM, or canonical PCM16 mono/16 kHz WAV input. |
| mixpcm=path; audiomute=0/1; audiolive=0/1 | Capture pre-arbitration mixed s16le PCM, or simulate speaker priority. |

## Verification

Run from the repository with MinGW, Ninja and the supplied CMake on PATH:

```powershell
cmake -S bench -B bench/build -G Ninja -DLS_SANITIZE=OFF
cmake --build bench/build --target test_lap_net_audio test_lap_net test_lap_net_dns test_lap_api test_lap_radio test_lap_kit test_lap_manifest -j 8
ctest --test-dir bench/build -R '^test_lap_(net_audio|net|net_dns|api|radio|kit|manifest)$' --output-on-failure
python app-sdk/tests/test_example_contracts.py --frames 40 --build-dir bench/build
python app-sdk/tests/test_net_audio.py
python -m unittest discover -s app-sdk/tests -p test_phase1.py
python app-sdk/apidoc.py --generate
```

`bench/tools/check_lap_net_audio_device.py <existing-IDF-compile_commands.json>` syntax-checks the 19 integration translation units with the real device compiler/configuration. It does not configure, link, build images or modify that build directory; logs go to bench/build. This check also caught/fixed the pre-existing strong-function address warning in the Phase 1 device input path.

The board must still verify credential persistence/auth failures, TLS certificate/time/redirect behavior, broadcast/multicast and DNS-SD interoperability/conflicts/goodbyes, SNTP resync, head/HID/GATT coexistence and late disconnects, and Nano internal-RAM admission. Audio needs ADC slot/channel/gain, capture clock continuity, mixer ducking/clipping/live-radio priority, visible indicator in all display modes, SD latency, and power/unload tests. API 2.6 gives each resident a lazy firmware-owned network task with an 8 KiB PSRAM stack; drained tasks are joined and released on unload. They do not block the mic/mixer/UDP worker. SD latency and radio load can still affect audio; the host frame clock is not evidence of real-time throughput.

## Firmware inventory

- `components/apps/CMakeLists.txt`
- `components/apps/tui/desk/ls_desk.c`
- `components/apps/tui/ls_lap_api.c`
- `components/apps/tui/ls_lap_ble_client.c`
- `components/apps/tui/ls_lap_exports.inc`
- `components/apps/tui/ls_lap_grants.h`
- `components/apps/tui/ls_lap_manifest.c`
- `components/apps/tui/ls_lap_manifest.h`
- `components/apps/tui/ls_lap_mdns.c`
- `components/apps/tui/ls_lap_net.c`
- `components/apps/tui/ls_lap_net_audio.c`
- `components/apps/tui/ls_lap_net_audio.h`
- `components/apps/tui/ls_lap_radio.c`
- `components/apps/tui/ls_lap_sound.c`
- `components/apps/tui/ls_lap_sys.c`
- `components/apps/tui/ls_lap_udp.c`
- `components/apps/tui/ls_lap_ui.c`
- `components/apps/tui/ls_lap_wifi_http.c`
- `components/apps/tui/ls_tui_screen.c`
- `components/lakeshark/audio/audio_out.c`
- `components/lakeshark/audio/audio_out.h`
- `components/lakeshark/core/ls_time.c`
- `components/lakeshark/core/ls_time.h`
- `main/CMakeLists.txt`
- `main/ble_hid.c`
- `main/ble_lap_client.c`
- `main/ble_link.c`
- `main/ble_link_stub.c`
