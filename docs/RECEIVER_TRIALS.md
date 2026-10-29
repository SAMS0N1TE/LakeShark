# Manual receiver before / after checks

DIAG → ENDPOINT offers a BEFORE / AFTER TRIAL page. `B` saves a baseline,
`C` compares a new snapshot, and `X` discards the pair. The trial page also
offers touch buttons. These actions read metadata only: they never start,
stop, tune, transmit, or record a receiver. Reopening DIAG retains the pair
until another baseline, reset, or reboot.

The console exposes the same saved pair:

```text
receiver-trial baseline
receiver-trial compare
receiver-trial status
receiver-trial json
receiver-trial reset
```

Save a baseline while your existing receiver is active, wait for additional
activity, then compare within two minutes. Change receiver settings yourself.
A changed setting or context rejects that comparison; save a new baseline
after the change and compare again. This supports repeating a controlled
check, but does not rank settings or estimate reception quality.

The report separates settings changed, settings unknown, counter epoch
changed, reset-or-wrap, stale snapshots, inactive receivers, and invalid or
reversed uptime. Unknown values are `?` in text and `null` in JSON; a known
zero delta remains zero. No modular counter subtraction is used. The sampled
current snapshot must be at most three seconds old. Snapshot time is monotonic
uptime in the current boot, with no cross-boot identity or wall-time RF join.

Comparable context requires the same app, receiver source/endpoint/owner,
mode, requested and effective tune, applied sample rate, bandwidth, requested
and effective gain, endpoint control epoch, and consumer segment epoch.
The endpoint epoch changes on control attempts, restart, release, and
registration; even a tune away and back invalidates the old baseline.
Pending controls, epoch exhaustion, missing fields, and a counter decrease
make the affected comparison unavailable. A failed control may have unknown
device side effects, so it also invalidates the previous context.

FM external-IQ segments currently publish the consumer epoch needed for IQ
byte deltas. Other paths leave this evidence unknown. Decoder reset epochs
are currently unavailable, so decoder deltas remain unknown even if native
cumulative counts exist. Pager sync counts and saved captures are not
promoted into validated RF frames. An endpoint byte delta describes bytes
returned by the endpoint, not DSP coverage or sample continuity.

`receiver-trial json` emits one `receiver_trial` object with
`schema_version: 1`, the exact before/after pair, comparison flags, masks and
nullable deltas. It can be saved and passed as an explicit receipt to
`tools/observation_catalog.py --receipt <file>`. The catalog preserves its
raw bytes and source identity. It does not join receipt uptime to files by
guessing timestamps. The pair is a saved observation, not a live status.

Changed/unknown masks use bits: app=1, source=2, mode=4, tune=8, rate=16,
config/gain=32, context=64, IQ epoch=128, decoder epoch=256. Counter delta and
reset masks use IQ=1, frame=2, valid=4, failed=8, control=16, voice=32,
audio drop=64. Each delta needs a known matching epoch and nondecreasing
counter in addition to comparable context.

RF coverage, calibration, signal direction, and reception confidence remain
unmeasured. Host tests and synthetic receipts establish software behavior;
they do not establish real RF reception.
