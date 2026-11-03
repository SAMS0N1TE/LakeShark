# API 2.6 services and worker DSP

An ordinary foreground app can run alongside resident services. The
`CONFIG_LS_LAP_SERVICES_MAX` build setting defaults to two on P4 Nano and
three on the other P4 profiles; its supported range is 1–4. There is one
additional foreground context. The 228-byte `.lap` descriptor is unchanged.
Firmware still accepts API 1.0–1.3 and 2.0–2.5. `LAP_TARGET_API_25` hides
the new declarations when building a legacy artifact.

`lap_service_limit()` reports the actual configured service limit.
`lap_service_list(out,max)` copies resident rows, and
`lap_service_query(app_id,out)` returns 1 for a resident, 0 for absence.
Each row reports screen visibility, app heap/cache bytes, SDK pool bytes,
published handles, limits and tick timing. Settings > Services lists every
resident with an individual stop control. `app svc` lists all residents;
`app stop <id>` stops the named service. Nothing starts automatically at boot.

## Ownership and budgets

Each context owns its heap, cached files and file tickets, legacy SD jobs,
scoped streams/storage jobs, TCP sockets, HTTP/Wi-Fi operations, UDP sockets,
mDNS leases, BLE scan/client handles, mixer channels, microphone handle and
WAV streams, IQ session/channel/decoder, packet/scan/database/recording/TX/NFC
jobs, service timer, callback guard, input queues, overlays and IPC topics.
Worker selection is task local; physical producer callbacks select the
owning lease explicitly. DNS completions retain only stable socket-table
storage. HTTP tasks retain copied requests, never ELF pointers.

The public header lists per-family handle limits. Every context has the
same bounded tables and a 4 MiB tracked heap/cache allowance. SDK resource
pools have a separate 2 MiB envelope; the snapshot accounts fixed tables,
staged SD data and live SDK radio/audio buffers. Driver/controller heaps and
task control blocks are firmware overhead, outside the app heap accounting.
Admission can fail before a limit if the board has insufficient free PSRAM.
The aggregate published-handle ceiling is 64; each family has its own smaller
enforced table limit. No context can allocate another context's table slots.

Ticks run on the screen task, in rotating resident order, at 50–10,000 ms
periods. Each gets the existing 4 ms budget and strike policy. A tick must
return; native screen/tick faults retain the firmware's reboot/recovery
behavior. Use the worker hook for heavier computations.

Singleton physical backends remain exclusive: IQ receiver, packet/scan/TX
backend, BLE client/controller lease, microphone and NFC. Another resident
gets busy instead of stealing the device. Private mixer channel numbers are
local to an instance; all resident channels mix at one hardware sample clock.
Legacy manifest receivers also hold a lease, and another app cannot retune
them through actions or waterfall controls. GPS demand aggregates all resident leases. Closing a
hidden service cannot close another app's picker or change its screen mode.

Closing/unloading cancels owned operations, stops transmission, detaches
subscriptions and joins DSP before freeing app code/state. I/O workers finish
or discard copied operations and close their own backend handles. Receiver
audio rings are reclaimed after the producer copy quiesces. Crash recovery
still needs the board reboot path; this is trusted native code, not memory
isolation against hostile machine code.

API 2.6 topic subscriptions, queued IPC and notification handles expire on
unload. Notifications are capped at two per current app. API 2.5 applications
retain their original bounded copied notification/IPC mailbox behavior for
compatibility. Neither path retains callbacks into an unloaded ELF.

## Custom DSP

Open a normal IQ session, then a filtered/decimated channel, then call:

```c
int lap_rx_dsp_start(int channel, const lap_dsp_config_t *config);
int lap_rx_dsp_status(int worker, lap_dsp_status_t *out);
int lap_rx_dsp_next(int worker, lap_dsp_result_t *out, void *data, uint32_t cap);
void lap_rx_dsp_close(int worker);
```

One hook per context receives independent copies of the channel stream,
even when the screen's channel-read queue is full. Input is two blocks of
up to 2,048 IQ pairs. Output is eight messages of at most 192 bytes each.
Metadata preserves epoch, sample index, timestamp, retune/gap and
live/replay/synthetic flags. Overflow drops newest and increments counters.

