# GPS scanning and list imports

In P25, open SCAN. In FM, open SCAN on the radio panel or SCANNER from MORE.
Choose CHANNEL LIST for MIXED AUDIO and GPS FILTER.
START scans the saved list; HOLD keeps the selected channel. RESUME releases
the hold; NEXT advances. Each entry selects conventional Phase I P25 or analog FM. The decoder changes
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
  {"name": "Example P25", "frequency_hz": 154785000, "mode": "P25",
   "latitude": 0.0, "longitude": 0.0, "radius_km": 25, "zone": 0},
  {"name": "Example FM", "frequency_hz": 154400000, "mode": "NFM",
   "latitude": 0.0, "longitude": 0.0, "radius_km": 25, "zone": 0}
]}
```

The example uses placeholder coordinates. Replace the entries with your own
verified frequencies and coverage. It is not a local frequency list.

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

Limits: 16,384 channels on SD (64 without SD), 15-character names, zones 0-7, radius 0-500 km. Existing
version-1 channel lists migrate with their names, flags and zones intact. Zero
radius is accepted on-device for old/unlocated entries, but the computer importer
requires explicit coverage. The `.report.json` beside the output preserves the
normalized entries and lists unsupported records skipped by the RR adapter.

Console integrations can use `ch stage`, `ch row "<line>"`, `ch commit`,
`ch abort`, `ch dump`, `scan mixed on`, `scan gps on`, and `scan on`.
An incomplete staged upload expires after 60 seconds of inactivity.

## RadioReference

The importer has a RadioReference SOAP adapter. It requires your own account
credentials and an application key. Check the provider's current access
requirements before using it. The adapter does not include a database.

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
into this path. Load a P25 profile in SETTINGS for trunked systems.
The importer does not apply PL/DPL/CTCSS or NAC filters. Records carrying those
settings require `--allow-unfiltered` to accept carrier squelch and any NAC.
This import limit is separate from FM > OPTIONS > TONE, which can set CTCSS
or DCS for a saved NFM channel. Encrypted, Phase II, DMR and other unsupported modes are rejected or
listed as skipped. Live authentication requires valid credentials and a key.

## SD collections

`ch caps` reports the channel capacity and mounted-card state. Serial `ch stage`, acknowledged `ch row` commands, and `ch commit` replace the active list. The active collection is persisted to `/sdcard/channels/active.bin` and restored at boot. `ch save <name>` keeps a named copy; `ch profiles` lists stored names, and `ch profile <name>` activates one with scanning stopped. Names use up to 32 letters, digits, hyphens or underscores; `active` is reserved. `ch load /sdcard/...` accepts LSCAN1 text up to 4 MB.

Saved lists have a checksum. A failed or corrupt current file can fall back to
the previous `.bak` save. Without a mounted card, only 64 channels can be saved.
Large unfiltered sweeps are slow. Use local profiles or verified coverage radii.

During mixed scanning, FM reaches the speaker only after the scanner accepts a
channel. Status shows the running decoder and confirmed tuner frequency. The
receiver alternates modes; it does not hear two frequencies at once.

CALLS can save clear P25 and squelched FM while scanning. Set Record P25,
Record FM and Keep newest days in CALLS > OPTIONS. See
[Updates and calls](TDP4_UPDATES_AND_CALLS.md) for card use and file sizes.
