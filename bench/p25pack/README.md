# P25 GPS state packs

Backend extension of the existing P25 PROGRAM app. No screen layout or drawing is changed.
`LS_P25_SETTING_GPS_SITES` is reserved as setting id **27** in `ls_p25_settings.h`;
its name, value reader and toggle callback are ready for the screen owner to place in OPTIONS.

## Produce packs and a catalog locally

```powershell
cmake -S bench -B bench/build -G Ninja
cmake --build bench/build --target p25pack_validate test_p25_pack test_p25_sites_runtime test_p25_packs_runtime
python -B bench/p25pack/make_pack.py bench/p25pack/sample/sites.csv --out bench/build-p25gps-evidence/packs
python -B bench/p25pack/make_catalog.py bench/build-p25gps-evidence/packs --out bench/build-p25gps-evidence/packs/catalog.txt --key <PEM-path>
```

The publisher requires Python `cryptography` and the OTA publisher directory, given with
`--publisher-dir` or the `LAKESHARK_OTA_TOOLS` environment variable.
It loads keys through `ota_common.key_path` / `load_private_key`, checks the public key
against `main/ls_ota_key.h`, and signs with the same ECDSA P-256/SHA-256 path as
`ota_publish.py`. Only `--key` or `LAKESHARK_OTA_KEY` supplies a private key.
It never deploys. The firmware host validator must exist before either production command runs.
`--validator` can point at another host build. `catalog.txt` may share the packs directory;
regenerating it does not treat the previous catalog as a state pack.

CSV required columns: `state,system,site,lat,lon,range_km,control_mhz`.
Optional columns: `sysid,wacn,nac,talkgroup_file` (or `sysid/wacn` as a slash pair).
IDs accept decimal or `0x` hexadecimal and are emitted as validated decimal profile fields.
Control MHz values are separated by semicolons. Coordinates use decimal degrees;
ranges must resolve to whole metres, controls to whole Hz. Talkgroup CSV columns are
`id,alias,enabled,priority`, with true/false enabled and priority defaulting to zero.
The single sample system is fictional. There is no real site database here.

Each pack concatenates complete existing **version=2** profiles; every new record starts
with `version=2`, followed by its system/site, optional IDs, georeferenced control lines and
optional talkgroups. Every control requires coordinates and a radius of 500..200000 metres.
The original profile parser validates each record. Individual legacy v1/v2 files and
`/sdcard/p25_profile.txt` retain their existing parser and load route.

Catalog metadata defaults are in `data/p25/catalog-defaults.json`. Schema 1 requires
`schema_version`, `version`, `tier` and `states`; a state override may contain `name`,
`version`, `tier`. Unknown fields, invalid versions, tiers and USPS codes are refused.
Pass `--metadata` for another file. Free/plus is data, never an embedded state tier table.

The signed catalog uses the OTA v1 envelope with board `P25-sites`, channel `catalog`,
and `schema=p25-sites-1`. Each signed entry is:

```text
entry=ST|Name|ST.txt|bytes|sha256|version|free-or-plus
```

The firmware reuses the OTA parser, key rotation list, SHA-256/ECDSA verifier, HTTPS
certificate bundle, host fallback and bounded connection waits. OTA firmware manifest
allocation remains 4096 bytes; the same parser permits data catalogs up to 16384 bytes.
The primary catalog URL is `https://ota.terminalbay.com/p25/catalog.txt`, with
`https://lakeshark-ota.pages.dev/p25/catalog.txt` as fallback. Pack paths are restricted
to `ST.txt` beside the catalog. Downloaded bytes must match both the signed byte count
and SHA-256, then pass the firmware pack parser, before installation.

FAT replacement retains `.bak` until the verified `.tmp` has been renamed into place.
A failed final rename rolls back. The loader recovers `.txt.bak` when the final file is
missing after interrupted replacement. Optional `.txt.version` is display metadata;
when installed bytes match a catalog entry, the signed catalog version takes precedence.

## Device commands and UI seams

```text
p25 geo on|off|status
p25 pack refresh
p25 pack list
p25 pack get MA
p25 pack rm MA
p25 pack token <token>
```

Settings and token use NVS namespace `p25packs`, keys `gps_sites` and `token`.
GPS sites defaults enabled; with no packs the state is `no packs`. Manual PROGRAM
control picks and surveys pause following. `p25.geo.resume` clears manual pause and
reconsiders the nearest site; it does not override the persisted OFF setting.
GPS acquisition remains owned by the existing GPS module: no-fix status includes GPS off.

The entitlement seam currently means **a nonempty stored token**: plus packs remain
listed but installation is refused without one. The network worker sends a stored token
in `X-LakeShark-Token` only to the OTA hosts; token-bearing redirects are refused.
Future server verification belongs at this HTTP header seam. Tokens are never published
as values or printed by these commands. This is not a cryptographic entitlement proof,
and sideloaded SD files remain supported.

