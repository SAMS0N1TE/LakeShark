/* A 2x2 patch of ink, drawn as a stroke instead of as a block. */

#ifndef LS_STROKE_H
#define LS_STROKE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stroke for one cell's four sub-pixels. */

char ls_stroke_glyph(bool tl, bool tr, bool bl, bool br);

#ifdef __cplusplus
}
#endif

#endif /* LS_STROKE_H */
