# Offline observation catalog

This is a **host-side** read-only SD-card catalog and export, not a new device
screen. It connects the existing stores in one searchable offline HTML view
and a JSONL manifest. It does not tune radios, start recording, infer links
between observations, or measure coverage. No network or third-party Python
packages are needed. Use Python 3.10 or newer and a quiescent card/local copy.

```powershell
python tools/observation_catalog.py E:/ --output C:/captures/investigation-01
python tools/observation_catalog.py E:/ --output C:/captures/investigation-02 --sigmf
```

The output parent must exist. The output directory must be new and outside the
source root. Open `index.html`; search by channel, frequency, native ID or
available provenance, and filter by store. `catalog.jsonl` contains the records;
`summary.json` contains limits and every retained input issue. Exit 0 means no
input issues detected, 2 means a partial export was produced, and 1 means export
failed. A directory is published only after all output files have been written.
Source files are never modified. By default artifact links refer to their original local
card/copy, so keep that location available. `--bundle` explicitly opts into bounded
source copies as described below. Sharing the output shares any location/identifier information
already retained in those stores; check it before distribution.

## Supported stores and identities

* `lakeshark/scan/*.jsonl`: scanner schema 1. Native session and sequence are
  scoped to the relative source file because the record has no boot ID.
* `detector/events/*.jsonl`: detector version 1. Boot and sequence identify the
  native event; exact duplicate records across rotation retain all locators.
  Conflicting records with the same tuple remain separate and raise an issue.
  Validated-message status remains separate from activity.
* `lakeshark/calls/*/*.wav.meta`: the existing nine-field CSV sidecar, plus a
  reference to its WAV. The `source` field is retained as its numeric unit ID;
  it is not a radio name. WAV contents are not decoded or integrity-verified.
* `lakeshark/*.sub`, `subghz/*.sub`, `lakeshark/rec/*.sub` and their available
  `.json` sidecars: legacy REC/native IQ, watch exports and CATCH references, existing
  tuning and available radio/board/firmware fields. Unsupported or missing
  sidecars are reported. No arbitrary recursive walk of music/maps is made.

IDs combine source namespace/scope, native identity and SHA-256 of the original
record bytes. Scanner IDs include relative file provenance. Identical detector
records keep the same ID through rotation; conflicting versions cannot overwrite
one another. These are catalog identities, not a newly invented global RF event
ID. `relations` is empty and explicitly unknown. No timestamp-based joins or
transmitter locations are inferred.

Unix time zero/null stays unknown. Monotonic values and IDs are strings where
needed to preserve integers beyond browser precision; their source units and
boot scope are explicit. GPS coordinates are omitted when validity is false.
Scanner mode numbers are preserved; absent receiver/mode/build information is
null. REC sidecar time is retained as source text, not promoted to exact sample
time (the IQ writer creates it after capture).

## Bounded native IQ to SigMF

`--sigmf` adds a byte-preserving conversion only for the existing version 1
`LakeShark SubGhz IQ File`, `Protocol: LR2021_IQ`, with a matching LR2021 sidecar.
The strict producer layout supplies frequency, nominal 125000 sample pairs/s,
and 1–32767 pairs. Every hex line and the complete sample count are validated.
Signed little-endian I then Q is established by this version's firmware
`rec_lr_iq_save/load` and `iq_consume`, not guessed from a file extension or
hardware name. Other versions/rates, pulse captures, audio, arbitrary external
IQ and mismatched metadata are not converted.