The callback is pure computation. It may read its borrowed input and mutate
only its declared app-owned state and borrowed output. It must not use SDK
calls, blocking, locks, allocation or firmware internals. Its state (up to
64 KiB) must remain alive until status says STOPPED, OVERRUN or FAILED. The app's
screen must not concurrently mutate worker state. Close is asynchronous.

The per-block budget is 1–10 ms (default 2 ms), conservatively measured as
elapsed time including scheduling. Device waits round up to scheduler ticks.
The IO supervisor and private lower-priority task share core 0, so the
supervisor can preempt and delete a non-returning computation without
executing app code on capture/UI tasks. It joins before ELF cleanup. Host
tests use a private thread and the same deadline/result/drop contract.
Overrun disables that hook; partial state must be reset before restarting.
Worker admission failure reports FAILED and an explicit error without
incrementing the CPU-overrun counter. The device semaphore/control block is
internal RAM; the private computation stack and copied data use PSRAM.
This does not make arbitrary native code safe to terminate while it holds
firmware locks: obey the pure-computation contract.

## Examples and headless scenarios

`service_logger` records CSV and publishes copied IPC records;
`service_notifier` receives those records and exercises notifications and
service discovery. Open both, close their screens, then open a third app.
`custom_decoder` implements app-owned A5/payload OOK framing without an
internal firmware include. It checks `lap_caps` and radio capabilities and
handles absent/busy/unsupported hardware.

```text
LAPSIM_SERVICES=service_notifier,service_logger
LAPSIM=dice
LAPSIM_SCRIPT=50:services;60:service_stop=service_logger;80:services

LAPSIM=custom_decoder
LAPSIM_SCRIPT=0:dspook=90

LAPSIM=custom_decoder
LAPSIM_SCRIPT=0:dspcost=20000
```

`dspook=-1` restores the ordinary synthetic radio source. `dspcost=0`
restores measured execution; a cost above budget drops the block and
disables the hook without executing it. `dspfail=1` injects worker-memory
admission failure (`0` restores it). `LAPSIM_RADIO=none` exercises board
adaptation. The standalone C regression also executes a genuinely
non-returning callback and verifies termination and subsequent SDK use.

Run `test_lap_services26`, `test_lap_services26_nano`, the real-loader
`test_lap_service` host harness, `app-sdk/tests/test_services26.py`, and
`app-sdk/tests/test_example_contracts.py`. The registry and coverage matrix
come from `python app-sdk/apidoc.py --generate` / `api_registry.py`.

On boards verify FreeRTOS deadline/tick granularity and watchdog health,
per-service CPU/PSRAM admission under receiver/TLS/audio load, real concurrent
mixing, late BLE/DNS/controller completions, SD flush and hot unplug, TX/PA
cancellation, foreground overlays, and native crash/reboot attribution and
hardware cleanup. Host tests do not establish RF or real-time performance.

## Firmware files

In `components/apps/tui/`: `ls_lap.c/.h`, `ls_lap_api.c/.h`,
`ls_lap_context.h`, `ls_lap_usage.h`, `ls_lap_service.c/.h`,
`ls_lap_dsp.c/.h`, `ls_lap_sys.c`, `ls_lap_system.c/.h`,
`ls_lap_net.c`, `ls_lap_net_audio.c/.h`, `ls_lap_wifi_http.c`,
`ls_lap_udp.c`, `ls_lap_mdns.c`, `ls_lap_ble_client.c`, `ls_lap_sound.c`,
`ls_lap_audio.c`, `ls_lap_radio.c/.h`, `ls_lap_radio_hw.c`,
`ls_lap_radio_io.c`, `ls_lap_rx24.inc`, `ls_lap_sensors.inc`,
`ls_lap_kit.c`, `ls_lap_events.c`, `ls_lap_console.c`,
`ls_lap_manifest.h`, and `ls_lap_exports.inc`.

Also: `components/apps/tui/screens/ext/scr_settings.c`,
`components/apps/tui/screens/ext/scr_radios.c`,
`components/lakeshark/core/ls_decoded.h`, and `main/Kconfig.projbuild`.
