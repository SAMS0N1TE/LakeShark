/* Glyph tables for the TUI. */

#ifndef LS_FONT_H
#define LS_FONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t bitmap_index;
    uint8_t  box_w, box_h;
    int8_t   ofs_x, ofs_y;
} ls_font_glyph_t;

typedef struct {
    const ls_font_glyph_t *dsc;
    const uint8_t         *bitmap;

    uint32_t               bitmap_len;
    uint8_t  first;       /* first codepoint, 0x20 for all current tables */
    uint8_t  count;
    uint8_t  cell_w;      /* whole pixels; every face here is monospaced */
    uint8_t  cell_h;
    uint8_t  base_line;   /* measured up from the bottom of the cell */
    const char *name;
} ls_font_t;

extern const ls_font_t ls_font_mono_14;   /*  9x16 - 131x33 landscape */
extern const ls_font_t ls_font_mono_16;   /* 10x17 - 115x27 landscape */

/* Glyph for a codepoint, or NULL when the font has none or it is blank.
   A blank glyph is not an error: space has no ink and needs no blit. */
static inline const ls_font_glyph_t *ls_font_glyph(const ls_font_t *f,
                                                   uint8_t ch)
{
    if (!f || ch < f->first || ch >= f->first + f->count) return NULL;
    const ls_font_glyph_t *g = &f->dsc[ch - f->first];
    return (g->box_w && g->box_h) ? g : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* LS_FONT_H */
