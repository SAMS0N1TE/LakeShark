# LCD4.3 troubleshooting

Applies only to the experimental LCD4.3 candidate. P25 voice is unverified for
build EF70D1. Do not treat a control-frame test as speech or trunking acceptance.

## Blank display, reboot or failed upgrade

Check stable power and the exact board variant first. Do not install a Nano or
LCD-4B image to test the display. An application-only package cannot repair an
unknown bootloader or partition layout.

If the console is available, start with `version`, `safemode` and `crash`.
Record the output before changing anything. Safe mode intentionally does not
start the receiver backend; its reduced console does not provide the normal
P25/audio commands.

If a normal startup repeatedly fails, `safemode on` arms recovery for the next
boot; `safemode reboot` restarts. After preserving evidence and addressing the
cause, `safemode normal` attempts normal startup again. Avoid repeated normal
boot attempts that might replace crash evidence.

For an interrupted application upgrade, recover through the board's documented
download procedure using the previous compatible application and verified
manifest. If the partition layout or security configuration is uncertain,
stop and request a board-specific recovery procedure. Do not erase the full
chip, overwrite data partitions or guess a recovery-button sequence.

## P25 is quiet

Check these in order:

1. **Active signal:** verify a transmission is actually present. Check antenna,
   RTL attachment, SIGNAL activity and HEALTH. A control channel need not carry
   voice. Signal strength alone does not prove valid P25 data.
2. **Applied tune and mode:** confirm the tune completed and the demodulation
   mode is appropriate. Try modest gain changes or AGC; excessive gain can
   worsen decoding. A failed/pending request is not an effective retune.
3. **Audio:** inspect volume and MUTE. Console `vol` reports both without
   changing them. `mute` toggles state-it is not a query. Start at a comfortable
   volume, not maximum.
4. **Metadata and encryption:** `p25 acquisition` reports acquisition/tuning/IQ
   state; `p25tsbk` reports accumulated control metadata; `p25enc` reports the
   encryption gate. Unknown encryption metadata or encrypted calls are muted
   intentionally. Do not disable safeguards to manufacture audio.
5. **Voice quality gate:** `p25gate` reports its current value. Lower thresholds
   suppress more damaged frames. This does not affect initial synchronization.

The audio buffer adds startup delay, and the first valid encryption metadata
can arrive after joining a call. Voice counters may include PCM that was
silenced by the error gate; they do not prove audible speech. An
`imbe` allocation-failure message means the decoder could not obtain memory,
not that speech was successfully decoded.

## Wi-Fi, clock or HOME looks stale

Read the Wi-Fi status/result text, not just the selected network. **SAVED** is
a reconnect request, not proof of connection. Check password entry and C6
status; keep credentials out of shared logs. A CLOCK waiting-for-sync message
means valid wall-clock time is unavailable. HOME's RECEIVER card is explicitly
last-used state: reopen the receiver for live reception.

## Preserve a crash

`crash` reports bounded raw metadata and preserves the stored dump. It does not
decode the ELF on-device or certify that a checksum/header is valid.

Export the **full coredump partition** with a host flash reader, using its
reported address/size and a verified board connection. Keep that raw backup
and the ELF matching the firmware that crashed-not merely the newest ELF-for
offline decoding. A later unrelated panic can replace the on-device dump.

`crash clear` is destructive. Use it only after a verified backup and a deliberate
decision to erase the evidence. Do not clear automatically during an upgrade.
Share a sanitized summary, firmware version, steps and relevant counters;
keep raw dumps, credentials and private radio/location identifiers private.