The `.sigmf-data` bytes are unchanged (`ci16_le`). The accompanying metadata uses
[SigMF core 1.2.6](https://sigmf.org/), nominal rate, center frequency,
`sample_start: 0`, a SHA-512 data digest and a source/catalog reference. No UTC,
geolocation, calibrated power, exact physical timing, or independently verified
continuity is invented. Sample rate is nominal and not ppm calibrated. A hash
establishes byte integrity, not decoder correctness or RF acceptance. The
conversion neither transmits nor reruns a decoder.

## Bounds, errors and verification

Defaults cap 512 candidate files, 20000 directory entries, 4096 records, 8192
bytes per JSONL line, 1 MiB per REC/metadata file, 32 MiB of metadata input and
512 detailed issues. More issues are counted. Truncated/oversized/malformed or
unsupported records are omitted with an explicit issue; valid preceding records
remain available. Missing audio remains an issue on a retained sidecar record.
Symlinks/junctions are refused. Input changes detected during reading are
reported; use an offline copy for reproducibility. This is a bounded adapter
catalog, not an exhaustive inventory of every file/recording on a card.

Record/metadata SHA-256 hashes do not imply full source-file or large-artifact
hash verification. Output generation is host-only and introduces no firmware
RAM, scheduler, SD worker or display changes.

```powershell
python -B bench/tests/test_observation_catalog.py
pwsh -NoProfile -File bench/verify.ps1 -Level host -Filter observation_catalog
```

Tests use native schema fixtures plus identity collisions, rotation duplicates,
truncation, bounds, invalid fields, privacy, missing artifacts, symlink refusal,
HTML escaping and exact signed IQ byte conversion. No hardware/RF acceptance
is implied.

## Device observation receipt

The device console command `observations` (or `observations json`) emits a
bounded JSONL receipt from the snapshots existing workers already publish.
It is read-only: it does not retune, start a receiver, write to SD, walk
directories or start another task. Capture console output on the host if a
saved receipt is wanted; the command itself does not create a file.

The receipt contains the current scanner journal path/status and its written,
dropped and error counters; up to four recent detector events with native
sequence, revision, uptime and validated/test distinctions; and up to eight
recent CALLS WAV references with their existing time/frequency/unit/talkgroup,
PCM duration and artifact size. Per-file recorder drops remain unknown: the
native sidecar does not retain them. These are bounded RAM snapshots,
not a complete history or an SD inventory. A busy/uninitialized detector is
explicitly unavailable; no zero match count is invented. Subsystem snapshots
are not atomic together, so references may change while the command runs.
No relationship between a detector event, scanner hold and call is inferred.
The detector snapshot has no persistent boot ID/log locator, so those fields
remain null and its sequence is not presented as a global event ID.

GPS coordinates and payload text are deliberately omitted. Native artifact
paths and unit/talkgroup identifiers remain; review them before sharing.
SAME alert logs, REC and other stores are outside this first device receipt.
Use the host catalog above for the supported saved scanner/detector/call/REC
stores. Neither export reads or verifies audio capture contents.

The `fm_dsp_iq` record accounts only for the latest external-SDR FM segment:
complete U8 I/Q blocks actually passed through the selected DSP path. It
excludes partial/failed reads, the frequency sweep and the LR2021 path.
Counters reset when a new segment starts after a tune, mode/session change or
receiver error; the closed segment remains available until the next delivery.
Configured center frequency is a receiver setting, not proof of the RF
frequency of buffered samples. Nominal sample duration is delivered pairs
divided by configured nominal sample rate; it is explicitly **not elapsed
observation time or continuous RF coverage**. Sample continuity, lost-sample
counts and RF coverage remain null. Endpoint ownership/streaming is never
converted to observation credit. Counter saturation is explicit and makes
nominal duration unavailable.

Output is limited to 15 records of at most 768 bytes each; workspace is capped
at 14 KiB of PSRAM per invocation. Large counters/monotonic times are decimal
strings to retain integer precision. A native field that cannot fit safely
produces an explicit error record instead of truncated JSON. Allocation,
snapshot or output failure returns a nonzero command result.

## Saved receipts and portable capture bundles

Save only JSONL output from `observations json`, `observations mission json`,
`receiver-trial json`, `rx-plan json` or `rx-coverage json`
to a host file, then import it explicitly. Console banners/prompts are rejected
as malformed lines; valid preceding lines remain available. Mission schema 2
keeps mission ID/sequence, control boundaries, delivered sample counts, nominal
durations and overwrite/saturation evidence. Mission times remain scoped to the
current boot. The catalog scopes each saved receipt by its complete SHA-256 and
line number; it does not treat a mission ID or a detector sequence as a global
event ID, or connect receipt records to saved observations using timestamps.
Only explicit native scanner/CALLS paths can add artifact references.

Start a passive mission explicitly with `observations mission start <name>`
(1–31 printable ASCII bytes), inspect `observations mission status`, and stop
with `observations mission stop`. These commands do not start/tune a receiver.
Only one mission is active; the latest stopped receipt remains in RAM until
the next explicit start or reboot. The bounded 32-entry ledger reports
overwritten entries; neither retained deltas nor wall intervals claim RF
coverage. Default `observations`/`observations json` remain the schema 1 receipt.

`receiver-trial baseline`, `receiver-trial compare` and `receiver-trial json`
provide the separate diagnostic schema_version 1 receipt. Its saved before/after
snapshots retain source/endpoint, configuration, counter epochs, nullable
counters, context-change/reset/unknown masks and comparability flags. The host
checks native field types, known-mask/delta agreement and flags that prohibit
deltas; it does not treat a counter increase as RF proof. Native 64-bit JSON
integers become decimal strings in the normalized browser catalog while the
original receipt bytes remain unchanged. Calibration, direction and coverage
remain null; missing firmware identity stays unknown.

```powershell
python tools/observation_catalog.py E:/ --receipt C:/captures/observations.jsonl --receipt C:/captures/mission.jsonl --receipt C:/captures/trial.jsonl --bundle --sigmf --output C:/captures/case-01
python tools/observation_catalog.py --verify-bundle C:/captures/case-01
python tools/observation_catalog.py --reindex-bundle C:/captures/case-01 --sigmf --output C:/captures/case-01-reindexed
```

`--bundle` streams copies of source files encountered by the selected adapters
into `sources/card/<native-relative-path>` and saved receipts into
`sources/receipts/<sha256>.jsonl`. It preserves complete original bytes,
including malformed/truncated source data, within the copy bounds. Unsupported
and incomplete source data remain explicit issues. No card file is repaired,
rewritten or removed. The HTML view uses relative links to copied artifacts, so
the card and original receipt files can be removed after export.

`bundle.json` records source/native identities and scopes, known source
board/firmware/radio provenance, capture frequency/rate/settings when present,
catalog/conversion options and bounds, the host tool SHA-256, omitted sources,
and known unknowns. Every copied source and generated catalog/view/SigMF member
has a complete byte length and SHA-256. Unknown capture settings/builds remain
null; the host tool hash identifies the script used and does not supply it.
Keep that script revision if identical host analysis must be repeated.

Verification validates **all** manifest paths, roles, lengths and hashes before
reading any member, then streams each member to check its bytes. Relative path
traversal, absolute/drive paths, alternate streams, duplicate names and
symlink/junction references are refused. Reindexing first verifies integrity,
then reruns the existing catalog adapters using only listed source members and
creates a new portable bundle outside the original bundle. Unexpected files
are never imported as extra evidence. A partial source bundle stays partial;
reindexing cannot recover omitted input.

The opt-in copy limits are 1024 source files, 16 MiB per source and 64 MiB of
copied input. Generated files have separate bounds (32 MiB per file; verification
caps declared copied/generated input at 96 MiB total), and the manifest is capped
at 4 MiB. Receipts are limited to eight explicitly selected files, 64 KiB and
64 JSONL lines each; regular catalog line/record bounds still apply. A source
that grows/changes or exceeds a copy limit is omitted with an issue and a partial
manifest. The default export remains reference-only and does not hash large
artifacts. All export directories must be new; publication uses a staging
directory so an output failure does not expose an incomplete export.

Verification prints a JSON report. Exit 0 means listed bytes match with no
reported source omissions/issues, 2 means matching bytes but a partial source
bundle, and 1 means malformed manifest, missing/tampered members or another
failure. The HTML links the manifest and the limits/issues report for offline
inspection. Verify again before re-analysis or after transfer.

Re-analysis can repeat only the strict native LR2021 IQ-to-SigMF conversion
described above. This is **not decoder replay**: the existing P25 host replay
requires U8 IQ at 240000 Hz, while these LR2021 captures carry signed little
endian IQ at 125000 Hz. No resampling, new decoder or RF acceptance claim is
added. Matching hashes establish byte integrity relative to the manifest;
they do not authenticate the source/manifest or establish RF truth, coverage,
calibration, continuity, correct decoding or complete SD history.

## Automatic receive plans and measured software intervals

`rx-plan` schema 3 records a bounded receive-only plan for the already-running
RTL USB FM LISTEN path. A plan has at most eight steps, one to four repeats,
1000–30000 ms dwell per step, and a 120-second total budget including guard
budgets. Loading a plan starts no receiver. Starting it validates the existing
owned receiver context, queues automatic retunes, suppresses the FM auto-CALLS
sink during the run, and creates a paired software observation session. The
native recording-policy and suppression flags are retained in the receipts.
Manual changes cancel the plan; restoration is attempted only while the
original context remains valid. No plan operation transmits or writes settings.
The separate explicit preflight helper `rx-plan context` reads retained FM,
entry/saved/default LISTEN and CALLS settings. `rx-plan record-fm on|off` changes
only `CALL_OPT_FM` while parked, with no active plan/archive job; runtime state
is checked and persistence is queued, not confirmed. This deliberate setting
operation is separate from loading/running a plan. Restore the original value
after any temporary recording-policy change.

```text
rx-plan load survey 1 162400000:2000 162550000:2000
rx-plan show
rx-plan start
rx-plan json
rx-coverage json
```

Choose frequencies appropriate to the connected receiver. `rx-plan stop`
requests a controlled stop. `rx-coverage start|stop|status|json` also supports a
separate explicit software tracking session; it starts no radio. A separate
active tracking session must be stopped before starting a plan. Latest RAM
receipts are replaced by the next explicit session/plan and lost on reboot.

The host's **planned versus actual control** table shows each planned center
frequency/dwell alongside actual attempts, completions, first/last tune and
last dwell start/end. These are control events, not observation credit.
Interrupted/failed plans and restoration results remain visible. Current step
and repeat indices are zero-based. Clock-invalid plans keep raw native times
and display a limited-timeline warning. A missing/duplicate step or truncated
receipt is a partial result, never a complete-looking plan history.

Each step also retains independent actual delivery aggregates across all its
repeats: qualified U8 IQ bytes/blocks, summed successful read/DSP window time,
first/last delivery completion times and separately calculated nominal sample
duration when one nominal rate remains known. These bounded per-step totals
survive overwrite of the recent 32-interval timeline, so every planned frequency
can retain its actual delivery evidence at the end of a run. They do not fill
gaps, establish per-step loss, or paint a full dwell as observed. Saturated or
timing-invalid aggregate fields remain null, mixed/unknown nominal rates remain
null, per-step loss stays explicitly unknown, and `detail_truncated` records
the missing detailed history independently of the retained aggregates.

`rx-coverage` schema 4 reports **software observations** on the RTL FM path.
Its five elapsed software states are receiving, settling, busy, unavailable and
unknown. Receiving windows run from a provenance-qualified read's start to
successful DSP completion. Intervening gaps are unknown. The guard requires a
confirmed post-reset context plus at least 100 ms and two discarded blocks;
this is a software policy, not calibrated physical tuner settling.

The offline view shows the retained windows on a bounded timeline and an exact
microsecond table. Unreported spans use a dashed unknown pattern; no time after
`last_update_us` is inferred. Overwritten intervals, missing rows, saturation,
clock-invalid boundaries and input truncation produce prominent limitations.
Clock-invalid sessions retain raw endpoints without interval widths/durations.
Session state totals include native accounting whose detailed rows may already
have been overwritten. The view never reconstructs missing rows from totals.

Delivered bytes/blocks and nominal sample duration appear separately from
measured software intervals. Nominal samples divided by configured rate are
never converted into wall-time coverage. Configured frequency, rate, context
and stream epochs stay null when provenance is unknown. FM auto-CALLS sink
suppression is shown explicitly per interval and at the session snapshot; this
flag does not describe every possible recording path or change a global
recording preference. Native plan ID/step/repeat bindings are displayed only
as emitted; queued/restoration phases can have a known plan ID and unknown
step/repeat. Separate saved receipts are not joined by approximate times or
reused numeric IDs with no persistent boot identity.

Loss classes remain distinct: exact observed ring-rejected bytes, flush
discards, transfer/submission error **events**, provenance-rejected blocks and
settling discards. First/new transport counter epochs establish a baseline;
loss totals cover observed counter deltas only. An error event does not supply
a lost-byte count. Physical RF coverage, hardware loss bytes, sample continuity,
device timestamps and per-sample RF frequency remain null. Neither unknown nor
unavailable software time proves an RF signal was absent.

Import both receipts with the same explicit `--receipt` options used above.
Schema 3 exports at most nine lines of 1536 bytes with a 3 KiB PSRAM workspace;
schema 4 exports at most 33 lines of 3072 bytes and 64 KiB total using at most
8 KiB of PSRAM. The existing host bounds (64 lines, 64 KiB per receipt) apply.
Both formats preserve decimal strings for native integer precision. Portable
bundles and reindexing retain receipt order, record IDs, source bytes and their
hashes. Existing observations v1, passive mission v2 and receiver-trial v1
remain supported with their original unknown RF/wall-time fields.
