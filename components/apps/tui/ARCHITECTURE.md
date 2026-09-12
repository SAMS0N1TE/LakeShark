# LakeShark TUI

A character grid painted straight onto the panel. No LVGL.

## Why

Measured on the device, LVGL spent 23 ms per refresh rasterising in portrait
and 41 ms in landscape, against 6 ms to present. Drawing was four times the
cost of showing it, and no layout change touches that. A cell grid only
redraws cells that changed, has no anti-aliased geometry, no gradients and no
blending, and because the blitter writes transposed it needs no rotation pass
at all. Rotation alone was 19 ms a frame under LVGL.

An animated frame is 3.9 ms. A full repaint of every cell is 21 ms, which
still beats LVGL's typical case.

## Layers

Each layer knows only about the one below it. That is the whole discipline;
keep it and screens stay portable between boards and orientations.

```
  screens/main/     P25, FM, ADS-B, HOME      draw cells, handle keys, touch
  screens/ext/      FALLS and other tools
  (user apps)       ls_userapp, described in a file on the card
  ---------------------------------------------------------------------
  ls_app            the directory: identity, icon, category, live lamp
  ls_tui_screen     registry, router, chrome, help, rotation, touch routing
  ---------------------------------------------------------------------
  ls_waterfall      the one spectrum + waterfall instrument
  ls_wf_source      which receiver feeds it
  ls_tui_ui         big buttons, tiles, panels, meters, hit testing
  ls_icons          12x12 pixel art, packed into quadrant cells
  ls_anim           per-app opening animations
  ls_value          named read-only values, for user apps
  ls_tui_widgets    panels, lists, meters, bar graphs, fields
  ---------------------------------------------------------------------
  tuilib/tui_core   cells, rects, clipping, primitives   (MIT, vendored)
  ---------------------------------------------------------------------
  ls_tui            grids, diff, glyph blit, present
  ls_font           glyph tables and metrics
  ---------------------------------------------------------------------
  ls_panel          framebuffer, scanout
```

A screen must never touch `ls_panel` or a framebuffer pointer. If it needs
something the widget layer cannot express, the widget layer grows.

**A screen never owns a waterfall.** `ls_waterfall` is one instrument with one
history in PSRAM, one set of controls and one measured cost. A screen claims
it, pushes rows, and draws it - full size with `ls_wf_draw`, or as a panel
among others with `ls_wf_draw_mini`. Four separate waterfalls is what this
replaced, and three of them had the same bug fixed in only one place.

## Rules

**Screens draw, they do not own timing.** A screen's `draw` is called with a
surface and must be idempotent: same state in, same cells out. The router
decides when. That is what makes the diff work and what makes a screen
testable on the host with no panel.

**No pixel coordinates above `ls_tui`.** Cells only. A screen that computes a
pixel offset has broken the layer and will break again on a different font
size.

**One state read per frame, at the top of `draw`.** Do not interleave reads
with drawing; a value that changes mid-frame produces a screen that disagrees
with itself.

**Colour is free, motion is not.** The attribute byte is already in every
cell, so colour costs nothing. A cell that changes costs about 10 us, so an
animation's price is the number of cells it moves, not how it looks.

## Fonts

`ls_font_t` carries the glyph table and its metrics, so the grid size follows
from the font rather than being hardcoded. Generated from lv_font_conv output
by `tools/gen_font_mono.py`, which verifies the parsed byte count against what
the descriptors require - the first version trusted a regex and silently
dropped every single-digit hex byte, which corrupted every glyph after the
first few.

| font | cell | landscape grid | portrait grid |
|---|---|---|---|
| `ls_font_mono_14` | 8x15 | 144 x 31 | 66 x 78 |
| `ls_font_mono_16` | 10x17 | 115 x 27 | 52 x 68 |

Adding a size means running lv_font_conv for it and re-running the generator.
Nothing else changes.

## Orientation and touch

Landscape is the keyboard posture, portrait the one-handed touch posture. The
blitter transposes on write, so neither costs more than the other.

Touch maps pixel to cell and dispatches to whatever is under it. A screen
handles a tap on a cell the same way it handles a key, so it does not need to
know which happened. Portrait is therefore not a separate UI, it is the same
screens at a different grid size with taps instead of keys.

## Performance notes

Established by measurement, not assumption:

- The vsync wait was 30 ms a frame and bought nothing visible. Single
  framebuffer, no wait.
- Writing across the framebuffer stride rather than along it cost 26%. In
  landscape the contiguous axis is a column, not a row.
- Three integer divides per blended pixel cost more than the blend itself.
  Shift by 256, not divide by 255.
- Backgrounds fill two pixels per store; RGB565 means two fit in a word.
- LVGL's glyph lookup went through a function pointer into a cmap binary
  search per cell. A direct array index is worth about 6%.

`display bench` prints the per-phase cost, `tui` prints full-paint and
incremental costs on entry. Measure before and after; several plausible
optimisations in this file were tried and reverted because they lost.


## Touch and the keyboard are the same interface

Every control is drawn as a block of cells and hit-tested where it was drawn.
A screen with controls implements `touch` and resolves the tap against the
rects it just recorded; the router's fallback - top third UP, bottom third
DOWN, middle ENTER - exists for lists and is deliberately too crude for
anything else.

Every button also carries a character shortcut, drawn in its corner. That is
not decoration: it is how the two input methods teach each other, the same way
the tab strip prints the function-key number. A control reachable by only one
of them is a bug.

Chrome grows in portrait. The tab strip is one row in landscape and two in
portrait, and a control bar is two rows against four, because a 10x17 pixel
cell is about 0.8 mm and that is not a touch target. `ls_tui_is_wide()` is the
only thing that needs to know, and screens should prefer `ls_tui_split()`,
which already does.

## Openings

`ls_anim` plays about 400 ms of drawn animation when an app opens, built from
that app's own icon. It is decoration, it is bounded, and it never waits on
anything: an app is not ready because its opening finished, and nothing is
allowed to block on one. Any key or tap skips it.

The cost is why it is affordable at all - a frame touches a few dozen cells
out of the thousand on screen, so the diff renderer pushes a few dozen, and
the whole opening costs less than one full repaint.

## User apps

`ls_userapp` parses a `.lsapp` file from `/sdcard/apps` into a bounded model
and draws it with the same widgets a built-in screen uses. Parsing is pure and
transactional - a buffer in, a model out, whole file or nothing - so it runs
on the host bench exactly as it runs on the board.

A user app binds to `ls_value` paths, which is a read-only registry of the
handful of things worth naming. It has no way to express an action, which is
the entire reason it is safe to load a file off a card and draw it. See
`USER_APPS.md`.
