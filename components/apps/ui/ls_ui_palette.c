#include "ui/ls_ui_palette.h"

typedef struct {
    const char *name;
    uint32_t rgb;
} ls_ui_palette_entry_t;

static const ls_ui_palette_entry_t PALETTE[LS_UI_COLOR_COUNT] = {
    [LS_UI_COLOR_BACKGROUND]   = { "background",   0x0A0C0D },
    [LS_UI_COLOR_PANEL]        = { "panel",        0x171A1C },
    [LS_UI_COLOR_PANEL_BORDER] = { "panel border", 0x2C3237 },
    /* both were raised for daylight. */

    [LS_UI_COLOR_TEXT]         = { "text",         0xDCE3E8 },
    [LS_UI_COLOR_DIM_TEXT]     = { "dim text",     0x9AA4AC },
    [LS_UI_COLOR_ACCENT]       = { "accent",       0x84D2CE },
    [LS_UI_COLOR_WARN]         = { "warn",         0xE3A83F },
    [LS_UI_COLOR_ALARM]        = { "alarm",        0xE45B50 },

    /* app identity.  Chosen for separation from each other at a
     * glance and for contrast against the 0x0A0C0D background, and kept
     * clear of ALARM's 0xE45B50 so a red header is never mistaken for a
     * red warning.  ID_RED is deliberately deeper and less orange than
     * ALARM for that reason. */
    [LS_UI_COLOR_ID_RED]       = { "id red",       0xD1495B },
    [LS_UI_COLOR_ID_ORANGE]    = { "id orange",    0xE07A3F },
    [LS_UI_COLOR_ID_TEAL]      = { "id teal",      0x84D2CE },
    [LS_UI_COLOR_ID_BLUE]      = { "id blue",      0x5A9FD4 },
    [LS_UI_COLOR_ID_VIOLET]    = { "id violet",    0x9B87D4 },
    [LS_UI_COLOR_ID_GREEN]     = { "id green",     0x6BBF8A },
    [LS_UI_COLOR_ID_ROSE]      = { "id rose",      0xD489AE },
    [LS_UI_COLOR_ID_STEEL]     = { "id steel",     0x8FA6B5 },
};

static const uint32_t THEME_PALETTE[][LS_UI_THEME_PART_COUNT] = {
    { 0xD93B30, 0x6E2019, 0x230806 },
    { 0xDCE4E9, 0x545E65, 0x1A1F23 },
    { 0x3E8FD9, 0x1D4467, 0x061622 },
};

uint32_t ls_ui_palette_hex(ls_ui_color_role_t role)
{
    if (role < 0 || role >= LS_UI_COLOR_COUNT)
        role = LS_UI_COLOR_BACKGROUND;
    return PALETTE[role].rgb;
}

const char *ls_ui_palette_name(ls_ui_color_role_t role)
{
    if (role < 0 || role >= LS_UI_COLOR_COUNT)
        return "unknown";
    return PALETTE[role].name;
}

uint32_t ls_ui_palette_theme_hex(int theme, ls_ui_theme_part_t part)
{
    if (theme < 0 || theme >= (int)(sizeof(THEME_PALETTE) / sizeof(THEME_PALETTE[0])))
        theme = 0;
    if (part < 0 || part >= LS_UI_THEME_PART_COUNT)
        part = LS_UI_THEME_ACCENT;
    return THEME_PALETTE[theme][part];
}