Actions: `p25.geo.toggle`, `p25.geo.resume`, `p25.pack.refresh`, `p25.pack.get` (text USPS code),
`p25.pack.remove` (text USPS code). Store actions require `LS_CAP_STORE`; resume requires
`LS_CAP_TUNE`. Jobs are queued without waiting on the input task. Inspect cached state
for completion or refusal; queued does not mean installed.

Values:

| Path | Meaning |
| --- | --- |
| `p25.geo.enabled` | persisted preference, boolean |
| `p25.geo.state` | following / paused / no fix / no packs |
| `p25.geo.site`, `p25.geo.system` | current selected record names |
| `p25.geo.distance` | km; unavailable before a selection |
| `p25.geo.sites`, `p25.geo.packs` | loaded counts |
| `p25.geo.error` | load or persistence failure |
| `p25.pack.state` | catalog/job completion or refusal reason |
| `p25.pack.count` | catalog entries |
| `p25.pack.progress` | received/expected bytes, 0..1 |
| `p25.pack.entries` | cached lines: state|name|tier|installed-version-or-absent|available-version |

`ls_p25_pack_entry(index, out)` returns cached per-entry metadata and installed/version
fields without any I/O. `p25_sites_status(out)` is the equivalent GPS snapshot API.
These headers are the screen integration seam. No new app registration is needed:
the existing P25 app and its owner/PROGRAM tune path remain authoritative.

## Bounds, tasks and costs

Up to 51 state/DC packs, **256 profile records total**, 16 controls per record, and
1 MiB per pack. A pack exceeding remaining aggregate capacity is skipped as a whole,
with a clear `p25.geo.error`; previous accepted packs survive. Directory iteration
order is not an ordering promise. Production datasets must fit these explicit bounds.

Host ABI measurement: profile 2256 bytes, database 577552 bytes (about 564 KiB), parser
scratch 2392 bytes. Steady PSRAM is one database plus roughly 65 KiB of catalog/cache
storage, about 2 KiB of enlarged value/action registries and 10 KiB of worker stacks.
Loading adds a second database and a 1 MiB input buffer; verification of a concurrently
downloaded pack adds another bounded database/input allocation. Worst overlapping
load/download staging can approach 4 MiB of PSRAM. No large internal-RAM data buffers.
Internal queue/TCB overhead and TLS working memory remain IDF-controlled.

Per frame: no added render calls, allocations or I/O. When read, GPS getters copy a small
cached snapshot and pack entry getters copy one entry; the aggregate text getter copies
at most 8 KiB. Per second while P25 is active: one existing GPS poll now visits up to 4096
controls with the shared haversine calculation and the existing 2 km hysteresis. No
steady-state allocation. The P25 owner applies tunes only when permitted by the existing
traffic/scan guards. `p25_sites` loads SD/NVS off the UI; `p25_packs` handles HTTPS and
installation off the UI. Both use priority 3 and PSRAM stacks, and NVS work uses the
existing cache-safe dispatcher. Worker CPU is proportional to bytes/records only during jobs.

Estimated firmware flash delta: **15-25 KiB**, mostly the two adapters, parser and strings;
no new TLS/crypto implementation. This is an estimate, not a firmware link measurement.
On-device CPU timing is still needed; host tests cannot establish P4 microseconds.

## Verification and remaining hardware checks

Added tests exercise pack transactional validation/IDs/rosters/capacity, multi-pack
nearest selection and hysteresis, signed-catalog refusal callbacks, digest/byte mismatch,
plus-tier refusal before network, NVS preference handling, owner handoff/pause/resume,
cached APIs, FAT replacement rollback, and real publisher signature verification with a
disposable test key (deleted after testing). `p25_program_adapter_check` compiles the
actual PROGRAM device adapter against host declarations. Existing assertions are untouched.
`P25PACK_PYTHON_EXECUTABLE` can select a Python with cryptography independently of the
suite's normal `Python3_EXECUTABLE` (which also needs Pillow/zstd for its existing tests).

No hardware, COM ports, deployment or commits were used. Hardware verification still needed:

1. Card containing two fictional state packs: boot, confirm counts and default ON;
   turn OFF, reboot, confirm OFF; turn ON and verify nearest-site choice across the state boundary.
2. Feed real GPS fixes around the radius plus 2 km margin, remove/stale the fix, restore it;
   confirm no retune during voice calls, Phase II ownership or scanning. Manually pick a
   control, confirm paused, invoke resume and confirm the nearest-site decision is reconsidered.
3. Refresh catalogs through primary and fallback hosts; reject an untrusted signature,
   modified pack hash, short/oversized transfer and invalid pack while retaining the old pack.
4. List a plus pack with no token and verify zero download request; set a test token,
   reboot, verify the header on both allowed hosts and refusal of token-bearing redirects.
5. Update an existing FAT pack, interrupt power between renames, verify backup recovery;
   remove a pack and confirm the next owner handoff drops it from loaded counts.
6. Measure P25 owner latency at 4096 controls, RF/audio continuity during TLS/SD work,
   PSRAM/internal/DMA minima, stack high-water marks and final firmware flash delta.
