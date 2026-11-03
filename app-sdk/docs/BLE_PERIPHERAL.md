# Bluetooth peripheral services — API 2.6 (B3/B4)

Apps own beacon content, sensor values, input mapping and UI. Firmware owns the
NimBLE host, copied database, controller commands, pairing keys and leases.
The 228-byte RV32 descriptor is unchanged. API 1.0–1.3 and 2.0–2.5 remain loadable;
`LAP_TARGET_API_25` hides the new declarations when compiling for 2.5 firmware.

## Admission and lifetime

Check `lap_caps.hardware` independently for `LAP_HW_BLE_ADVERTISER`,
`LAP_HW_GATT_SERVER` and `LAP_HW_HID_DEVICE`. BLE central support alone implies
none of these. All symbols resolve in disabled/no-BLE builds; starts return
`LAP_IO_ENOTSUP`. A single .lap can display an unsupported state on those boards.

Every emitting operation requires the manifest's `LAP_CAP_RADIO_TX`. The HID
convenience service additionally requires `LAP_CAP_HID_DEVICE` (bit 32 of the
existing flags byte). There are no licence, region, legal-band or trusted-TX
confirmation checks. Advertising power is bounded by the C5/C6 PA range
(-24..20 dBm) and checked against the actual controller-selected power before
starting; a higher-than-requested selection is rejected with `EPOWER`.
The controller's smaller supported range can reject a request. No increased PA,
connection, ACL, event or mbuf pools are requested by these services.

One exclusive peripheral session exists. LINK (including passive listening),
SWEEP, RID, FIND, HID host and SDK scan/central leases must be released first.
An existing owner is never stopped. Start can return `EBUSY` immediately for an
SDK owner, or a handle whose asynchronous status reports `EBUSY` for a firmware
owner. Close the failed handle before retrying. Other owners also receive busy
while a peripheral lease is held. Idle HID scan/command leases are released when no peer, retry, discovery, bond
operation or queued command still needs the host. Queue admission is serialized
with that release and peripheral acquisition. The HID timer continues pumping the peripheral
backend but suspends host reconnect/command work during that lease.

All starts return a positive session handle; poll `lap_ble_peripheral_status`
for READY/error. A GATT database must be READY before `lap_ble_advertise` attaches
advertising with `cfg.server=handle`. A beacon uses `cfg.server=0`. Advertisement
stop closes a beacon; it only stops advertising for a GATT/HID session. Close
ends the whole session. Reconfiguration requires a disconnected peer and retains
the same database. Stop, close and unload are asynchronous and generation-tagged;
a new app cannot reuse the service until the host has acknowledged disconnect
and database cleanup. Stale close calls cannot stop a newer session.

The backend preserves firmware GAP/GATT services and resets the IDF resource
counts when rebuilding a database, using two pinned IDF 5.5.4 host internals.
It avoids the boot-only service initializers in runtime operations, so allocation
failures return ENOSPC. Repeated opens do not accumulate resource counts.

The host does not restart connectable advertising after disconnect: the app can
choose whether to accept another peer by advertising again (the sensor example
does). This also avoids an app accidentally remaining discoverable after a
completed workflow.

## Advertising

`lap_ble_advertise(const lap_ble_advert_config_t *cfg)` copies both 31-byte AD
and scan-response records. Each is an exact sequence of length/type/value TLVs;
malformed records or overflow are rejected. Include name (0x09), manufacturer
company/data (0xff), 16/32/128-bit service UUID lists (0x03/0x05/0x07), and service
data (0x16/0x20/0x21) as needed. Empty payloads are supported. Intervals are
20..10240 ms and converted to the controller's 0.625ms units. Nonconnectable,
scannable and connectable legacy PDUs are supported; arbitrary extended payloads,
periodic advertising and multiple advertising sets are outside this API.

The backend uses extended HCI commands with legacy PDUs, advertising set 0,
so TX power is per advertising set. `CONFIG_BT_NIMBLE_EXT_ADV` is a build
prerequisite. This avoids mixing legacy advertising and the firmware's extended
scan commands, which the hosted controller has previously rejected.

## GATT database and events

