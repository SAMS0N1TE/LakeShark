/* Which fonts this build has, and which one is wanted. */

#include "ls_tui.h"
#include "ls_font.h"

static const ls_font_t *const FONTS[] = {
    &ls_font_mono_16,
    &ls_font_mono_14,
};

static const char *const FONT_LABEL[] = { "10x17", "9x16" };

static int s_font_index;

int ls_tui_font_count(void) { return (int)(sizeof(FONTS) / sizeof(FONTS[0])); }

const ls_font_t *ls_tui_font_at(int index)
{
    if (index < 0 || index >= ls_tui_font_count()) return FONTS[0];
    return FONTS[index];
}

const char *ls_tui_font_label(int index)
{
    if (index < 0 || index >= ls_tui_font_count()) return FONT_LABEL[0];
    return FONT_LABEL[index];
}

int ls_tui_font_index(void) { return s_font_index; }

void ls_tui_set_font_index(int index)
{
    if (index < 0 || index >= ls_tui_font_count()) index = 0;
    s_font_index = index;
}
