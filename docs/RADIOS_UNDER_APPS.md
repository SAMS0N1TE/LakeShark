# Radios under user apps: plan

Status, 2026-10-23: steps 1 and 2 are in `next` and checked on the LR board.
Steps 3 and 4 are open.

## The problem

A big radio app keeps running after you leave its screen. P25 does this on
purpose, so trunk calls keep being followed and recorded (`call_sd`). On the
board, measured with `tui cost` and `heap`:

| | DICE draw | frame every | CPU |
|---|---|---|---|
| DICE alone | 11 ms | 40 ms (the screen's cap) | - |
| DICE with P25 decoding in the background | 47-89 ms | 68-130 ms | core 0 75%, core 1 95% |

The TUI task and `p25_rx` share core 1, and there is no spare CPU on core 0
to move to. A user app therefore gets a third of its frame rate or less.
Making that go away while P25 keeps decoding would mean making the P25 DSP
much cheaper, which is a project of its own.

Memory was the other half. With P25 decoding, DRAM has about 7 KB free in
pieces of 4 KB, so the 6 KB stack an app is linked on could not be found,
and before that it landed in TCM and tripped an IDF assert. That part is
fixed in `next`: the stack is 2 KB and comes from the LP RAM first
(commits e2c88a73 and 7eb0d7e3). Apps now open with P25 running; they are
only slow.

## The rule (owner's call)

Big radio apps and user apps do not run at the same time. Opening a user app
stops the background radio work; it should come back when the app closes.
Later, users should be able to choose for themselves.

## Steps

1. **Park the receiver while a user app is open** (done, `components/apps/tui/ls_lap.c`).
   Choosing an app parks the receiver mode HOME was keeping (P25, FM, ADS-B,
   REC), whichever radio it runs on, and leaving the app's screen asks for it
   again; a screen switched to after that asks for its own, which wins. An app
   that names a radio in its manifest keeps asking for that one. MeshCore is
   not a receiver mode and keeps running. Checked on the LR board (P25 on the
   LR2021, searching): HOME with P25 behind it 57% / 90% (core 0 / 1), DICE
   open 12% / 34% and 11 ms a draw, back at HOME 57% / 90% with P25 running.
2. **Say so on screen** (done). For four seconds after an app opens its top
   line reads "P25 PAUSED UNTIL THIS APP CLOSES".
3. **Let users choose** (the "eventually" part). A setting in SET or RADIOS:
   *Radios under apps: pause (default) / keep running.* Keep-running is for
   someone who wants the call recorder going while they play, and accepts
   the frame rate. Possibly per app later, but one switch first.
4. **Measure each big radio app's cost.** Do the same `tui cost` and `heap`
   check with ADS-B, FM, FALLS and SUB-GHZ in the background, to know which
   ones matter. The rule may only need to cover the heavy ones.

## Not in scope here

- Running two big radio apps at once (P25 and ADS-B both want the RTL-SDR;
  the endpoint lease already settles that).
- Speeding up the P25 DSP.