Declare at most 16 flat ordered `lap_gatt_attribute_t` records: a primary
service, its characteristics, each followed by its descriptors. IDs are unique,
nonzero app-chosen numbers; parents refer to the current service/characteristic.
UUIDs are little-endian (2, 4 or 16 bytes). GAP/GATT service UUIDs 0x1800/0x1801
are reserved. CCCDs are generated automatically. Values are copied, at most
128 bytes per attribute. Firmware never retains an app pointer.

READ/WRITE/WRITE_NR/NOTIFY/INDICATE and encryption/authentication flags are mapped
to NimBLE permissions; descriptors support read/write/security. One peripheral
peer is supported without increasing the board's configured maximum connections.
`lap_gatt_server_set` updates a cached value synchronously. ATT READ returns that
value and queues a READ event; WRITE validates permissions/length, atomically
updates the cached value and queues the copied write. Writes can be processed
from the screen task with `lap_gatt_server_next`. An eight-event ring and a
reported dropped counter bound memory; a full ring rejects ATT read/write access
with insufficient resources instead of silently accepting a lost write. An
undersized event output returns ELIMIT without consuming the event.

There is deliberately no deferred ATT reply: NimBLE's access callback must
return promptly, so it never waits for an app or calls app code. Refresh dynamic
values before peer reads. Conditional write authorization must be represented
by permissions or an application-level command/result characteristic; accepting
an ATT write is not an application acknowledgement. There is no multi-attribute
app transaction. The API reports connection, disconnection, MTU, encryption/bond,
subscription, advertising and send events. Bond event value bits are bonded=1,
encrypted=2, authenticated=4. Keys remain in the existing firmware NVS store.

`lap_gatt_notify(server,id,data,len,indicate)` queues at most 20 copied bytes,
even when a peer negotiates a larger MTU. It requires the relevant subscription,
connection and attribute security. Only one send is outstanding. SENT reports
host submission (notification) or confirmation/error (indication). Close/unload
interrupts an unacknowledged indication by disconnecting. UUID/attribute layouts
can differ between apps using the same board identity; a peer with a stale GATT
cache may need service rediscovery or to forget/re-pair the board.

## HID peripheral

`lap_hid_device_open(kind,name)` builds the standard HID-over-GATT service and
connectable advertising at 100ms/0dBm. `lap_hid_device_report_map` copies the exact
map used. All maps use report ID 1, which is not included in send payloads:

| Kind | Input bytes | Layout |
|---|---:|---|
| Keyboard | 8 | modifiers, reserved, six USB keyboard usages |
| Mouse | 4 | three button bits, signed relative X/Y/wheel |
| Gamepad | 6 | little-endian buttons16, signed absolute X/Y/Z/Rz |

Keyboard LED output uses report ID 1/type output and arrives as WRITE on ID 8.
Input report characteristic ID is 6, report map ID 3. ID 5 is report protocol
(only value 1 is supported); no boot-protocol emulation. Focus loss, pause or
hide queues a neutral report using the same bounded send path. On unload the
peer is disconnected. Apps should send both press and release states.

The exclusive session temporarily uses firmware-owned NoIO/Just Works bonding
and restores the previous host security settings on close. HID reports require
encryption. No MITM claim is made; a peer requesting passkey/numeric comparison
is rejected rather than silently accepting it. Attributes that require MITM
will reject unauthenticated access. Existing authenticated bonds can satisfy
that permission. No app can export/delete keys or silently erase another BLE
user's bonds. Windows/macOS/Linux/Android/iOS pairing, host caching, report
interpretation and reconnect behavior still require interoperability tests.

## Nano RAM and build options

`CONFIG_LS_LAP_BLE_ADVERTISER`, `CONFIG_LS_LAP_GATT_SERVER` and
`CONFIG_LS_LAP_HID_DEVICE` are switches along a dependency chain.
HID also requires existing NimBLE security and persistent bonding support.
All default on when their NimBLE prerequisites exist, including Nano. The
T-Display OTA profile explicitly disables them to preserve its existing apps
and compressed music playback. The no-feature common object
contains only unsupported stubs, with no peripheral queues/database. The
firmware host stack and transport/DMA allocation classes remain unchanged.

The following are static storage figures from RV32 `-Os` compile-only checks;
the adapter check mirrors the local IDF 5.5.4 public definition layouts. They
are not measured total-board heap deltas or a firmware link-map measurement.

