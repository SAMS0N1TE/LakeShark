# Adding a board to LakeShark

The rule this whole layer exists to enforce:

> **A new board is never a new branch.**

Board support is one header. If you find yourself branching, or adding
`#ifdef CONFIG_LS_BOARD_<yours>` somewhere in the radio or DSP code, something
has gone wrong - read "When no capability fits" below.

## The layer

```
components/lakeshark/board/
├─ ls_board.h            selects a variant, then fills in shared ESP32-P4 pins
├─ ls_caps.h             LS_HAS_* capabilities - all DERIVED, never hand-set
├─ TEMPLATE.h            copy this to add a board
└─ variants/
   ├─ p4_nano.h          physical facts only: pins, panel, flash size
   ├─ p4_wifi6.h
   ├─ p4_touch_lcd_4b.h
   └─ p4_touch_lcd_43.h
```

Three kinds of thing, kept apart on purpose:

| | Where | Example |
|---|---|---|
| **Board identity** | `ls_board.h` | which variant header gets included |
| **Physical facts** | `variants/*.h` | `LS_BOARD_VBUS_EN_GPIO 46` |
| **Capabilities** | `ls_caps.h` | `LS_HAS_VBUS_CTRL` |

Capabilities are derived from facts. A board gains one by declaring the pin
that provides it and loses one by omitting it. No variant may `#define` an
`LS_HAS_*` itself - CI rejects that.

## Steps

1. **Copy the template.**
   `cp board/TEMPLATE.h board/variants/my_board.h` and fill it in. Declare
   only what the board physically has. Omit the display block on a headless
   board; omitting `LS_BOARD_LCD_H_RES` is what makes `LS_HAS_DISPLAY` 0.

2. **Add the Kconfig choice** in `main/Kconfig.projbuild`, next to the others:

   ```
   config LS_BOARD_MY_BOARD
       bool "My board (32MB)"
   ```

3. **Add the include arm** in `board/ls_board.h`:

   ```c
   #elif defined(CONFIG_LS_BOARD_MY_BOARD)
   #include "variants/my_board.h"
   #define LS_BOARD_SELECTED 1
   ```

4. **Add `boards/my_board.defaults`** - flash size, partition table, and
   whether this is a headless or GUI build. Copy the closest existing one.

5. **Add it to the CI matrix** in `.github/workflows/build-matrix.yml`.

Nothing else should need to change. If it does, that is the bug.

## Never guess a pin

A wrong GPIO is worse than an absent feature: it can drive a pin that is
something else entirely on that board and damage hardware. This is not
hypothetical here - `LS-904` records that GPIO46 is the USB VBUS enable on the
NANO and the **audio PA enable** on the Touch-LCD-4.3. Guessing it there would
have driven the amplifier every time the firmware tried to power-cycle a
dongle.

If you do not know a pin, leave it out and say so in a comment. An absent
capability is a working build with one less feature. A wrong pin is a support
thread.

## When no capability fits

Add one. Derive it in `ls_caps.h` from a fact the variant already declares:

```c
#if defined(LS_BOARD_MY_THING_GPIO) && (LS_BOARD_MY_THING_GPIO >= 0)
#define LS_HAS_MY_THING 1
#else
#define LS_HAS_MY_THING 0
#endif
```

Then test `LS_HAS_MY_THING` in the code. What you must not do is test the
board name outside `board/`. That is the difference between board five costing
one header and costing edits in ten files.

## Hardware you do not have

You can get a long way without the board:

- **It has to build.** `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/my_board.defaults" build`
  catches most pin-map mistakes, and CI will run it for every board on every
  push so your board does not quietly rot.
- **The boot log states the profile.** Every boot prints `board profile: <name>`
  and the VBUS pin, precisely because nothing at runtime can detect a wrong
  profile - a misconfigured pin just misbehaves quietly. If no
  `CONFIG_LS_BOARD_*` was set at all, the build falls back to the NANO map and
  the log says so in capitals. Read that line before believing any USB-power
  or audio symptom.
- **Mark unverified values.** `p4_touch_lcd_43.h` was written from a product
  page with the board not in hand, and says so. That comment is why the later
  VBUS finding was cheap to fix rather than confusing.

## A capability is not a build option

`LS_HAS_DISPLAY` means *this board has a panel*. It does not mean *this build
draws a GUI* - a headless build on a board that has a panel is a legitimate
configuration, which is why `CONFIG_LAKESHARK_HEADLESS` still exists. Use
`LS_USE_DISPLAY` (`LS_HAS_DISPLAY && !headless`) for the build question.
Keeping the two apart is what stops "headless" from becoming a fork again.
