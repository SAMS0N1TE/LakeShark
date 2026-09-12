/* breezy_tui layout: ratatui-style rect splitting with constraints, plus
 * margin/centering helpers for dialogs. Pure math, no allocation, no
 * surface access — testable without a tty. */
#ifndef TUI_LAYOUT_H
#define TUI_LAYOUT_H

#include <stdint.h>

#include "tui_core.h"

typedef enum {
    TUI_DIR_HORIZ, /* split into columns (varies x/w) */
    TUI_DIR_VERT   /* split into rows    (varies y/h) */
} tui_dir;

typedef enum {
    TUI_CON_LEN, /* exactly n cells */
    TUI_CON_MIN, /* at least n cells; grows if no FILL wants the leftover */
    TUI_CON_PCT, /* p percent of the parent extent, floored */
    TUI_CON_FILL /* equal share of whatever is left */
} tui_constraint_kind;

typedef struct {
    uint8_t kind;   /* tui_constraint_kind */
    uint16_t value; /* n for LEN/MIN, p for PCT, unused for FILL */
} tui_constraint;

#define TUI_LEN(n) ((tui_constraint){TUI_CON_LEN, (uint16_t)(n)})
#define TUI_MIN(n) ((tui_constraint){TUI_CON_MIN, (uint16_t)(n)})
#define TUI_PCT(p) ((tui_constraint){TUI_CON_PCT, (uint16_t)(p)})
#define TUI_FILL   ((tui_constraint){TUI_CON_FILL, 0})

void tui_layout_split(tui_rect parent, tui_dir dir,
                      const tui_constraint *cons, int n, tui_rect *out);

/* Shrink r by n cells on every side (empty rect if it doesn't fit). */
tui_rect tui_rect_margin(tui_rect r, int n);

/* w x h rect centered in parent, clamped to parent's size (for dialogs). */
tui_rect tui_rect_center(tui_rect parent, int w, int h);

#endif /* TUI_LAYOUT_H */
