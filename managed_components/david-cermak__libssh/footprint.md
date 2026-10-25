# libssh ESP-IDF Footprint

Tested on **ESP32-S3**, IDF v5.5, server example with Ed25519 host key.

## Flash Size (`idf.py size-components`)

Binary size: **1,057 KB** (31% of partition free)

Key components:

| Archive | Total | Flash Code | Flash Data (.rodata) | DRAM (.data+.bss) |
|---|---:|---:|---:|---:|
| **liblibssh.a** | **233 KB** | 101 KB | 117 KB | 10 KB |
| libmbedcrypto.a | 78 KB | 72 KB | 6 KB | < 1 KB |
| libmain.a (app) | 5 KB | 3 KB | < 1 KB | 2 KB |
| libpthread.a | 1 KB | 1 KB | < 1 KB | < 1 KB |

For reference, largest non-SSH components: libnet80211.a (WiFi, 143 KB), liblwip.a (72 KB),
libc.a (67 KB), libwpa_supplicant.a (64 KB).

## Runtime Memory (Heap)

Heap consumption during an SSH session lifecycle:

| Stage | Free Heap | Delta | Min Free Ever |
|---|---:|---:|---:|
| Before `ssh_init()` | 257,564 | — | 254,784 |
| After `ssh_init()` | 254,996 | −2.6 KB | 254,784 |
| After `ssh_bind_accept()` | 249,508 | −5.5 KB | 247,860 |
| Session ready (auth + channel) | 229,644 | −19.9 KB | 225,672 |

- **Total heap cost of one SSH session: ~28 KB**
- **Transient peak: ~32 KB** (min-ever 225,672 — crypto buffers during key exchange are freed afterward)
- `ssh_init()` itself is cheap (~2.6 KB); the key exchange + session setup is the expensive part (~20 KB)

## Runtime Memory (Stack)

Main task stack (`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`):

| Stage | Stack HWM (bytes free) | Used |
|---|---:|---:|
| Before `ssh_init()` | 5,508 | ~2.7 KB |
| After `ssh_bind_accept()` | 4,884 | ~3.3 KB |
| Session ready (auth + channel) | 3,460 | ~4.7 KB |

Peak stack usage is ~4.7 KB, leaving ~3.5 KB headroom with the default 8 KB stack.

## Task List (during active session)

No additional tasks are spawned by libssh or mbedtls. All work runs in the main task.
`CONFIG_MBEDTLS_THREADING_PTHREAD=y` only adds mutex protection — it does not create threads.

| Task | Description | Stack HWM | Prio |
|---|---|---:|---:|
| main | SSH server (application) | 3,460 | 1 |
| IDLE0 / IDLE1 | FreeRTOS idle (dual-core) | ~680 | 0 |
| tiT | lwIP TCP/IP | 1,412 | 18 |
| Tmr Svc | FreeRTOS timer service | 1,308 | 1 |
| ipc0 / ipc1 | Inter-processor comms | ~520 | 24 |
| sys_evt | ESP-IDF event loop | 588 | 20 |
| esp_timer | High-resolution timer | 3,076 | 22 |
| wifi | WiFi driver | 3,368 | 23 |
