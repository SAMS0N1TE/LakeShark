/* The interface language: the string table, the Cyrillic cells, the glyph
   pages and the stored setting. */

#include "ls_test.h"

#include <string.h>

#include "ls_font.h"
#include "ls_i18n.h"
#include "settings_schema.h"
#include "tui_core.h"
#include "tui_cyr.h"

/* English is the code as written: the very same pointer comes back. */
LS_CASE(english_returns_the_argument_itself)
{
    ls_i18n_load(LS_LANG_EN);
    for (int i = 0; i < ls_i18n_entries(); i++) {
        const char *en = ls_i18n_entry_en(i);
        LS_CHECK(ls_tr(en) == en);
    }
    const char *home = "HOME";
    LS_CHECK(ls_tr_ctx("place|HOME", home) == home);
    LS_CHECK(ls_tr(NULL) == NULL);
}

LS_CASE(every_key_is_unique_and_every_entry_filled)
{
    const int n = ls_i18n_entries();
    LS_CHECK(n > 300);
    for (int i = 0; i < n; i++) {
        LS_CHECK_MSG(ls_i18n_entry_uk(i)[0] != 0, "empty translation for %s", ls_i18n_entry_en(i));
        for (int j = i + 1; j < n; j++)
            LS_CHECK_MSG(strcmp(ls_i18n_entry_en(i), ls_i18n_entry_en(j)) != 0,
                         "key twice: %s", ls_i18n_entry_en(i));
    }
}

/* The conversions of a format string, in order: what printf will read. */
static void conversions(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; *s && n + 2 < cap; s++) {
        if (*s != '%') continue;
        s++;
        if (*s == '%') continue;
        while (*s && n + 2 < cap && !strchr("diouxXeEfgGcsp", *s)) out[n++] = *s++;
        if (!*s) break;
        out[n++] = *s;
        out[n++] = '|';
    }
    out[n] = 0;
}

LS_CASE(format_strings_keep_their_conversions)
{
    for (int i = 0; i < ls_i18n_entries(); i++) {
        char a[64], b[64];
        conversions(ls_i18n_entry_en(i), a, sizeof(a));
        conversions(ls_i18n_entry_uk(i), b, sizeof(b));
        LS_CHECK_MSG(!strcmp(a, b), "conversions differ: %s", ls_i18n_entry_en(i));
    }
}

/* Every letter used in the table has a cell: nothing folds to '?', and the
   cell string is one byte a cell. */
LS_CASE(every_translation_is_drawable_one_byte_a_cell)
{
    ls_i18n_load(LS_LANG_UK);
    LS_EQ_INT(ls_i18n_dropped(), 0);
    for (int i = 0; i < ls_i18n_entries(); i++) {
        const char *uk = ls_i18n_entry_uk(i);
        char cells[256];
        tui_utf8_to_cells(cells, sizeof(cells), uk);
        int q_src = 0, q_out = 0;
        for (const char *p = uk; *p; p++) q_src += *p == '?';
        for (const char *p = cells; *p; p++) q_out += *p == '?';
        LS_CHECK_MSG(q_src == q_out, "undrawable letter in %s", uk);
        const char *t = ls_tr(ls_i18n_entry_en(i));
        LS_EQ_STR(t, cells);
        LS_EQ_INT((int)strlen(t), tui_utf8_cells(t, strlen(t)));
    }
    ls_i18n_load(LS_LANG_EN);
}

/* A Ukrainian word on the grid, read back cell by cell. */
LS_CASE(a_ukrainian_word_lands_one_letter_a_cell)
{
    static tui_cell back[40], front[40];
    tui_surface s;
    tui_surface_setup(&s, back, front, 40, 1);
    tui_frame_begin(&s);
    ls_i18n_load(LS_LANG_UK);
    const char *word = ls_tr("BACK");          /* НАЗАД */
    tui_put_str(&s, tui_surface_rect(&s), 0, 0, word, TUI_DEFAULT_ATTR);
    const uint32_t want[5] = { 0x041D, 0x0410, 0x0417, 0x0410, 0x0414 };
    for (int i = 0; i < 5; i++) {
        const int slot = tui_cyr_slot(back[i].ch);
        const uint32_t cp = slot >= 0 ? tui_cyr_slot_codepoint(slot) : 0;
        /* Н and А are drawn as the Latin H and A they look like. */
        if (want[i] == 0x041D) LS_EQ_INT(back[i].ch, 'H');
        else if (want[i] == 0x0410) LS_EQ_INT(back[i].ch, 'A');
        else LS_EQ_UINT(cp, want[i]);
    }
    LS_EQ_INT(back[5].ch, ' ');
    /* The same word arriving as UTF-8 (a mesh message) lands the same way. */
    tui_frame_begin(&s);
    tui_put_str(&s, tui_surface_rect(&s), 0, 0, "НАЗАД", TUI_DEFAULT_ATTR);
    for (int i = 0; i < 5; i++) LS_EQ_INT((uint8_t)back[i].ch, (uint8_t)word[i]);
    LS_EQ_INT(tui_utf8_cells("НАЗАД", strlen("НАЗАД")), 5);
    ls_i18n_load(LS_LANG_EN);
}

