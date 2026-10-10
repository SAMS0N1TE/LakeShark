#ifndef LS_CARTOCORE_CELLS_H
#define LS_CARTOCORE_CELLS_H
#include "ls_tui.h"
#include "ls_theme.h"
#include "cartocore/out.h"
/* ANSI order is mapped explicitly to the host's theme palette. */
static uint8_t carto_colour(uint32_t colour)
{
    static const uint8_t base[]={TUI_BLACK,TUI_RED,TUI_GREEN,TUI_YELLOW,
                                 TUI_BLUE,TUI_MAGENTA,TUI_CYAN,TUI_WHITE};
    uint8_t index=cc_index16(colour);
    return base[index&7] | (index&8?TUI_BRIGHT:0);
}
/* In the quadrant/braille views printable cells are labels. Keep old 16-colour
 * sources legible too, including black class ink. Daylight reverses the palette,
 * so the same primary-ink/ground pair remains high contrast there. No buffers. */
static uint8_t carto_attr(cc_cell c)
{
    uint8_t fg=carto_colour(c.fg), bg=carto_colour(c.bg);
    if (c.codepoint>32 && !(c.codepoint>=0x2580 && c.codepoint<=0x259f) &&
        !(c.codepoint>=0x2800 && c.codepoint<=0x28ff) && c.bg==UINT32_C(0x80000000)) {
        /* Uniform warm label ink; Daylight keeps its reversed class ink. */
        if (!ls_tui_daylight()) fg=TUI_YELLOW;
        else if ((fg&7)==TUI_BLACK) fg=TUI_WHITE;
        fg|=TUI_BRIGHT;
        bg=TUI_BLACK;
    }
    return TUI_ATTR(fg,bg);
}
static int16_t glyph(uint32_t cp)
{
    if (cp>=0x2800 && cp<=0x28ff) return (uint16_t)cp;
    static const uint32_t quadrants[]={0x20,0x2598,0x259d,0x2580,0x2596,0x258c,0x259e,0x259b,
                                     0x2597,0x259a,0x2590,0x259c,0x2584,0x2599,0x259f,0x2588};
    for (unsigned q=0;q<16;q++) if (cp==quadrants[q]) return (char)LS_TUI_QUAD(q&1,q&2,q&4,q&8);
    return cp>=32 && cp<127?(char)cp:'?';
}
#endif

