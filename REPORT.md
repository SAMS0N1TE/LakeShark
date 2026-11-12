# P25 trunk following, Manatee-Sarasota, 2026-11-08

Branch `work/p25-trunk-20261108` off `work/nano-pins-20261106` (806b3174).
LR2021 T-Display-P4 (USB serial 5B91081095), control 855.3875, NAC 9D1,
WACN BEE00, SYSID 9DC. Built on Linux with IDF v5.5.4 + the six patches,
flashed by USB (app + otadata), NVS backed up before every flash.

## Cause of VOICE 0

Not the decoder. The control channel decoded cleanly (7275 TSBKs in 2.5 min,
0 CRC errors, IDEN table complete) and no grant was followed because of two
saved settings:

1. `sdr-tool:p25_follow = 0` in NVS: trunking auto-follow off. 353 of 353
   grants in the first minute were dropped as `auto-follow-off`.
2. An allow list (list mode ALLOW; TGs 10001 10003 12003 12053 11003 2049
   2081 2101 515) left by loading `p25_profile.txt` (Franklin NH - West
   Dennis MA). Loading a profile writes its roster and auto_follow to NVS
   and both outlive the profile. Every Florida grant was then dropped as
   `allow-list`.

The screen said CONTROL LOCKED throughout. With both off, Phase I calls
followed and played in the clear (algid 0x80) on the first grant.

All grants seen here were Phase I (FDMA IDENs 0, 1, 4). No Phase II grant was
seen, and the site sends no Motorola patch grants (vendor opcodes $00 $05 $09
$0b $16 only).

## What changed

- `80bce6f6` Each grant's outcome is traced. `p25 follow` prints the follower
  counters, list mode, outcome counts and the last 8 grants with the reason
  each was left. `p25 follow on|off`, `p25 follow list off|allow|none`. The
  SYNC row shows `SKIP: <reason>` for 15 s. Motorola MFID $90 0x02/0x03 patch
  grants are decoded, and `p25tsbk` lists vendor opcodes.
- `bc96e118` After a TDU/TDULC the follower waits 1.5 s on the voice channel
  (`p25 follow linger <ms>`, 0 = the old behaviour, not saved).

App image 0x462180 bytes, +4224 over rc3, 0x1de80 free.

- `2d08fea8` Talkgroup rosters (lockouts, hold, allow list, priorities)
  are kept per system (WACN + SYSID), each under its own NVS key. A system
  with none follows every call. A control channel that has not named itself
  holds to the roster for 10 s at most. A roster saved unscoped by an older
  build is set aside once a system names itself. Profile `wacn=`, `sysid=`
  and `nac=` accept hex (`BEE00`, `0x9DC`), which made a profile fail to load
  before. Readout: `P25FOLLOW roster=<sys> here=<sys> applies=yes|no`.

Roster scope on air: after the flash the old New England list was set aside
(`allowed=0`), and the roster bound to `BEE00-9DC` with calls following. Set
`list none`, rebooted: it came back as `list=none` on `BEE00-9DC` from that
system's key, while the old unscoped blob still said off. `list off` then
followed calls again. Another system's roster swap is covered by host tests
only, using an invented non-BEE00 system (WACN 7A1C3, SYSID 1F2).

## On air

| | before linger (25 min) | with linger (2.3 min) |
|---|---|---|
| follows | 3977 | 11 |
| retunes | 7957, ~116 ms each | 24 |
| over ends / caught on channel | - | 28 / 17 |

The owner reports it sounds really good, with occasional cuts before the
linger.

### Quality check (5 min on air, 1.5 s linger, after 25202320)

- Decode: 97-100% of frames passed (BCH), 0 dropped, 0 erased, ~4.5% of IMBE
  frames rough, across 47 P25QUAL calls.
- Audio cuts (silences inside an over that audio resumed from, new counter):
  6 in 5 min, worst 272 ms. The underrun counter cannot show this: it
  also counts every 16 ms chunk of the 400 ms drain after each over, which is
  why `quality.py analyze` flags every call "requires review".
- Encrypted frames: 5% of frames muted. Not why it seems quieter.
- Voice decoded per minute: 1644 frames/min over 125 min with no linger,
  1639 over 23 min with the 1.5 s linger. The linger does not lose audio.
  A quieter period on the system explains "less active" better.
- Next over on the same TG and channel, ms (caught on channel / re-granted):
  <250 1/0, <500 20/0, <750 2/0, <1000 2/0, <1500 9/0, <2500 1/4, none
  beyond. That is two groups: back-to-back keying at 250-500 ms and replies
  at 1-1.5 s. 1.5 s catches both, and only 4 follow-ups came later, so the
  linger stays at 1.5 s.
- `quality.py check`: index valid, the same 9 release blockers as before
  (none P25 trunking). `quality.py` unit tests: 11 errors (`package`
  NameError in the packaging tests), the same 11 on 806b3174.

### On the screen (815a965f)

P25 OPTIONS: Trunking auto-follow, Follow linger (ms) (saved), Last grant.
Talkgroup policy: This system, Follow on this system, the existing hold,
lockout and allow rows (which now act on the current system's list), and
Forget this system's list. `test_tui_p25_screen` drives each new row
through the real picker. A linger set to 1200 survived a reboot on the
board. `lssim` does not build on Linux (error in `lakeshark_app.h` via
lap3d), so the rows were not rendered in the simulator. The owner checked
them on the panel: they look fine.

## Host tests

The 8 P25 suites pass except `test_tui_p25_screen`
`trunking_failure_and_unresolved_grants_are_visible` (Phase II toggle at line
1324), which already failed before this work. Bench configure on Linux needs
`IDF_PATH` set; `bench/CMakeLists.txt` otherwise falls back to `C:/esp/...`.

## Unproven

- Phase II following and the LR2021 Phase II demod: no Phase II grants here.
  Lee County (858.7125) is the next site for that.
- Motorola patch-grant decode: correct in host tests, never seen on air.
- Whether the linger misses grants on other talkgroups during its 1.5 s.
- 6 grants `unresolved` (channel IDEN not announced).

## Open, owner's call

- Done in 25202320: a profile's `auto_follow=false` is now a follow-off
  flag on that system's roster, not the global switch.
- Talkgroup names (`/sdcard/p25_names.csv`) are still one table for all
  systems.
- Dual radio: split mode (LR2021 control, RTL voice) is off since 2.13.2
  (`3437f63b`), when P25 on the LR2021 went IQ only. It needs an LR IQ
  control decoder fed to `p25_split_tsdu`.
- On Linux every serial open resets the board, and `handoff/linux/lscon.py`
  does not prevent it. Hold one connection open instead.