/* The bytes given to Cyrillic are ones no TUI glyph and no control a
   string may carry ever used. */
LS_CASE(cyrillic_bytes_stay_clear_of_the_graphics_glyphs)
{
    for (int slot = 0; slot < TUI_CYR_SLOTS; slot++) {
        int byte = -1;
        for (int b = 0; b < 256; b++) if (TUI_CYR_SLOT_OF[b] == slot) byte = b;
        LS_CHECK(byte > 0);
        LS_CHECK(byte != '\t' && byte != '\n' && byte != '\r' && byte != 0x1B);
        LS_CHECK(!(byte >= 0x20 && byte < 0x7F));
        LS_CHECK(!(byte >= 0x80 && byte <= 0x93));      /* quadrants, shades */
        LS_CHECK(!(byte >= 0xA0 && byte <= 0xA7));      /* traces            */
        LS_CHECK(byte != 0xB0);                          /* picture cell      */
        LS_CHECK(byte < 0xC0);                           /* sextants          */
        /* the codepoint the slot draws comes back to the same byte */
        LS_EQ_INT((uint8_t)tui_cell_codepoint_byte(tui_cyr_slot_codepoint(slot)), byte);
        /* a byte read back through a signed char finds the same slot */
        LS_EQ_INT(tui_cyr_slot((int)(signed char)byte), slot);
    }
    /* every Ukrainian letter, both cases, has a cell */
    const char *abc = "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯабвгґдеєжзиіїйклмнопрстуфхцчшщьюя";
    char cells[80];
    LS_EQ_INT((int)tui_utf8_to_cells(cells, sizeof(cells), abc), 66);
    LS_CHECK(strchr(cells, '?') == NULL);
    /* the Ukrainian apostrophe is the one the font has */
    LS_EQ_INT(tui_cell_codepoint_byte(0x2BC), '\'');
    /* the ASCII-only fold the map labels use is unchanged */
    LS_EQ_INT(tui_fold_codepoint(0x0416), '?');
}

LS_CASE(every_face_has_a_full_cyrillic_page)
{
    const ls_font_t *faces[] = { &ls_font_mono_14, &ls_font_mono_16, &ls_font_mono_24 };
    for (int f = 0; f < 3; f++) {
        const ls_font_t *page = ls_font_cyr_page(faces[f]);
        LS_CHECK(page != NULL);
        if (!page) continue;
        LS_EQ_INT(page->count, TUI_CYR_SLOTS);
        LS_EQ_INT(page->cell_w, faces[f]->cell_w);
        LS_EQ_INT(page->cell_h, faces[f]->cell_h);
        LS_EQ_INT(page->base_line, faces[f]->base_line);
        for (int slot = 0; slot < TUI_CYR_SLOTS; slot++) {
            const ls_font_glyph_t *g = ls_font_glyph(page, (uint8_t)slot);
            LS_CHECK(g != NULL);
            if (!g) continue;
            /* inside the cell, like every Latin glyph */
            const int top = (page->cell_h - page->base_line) - g->box_h - g->ofs_y;
            LS_CHECK(g->ofs_x >= 0 && g->ofs_x + g->box_w <= page->cell_w);
            LS_CHECK(top >= 0 && top + g->box_h <= page->cell_h);
        }
    }
    LS_CHECK(ls_font_cyr_page(NULL) == NULL);
}

static int s_saved = -1, s_saves;
static void fake_save(int lang) { s_saved = lang; s_saves++; }

/* Settings > Device > Language goes to the stored setting; boot reads it
   back without writing it again, and a stored value that is not a language
   is English. */
