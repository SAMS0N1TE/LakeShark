#include "ls_test.h"
#include "ui/ls_ui_palette.h"

#include <string.h>

LS_CASE(palette_roles_have_stable_names_and_values)
{
    static const char *names[LS_UI_COLOR_COUNT] = {
        "background", "panel", "panel border", "text",
        "dim text", "accent", "warn", "alarm",
        /* app identity */
        "id red", "id orange", "id teal", "id blue",
        "id violet", "id green", "id rose", "id steel"
    };
    static const uint32_t rgb[LS_UI_COLOR_COUNT] = {
        /* text and dim text were raised for daylight readability.
         * dim text was 0x515A61, only 2.5:1 against the panel fill, and it
         * is the colour of every row label in the shell. */
        0x0A0C0D, 0x171A1C, 0x2C3237, 0xDCE3E8,
        0x9AA4AC, 0x84D2CE, 0xE3A83F, 0xE45B50,
        0xD1495B, 0xE07A3F, 0x84D2CE, 0x5A9FD4,
        0x9B87D4, 0x6BBF8A, 0xD489AE, 0x8FA6B5
    };

    /* An identity colour must never be the alarm colour: a red app header
     * beside a red fault indicator is exactly the confusion these roles were
     * added to avoid. */
    LS_CHECK_MSG(ls_ui_palette_hex(LS_UI_COLOR_ALARM) !=
                 ls_ui_palette_hex(LS_UI_COLOR_ID_RED),
                 "id red must differ from alarm");

    for (int i = 0; i < LS_UI_COLOR_COUNT; ++i) {
        LS_EQ_UINT(rgb[i], ls_ui_palette_hex((ls_ui_color_role_t)i));
        LS_EQ_STR(names[i], ls_ui_palette_name((ls_ui_color_role_t)i));
    }
}

LS_CASE(invalid_palette_roles_are_safe)
{
    LS_EQ_UINT(ls_ui_palette_hex(LS_UI_COLOR_BACKGROUND),
               ls_ui_palette_hex((ls_ui_color_role_t)-1));
    LS_EQ_UINT(ls_ui_palette_hex(LS_UI_COLOR_BACKGROUND),
               ls_ui_palette_hex(LS_UI_COLOR_COUNT));
    LS_EQ_STR("unknown", ls_ui_palette_name((ls_ui_color_role_t)-1));
    LS_EQ_STR("unknown", ls_ui_palette_name(LS_UI_COLOR_COUNT));
}

LS_CASE(theme_colours_live_in_the_shared_palette)
{
    LS_EQ_UINT(0xD93B30, ls_ui_palette_theme_hex(0, LS_UI_THEME_ACCENT));
    LS_EQ_UINT(0x545E65, ls_ui_palette_theme_hex(1, LS_UI_THEME_DIM));
    LS_EQ_UINT(0x061622, ls_ui_palette_theme_hex(2, LS_UI_THEME_BACKGROUND));
    LS_EQ_UINT(0xD93B30, ls_ui_palette_theme_hex(-1, LS_UI_THEME_ACCENT));
    LS_EQ_UINT(0xD93B30, ls_ui_palette_theme_hex(0, LS_UI_THEME_PART_COUNT));
}
