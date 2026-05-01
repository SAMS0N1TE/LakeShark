# SAM (Software Automatic Mouth) — vendored source notice

The files in `main/sam/` are a lightly modified copy of the C port of SAM
maintained at https://github.com/vidarh/SAM, which is itself a refactor of
the earlier semi-automatic C translation by Stefan Macke
(https://github.com/s-macke/SAM).

## Origin and copyright

SAM (Software Automatic Mouth) is a formant speech synthesizer originally
published in 1982 by Don't Ask Software for the Commodore 64. The 6502
assembly was later disassembled and converted to C. The current copyright
holder is, on paper, SoftVoice, Inc. (http://www.text2speech.com).

The upstream C ports (s-macke, vidarh, discordier, Monczak, and several
others) all note that attempts to contact SoftVoice have gone unanswered
for over a decade and that the software is effectively abandonware. None
of those ports can place the code under a specific open-source license
for that reason.

## What's vendored here

- `sam.c`, `sam.h`, `render.c`, `render.h`, `reciter.c`, `reciter.h`,
  `processframes.c`, `createtransitions.c`, and the `*Tabs.h` lookup
  tables, as committed to vidarh/SAM at the time of vendoring.
- Our own `debug.c` stubs replacing the upstream printf-heavy debug
  hooks — on an embedded target we don't want the binary size or the
  stdout chatter.
- A one-line patch to `sam.c` replacing the internal `malloc(22050*10)`
  with a caller-provided buffer (see `sam_set_buffer()`), so the output
  buffer can live in PSRAM instead of being leaked on every utterance.

## Modifications

1. `sam.c`:
   - Added `sam_set_buffer(char *)` to accept an externally owned buffer.
   - Removed the `malloc` inside `Init()`.
2. `sam.h`:
   - Declared `sam_set_buffer` and wrapped in `extern "C"` guards.
3. `debug.c`:
   - Rewritten as empty stubs for the three `Print*` functions and a
     definition of `int debug = 0`.

Everything else is unchanged.

## Licence status

Same as upstream: unclear. Given the age of the original, the
unresponsiveness of the rights holder, and the many public forks that
have existed for 15+ years without challenge, using this in a personal
hobby firmware is almost certainly fine. If this project is ever
distributed commercially, replace SAM with a clearly-licensed alternative
(e.g. Espressif's `esp-sr` TTS, Flite, or a small cloud TTS).