LS_CASE(the_setting_round_trips_through_the_saver)
{
    s_saves = 0;
    ls_i18n_set_saver(fake_save);
    ls_i18n_load(LS_LANG_EN);
    LS_EQ_INT(s_saves, 0);
    ls_i18n_set_lang(ls_i18n_next(LS_LANG_EN));
    LS_EQ_INT(ls_i18n_lang(), LS_LANG_UK);
    LS_EQ_INT(s_saved, LS_LANG_UK);
    LS_EQ_INT(s_saves, 1);
    ls_i18n_set_lang(ls_i18n_next(LS_LANG_UK));
    LS_EQ_INT(ls_i18n_lang(), LS_LANG_EN);
    LS_EQ_INT(s_saved, LS_LANG_EN);
    ls_i18n_load(1);
    LS_EQ_INT(ls_i18n_lang(), LS_LANG_UK);
    ls_i18n_load(7);
    LS_EQ_INT(ls_i18n_lang(), LS_LANG_EN);
    ls_i18n_load(-1);
    LS_EQ_INT(ls_i18n_lang(), LS_LANG_EN);
    LS_EQ_INT(s_saves, 2);
    ls_i18n_set_saver(NULL);

    /* the schema's own check agrees with the module */
    LS_EQ_INT(SETTINGS_UI_LANG_COUNT, LS_LANG_COUNT);
    for (int v = -2; v < 5; v++)
        LS_EQ_INT(settings_ui_lang_valid(v), ls_i18n_lang_valid(v));
    LS_CHECK(SETTINGS_SCHEMA_VERSION >= 8);
    LS_EQ_STR(ls_i18n_lang_name(LS_LANG_EN), "English");
    char cells[24];
    tui_utf8_to_cells(cells, sizeof(cells), "Українська");
    LS_EQ_STR(ls_i18n_lang_name(LS_LANG_UK), cells);
}

/* Words that sit in fixed-size places stay inside them. */
LS_CASE(fixed_places_keep_their_width)
{
    ls_i18n_load(LS_LANG_UK);
    /* home tiles: fourteen cells inside the frame */
    static const char *const TILES[] = {
        "HOME", "directory", "trunking", "analogue", "aircraft", "REC", "capture",
        "DIAG", "health", "TERMINAL", "console", "UPDATE", "firmware", "SET",
        "display", "position", "MAP", "charts", "vector tiles", "TILES",
        "map regions", "FALLS", "spectrum", "MESH", "lora", "meshcore", "RADIOS",
        "power", "LORA LABS", "experiments", "EXPERIMENTS", "LR2021 and friends",
        "RF trials", "JOURNAL", "NOTES", "field notes", "COMPASS", "bearings",
        "passive watch", "watch + replay", "FILES", "the SD card", "MUSIC",
        "player", "audio player", "keyboard radios", "keyboard RF", "P25 DESIGN",
        "FM DESIGN", "preview", "LINK", "wireless", "DRONES", "SWEEP",
        "passive nearby signals", "DETECTOR", "rule matches",
    };
    for (unsigned i = 0; i < sizeof(TILES) / sizeof(TILES[0]); i++)
        LS_CHECK_MSG(strlen(ls_tr(TILES[i])) <= 14, "tile word too wide: %s", TILES[i]);
    /* portrait tabs show nine cells of a screen name */
    static const char *const TABS[] = { "HOME", "MESH", "RADIOS", "SET" };
    for (unsigned i = 0; i < 4; i++) LS_CHECK(strlen(ls_tr(TABS[i])) <= 9);
    /* the status row's TX warning takes the brand's nine cells */
    LS_CHECK(strlen(ls_tr("MESH TX")) <= 9 && strlen(ls_tr("MESH ARM")) <= 9);
    /* the tile's LIVE tag has four cells */
    LS_CHECK(strlen(ls_tr("LIVE")) <= 4);
    /* the radio card puts its number at column 12: labels of nine or less */
    static const char *const CARDS[] = { "VOLUME", "GAIN", "GAIN dB", "SQUELCH", "HANG", "OPEN AT" };
    for (unsigned i = 0; i < 6; i++) LS_CHECK_MSG(strlen(ls_tr(CARDS[i])) <= 9, "%s", CARDS[i]);
    /* column headers keep every column */
    static const char *const HEADS[] = {
        "ICAO   CALLSIGN S    ALT  SPD DIR  AGE  MSGS", "ICAO   CALLSIGN S    ALT  AGE",
        "TALKGROUP    HEARD   AGE  SEEN IN",
    };
    for (unsigned i = 0; i < 3; i++) {
        const char *uk = ls_tr(HEADS[i]);
        LS_CHECK(strlen(uk) <= strlen(HEADS[i]));
        /* each column starts where the English one does */
        for (size_t c = 1; c < strlen(uk); c++)
            if (uk[c] != ' ' && uk[c - 1] == ' ')
                LS_CHECK_MSG(HEADS[i][c] != ' ' && HEADS[i][c - 1] == ' ', "%s col %d", HEADS[i], (int)c);
    }
    ls_i18n_load(LS_LANG_EN);
}
