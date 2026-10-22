# Notification tone tuning

Copy `notification.json` to `/sdcard/audio/notification.json`, then reboot.
The sound worker reads it once, when RADIO KEY, ROGER or CHIRP first plays.
A missing, unreadable, oversized (over 511 bytes) or invalid file keeps the built-in defaults.
The selection itself is saved by the existing settings worker in NVS (`notify_snd`, schema 8).

Version 1 has exactly `version` followed by `presets`, with three numeric rows in
RADIO KEY, ROGER, CHIRP order. Each row contains nine integers:

| Column | Meaning | Allowed |
|---|---|---|
| 1, 2 | First blip start/end frequency, Hz | 300-3000 |
| 3 | First blip duration, ms | 10-150 |
| 4 | Silent gap, ms | 0-100 |
| 5, 6 | Second blip start/end frequency, Hz | 300-3000 |
| 7 | Second blip duration, ms | 0 (omit) or 10-150 |
| 8 | Attack ramp, ms | 1-5 |
| 9 | Release ramp, ms | 1-10 |

Attack plus release cannot exceed either active blip duration. Unknown keys,
extra rows/columns, partial numbers and trailing data are rejected. Keep the
field order above. The table replaces atomically only after all rows validate.
Sine synthesis stays in voice band; ramps soften keying edges. Output is capped
at 4800 peak PCM, with no gain boost, samples, noise tail or new worker/task.
The existing EQ, board volume and mute still apply.

Console: `snd test radiokey|roger|chirp` previews a preset without changing the
setting; `notify sound alert|radiokey|roger|chirp|off` saves the selection.
The old P25 diagnostic chirp remains available as `snd test p25chirp`.

UI integration is backend-only: `settings_notification_sound_names()` returns
the names, count and current index; `settings_cycle_notification_sound()` or
`settings_set_notification_sound()` queues a persistent edit and reports acceptance.
Getters are RAM-only. Existing quiet notification and DETECTOR policies stay quiet.
