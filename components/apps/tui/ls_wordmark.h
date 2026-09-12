

#ifndef LS_WORDMARK_H
#define LS_WORDMARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_tui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many rows tall every block letter is. */
#define LS_WORDMARK_ROWS 5

int ls_wordmark_width(const char *text);

/* Split a wordmark that will not fit into two lines at a space. */

bool ls_wordmark_split(const char *text, int cols,
                       char *a, size_t a_cap, char *b, size_t b_cap);

/* Draw row `row` (0..4) of `text` with its left edge at `x`, so a caller can
   grade the colour down the wordmark instead of painting it flat. */
void ls_wordmark_row(tui_surface *sf, tui_rect clip, int x, int y, int row,
                     const char *text, uint8_t attr);

#ifdef __cplusplus
}
#endif

#endif /* LS_WORDMARK_H */