| Build feature | Added PSRAM static storage | Added internal control state | Additional NimBLE heap |
|---|---:|---:|---|
| All disabled | 0 | one shared lease flag | 0 |
| Advertising | 7,100 B | under 64 B | advertising command/mbufs; estimate <2 KiB transient |
| GATT server | 9,388 B total | same | ATT/CCCD/service allocations; budget estimate up to 8 KiB |
| HID device | same 9,388 B | same | uses the same server; 162 B of report maps in flash |

Breakdown: shared copied values/events 3,952 B; host command 2,708 B; pending host
state events 440 B; optional NimBLE definition arrays 2,200 B and preserved firmware service definitions 88 B. Definitions and
command snapshots stay in PSRAM. Small try-lock/handle fields stay internal.
The shared bounded storage is reserved whenever advertising is compiled in,
even if the app only beacons. HID adds no task or connection pool. Compiler
stack-use checks report 176 B for the ATT bridge and 96 B for the host pump;
HID construction uses an 80 B frame plus a 208 B attribute helper frame. These
exclude called NimBLE/library stack frames and must be checked on the board.

Admission requires an internal largest block/free heap of at least 4/8 KiB for
beacons or 12/24 KiB for server/HID. Failure reports ENOSPC through status and
leaves other BLE users untouched. These are reserve thresholds, not measured
allocation costs. NimBLE heap may be internal or external according to the
board's existing allocation configuration; never relocate DMA buffers or the
flash/NVS-writing host stack based on these numbers. Do not undo the Nano's
trimmed connection/ACL/event pools to make this work. Measure internal free,
largest block, task high-water marks and heap deltas when changing board resource
budgets. Disabling a feature clears the corresponding `lap_caps` bit.

## Simulator and validation

The same copied-state service runs in bench; a synthetic peer replaces the
controller. `LAPSIM_HW=none` removes Bluetooth. `LAPSIM_PERIPHERAL=none`,
`advertiser`, `gatt` or `hid` limits peripheral build features (default all).
`LAPSIM_SCRIPT` steps use `frame:command`, separated by semicolons:

- `periphbusy=1|0`, `peripherror=<negative code>`: lease/backend failure.
- `periphconnect`, `periphdisconnect`, `periphmtu=23..517`.
- `periphbond=0|1`: unencrypted or bonded/encrypted Just Works peer.
- `periphsubscribe=ID,BITS`: notify=1, indicate=2.
- `periphread=ID`, `periphwrite=ID,HEX`: permission-checked ATT access.
- `periphack=0|1`: stall/resume indication confirmation; close still completes.

Examples: `ble_beacon`, `gatt_sensor` (battery + standard Location/Speed GPS),
`board_gamepad` (also selectable keyboard/mouse). The firmware adapter bench
harness compiles the actual main backend against bounded NimBLE test doubles;
it checks database lifetime, GAP callbacks, power, allocation failure, early
close, reset contention and 32 repeated opens without resource-count growth. Public coverage is generated through
`python app-sdk/apidoc.py --generate` using `api_registry.py`.

Run CMake targets `test_lap_ble_peripheral`,
`test_lap_ble_peripheral_disabled`, `test_lap_ble_peripheral_host`,
`test_lap_ble_advertiser_only`;
`python app-sdk/tests/test_ble_peripheral.py`; and
`python app-sdk/tests/test_example_contracts.py --frames 40 --build-dir bench/build`.
No bench test emits RF or builds/flashes a firmware image. Board verification
must cover controller power/scan-response content, lease handoff under concurrent
LINK/HID/RID use, security/bond persistence, GATT cache rediscovery, indication
timeouts, neutral reports on focus loss, teardown after host reset and the RAM
measurements above.

## Firmware files changed

New service and backend: `components/apps/tui/ls_lap_ble_peripheral.{c,h}`,
`main/ble_lap_peripheral.c`. Integration: `main/ble_link.{c,h}`,
`main/ble_link_stub.c`, `main/ble_hid.c`, `main/Kconfig.projbuild`,
`main/CMakeLists.txt`; and `components/apps/tui/ls_lap_api.c`,
`ls_lap_ble_client.c`, `ls_lap_exports.inc`, `ls_lap_manifest.{c,h}`,
`ls_lap_net_audio.c`, `ls_lap_radio.c`, `ls_lap_sys.c`, `ls_lap_wifi_http.c`.
The SDK header/allowlist, registry/coverage, simulator wrappers, tests and examples
are updated together. Changes are intentionally uncommitted.
