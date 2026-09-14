# GPS scanning and list imports

P25 and FM > SCAN > CHANNEL LIST now offers MIXED AUDIO and GPS FILTER.
START scans the saved list; HOLD keeps the selected channel and NEXT releases
it. Each entry selects conventional Phase I P25 or analog FM. The decoder changes
in the background while the scanner screen stays open. Phase II stays off.

GPS FILTER uses channel coverage circles. A channel enters at its configured
radius and leaves beyond that radius plus 10% (at least 1 km). An actual position
fix must be recent; satellite/status sentences do not refresh it. After GPS loss,
the last shortlist remains usable for 30 seconds. Scanning then pauses between
calls until another fix arrives. A held call is not interrupted by GPS loss or
a boundary crossing. Channels without coordinates/radius are excluded in GPS mode.
Coverage circles describe where to consider a channel, not measured reception.

The two options survive restart; START is still explicit. BAND SCAN uses the
selected decoder and turns GPS filtering off. Mixed list scanning involves a
decoder handoff, so crossing between P25 and FM is slower than a same-mode retune.
Group channels by audio mode when fast scanning matters. Cross-mode priority
checks do not interrupt an active call.

## Import API

`bench/tools/scan_import.py` accepts ordinary CSV or JSON. Frequency is MHz in a
`Frequency` CSV column, or integer Hz in `frequency_hz`. Supported mode names:
`P25`, `NFM`, `FMN`, `FM`. FM entries use the analog voice receiver. Coordinates
and radius can be per-channel fields or explicit command-line defaults.

```json
{"channels": [
  {"name": "Local P25", "frequency_hz": 154785000, "mode": "P25",
   "latitude": 43.44, "longitude": -71.65, "radius_km": 25, "zone": 0},
  {"name": "Local FM", "frequency_hz": 154400000, "mode": "NFM",
   "latitude": 43.44, "longitude": -71.65, "radius_km": 25, "zone": 0}
]}
```

The example illustrates the format; it is not a verified local frequency list.

```powershell
python bench/tools/scan_import.py --input my-channels.json --output nearby.lscan
python bench/tools/scan_import.py --input my-channels.json --output nearby.lscan --port COM13
```

The optional USB installation requires pyserial and the actual P4 port. It stages
the complete list, validates every entry, then replaces the saved list with one
commit and stops scanning. A rejected record invalidates the whole upload. Missing
acknowledgement after commit is uncertain: inspect `ch dump` before retrying.
Use `ch dump` and `ch list` to back up entries and enabled/lockout state first.

Alternatively copy the file to SD and run `ch load /sdcard/nearby.lscan`.
The bounded format is `LSCAN1` followed by one line per channel:

```
name|frequency_hz|P25-or-NFM|zone|latitude|longitude|radius_km|priority
```

Limits: 16,384 channels on SD (64 without SD), 15-character names, zones 0–7, radius 0–500 km. Existing
version-1 channel lists migrate with their names, flags and zones intact. Zero
radius is accepted on-device for old/unlocated entries, but the computer importer
requires explicit coverage. The `.report.json` beside the output preserves the
normalized entries and lists unsupported records skipped by the RR adapter.

Console integrations can use `ch stage`, `ch row "<line>"`, `ch commit`,
`ch abort`, `ch dump`, `scan mixed on`, `scan gps on`, and `scan on`.
An incomplete staged upload expires after 60 seconds of inactivity.

## RadioReference

RadioReference provides a SOAP database API for approved radio-programming
applications. Each user needs their own Premium subscription and credentials;
LakeShark also needs an approved application key. Apply through
https://www.radioreference.com/account/api/apply . Requirements:
https://support.radioreference.com/hc/en-us/articles/18844460198932-Database-Web-Service-API

```powershell
python bench/tools/scan_import.py --rr-county COUNTY_ID --rr-subcat SUBCATEGORY_ID --radius-km 25 --allow-unfiltered --output nearby.lscan
```

Repeat `--rr-subcat` to select several categories. The tool prompts for credentials
without echoing the password/key; environment variables `RR_USERNAME`,
`RR_PASSWORD`, and `RR_APP_KEY` are also supported. Credentials stay on the
computer and are never written into the scan list or report. Requests use HTTPS
and refuse redirects. No RadioReference database is bundled or shared.

This adapter imports conventional county subcategories, not trunked-site traffic
frequencies. It uses an explicit radius around database category coordinates;
county-wide agency discovery and automatic trunked-site roaming are not yet wired
into this path. Existing P25 PROGRAM profiles remain the route for trunked systems.
PL/DPL/CTCSS and NAC receive filtering are not implemented here: imports carrying
those settings require `--allow-unfiltered`, rather than silently claiming filters
are active. Encrypted, Phase II, DMR and other unsupported modes are rejected or
listed as skipped. Live RR authentication requires an approved key and account;
only schema/fixture tests can run without them.

## SD collections

`ch caps` reports the channel capacity and mounted-card state. Serial `ch stage`, acknowledged `ch row` commands, and `ch commit` replace the active list. The active collection is persisted to `/sdcard/channels/active.bin` and restored at boot. `ch save <name>` keeps a named copy; `ch profiles` lists stored names, and `ch profile <name>` activates one with scanning stopped. Names use up to 32 letters, digits, hyphens or underscores; `active` is reserved. `ch load /sdcard/...` accepts LSCAN1 text up to 4 MB.

Files carry a format header and payload checksum. Writes use a temporary file and retain the previous save as `.bak`; a missing or corrupt current file falls back to that backup. Without a mounted card, only the legacy 64-channel NVS list can be saved. Selected profiles are read into PSRAM; scanning does not read SD for each hop. GPS and session-skip bitsets cover every channel. Large unfiltered sweeps are slow, so use local profiles or verified coverage radii.

Ground Station includes a Franklin, NH two-channel preset: 154.785 MHz P25 police and 159.900 MHz analog NFM Lakes Region fire dispatch. Run `scan mixed on`, `scan gps off`, `ch zone all`, select P25, then `scan on`. The receiver alternates modes; it does not hear two frequencies simultaneously.

During mixed scanning, analog samples reach the speaker only after the scanner accepts a channel. Decoder handoffs re-prime the audio queue without reconfiguring I2S. Status uses the running decoder and confirmed tuner frequency. Manual gain may be needed: automatic gain can raise a quiet-channel noise floor above analog squelch. These are conventional channels, not a trunked-system talkgroup configuration.
