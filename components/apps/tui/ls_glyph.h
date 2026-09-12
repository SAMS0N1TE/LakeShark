/* A character drawn at the size of the thing it is written on. */

#ifndef LS_GLYPH_H
#define LS_GLYPH_H

#include <stdbool.h>
#include <stdint.h>

#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_GLYPH_COLS 3
#define LS_GLYPH_ROWS 5

/* True when `c` has a stroke figure: 0-9 and A-Z, either case. Anything else
   - punctuation, a space - has to be drawn as ordinary text. */
bool ls_glyph_has(char c);

void ls_glyph_draw(tui_surface *sf, tui_rect a, char c, uint8_t at);

#ifdef __cplusplus
}
#endif

#endif /* LS_GLYPH_H */
