/* FREQS as a desktop program: what the library is when it has a window, a
   mouse and a keyboard instead of a thumb.

       File  Edit  View  Tune                                  menu bar
       [New] [Edit] [Delete] [Undo] [Tune] [Star] ...          toolbar
       Find [........] [Band ALL] [Group ALL] [Near] ...       filters
       1    3    10    30    100   300   1G                    the dial, flat:
       --.-:|--||-.---:-|------.--                             where the view sits
                 ^146.940      ^FM                             and where FM is now
       +BANKS----+ +N of M-----------------------+ +THIS ONE-+
       | ALL     | | * MHz  NAME  MODE  GROUP ...| | details |
       | banks   | | rows, sorted by any column  | | or the  |
       | GROUPS  | |                             | | form    |
       +---------+ +-----------------------------+ +---------+
       status: counts, filters, selection, the last thing said

   The data and every action are the handheld's (ls_freqs.c, ls_freqs_run.c);
   what is only the desk's is in ls_freqs_desk.c, and the widgets are
   ls_deskui.c. The letters the handheld takes (T S K O B G N F) do the same
   here. */
#include "scr_freqs_desk.h"

#if LS_TUI_HAS_COMPACT

#include "../../ls_deskui.h"
#include "../../ls_freqs_desk.h"
#include "../../ls_box.h"
#include "../../ls_radio_panel.h"
#include "../../ls_value.h"
#include "../../ls_app.h"
#include "../../ls_input_ev.h"
#include "../../ls_tui_touch.h"
#include "ls_gps.h"
#include "core/settings.h"
#include "scan_import.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HUE TUI_YELLOW
#define PUMP_LINES 48
#define WHITE TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define AMBER TUI_ATTR(HUE | TUI_BRIGHT, TUI_BLACK)

/* ----------------------------------------------------------------- state -- */

static EXT_RAM_BSS_ATTR uint16_t s_idx[LS_FREQ_MAX];     /* the view: loaded indices, in order */
static EXT_RAM_BSS_ATTR uint16_t s_act[LS_FREQ_MAX];     /* what a command acts on */
static EXT_RAM_BSS_ATTR uint8_t s_pick[LS_FREQ_MAX / 8 + 1], s_dup[LS_FREQ_MAX / 8 + 1];
static EXT_RAM_BSS_ATTR ls_freq_filter_t s_f;
static EXT_RAM_BSS_ATTR char s_say[112];
static int64_t s_said_us;
static int s_n, s_sel, s_anchor, s_npick, s_ndup;
static int s_sort = LS_FC_LISTED;
static bool s_desc, s_only_star, s_only_dup;
static bool s_free;                     /* the wheel moved the window off the cursor */
static uint32_t s_shown = (1u << LS_FC__COUNT) - 1;
static bool s_show_banks = true, s_show_detail = true, s_show_ruler = true;
static uint32_t s_built_gen = UINT32_MAX;
static bool s_stale = true, s_fix;
static double s_lat, s_lon, s_built_lat, s_built_lon;

enum { F_TABLE, F_BANKS, F_FIND };
static int s_focus = F_TABLE;
static int s_bank_cur;                  /* the tree row the keyboard is on */
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_find;

/* Everything of any size is in PSRAM: the Nano has a few KB of its own RAM
   to spare, and a window's layout is not worth any of it. */
static EXT_RAM_BSS_ATTR ls_dui_table_t s_t;
static EXT_RAM_BSS_ATTR tui_rect s_area, s_r_menu, s_r_tools, s_r_filter, s_r_ruler, s_r_banks, s_r_table,
                                 s_r_detail, s_r_status, s_r_find;
static EXT_RAM_BSS_ATTR int16_t s_bar_x[8];

static const ls_dui_col_t COLS[LS_FC__COUNT] = {
    [LS_FC_STAR]  = { "*",     1,  1, 0, 0 },
    [LS_FC_FREQ]  = { "MHz",   10, 10, LS_DUI_RIGHT, 0 },
    [LS_FC_NAME]  = { "NAME",  24, 10, LS_DUI_FLEX, 0 },
    [LS_FC_MODE]  = { "MODE",  7,  7, 0, 0 },
    [LS_FC_BAND]  = { "BAND",  7,  7, 0, 7 },
    [LS_FC_GROUP] = { "GROUP", 16, 16, 0, 4 },
    [LS_FC_TONE]  = { "TONE",  6,  6, 0, 6 },
    [LS_FC_DIST]  = { "DIST",  10, 10, LS_DUI_RIGHT, 3 },
    [LS_FC_BANK]  = { "BANK",  10, 10, 0, 5 },
    [LS_FC_TAGS]  = { "TAGS",  14, 14, 0, 8 },
    [LS_FC_NOTES] = { "NOTES", 40, 8, LS_DUI_FLEX, 9 },
};

/* Commands: what a menu row, a key and a toolbar key all come down to. */
enum {
    C_TUNE, C_NEW, C_EDIT, C_DUP, C_DELETE, C_UNDO, C_STAR, C_ALL, C_NONE_SEL,
    C_COPY_TO, C_MOVE_TO, C_COPY_MINE,
    C_BANK_NEW, C_BANK_RENAME, C_BANK_TRASH, C_IMPORT,
    C_EXP_CSV, C_EXP_SEL, C_EXP_CHIRP, C_EXP_LSCAN, C_RELOAD,
    C_FIND, C_BAND, C_GROUP, C_NEAR, C_ONLY_STAR, C_ONLY_DUP, C_CLEAR,
    C_PANE_BANKS, C_PANE_DETAIL, C_PANE_RULER, C_SORT_LISTED, C_SORT_FLIP,
    C_SAVE_FM, C_SAVE_P25, C_SCAN_VIEW, C_SCAN_SEL, C_OPEN, C_CUT, C_COPY, C_PASTE,
    C_COL = 100,            /* + column: show or hide it */
    C_SORT = 120,           /* + column: sort by it */
};

typedef struct { tui_rect r; int16_t cmd; } hit_t;
static EXT_RAM_BSS_ATTR hit_t s_hit[24];
static int s_nhit;

/* The popup: a menu (ids are commands) or a list to choose from. */
enum { K_MENU, K_COPY, K_MOVE, K_IMPORT, K_GROUP, K_BAND, K_MODE, K_OPEN };
#define ITEMS 56
static EXT_RAM_BSS_ATTR ls_dui_popup_t s_pop;
static EXT_RAM_BSS_ATTR ls_dui_item_t s_items[ITEMS];
static EXT_RAM_BSS_ATTR char s_label[ITEMS][44], s_keys[ITEMS][16];
static int s_nitems, s_pop_kind;

/* A question in the middle of the window. */
enum { D_NONE, D_BANK_NEW, D_BANK_RENAME, D_BANK_TRASH, D_DELETE, D_SCAN_VIEW, D_SCAN_SEL, D_COPY_NEW, D_MOVE_NEW };
static int s_dlg, s_dlg_bank;
static bool s_dlg_prompt;
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_dlg_edit;
static EXT_RAM_BSS_ATTR char s_dlg_text[2][72];
static EXT_RAM_BSS_ATTR tui_rect s_dlg_ok, s_dlg_no, s_dlg_field;

/* The form: an entry, a field a line. */
static bool s_form, s_form_new;
static int s_form_at, s_form_bad = -1, s_form_idx;
static uint32_t s_form_gen;
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_fld[LS_FF__COUNT];
static EXT_RAM_BSS_ATTR ls_freq_t s_form_base;
static EXT_RAM_BSS_ATTR char s_form_bank[24];
static EXT_RAM_BSS_ATTR tui_rect s_fld_r[LS_FF__COUNT], s_form_ok, s_form_no;

/* The tree at the left, a row at a time as it was drawn. */
enum { T_NONE, T_ALL, T_BANK, T_BUILTIN, T_GROUP };
typedef struct { uint8_t kind; int8_t arg; } tree_t;
static EXT_RAM_BSS_ATTR tree_t s_tree[64];
static int s_ntree, s_tree_y;
#define GROUPS 32
static EXT_RAM_BSS_ATTR char s_group[GROUPS][LS_FREQ_GROUP];
static EXT_RAM_BSS_ATTR uint16_t s_group_n[GROUPS];
static int s_ngroups;
static EXT_RAM_BSS_ATTR char s_count_name[LS_FREQ_BANKS][24];
static EXT_RAM_BSS_ATTR int16_t s_count[LS_FREQ_BANKS];

/* A row held and carried: over the table it sweeps a selection, over a bank
   it is a move waiting for the button to come up. */
static EXT_RAM_BSS_ATTR struct { bool on; int from, col, row, drop; } s_drag;
static int64_t s_tap_us;
static int s_tap_row = -1;

/* The dial laid flat: how many of the view fall in each cell of the strip. */
static EXT_RAM_BSS_ATTR uint8_t s_rule_n[384], s_rule_hue[384];
static int s_rule_w;
static bool s_rule_stale = true;

static EXT_RAM_BSS_ATTR char s_files[24][96];
static int s_nfiles;

/* Cut, copy and paste: the entries by value, in PSRAM, until the next cut
   or copy. Paste goes where a new entry would. */
static ls_freq_t *s_clip;
static int s_nclip;

static void run(int cmd);
static void open_menu(int which);
static void open_chooser(void);
static const char *new_bank(void);

static void say(const char *text) { snprintf(s_say, sizeof(s_say), "%s", text); s_said_us = esp_timer_get_time(); }

static uint8_t mods_now(void) { return ls_tui_input_mods(); }

static const ls_freq_t *at(int k) { return k >= 0 && k < s_n ? ls_freqs_at(s_idx[k]) : NULL; }
static const ls_freq_t *selected(void) { return at(s_sel); }
static bool picked(int k) { return k >= 0 && k < s_n && ls_dui_bit(s_pick, s_idx[k]); }

static void count_picks(void)
{
    s_npick = 0;
    for (int k = 0; k < s_n; k++) s_npick += ls_dui_bit(s_pick, s_idx[k]);
}

static void pick_none(void) { memset(s_pick, 0, sizeof(s_pick)); s_npick = 0; }

static void pick_range(int a, int b)
{
    pick_none();
    if (a > b) { const int t = a; a = b; b = t; }
    for (int k = a < 0 ? 0 : a; k <= b && k < s_n; k++) ls_dui_bit_set(s_pick, s_idx[k], true);
    count_picks();
}

/* What a command acts on: the ticked rows that are in view, in view order,
   or with none ticked the row the cursor is on. */
static int targets(void)
{
    int n = 0;
    for (int k = 0; k < s_n && s_npick; k++) if (ls_dui_bit(s_pick, s_idx[k])) s_act[n++] = s_idx[k];
    if (!n && selected()) s_act[n++] = s_idx[s_sel];
    return n;
}

static const char *upper(const char *text)
{
    static EXT_RAM_BSS_ATTR char t[2][28];
    static int k;
    k ^= 1;
    snprintf(t[k], sizeof(t[k]), "%s", text);
    for (char *p = t[k]; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    return t[k];
}

static const char *bank_label(int which)
{
    return which == LS_FREQS_ALL && !ls_freqs_banks() ? "NO CARD: BUILT-IN" : upper(ls_freqs_bank(which));
}

/* The stem a file made from the view is named for. */
static const char *view_stem(void)
{
    const int w = ls_freqs_which();
    return w >= 0 ? ls_freqs_bank(w) : w == LS_FREQS_ALL ? "all" : "builtin";
}

/* -------------------------------------------------------------- the view -- */

/* The same stored word the handheld keeps: band, sort, NEAR and the bank. */
static uint32_t bank_hash(int which)
{
    if (which == LS_FREQS_ALL) return 0;
    if (which == LS_FREQS_BUILTIN) return 1;
    uint32_t h = 2166136261u;
    for (const char *p = ls_freqs_bank(which); *p; p++) h = (h ^ (uint8_t)(*p | 0x20)) * 16777619u;
    return 2 + h % 254;
}

static void store_view(void)
{
    const uint32_t sort = s_sort == LS_FC_FREQ ? LS_FS_FREQ : s_sort == LS_FC_NAME ? LS_FS_NAME
                        : s_sort == LS_FC_DIST ? LS_FS_NEAR : LS_FS_LISTED;
    settings_set_freqs_view((uint32_t)s_f.band | sort << 3 | (uint32_t)s_f.near << 5 | bank_hash(ls_freqs_which()) << 6);
}

static void changed(void) { s_stale = true; s_free = false; store_view(); }

static void fix_now(void)
{
    static EXT_RAM_BSS_ATTR ls_gps_state_t g;
    ls_gps_get(&g);
    const int64_t now = esp_timer_get_time();
    s_fix = (g.fix || (g.last_fix_us > 0 && now - g.last_fix_us < 60000000)) && fabs(g.lat_deg) <= 90 &&
            fabs(g.lon_deg) <= 180 && (g.lat_deg != 0 || g.lon_deg != 0);
    if (s_fix) { s_lat = g.lat_deg; s_lon = g.lon_deg; }
}

/* The entry the cursor is on, by value: a reload empties the set and fills
   it over several frames, and the cursor finds its entry when it is back. */
static uint32_t s_want_hz;
static EXT_RAM_BSS_ATTR char s_want_name[LS_FREQ_NAME];
static bool s_want_pending;
static int s_built_sel;

static void want(const ls_freq_t *e)
{
    s_want_hz = e ? e->hz : 0;
    snprintf(s_want_name, sizeof(s_want_name), "%s", e ? e->name : "");
}

/* Follow an entry through the reload a write is about to cause. */
static void follow(const ls_freq_t *e)
{
    want(e);
    s_want_pending = s_want_hz != 0;
    s_built_sel = s_sel;
}

static void census(void)
{
    /* The groups in what is loaded, most entries first. */
    s_ngroups = 0;
    for (int i = 0; i < ls_freqs_count(); i++) {
        const ls_freq_t *e = ls_freqs_at(i);
        if (!e) continue;
        int k = 0;
        while (k < s_ngroups && strcmp(s_group[k], e->group)) k++;
        if (k == s_ngroups) {
            if (s_ngroups >= GROUPS) continue;
            snprintf(s_group[k], sizeof(s_group[k]), "%s", e->group);
            s_group_n[k] = 0;
            s_ngroups++;
        }
        s_group_n[k]++;
    }
    for (int a = 1; a < s_ngroups; a++)
        for (int b = a; b > 0 && s_group_n[b] > s_group_n[b - 1]; b--) {
            char t[LS_FREQ_GROUP];
            const uint16_t n = s_group_n[b];
            memcpy(t, s_group[b], sizeof(t));
            memcpy(s_group[b], s_group[b - 1], sizeof(t));
            memcpy(s_group[b - 1], t, sizeof(t));
            s_group_n[b] = s_group_n[b - 1];
            s_group_n[b - 1] = n;
        }
    /* How many each bank holds, as far as loading has shown: ALL counts
       every one, one bank counts itself. A count is kept by the bank's name,
       since a new file moves the others along the list. */
    int16_t kept[LS_FREQ_BANKS];
    for (int b = 0; b < LS_FREQ_BANKS; b++) {
        kept[b] = -1;
        for (int o = 0; o < LS_FREQ_BANKS && b < ls_freqs_banks(); o++)
            if (s_count_name[o][0] && !strcmp(s_count_name[o], ls_freqs_bank(b))) kept[b] = s_count[o];
    }
    for (int b = 0; b < LS_FREQ_BANKS; b++) {
        s_count[b] = kept[b];
        snprintf(s_count_name[b], sizeof(s_count_name[b]), "%s", b < ls_freqs_banks() ? ls_freqs_bank(b) : "");
    }
    const int which = ls_freqs_which();
    if (ls_freqs_tally()->truncated) return;
    if (which == LS_FREQS_ALL) {
        for (int b = 0; b < ls_freqs_banks(); b++) s_count[b] = 0;
        for (int i = 0; i < ls_freqs_count(); i++) {
            const ls_freq_t *e = ls_freqs_at(i);
            if (e && e->bank < LS_FREQ_BANKS) s_count[e->bank]++;
        }
    } else if (which >= 0 && which < LS_FREQ_BANKS) s_count[which] = (int16_t)ls_freqs_count();
}

static void rebuild(void)
{
    const uint32_t hz = s_want_hz;
    const bool reloaded = s_built_gen != ls_freqs_generation();
    ls_freq_filter_t f = s_f;
    f.sort = LS_FS_LISTED;
    s_n = ls_freqs_view(&f, s_fix, s_lat, s_lon, s_idx, LS_FREQ_MAX);
    if (s_only_star) {
        int n = 0;
        for (int k = 0; k < s_n; k++) if (ls_freqd_starred(ls_freqs_at(s_idx[k]))) s_idx[n++] = s_idx[k];
        s_n = n;
    }
    s_ndup = ls_freqs_loading() ? 0 : ls_freqd_dups(s_idx, s_n, s_dup, sizeof(s_dup));
    if (s_only_dup) {
        int n = 0;
        for (int k = 0; k < s_n; k++) if (ls_dui_bit(s_dup, s_idx[k])) s_idx[n++] = s_idx[k];
        s_n = n;
    }
    /* Duplicates side by side is what the filter is for. */
    const int sort = s_only_dup && s_sort == LS_FC_LISTED ? LS_FC_FREQ : s_sort;
    if (sort != LS_FC_LISTED || s_desc) ls_freqd_sort(s_idx, s_n, sort, s_desc, s_fix, s_lat, s_lon);
    s_built_gen = ls_freqs_generation();
    s_built_lat = s_lat; s_built_lon = s_lon;
    s_stale = false;
    s_rule_stale = true;
    if (reloaded) {
        /* The ticks were on entries that are gone. */
        pick_none();
        if (!ls_freqs_loading()) census();
    } else count_picks();
    s_sel = s_built_sel = 0;
    s_want_pending = false;
    for (int i = 0; hz && i < s_n; i++) {
        const ls_freq_t *e = ls_freqs_at(s_idx[i]);
        if (e && e->hz == hz && !strcmp(e->name, s_want_name)) { s_sel = s_built_sel = i; s_anchor = i; return; }
    }
    s_anchor = 0;
    s_want_pending = hz && ls_freqs_loading();
}

static void refresh(void)
{
    if (s_want_pending && s_sel != s_built_sel) s_want_pending = false;
    if (!s_want_pending && s_built_gen == ls_freqs_generation()) want(selected());
    ls_freqs_pump(PUMP_LINES);
    fix_now();
    const bool wants_fix = s_f.near || s_sort == LS_FC_DIST;
    if (wants_fix && s_fix && ls_freq_km(s_lat, s_lon, s_built_lat, s_built_lon) > 0.3) s_stale = true;
    if (s_stale || s_built_gen != ls_freqs_generation()) rebuild();
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    if (s_anchor >= s_n) s_anchor = s_sel;
}

static void distance_text(const ls_freq_t *e, char *out, size_t n)
{
    if (e->lat_e5 == LS_FREQ_NOWHERE) snprintf(out, n, "anywhere");
    else if (!s_fix) snprintf(out, n, "no fix");
    else {
        const float lat = e->lat_e5 * 1e-5f, lon = e->lon_e5 * 1e-5f;
        const int d = (int)(ls_freq_distance(e, s_lat, s_lon) * 10 + 0.5f);
        const char *b = ls_freq_bearing(s_lat, s_lon, lat, lon);
        if (d < 1) snprintf(out, n, "here");
        else if (d < 100) snprintf(out, n, "%d.%d km %s", d / 10, d % 10, b);
        else snprintf(out, n, "%d km %s", (d + 5) / 10, b);
    }
}

/* What FM and P25 are on now, read the way a user app reads them. */
static bool app_freq(const char *path, uint32_t *hz)
{
    ls_val_t v;
    if (!ls_value_read(path, &v, NULL) || v.kind != LS_VAL_FLOAT || v.f < 0.5f) return false;
    *hz = (uint32_t)lround(v.f * 1e6 / 100.0) * 100u;
    return true;
}

/* ------------------------------------------------------------- the popup -- */

static void items_begin(void) { s_nitems = 0; }

static ls_dui_item_t *item(const char *label, const char *keys, int id, int flags)
{
    if (s_nitems >= ITEMS) return &s_items[ITEMS - 1];
    ls_dui_item_t *it = &s_items[s_nitems];
    memset(it, 0, sizeof(*it));
    if (label) { snprintf(s_label[s_nitems], sizeof(s_label[0]), "%s", label); it->label = s_label[s_nitems]; }
    if (keys) { snprintf(s_keys[s_nitems], sizeof(s_keys[0]), "%s", keys); it->keys = s_keys[s_nitems]; }
    it->id = (int16_t)id;
    it->flags = (uint8_t)flags;
    s_nitems++;
    return it;
}

static void rule(void) { item(NULL, NULL, -1, LS_DUI_SEP); }

static void popup(int kind, int x, int y, int bar)
{
    s_pop_kind = kind;
    ls_dui_popup_open(&s_pop, s_items, s_nitems, x, y, bar, HUE);
}

static const char *const MENUS[] = { "File", "Edit", "View", "Tune" };
#define NMENUS 4

static void build_menu(int which)
{
    const ls_freq_t *e = selected();
    const int n = s_npick ? s_npick : e ? 1 : 0;
    const bool file = e && e->bank != LS_FREQ_BANK_BUILTIN;
    const int no = n ? 0 : LS_DUI_OFF, one = e && s_npick <= 1 ? 0 : LS_DUI_OFF;
    const int bank = ls_freqs_which() >= 0 ? 0 : LS_DUI_OFF;
    char text[44];
    items_begin();
    switch (which) {
    case 0:
        item("Open bank...", "K", C_OPEN, 0);
        item("New bank...", NULL, C_BANK_NEW, 0);
        item("Rename bank...", NULL, C_BANK_RENAME, bank);
        item("Move bank to trash...", NULL, C_BANK_TRASH, bank);
        rule();
        item("Import a file...", NULL, C_IMPORT, 0);
        item("Export view as CSV", "Ctrl+E", C_EXP_CSV, s_n ? 0 : LS_DUI_OFF);
        item("Export selection as CSV", NULL, C_EXP_SEL, no);
        item("Export view for CHIRP", NULL, C_EXP_CHIRP, s_n ? 0 : LS_DUI_OFF);
        item("Export view as scanner list", NULL, C_EXP_LSCAN, s_n ? 0 : LS_DUI_OFF);
        rule();
        item("Reload the card", "F5", C_RELOAD, 0);
        break;
    case 1:
        item("New entry", "Ins", C_NEW, 0);
        item("Edit", "F2", C_EDIT, one | (file ? 0 : LS_DUI_OFF));
        item("Duplicate", "Ctrl+D", C_DUP, one);
        item(e && ls_freqd_starred(e) ? "Remove star" : "Star", "*", C_STAR, no);
        rule();
        item("Cut", "Ctrl+X", C_CUT, no);
        item("Copy", "Ctrl+C", C_COPY, no);
        snprintf(text, sizeof(text), s_nclip ? "Paste %d into %s" : "Paste", s_nclip, upper(new_bank()));
        item(text, "Ctrl+V", C_PASTE, s_nclip ? 0 : LS_DUI_OFF);
        item("Copy to bank...", NULL, C_COPY_TO, no);
        item("Move to bank...", NULL, C_MOVE_TO, no);
        item("Copy to MINE", NULL, C_COPY_MINE, no);
        rule();
        item("Delete", "Del", C_DELETE, no);
        snprintf(text, sizeof(text), "Undo delete (%d)", ls_freqs_undo_count());
        item(ls_freqs_undo_count() ? text : "Undo delete", "Ctrl+Z", C_UNDO, ls_freqs_undo_count() ? 0 : LS_DUI_OFF);
        rule();
        item("Select all", "Ctrl+A", C_ALL, s_n ? 0 : LS_DUI_OFF);
        item("Select none", "Esc", C_NONE_SEL, s_npick ? 0 : LS_DUI_OFF);
        break;
    case 2:
        item("Find", "Ctrl+F", C_FIND, 0);
        snprintf(text, sizeof(text), "Band: %s...", ls_freq_band_name(s_f.band));
        item(text, "B", C_BAND, 0);
        snprintf(text, sizeof(text), "Group: %s...", s_f.group[0] ? s_f.group : "ALL");
        item(text, "G", C_GROUP, 0);
        item("Near me", "N", C_NEAR, s_f.near ? LS_DUI_CHECK : 0);
        item("Starred only", NULL, C_ONLY_STAR, s_only_star ? LS_DUI_CHECK : 0);
        item("Duplicates only", NULL, C_ONLY_DUP, s_only_dup ? LS_DUI_CHECK : 0);
        item("Clear filters", NULL, C_CLEAR, 0);
        rule();
        item("As listed in the file", NULL, C_SORT_LISTED, s_sort == LS_FC_LISTED ? LS_DUI_CHECK : 0);
        item("Descending", NULL, C_SORT_FLIP, s_desc ? LS_DUI_CHECK : 0);
        rule();
        item("Bank list", NULL, C_PANE_BANKS, s_show_banks ? LS_DUI_CHECK : 0);
        item("Details", NULL, C_PANE_DETAIL, s_show_detail ? LS_DUI_CHECK : 0);
        item("Band ruler", NULL, C_PANE_RULER, s_show_ruler ? LS_DUI_CHECK : 0);
        rule();
        item("COLUMNS", NULL, -1, LS_DUI_HEAD);
        for (int c = LS_FC_BAND; c < LS_FC__COUNT; c++)
            item(ls_freqd_col_name(c), NULL, C_COL + c, s_shown >> c & 1 ? LS_DUI_CHECK : 0);
        break;
    default: {
        const ls_freq_route_t r = e ? ls_freq_route(e->mode, e->hz) : (ls_freq_route_t){ LS_FR_NONE, NULL, NULL, NULL };
        const ls_app_t *app = r.app ? ls_app_by_id(r.app) : NULL;
        uint32_t hz;
        snprintf(text, sizeof(text), "Tune in %s", app ? app->name : "its app");
        item(text, "Enter", C_TUNE, e && r.target != LS_FR_NONE ? 0 : LS_DUI_OFF);
        rule();
        item("New from what FM is on", NULL, C_SAVE_FM, app_freq("fm.freq", &hz) ? 0 : LS_DUI_OFF);
        item("New from what P25 is on", NULL, C_SAVE_P25, app_freq("p25.freq", &hz) ? 0 : LS_DUI_OFF);
        rule();
        item("Scan the view in FM...", NULL, C_SCAN_VIEW, s_n ? 0 : LS_DUI_OFF);
        item("Scan the selection in FM...", NULL, C_SCAN_SEL, no);
        break;
    }
    }
}

static void open_menu(int which)
{
    if (which < 0) which = NMENUS - 1;
    if (which >= NMENUS) which = 0;
    build_menu(which);
    popup(K_MENU, s_bar_x[which], s_r_menu.y + 1, which);
}

/* The menu under the right button on a row. */
static void row_menu(int x, int y)
{
    const ls_freq_t *e = selected();
    char text[44];
    if (!e) return;
    const bool file = e->bank != LS_FREQ_BANK_BUILTIN;
    const int one = s_npick <= 1 ? 0 : LS_DUI_OFF;
    items_begin();
    if (s_npick > 1) snprintf(text, sizeof(text), "%d SELECTED", s_npick);
    else snprintf(text, sizeof(text), "%s", e->name);
    item(text, NULL, -1, LS_DUI_HEAD);
    item("Tune", "Enter", C_TUNE, one | (ls_freq_route(e->mode, e->hz).target == LS_FR_NONE ? LS_DUI_OFF : 0));
    item("Edit", "F2", C_EDIT, one | (file ? 0 : LS_DUI_OFF));
    item("Duplicate", "Ctrl+D", C_DUP, one);
    item(ls_freqd_starred(e) ? "Remove star" : "Star", "*", C_STAR, 0);
    rule();
    item("Copy to bank...", NULL, C_COPY_TO, 0);
    item("Move to bank...", NULL, C_MOVE_TO, 0);
    item("Copy to MINE", NULL, C_COPY_MINE, 0);
    item("Scan in FM...", NULL, C_SCAN_SEL, 0);
    item("Export as CSV", NULL, C_EXP_SEL, 0);
    rule();
    item("Delete", "Del", C_DELETE, 0);
    popup(K_MENU, x, y, -1);
}

static void head_menu(int x, int y)
{
    items_begin();
    item("SORT BY", NULL, -1, LS_DUI_HEAD);
    item("As listed in the file", NULL, C_SORT_LISTED, s_sort == LS_FC_LISTED ? LS_DUI_CHECK : 0);
    for (int c = 0; c < LS_FC__COUNT; c++)
        item(c == LS_FC_STAR ? "Star" : ls_freqd_col_name(c), NULL, C_SORT + c, s_sort == c ? LS_DUI_CHECK : 0);
    item("Descending", NULL, C_SORT_FLIP, s_desc ? LS_DUI_CHECK : 0);
    rule();
    item("COLUMNS", NULL, -1, LS_DUI_HEAD);
    for (int c = LS_FC_BAND; c < LS_FC__COUNT; c++)
        item(ls_freqd_col_name(c), NULL, C_COL + c, s_shown >> c & 1 ? LS_DUI_CHECK : 0);
    popup(K_MENU, x, y, -1);
}

static void bank_menu(int x, int y)
{
    const int bank = ls_freqs_which() >= 0 ? 0 : LS_DUI_OFF;
    items_begin();
    item(bank_label(ls_freqs_which()), NULL, -1, LS_DUI_HEAD);
    item("New bank...", NULL, C_BANK_NEW, 0);
    item("Rename...", NULL, C_BANK_RENAME, bank);
    item("Move to trash...", NULL, C_BANK_TRASH, bank);
    rule();
    item("Import a file...", NULL, C_IMPORT, 0);
    item("Export as CSV", NULL, C_EXP_CSV, s_n ? 0 : LS_DUI_OFF);
    item("Export for CHIRP", NULL, C_EXP_CHIRP, s_n ? 0 : LS_DUI_OFF);
    item("Reload the card", "F5", C_RELOAD, 0);
    popup(K_MENU, x, y, -1);
}

/* A list of banks to send the selection to; the last row makes a new one. */
static void bank_chooser(int kind)
{
    char text[44];
    if (!targets()) { say("Nothing selected"); return; }
    items_begin();
    snprintf(text, sizeof(text), "%s %d TO", kind == K_MOVE ? "MOVE" : "COPY", targets());
    item(text, NULL, -1, LS_DUI_HEAD);
    for (int b = 0; b < ls_freqs_banks(); b++)
        item(upper(ls_freqs_bank(b)), NULL, b, b == ls_freqs_which() ? LS_DUI_OFF : 0);
    rule();
    item("A new bank...", NULL, LS_FREQ_BANKS, 0);
    popup(kind, s_r_table.x + 6, s_r_table.y + 2, -1);
}

/* Which bank to look at, for a window too narrow to show the tree. */
static void open_chooser(void)
{
    const int which = ls_freqs_which();
    ls_freqs_scan();
    items_begin();
    item("ALL BANKS", ls_freqs_banks() ? "every file" : "no card", 0, which == LS_FREQS_ALL ? LS_DUI_CHECK : 0);
    for (int b = 0; b < ls_freqs_banks() && b < LS_FREQ_BANKS; b++) item(upper(ls_freqs_bank(b)), NULL, 1 + b, which == b ? LS_DUI_CHECK : 0);
    item("BUILT-IN", "no card needed", 1 + LS_FREQ_BANKS, which == LS_FREQS_BUILTIN ? LS_DUI_CHECK : 0);
    popup(K_OPEN, s_r_table.x + 2, s_r_table.y + 1, -1);
}

static void band_chooser(void)
{
    static const char *const RANGE[LS_FB__COUNT] = { "everything", "below 30 MHz", "30-300 MHz", "300-700 MHz",
                                                     "700-960 MHz", "960 MHz and up" };
    items_begin();
    for (int b = 0; b < LS_FB__COUNT; b++) item(ls_freq_band_name(b), RANGE[b], b, b == s_f.band ? LS_DUI_CHECK : 0);
    popup(K_BAND, s_r_filter.x + 28, s_r_filter.y + 1, -1);
}

static void group_chooser(void)
{
    char n[12];
    items_begin();
    snprintf(n, sizeof(n), "%d", ls_freqs_count());
    item("ALL GROUPS", n, 0, s_f.group[0] ? 0 : LS_DUI_CHECK);
    for (int g = 0; g < s_ngroups && s_nitems < ITEMS; g++) {
        snprintf(n, sizeof(n), "%d", s_group_n[g]);
        ls_dui_item_t *it = item(s_group[g][0] ? s_group[g] : "(none)", n, g + 1,
                                 s_f.group[0] && !strcmp(s_group[g], s_f.group) ? LS_DUI_CHECK : 0);
        it->hue = ls_freq_group_hue(s_group[g]);
    }
    popup(K_GROUP, s_r_filter.x + 40, s_r_filter.y + 1, -1);
}

static void mode_chooser(void)
{
    items_begin();
    const int now = ls_freq_mode_parse(s_fld[LS_FF_MODE].text);
    for (int m = 0; m < LS_FM__COUNT; m++) {
        const ls_freq_route_t r = ls_freq_route(m, 0);
        const ls_app_t *a = r.app ? ls_app_by_id(r.app) : NULL;
        item(ls_freq_mode_name(m), a ? a->name : "listed", m, m == now ? LS_DUI_CHECK : 0);
    }
    popup(K_MODE, s_fld_r[LS_FF_MODE].x, s_fld_r[LS_FF_MODE].y + 1, -1);
}

static void add_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_nfiles < 24) {
        const char *n = de->d_name, *dot = strrchr(n, '.');
        if (n[0] == '.' || !dot || strlen(dir) + strlen(n) + 2 > sizeof(s_files[0])) continue;
        if (strcasecmp(dot, ".lscan") && strcasecmp(dot, ".json") && strcasecmp(dot, ".txt") && strcasecmp(dot, ".csv")) continue;
        snprintf(s_files[s_nfiles++], sizeof(s_files[0]), "%s/%s", dir, n);
    }
    closedir(d);
}

static void import_chooser(void)
{
    char dir[96];
    s_nfiles = 0;
    snprintf(dir, sizeof(dir), "%s/import", ls_freqs_root());
    add_files(dir);
    add_files("/sdcard");
    if (!s_nfiles) { say("Nothing to import: put CHIRP, gqrx, SDR++ or .lscan files in freqs/import"); return; }
    items_begin();
    item("IMPORT INTO A NEW BANK", NULL, -1, LS_DUI_HEAD);
    for (int i = 0; i < s_nfiles; i++) {
        const char *base = strrchr(s_files[i], '/') + 1;
        item(base, strstr(s_files[i], "/import/") ? "freqs/import" : "card", i, 0);
    }
    popup(K_IMPORT, s_r_menu.x + 2, s_r_menu.y + 2, -1);
}

/* ------------------------------------------------------------ the dialog -- */

static void ask(int what, const char *line0, const char *line1, const char *field, int max)
{
    s_dlg = what;
    s_dlg_prompt = field != NULL;
    snprintf(s_dlg_text[0], sizeof(s_dlg_text[0]), "%s", line0);
    snprintf(s_dlg_text[1], sizeof(s_dlg_text[1]), "%s", line1 ? line1 : "");
    if (field) ls_dui_edit_set(&s_dlg_edit, field, max);
}

static int whole_view(void)
{
    memcpy(s_act, s_idx, (size_t)s_n * sizeof(s_idx[0]));
    return s_n;
}

static void load_scanner(int n)
{
    char path[128], msg[96];
    snprintf(path, sizeof(path), "%s/export/scanner.lscan", ls_freqs_root());
    if (!ls_freqs_export(s_act, n, path, true, msg, sizeof(msg))) { say(msg); return; }
    scan_import_file(path, msg, sizeof(msg));
    say(msg);
}

/* Where the cursor lands when what a command acts on leaves the list: the
   row after the last of them, or failing that the one before. */
static bool survivor(ls_freq_t *out)
{
    int after = s_sel;
    for (int k = 0; k < s_n; k++) if (picked(k)) after = k;
    const ls_freq_t *next = at(after + 1);
    for (int k = after - 1; !next && k >= 0; k--) if (!picked(k) && (s_npick || k != s_sel)) next = at(k);
    if (next) *out = *next;
    return next != NULL;
}

static void delete_now(void)
{
    char msg[96];
    const int n = targets();
    ls_freq_t keep;
    const bool some = survivor(&keep);
    if (ls_freqs_remove(s_act, n, msg, sizeof(msg)) > 0) {
        follow(some ? &keep : NULL);
        const size_t k = strlen(msg);
        snprintf(msg + k, sizeof(msg) - k, "; Ctrl+Z puts %s back", n == 1 ? "it" : "them");
    }
    say(msg);
}

static void clip_take(bool cut)
{
    char msg[96];
    const int n = targets();
    if (!n) { say("Nothing selected"); return; }
    heap_caps_free(s_clip);
    s_nclip = 0;
    s_clip = heap_caps_malloc((size_t)n * sizeof(ls_freq_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_clip) { say("No memory for that many"); return; }
    for (int k = 0; k < n; k++) {
        const ls_freq_t *e = ls_freqs_at(s_act[k]);
        if (e) s_clip[s_nclip++] = *e;
    }
    if (cut) { delete_now(); return; }
    snprintf(msg, sizeof(msg), "Copied %d; Ctrl+V pastes into the bank in view", s_nclip);
    say(msg);
}

static void clip_paste(void)
{
    char msg[96];
    if (!s_nclip) { say("Nothing cut or copied yet"); return; }
    if (ls_freqs_append_many(new_bank(), s_clip, s_nclip, msg, sizeof(msg))) {
        snprintf(msg, sizeof(msg), "Pasted %d into %s", s_nclip, new_bank());
        follow(&s_clip[0]);
    }
    say(msg);
}

static void send_to(const char *bank, bool move)
{
    char msg[96];
    const int n = targets(), to = ls_freqs_bank_find(bank);
    ls_freq_t keep;
    const bool some = move && survivor(&keep);
    const int went = move ? ls_freqd_move(s_act, n, bank, msg, sizeof(msg))
                          : ls_freqd_copy(s_act, n, bank, true, msg, sizeof(msg));
    if (went && move) follow(some ? &keep : NULL);
    /* The tree's count for the bank they went to, until it is next read. */
    if (went && to >= 0 && to < LS_FREQ_BANKS && s_count[to] >= 0) s_count[to] = (int16_t)(s_count[to] + went);
    say(msg);
}

static void dialog_done(bool yes)
{
    const int what = s_dlg;
    char msg[96] = "";
    s_dlg = D_NONE;
    if (!yes) return;
    switch (what) {
    case D_BANK_NEW:
        if (ls_freqd_bank_new(s_dlg_edit.text, msg, sizeof(msg))) {
            char name[24], why[8];
            ls_freqd_bank_name(s_dlg_edit.text, name, sizeof(name), why, sizeof(why));
            const int b = ls_freqs_bank_find(name);
            if (b >= 0) { ls_freqs_load(b); changed(); }
        }
        break;
    case D_BANK_RENAME: {
        const bool shown = ls_freqs_which() == s_dlg_bank;
        if (ls_freqd_bank_rename(s_dlg_bank, s_dlg_edit.text, msg, sizeof(msg)) && shown) {
            char name[24], why[8];
            ls_freqd_bank_name(s_dlg_edit.text, name, sizeof(name), why, sizeof(why));
            const int b = ls_freqs_bank_find(name);
            ls_freqs_load(b >= 0 ? b : LS_FREQS_ALL);
            changed();
        }
        break;
    }
    case D_BANK_TRASH:
        if (ls_freqd_bank_trash(s_dlg_bank, msg, sizeof(msg))) { ls_freqs_load(LS_FREQS_ALL); changed(); }
        break;
    case D_DELETE: delete_now(); return;
    case D_SCAN_VIEW: load_scanner(whole_view()); return;
    case D_SCAN_SEL: load_scanner(targets()); return;
    case D_COPY_NEW: case D_MOVE_NEW: {
        char name[24];
        if (ls_freqd_bank_name(s_dlg_edit.text, name, sizeof(name), msg, sizeof(msg))) { send_to(name, what == D_MOVE_NEW); return; }
        break;
    }
    default: break;
    }
    if (msg[0]) say(msg);
}

/* -------------------------------------------------------------- the form -- */

static void form_open(bool is_new, const ls_freq_t *from, const char *bank)
{
    s_form = true;
    s_form_new = is_new;
    s_form_at = is_new && !from->hz ? LS_FF_FREQ : LS_FF_NAME;
    s_form_bad = -1;
    s_form_base = *from;
    s_form_idx = selected() ? s_idx[s_sel] : -1;
    s_form_gen = ls_freqs_generation();
    snprintf(s_form_bank, sizeof(s_form_bank), "%s", bank);
    for (int f = 0; f < LS_FF__COUNT; f++) {
        char text[LS_FF_TEXT];
        ls_freqd_field_get(from, f, text, sizeof(text));
        ls_dui_edit_set(&s_fld[f], text, ls_freqd_field_max(f));
    }
    s_focus = F_TABLE;
}

/* A new entry goes where the window is looking, or into mine. */
static const char *new_bank(void)
{
    return ls_freqs_which() >= 0 ? ls_freqs_bank(ls_freqs_which()) : "mine";
}

static void form_new(const ls_freq_t *like)
{
    static EXT_RAM_BSS_ATTR ls_freq_t e;
    if (like) e = *like;
    else {
        ls_freqd_blank(&e);
        snprintf(e.group, sizeof(e.group), "%s", s_f.group[0] ? s_f.group : !strcmp(new_bank(), "mine") ? "MINE" : "");
    }
    const char *bank = like && like->bank < ls_freqs_banks() ? ls_freqs_bank(like->bank) : new_bank();
    form_open(true, &e, bank);
}

static void form_commit(void)
{
    ls_freq_t e = s_form_base;
    char why[96];
    for (int f = 0; f < LS_FF__COUNT; f++)
        if (!ls_freqd_field_set(&e, f, s_fld[f].text, why, sizeof(why))) {
            s_form_bad = s_form_at = f;
            say(why);
            return;
        }
    const bool lat = e.lat_e5 != LS_FREQ_NOWHERE, lon = e.lon_e5 != LS_FREQ_NOWHERE;
    if (lat != lon) {
        s_form_bad = s_form_at = lat ? LS_FF_LON : LS_FF_LAT;
        say("Lat and Lon go together: both, or neither for everywhere");
        return;
    }
    if (!lat && e.radius_km) {
        s_form_bad = s_form_at = LS_FF_RADIUS;
        say("Reach is from a place: give Lat and Lon, or leave it blank");
        return;
    }
    if (!e.name[0]) ls_freq_mhz_text(e.hz, e.name, sizeof(e.name));
    bool ok;
    if (s_form_new) ok = ls_freqs_append(s_form_bank, &e, why, sizeof(why));
    else if (s_form_gen != ls_freqs_generation()) {
        ok = false;
        snprintf(why, sizeof(why), "The list was reloaded under the edit; F2 again");
        s_form = false;
    } else ok = ls_freqs_rewrite(s_form_idx, &e, why, sizeof(why));
    say(why);
    if (!ok) return;
    s_form = false;
    follow(&e);
}

static bool form_key(ls_tk_t k, char ch)
{
    const bool shift = mods_now() & LS_IMOD_SHIFT;
    if (k == LS_TK_ESC) { s_form = false; say(s_form_new ? "Nothing added" : "Left as it was"); return true; }
    if (k == LS_TK_ENTER) { form_commit(); return true; }
    if (k == LS_TK_DOWN || (k == LS_TK_TAB && !shift)) { s_form_at = (s_form_at + 1) % LS_FF__COUNT; return true; }
    if (k == LS_TK_UP || k == LS_TK_TAB) { s_form_at = (s_form_at + LS_FF__COUNT - 1) % LS_FF__COUNT; return true; }
    if (k == LS_TK_F4 && s_form_at == LS_FF_MODE) { mode_chooser(); return true; }
    if (ls_dui_edit_key(&s_fld[s_form_at], k, ch)) { if (s_form_bad == s_form_at) s_form_bad = -1; return true; }
    return true;
}

static void form_draw(tui_surface *sf, tui_rect in)
{
    int rows = in.h - 2;
    if (rows < 1 || in.w < 24) return;
    if (rows > LS_FF__COUNT) rows = LS_FF__COUNT;
    const int cols = (LS_FF__COUNT + rows - 1) / rows, cw = in.w / cols;
    for (int f = 0; f < LS_FF__COUNT; f++) {
        const int x = in.x + (f / rows) * cw, y = in.y + f % rows;
        const bool on = f == s_form_at, bad = f == s_form_bad;
        tui_put_str(sf, in, x, y, ls_freqd_field_label(f),
                    bad ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK) : on ? AMBER : LS_ATTR_DIM);
        s_fld_r[f] = tui_rect_make(x + 7, y, cw - 8, 1);
        ls_dui_edit_draw(sf, in, x + 7, y, cw - 8, &s_fld[f], on, bad ? TUI_RED : on ? HUE : TUI_WHITE);
    }
    const int y = in.y + in.h - 2;
    char text[96];
    snprintf(text, sizeof(text), "%s%s", ls_freqd_field_hint(s_form_at), s_form_at == LS_FF_MODE ? "  (F4 lists them)" : "");
    tui_put_str(sf, in, in.x, y, text, LS_ATTR_DIM);
    int x = in.x;
    x += ls_dui_key(sf, in, x, y + 1, "Save  Enter", false, false, TUI_GREEN, &s_form_ok) + 1;
    x += ls_dui_key(sf, in, x, y + 1, "Cancel  Esc", false, false, TUI_RED, &s_form_no) + 2;
    snprintf(text, sizeof(text), "%s %s", s_form_new ? "into" : "in", upper(s_form_bank));
    tui_put_str(sf, in, x, y + 1, text, LS_ATTR_FAINT);
}

static bool form_tap(int col, int row)
{
    if (tui_rect_contains(s_form_ok, col, row)) { form_commit(); return true; }
    if (tui_rect_contains(s_form_no, col, row)) { s_form = false; return true; }
    for (int f = 0; f < LS_FF__COUNT; f++)
        if (tui_rect_contains(s_fld_r[f], col, row) || (row == s_fld_r[f].y && col >= s_fld_r[f].x - 7 && col < s_fld_r[f].x)) {
            /* A second click on Mode lists the modes. */
            if (f == LS_FF_MODE && s_form_at == f) { mode_chooser(); return true; }
            s_form_at = f;
            ls_dui_edit_click(&s_fld[f], s_fld_r[f].x, s_fld_r[f].w, col);
            return true;
        }
    return false;
}

/* -------------------------------------------------------------- commands -- */

static void tune(void)
{
    const ls_freq_t *e = selected();
    char msg[96];
    if (!e) { say("Nothing selected"); return; }
    ls_freqs_tune(e, msg, sizeof(msg));
    say(msg);
}

static void star(void)
{
    char msg[96] = "Nothing selected";
    int n = targets(), done = 0;
    if (!n) { say(msg); return; }
    /* Each is one rewrite of its file: a long list is done in pieces. */
    if (n > 64) n = 64;
    const bool on = !ls_freqd_starred(ls_freqs_at(s_act[0]));
    ls_freq_t keep = *selected();
    for (int k = 0; k < n; k++) {
        const ls_freq_t *e = ls_freqs_at(s_act[k]);
        if (!e || ls_freqd_starred(e) == on) continue;
        ls_freq_t with = *e;
        ls_freqd_set_star(&with, on);
        if (!ls_freqs_rewrite(s_act[k], &with, msg, sizeof(msg))) break;
        done++;
    }
    if (done) {
        follow(&keep);
        if (done > 1 || n > 1) snprintf(msg, sizeof(msg), "%s %d%s", on ? "Starred" : "Unstarred", done, targets() > 64 ? "; again for the rest" : "");
        else snprintf(msg, sizeof(msg), "%s %s", on ? "Starred" : "Unstarred", keep.name);
    }
    say(msg);
}

static void sort_by(int col)
{
    /* The same header again turns it round; a third time is the file's order. */
    if (s_sort != col) { s_sort = col; s_desc = false; }
    else if (!s_desc) s_desc = true;
    else { s_sort = LS_FC_LISTED; s_desc = false; }
    changed();
}

static void export_to(const char *suffix, int n, int kind)
{
    char path[128], msg[96];
    snprintf(path, sizeof(path), "%s/export/%s%s", ls_freqs_root(), view_stem(), suffix);
    if (kind == 2) ls_freqd_export_chirp(s_act, n, path, msg, sizeof(msg));
    else ls_freqs_export(s_act, n, path, kind == 1, msg, sizeof(msg));
    const size_t k = strlen(msg);
    if (k < sizeof(msg)) snprintf(msg + k, sizeof(msg) - k, " in freqs/export");
    say(msg);
}

static void find_focus(void)
{
    ls_dui_edit_set(&s_find, s_f.find, (int)sizeof(s_f.find) - 1);
    s_focus = F_FIND;
}

static void toggle_near(void)
{
    static int before = LS_FC_LISTED;
    s_f.near = !s_f.near;
    if (s_f.near) { before = s_sort; s_sort = LS_FC_DIST; s_desc = false; s_sel = 0; want(NULL); }
    else if (s_sort == LS_FC_DIST) s_sort = before;
    changed();
    say(!s_f.near ? "Near off: everything" : s_fix ? "Near: placed within reach, nearest first"
                                                   : "Near waits for a GPS fix; showing all");
}

static void run(int cmd)
{
    const ls_freq_t *e = selected();
    char text[72];
    uint32_t hz;
    if (cmd >= C_SORT && cmd < C_SORT + LS_FC__COUNT) { if (s_sort != cmd - C_SORT) sort_by(cmd - C_SORT); return; }
    if (cmd >= C_COL && cmd < C_COL + LS_FC__COUNT) { s_shown ^= 1u << (cmd - C_COL); return; }
    switch (cmd) {
    case C_TUNE: tune(); break;
    case C_NEW: form_new(NULL); break;
    case C_EDIT:
        if (!e) say("Nothing selected");
        else if (e->bank == LS_FREQ_BANK_BUILTIN) say("The built-in set cannot change; Duplicate makes one of your own");
        else form_open(false, e, ls_freqs_bank(e->bank));
        break;
    case C_DUP: if (e) form_new(e); else say("Nothing selected"); break;
    case C_DELETE: {
        const int n = targets();
        if (!n) say("Nothing selected");
        else if (n == 1) delete_now();
        else {
            snprintf(text, sizeof(text), "Delete %d entries from their files?", n);
            ask(D_DELETE, text, "Ctrl+Z puts them back.", NULL, 0);
        }
        break;
    }
    case C_UNDO: {
        char msg[96];
        ls_freqs_undo(msg, sizeof(msg));
        say(msg);
        break;
    }
    case C_STAR: star(); break;
    case C_ALL: pick_range(0, s_n - 1); break;
    case C_NONE_SEL: pick_none(); break;
    case C_COPY_TO: bank_chooser(K_COPY); break;
    case C_MOVE_TO: bank_chooser(K_MOVE); break;
    case C_COPY_MINE: if (targets()) send_to("mine", false); else say("Nothing selected"); break;
    case C_BANK_NEW: ask(D_BANK_NEW, "A new bank, a CSV file in freqs/", "Its name:", "", 19); break;
    case C_BANK_RENAME:
        if (ls_freqs_which() < 0) { say("Choose a bank on the card first"); break; }
        s_dlg_bank = ls_freqs_which();
        snprintf(text, sizeof(text), "Rename %s", bank_label(s_dlg_bank));
        ask(D_BANK_RENAME, text, "Its new name:", ls_freqs_bank(s_dlg_bank), 19);
        break;
    case C_BANK_TRASH:
        if (ls_freqs_which() < 0) { say("Choose a bank on the card first"); break; }
        s_dlg_bank = ls_freqs_which();
        snprintf(text, sizeof(text), "Move %s.csv to freqs/trash?", ls_freqs_bank(s_dlg_bank));
        ask(D_BANK_TRASH, text, "It leaves the list; the file is kept there.", NULL, 0);
        break;
    case C_CUT: clip_take(true); break;
    case C_COPY: clip_take(false); break;
    case C_PASTE: clip_paste(); break;
    case C_IMPORT: import_chooser(); break;
    case C_OPEN: open_chooser(); break;
    case C_EXP_CSV: export_to(".csv", whole_view(), 0); break;
    case C_EXP_SEL: export_to("-selection.csv", targets(), 0); break;
    case C_EXP_CHIRP: export_to("-chirp.csv", whole_view(), 2); break;
    case C_EXP_LSCAN: export_to(".lscan", whole_view(), 1); break;
    case C_RELOAD: ls_freqs_scan(); ls_freqs_load(ls_freqs_which()); say("Reading the card again"); break;
    case C_FIND: find_focus(); break;
    case C_BAND: band_chooser(); break;
    case C_GROUP: group_chooser(); break;
    case C_NEAR: toggle_near(); break;
    case C_ONLY_STAR: s_only_star = !s_only_star; changed(); break;
    case C_ONLY_DUP:
        s_only_dup = !s_only_dup;
        changed();
        if (s_only_dup) say("The same frequency and mode in the same place, listed more than once");
        break;
    case C_CLEAR:
        s_f.band = LS_FB_ALL; s_f.group[0] = s_f.find[0] = 0; s_f.near = false;
        s_only_star = s_only_dup = false;
        if (s_focus == F_FIND) s_focus = F_TABLE;
        changed();
        break;
    case C_PANE_BANKS: s_show_banks = !s_show_banks; if (s_focus == F_BANKS) s_focus = F_TABLE; break;
    case C_PANE_DETAIL: s_show_detail = !s_show_detail; break;
    case C_PANE_RULER: s_show_ruler = !s_show_ruler; break;
    case C_SORT_LISTED: s_sort = LS_FC_LISTED; s_desc = false; changed(); break;
    case C_SORT_FLIP: s_desc = !s_desc; changed(); break;
    case C_SAVE_FM: case C_SAVE_P25: {
        static EXT_RAM_BSS_ATTR ls_freq_t n;
        if (!app_freq(cmd == C_SAVE_FM ? "fm.freq" : "p25.freq", &hz)) { say(cmd == C_SAVE_FM ? "FM is not running" : "P25 is not running"); break; }
        ls_freqd_blank(&n);
        n.hz = hz;
        n.bank = LS_FREQ_BANK_BUILTIN;                 /* no bank of its own: form_new picks */
        snprintf(n.group, sizeof(n.group), "MINE");
        snprintf(n.tags, sizeof(n.tags), "%s", cmd == C_SAVE_FM ? "fm" : "p25");
        n.mode = LS_FM_P25;
        if (cmd == C_SAVE_FM) {
            ls_val_t m;
            const int mode = ls_value_read("fm.mode", &m, NULL) && m.kind == LS_VAL_TEXT ? ls_freq_mode_parse(m.s) : -1;
            n.mode = (uint8_t)(mode >= 0 ? mode : LS_FM_NFM);
        }
        form_new(&n);
        break;
    }
    case C_SCAN_VIEW:
        snprintf(text, sizeof(text), "Replace FM's scan list with the view (%d)?", s_n);
        ask(D_SCAN_VIEW, text, "The scanner takes the NFM and P25 ones.", NULL, 0);
        break;
    case C_SCAN_SEL:
        if (!targets()) { say("Nothing selected"); break; }
        snprintf(text, sizeof(text), "Replace FM's scan list with the selection (%d)?", targets());
        ask(D_SCAN_SEL, text, "The scanner takes the NFM and P25 ones.", NULL, 0);
        break;
    default: break;
    }
}

static void chosen(int kind, int id)
{
    char msg[96];
    switch (kind) {
    case K_MENU: run(id); break;
    case K_COPY: case K_MOVE:
        if (id >= LS_FREQ_BANKS)
            ask(kind == K_MOVE ? D_MOVE_NEW : D_COPY_NEW, kind == K_MOVE ? "Move to a new bank" : "Copy to a new bank", "Its name:", "", 19);
        else if (id < ls_freqs_banks()) send_to(ls_freqs_bank(id), kind == K_MOVE);
        break;
    case K_IMPORT:
        if (id < 0 || id >= s_nfiles) break;
        if (ls_freqs_import(s_files[id], msg, sizeof(msg))) {
            const int b = ls_freqs_bank_find(ls_freqs_imported());
            ls_freqs_load(b >= 0 ? b : LS_FREQS_ALL);
            changed();
        }
        say(msg);
        break;
    case K_GROUP:
        if (id == 0) s_f.group[0] = 0;
        else if (id <= s_ngroups) snprintf(s_f.group, sizeof(s_f.group), "%s", s_group[id - 1]);
        changed();
        break;
    case K_BAND: if (id < LS_FB__COUNT) { s_f.band = (uint8_t)id; changed(); } break;
    case K_OPEN:
        ls_freqs_load(id == 0 ? LS_FREQS_ALL : id > LS_FREQ_BANKS ? LS_FREQS_BUILTIN : id - 1);
        want(NULL);
        s_sel = 0;
        changed();
        break;
    case K_MODE:
        if (s_form && id < LS_FM__COUNT) ls_dui_edit_set(&s_fld[LS_FF_MODE], ls_freq_mode_name(id), ls_freqd_field_max(LS_FF_MODE));
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- layout -- */

static void layout(tui_rect a)
{
    int y = a.y;
    const tui_rect none = tui_rect_make(0, 0, 0, 0);
    s_r_menu = tui_rect_make(a.x, y++, a.w, 1);
    s_r_tools = none;
    if (a.h >= 20) s_r_tools = tui_rect_make(a.x, y++, a.w, 1);
    s_r_filter = tui_rect_make(a.x, y++, a.w, 1);
    s_r_ruler = none;
    if (s_show_ruler && a.h >= 24) { s_r_ruler = tui_rect_make(a.x + 1, y, a.w - 2, 3); y += 3; }
    s_r_status = tui_rect_make(a.x, a.y + a.h - 1, a.w, 1);
    const tui_rect body = tui_rect_make(a.x, y, a.w, a.y + a.h - 1 - y);
    const int bw = s_show_banks && a.w >= 84 ? (a.w >= 140 ? 26 : 22) : 0;
    int dw = 0, dh = 0;
    if (s_show_detail || s_form) {
        if (a.w - bw >= 98) { dw = a.w / 4; if (dw < 34) dw = 34; if (dw > 44) dw = 44; }
        else if (body.h >= 20) dh = 8;
    }
    /* Panels side by side share the line between them. */
    const int tx = bw ? body.x + bw - 1 : body.x;
    s_r_banks = bw ? tui_rect_make(body.x, body.y, bw, body.h) : none;
    s_r_table = tui_rect_make(tx, body.y, body.x + body.w - tx - (dw ? dw - 1 : 0), body.h - (dh ? dh - 1 : 0));
    s_r_detail = dw ? tui_rect_make(body.x + body.w - dw, body.y, dw, body.h)
               : dh ? tui_rect_make(tx, body.y + body.h - dh, body.x + body.w - tx, dh) : none;
    uint32_t shown = s_shown | 0xF;                       /* star, MHz, name and mode always */
    if (ls_freqs_which() != LS_FREQS_ALL) shown &= ~(1u << LS_FC_BANK);
    ls_dui_table_layout(&s_t, tui_rect_make(s_r_table.x + 1, s_r_table.y + 1, s_r_table.w - 2, s_r_table.h - 2),
                        COLS, LS_FC__COUNT, shown);
}

static void hit(tui_rect r, int cmd)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) { s_hit[s_nhit].r = r; s_hit[s_nhit].cmd = (int16_t)cmd; s_nhit++; }
}

static int tool(tui_surface *sf, tui_rect row, int x, const char *label, int cmd, bool on, bool off, uint8_t hue)
{
    tui_rect r;
    if (x + (int)strlen(label) + 2 > row.x + row.w) return 0;
    const int w = ls_dui_key(sf, row, x, row.y, label, on, off, hue, &r);
    if (!off) hit(r, cmd);
    return w + 1;
}

/* ------------------------------------------------------------------ draw -- */

static void draw_menu(tui_surface *sf)
{
    ls_dui_bar_draw(sf, s_r_menu, MENUS, NMENUS, s_pop.open ? s_pop.bar : -1, HUE, s_bar_x);
    char text[64];
    snprintf(text, sizeof(text), "FREQS / %s", bank_label(ls_freqs_which()));
    const int x = s_r_menu.x + s_r_menu.w - (int)strlen(text) - 1;
    if (x > s_bar_x[NMENUS] + 2) tui_put_str(sf, s_r_menu, x, s_r_menu.y, text, TUI_ATTR(HUE, TUI_BLACK));
}

static void draw_tools(tui_surface *sf)
{
    const tui_rect r = s_r_tools;
    if (!r.h) return;
    const ls_freq_t *e = selected();
    const int n = s_npick ? s_npick : e ? 1 : 0;
    const bool file = e && e->bank != LS_FREQ_BANK_BUILTIN, one = e && s_npick <= 1;
    int x = r.x + 1;
    x += tool(sf, r, x, "New", C_NEW, false, false, TUI_GREEN);
    x += tool(sf, r, x, "Edit", C_EDIT, false, !one || !file, HUE);
    x += tool(sf, r, x, "Duplicate", C_DUP, false, !one, HUE);
    x += tool(sf, r, x, "Delete", C_DELETE, false, !n, TUI_RED);
    x += tool(sf, r, x, "Undo", C_UNDO, false, !ls_freqs_undo_count(), TUI_RED);
    x += 1;
    x += tool(sf, r, x, "Tune", C_TUNE, false, !one || ls_freq_route(e->mode, e->hz).target == LS_FR_NONE, TUI_CYAN);
    x += tool(sf, r, x, "Star", C_STAR, false, !n, HUE);
    x += 1;
    x += tool(sf, r, x, "Copy to", C_COPY_TO, false, !n, TUI_MAGENTA);
    x += tool(sf, r, x, "Move to", C_MOVE_TO, false, !n, TUI_MAGENTA);
    x += tool(sf, r, x, "Export", C_EXP_CSV, false, !s_n, TUI_GREEN);
}

static void draw_filter(tui_surface *sf)
{
    const tui_rect r = s_r_filter;
    char text[48];
    int x = r.x + 1;
    tui_put_str(sf, r, x, r.y, "Find", s_focus == F_FIND ? AMBER : LS_ATTR_DIM);
    x += 5;
    const int fw = r.w >= 110 ? 24 : 16;
    s_r_find = tui_rect_make(x, r.y, fw, 1);
    if (s_focus == F_FIND) ls_dui_edit_draw(sf, r, x, r.y, fw, &s_find, true, HUE);
    else {
        ls_dui_edit_t shown;
        ls_dui_edit_set(&shown, s_f.find, (int)sizeof(s_f.find) - 1);
        ls_dui_edit_draw(sf, r, x, r.y, fw, &shown, false, s_f.find[0] ? HUE : TUI_WHITE);
    }
    x += fw + 1;
    snprintf(text, sizeof(text), "Band %s", ls_freq_band_name(s_f.band));
    x += tool(sf, r, x, text, C_BAND, s_f.band != LS_FB_ALL, false, TUI_CYAN);
    snprintf(text, sizeof(text), "Group %s", s_f.group[0] ? s_f.group : "ALL");
    x += tool(sf, r, x, text, C_GROUP, s_f.group[0] != 0, false, s_f.group[0] ? ls_freq_group_hue(s_f.group) : TUI_CYAN);
    x += tool(sf, r, x, !s_f.near ? "Near" : s_fix ? "Near ON" : "Near: no fix", C_NEAR, s_f.near && s_fix, false, TUI_GREEN);
    x += tool(sf, r, x, "Starred", C_ONLY_STAR, s_only_star, false, HUE);
    snprintf(text, sizeof(text), s_ndup && !s_only_dup ? "Dups %d" : "Dups", s_ndup);
    x += tool(sf, r, x, text, C_ONLY_DUP, s_only_dup, false, TUI_RED);
    if (s_f.band != LS_FB_ALL || s_f.group[0] || s_f.find[0] || s_f.near || s_only_star || s_only_dup)
        x += tool(sf, r, x, "Clear", C_CLEAR, false, false, TUI_WHITE);
}

/* The strip's ends in MHz for the band the view is cut to. */
static void rule_span(float *lo, float *hi)
{
    static const float SPAN[LS_FB__COUNT][2] = { { 1, 3000 }, { 1.5f, 30 }, { 30, 300 }, { 300, 700 }, { 700, 960 }, { 960, 6000 } };
    *lo = SPAN[s_f.band < LS_FB__COUNT ? s_f.band : 0][0];
    *hi = SPAN[s_f.band < LS_FB__COUNT ? s_f.band : 0][1];
}

static int rule_x(float mhz, int w)
{
    float lo, hi;
    rule_span(&lo, &hi);
    if (mhz < lo) mhz = lo;
    if (mhz > hi) mhz = hi;
    return (int)(logf(mhz / lo) / logf(hi / lo) * (float)(w - 1) + 0.5f);
}

static void draw_ruler(tui_surface *sf)
{
    const tui_rect r = s_r_ruler;
    if (!r.h) return;
    const int w = r.w > 384 ? 384 : r.w;
    if (s_rule_stale || w != s_rule_w) {
        memset(s_rule_n, 0, sizeof(s_rule_n));
        for (int k = 0; k < s_n; k++) {
            const ls_freq_t *e = ls_freqs_at(s_idx[k]);
            if (!e) continue;
            const int x = rule_x((float)e->hz * 1e-6f, w);
            if (!s_rule_n[x]) s_rule_hue[x] = ls_freq_group_hue(e->group);
            if (s_rule_n[x] < 255) s_rule_n[x]++;
        }
        s_rule_w = w;
        s_rule_stale = false;
    }
    /* The scale: decades and thirds, as far as they do not run together. */
    static const float MARK[] = { 1, 3, 10, 30, 50, 100, 150, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1500, 2000, 3000, 6000 };
    float lo, hi;
    rule_span(&lo, &hi);
    int free_x = r.x;
    for (unsigned i = 0; i < sizeof(MARK) / sizeof(MARK[0]); i++) {
        if (MARK[i] < lo || MARK[i] > hi) continue;
        char t[8];
        if (MARK[i] >= 1000) snprintf(t, sizeof(t), MARK[i] == 1500 ? "1.5G" : "%dG", (int)MARK[i] / 1000);
        else snprintf(t, sizeof(t), "%d", (int)MARK[i]);
        /* The right end is the unit's. */
        const int x = r.x + rule_x(MARK[i], w);
        if (x < free_x || x + (int)strlen(t) > r.x + w - 4) continue;
        tui_put_str(sf, r, x, r.y, t, TUI_ATTR(TUI_CYAN, TUI_BLACK));
        free_x = x + (int)strlen(t) + 2;
    }
    tui_put_str(sf, r, r.x + w - 3, r.y, "MHz", LS_ATTR_FAINT);
    /* One cell a slice of the dial: a line, taller the more entries of the
       view fall in it, in the colour of the first one's group. */
    for (int x = 0; x < w; x++) {
        const int n = s_rule_n[x];
        const uint8_t ink = TUI_ATTR(s_rule_hue[x] | TUI_BRIGHT, TUI_BLACK);
        if (!n) ls_box_put(sf, r, r.x + x, r.y + 1, LS_BOX_DASH_H, LS_ATTR_FAINT);
        else if (n == 1) ls_box_put(sf, r, r.x + x, r.y + 1, LS_BOX_HALF_D, ink);
        else if (n < 4) ls_box_put(sf, r, r.x + x, r.y + 1, LS_BOX_V, ink);
        else ls_box_put(sf, r, r.x + x, r.y + 1, LS_BOX_DBL_V, ink);
    }
    /* Under it: the entry the cursor is on, and what FM and P25 are tuned
       to now, when they are running. */
    const ls_freq_t *e = selected();
    char text[3][24];
    int at[3], n = 0;
    uint8_t ink[3];
    uint32_t hz;
    if (e) {
        at[n] = r.x + rule_x((float)e->hz * 1e-6f, w);
        ls_freq_mhz_text(e->hz, text[n], sizeof(text[n]));
        ink[n++] = AMBER;
    }
    static const struct { const char *path, *name; uint8_t hue; } LIVE[2] = { { "fm.freq", "FM", TUI_GREEN }, { "p25.freq", "P25", TUI_MAGENTA } };
    for (int i = 0; i < 2; i++) {
        char mhz[16];
        if (!app_freq(LIVE[i].path, &hz)) continue;
        at[n] = r.x + rule_x((float)hz * 1e-6f, w);
        ls_freq_mhz_text(hz, mhz, sizeof(mhz));
        snprintf(text[n], sizeof(text[n]), "%s %s", LIVE[i].name, mhz);
        ink[n] = TUI_ATTR(LIVE[i].hue | TUI_BRIGHT, TUI_BLACK);
        ls_box_put(sf, r, at[n], r.y + 1, LS_BOX_VH, ink[n]);
        n++;
    }
    /* Each pointer with its words beside it, on whichever side is clear of
       the ones already written; with no room, the pointer alone. */
    int x0[3], x1[3];
    for (int i = 0; i < n; i++) { x0[i] = at[i]; x1[i] = at[i] + 1; }
    for (int i = 0; i < n; i++) {
        const int len = (int)strlen(text[i]);
        int a = at[i], b = at[i] + 1;
        for (int side = 0; side < 2; side++) {
            const int ta = side == 0 ? at[i] + 1 : at[i] - len, tb = ta + len;
            bool clear = ta >= r.x && tb <= r.x + w;
            for (int j = 0; j < n && clear; j++) clear = j == i || tb + 1 <= x0[j] || ta >= x1[j] + 1;
            if (!clear) continue;
            tui_put_str(sf, r, ta, r.y + 2, text[i], ink[i]);
            if (ta < a) a = ta;
            if (tb > b) b = tb;
            break;
        }
        x0[i] = a;
        x1[i] = b;
        ls_box_put(sf, r, at[i], r.y + 2, LS_SHAPE_UP, ink[i]);
    }
}

static void tree_row(tui_surface *sf, tui_rect in, int y, int kind, int arg, const char *name, int count, uint8_t hue, bool on)
{
    const int row = y - s_tree_y;
    if (row < 0 || row >= (int)(sizeof(s_tree) / sizeof(s_tree[0])) || y >= in.y + in.h) return;
    s_tree[row].kind = (uint8_t)kind;
    s_tree[row].arg = (int8_t)arg;
    if (row >= s_ntree) s_ntree = row + 1;
    const bool cursor = s_focus == F_BANKS && row == s_bank_cur;
    const bool drop = s_drag.on && s_drag.drop == row && kind == T_BANK;
    if (drop || cursor) ls_fill_dither(sf, tui_rect_make(in.x, y, in.w, 1), drop ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, drop ? TUI_MAGENTA : HUE);
    if (on) ls_box_put(sf, in, in.x, y, LS_SHAPE_RIGHT, AMBER);
    char n[8] = "";
    if (count >= 0) snprintf(n, sizeof(n), "%d", count);
    const int nw = (int)strlen(n);
    tui_put_str(sf, tui_rect_make(in.x, y, in.w - nw - 1, 1), in.x + 2, y, name,
                TUI_ATTR(on ? hue | TUI_BRIGHT : hue, TUI_BLACK));
    tui_put_str(sf, in, in.x + in.w - nw, y, n, on ? WHITE : LS_ATTR_DIM);
}

static void draw_banks(tui_surface *sf)
{
    const tui_rect r = s_r_banks;
    s_ntree = 0;
    if (!r.w) return;
    ls_panel_box(sf, r, "BANKS", s_focus == F_BANKS ? HUE | TUI_BRIGHT : HUE);
    const tui_rect in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    memset(s_tree, 0, sizeof(s_tree));
    s_tree_y = in.y;
    const int which = ls_freqs_which();
    int y = in.y, all = 0;
    bool known = ls_freqs_banks() > 0;
    for (int b = 0; b < ls_freqs_banks(); b++) { if (s_count[b] < 0) known = false; else all += s_count[b]; }
    tree_row(sf, in, y++, T_ALL, 0, ls_freqs_banks() ? "ALL BANKS" : "NO CARD", known ? all : -1, TUI_WHITE, which == LS_FREQS_ALL);
    for (int b = 0; b < ls_freqs_banks(); b++)
        tree_row(sf, in, y++, T_BANK, b, upper(ls_freqs_bank(b)), s_count[b], !strcmp(ls_freqs_bank(b), "mine") ? HUE : TUI_CYAN, which == b);
    tree_row(sf, in, y++, T_BUILTIN, 0, "BUILT-IN", -1, TUI_GREEN, which == LS_FREQS_BUILTIN);
    if (y + 2 >= in.y + in.h) return;
    for (int x = 0; x < in.w; x++) ls_box_put(sf, in, in.x + x, y, LS_BOX_H, LS_ATTR_FAINT);
    tui_put_str(sf, in, in.x + 1, y++, " GROUPS ", TUI_ATTR(HUE, TUI_BLACK));
    for (int g = 0; g < s_ngroups; g++) {
        if (y == in.y + in.h - 1 && g + 1 < s_ngroups) {
            char more[24];
            snprintf(more, sizeof(more), "+%d more: G", s_ngroups - g);
            tui_put_str(sf, in, in.x + 2, y, more, LS_ATTR_FAINT);
            break;
        }
        const bool on = s_f.group[0] && !strcmp(s_f.group, s_group[g]);
        tree_row(sf, in, y++, T_GROUP, g, s_group[g][0] ? s_group[g] : "(none)", s_group_n[g], ls_freq_group_hue(s_group[g]), on);
    }
}

static void draw_table(tui_surface *sf)
{
    const tui_rect r = s_r_table;
    char title[64];
    const ls_freq_tally_t *t = ls_freqs_tally();
    if (ls_freqs_loading()) snprintf(title, sizeof(title), "reading the card... %d", ls_freqs_count());
    else if (t->truncated) snprintf(title, sizeof(title), "%d of %d; bank cut at %d", s_n, ls_freqs_count(), LS_FREQ_MAX);
    else if (t->skipped) snprintf(title, sizeof(title), "%d of %d, %d line%s skipped (%d)", s_n, ls_freqs_count(), t->skipped,
                                  t->skipped == 1 ? "" : "s", t->first_bad);
    else snprintf(title, sizeof(title), "%d of %d", s_n, ls_freqs_count());
    ls_panel_box(sf, r, title, s_focus == F_TABLE && !s_form ? HUE | TUI_BRIGHT : HUE);
    const int sort = s_only_dup && s_sort == LS_FC_LISTED ? LS_FC_FREQ : s_sort;
    ls_dui_table_head(sf, &s_t, COLS, LS_FC__COUNT, sort, s_desc, HUE);
    if (!s_n) {
        const char *why = ls_freqs_loading() ? "Reading the card..." :
            !ls_freqs_count() ? "No frequencies here yet." :
            s_f.near && s_fix ? "Nothing placed within reach of here." :
            s_f.near ? "Near waits for a GPS fix." : "Nothing matches the filters.";
        tui_put_str(sf, s_t.body, s_t.body.x + 2, s_t.body.y + 1, why, LS_ATTR_DIM);
        tui_put_str(sf, s_t.body, s_t.body.x + 2, s_t.body.y + 3,
                    !ls_freqs_count() ? "Ins adds one; File has Import." : "Esc clears Find; Clear drops every filter.", LS_ATTR_DIM);
        return;
    }
    if (!s_free) ls_dui_table_follow(&s_t, s_sel, s_n);
    else ls_dui_table_clamp(&s_t, s_n);
    for (int row = 0; row < s_t.rows && s_t.first + row < s_n; row++) {
        const int k = s_t.first + row, y = s_t.body.y + row;
        const ls_freq_t *e = at(k);
        if (!e) continue;
        const bool on = k == s_sel, tick = picked(k);
        if (tick) ls_fill_dither(sf, tui_rect_make(s_t.body.x, y, s_t.body.w, 1), LS_DITHER_LIGHT, TUI_CYAN);
        if (on) ls_fill_dither(sf, tui_rect_make(s_t.body.x, y, s_t.body.w, 1), LS_DITHER_MEDIUM, HUE);
        if (on) ls_box_put(sf, s_t.body, s_t.body.x, y, LS_SHAPE_RIGHT, s_focus == F_TABLE ? AMBER : LS_ATTR_DIM);
        else if (tick) ls_box_put(sf, s_t.body, s_t.body.x, y, LS_SHAPE_CHECK, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        char text[64];
        const uint8_t ink = on || tick ? WHITE : TUI_ATTR(TUI_WHITE, TUI_BLACK);
        const bool starred = ls_freqd_starred(e), dup = ls_dui_bit(s_dup, s_idx[k]);
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_STAR, row, starred ? "*" : ".", starred ? AMBER : LS_ATTR_FAINT);
        ls_freq_mhz_text(e->hz, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_FREQ, row, text,
                          dup ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK) : on ? AMBER : WHITE);
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_NAME, row, e->name, ink);
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_MODE, row, ls_freq_mode_name(e->mode), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        const int band = ls_freqd_band(e->hz);
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_BAND, row, band ? ls_freq_band_name(band) : "-", LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_GROUP, row, e->group,
                          TUI_ATTR(ls_freq_group_hue(e->group) | (on || tick ? TUI_BRIGHT : 0), TUI_BLACK));
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_TONE, row, e->tone, TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
        if (e->lat_e5 != LS_FREQ_NOWHERE) {
            distance_text(e, text, sizeof(text));
            ls_dui_table_cell(sf, &s_t, COLS, LS_FC_DIST, row, text, s_fix ? TUI_ATTR(HUE, TUI_BLACK) : LS_ATTR_FAINT);
        }
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_BANK, row, ls_freqs_bank(e->bank), TUI_ATTR(TUI_GREEN, TUI_BLACK));
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_TAGS, row, e->tags, LS_ATTR_DIM);
        ls_dui_table_cell(sf, &s_t, COLS, LS_FC_NOTES, row, e->notes, LS_ATTR_DIM);
    }
    ls_dui_table_strip(sf, &s_t, s_n, HUE);
}

static void draw_detail(tui_surface *sf, tui_rect r)
{
    if (!r.w) return;
    if (s_form) {
        ls_panel_box(sf, r, s_form_new ? "NEW ENTRY" : "EDIT", TUI_GREEN | TUI_BRIGHT);
        form_draw(sf, tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2));
        return;
    }
    const ls_freq_t *e = selected();
    ls_panel_box(sf, r, s_npick > 1 ? "SELECTION" : "THIS ONE", HUE);
    const tui_rect in = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    char text[96], mhz[16];
    int y = in.y;
    if (s_npick > 1) {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (int k = 0; k < s_n; k++) {
            if (!picked(k)) continue;
            const ls_freq_t *p = at(k);
            if (p && p->hz < lo) lo = p->hz;
            if (p && p->hz > hi) hi = p->hz;
        }
        snprintf(text, sizeof(text), "%d entries", s_npick);
        tui_put_str(sf, in, in.x, y++, text, WHITE);
        ls_freq_mhz_text(lo, mhz, sizeof(mhz));
        const size_t k = (size_t)snprintf(text, sizeof(text), "%s to ", mhz);
        ls_freq_mhz_text(hi, text + k, sizeof(text) - k);
        tui_put_str(sf, in, in.x, y++, text, AMBER);
        y++;
        static const char *const HELP[] = { "Del       delete, Ctrl+Z puts back", "*         star or unstar",
                                            "drag      onto a bank moves them", "right key copy, move, export, scan" };
        for (int i = 0; i < 4 && y < in.y + in.h; i++) tui_put_str(sf, in, in.x, y++, HELP[i], LS_ATTR_DIM);
        return;
    }
    if (!e) {
        if (!ls_freqs_loading()) tui_put_str(sf, in, in.x, y, "Nothing here: Ins adds one", LS_ATTR_DIM);
        return;
    }
    /* The dial with its needle where this one sits, as on the handheld. */
    const bool art = in.w >= 30 && in.h >= 5;
    if (art) ls_freqs_dial(sf, in, in.x + in.w - 11, in.y, e, ls_freqs_loading());
    const tui_rect left = tui_rect_make(in.x, in.y, art ? in.w - 12 : in.w, in.h);
    tui_put_str(sf, left, in.x, y++, e->name, WHITE);
    ls_freq_mhz_text(e->hz, mhz, sizeof(mhz));
    snprintf(text, sizeof(text), "%s MHz", mhz);
    tui_put_str(sf, left, in.x, y++, text, AMBER);
    int x = in.x;
    x = ls_radio_chip(sf, left, x, y, ls_freq_mode_name(e->mode), true, TUI_CYAN);
    if (ls_freqd_starred(e)) x = ls_radio_chip(sf, left, x, y, "STAR", true, HUE);
    if (strstr(e->tags, "tx-ok")) x = ls_radio_chip(sf, left, x, y, "TX OK", true, TUI_GREEN);
    y++;
    ls_radio_chip(sf, left, in.x, y++, e->group[0] ? e->group : "NO GROUP", e->group[0] != 0, ls_freq_group_hue(e->group));
    /* The rest beside the first four lines when the pane is a wide strip. */
    int kx = in.x, kw = in.w;
    if (in.w >= 84) { kx = in.x + 40; kw = in.w - 40 - (art ? 12 : 0); y = in.y; }
    else y++;
    const tui_rect kv = tui_rect_make(kx, in.y, kw, in.h);
    const ls_freq_route_t route = ls_freq_route(e->mode, e->hz);
    const ls_app_t *app = route.app ? ls_app_by_id(route.app) : NULL;
    snprintf(text, sizeof(text), "TUNE   %s%s%s", app ? app->name : "nowhere here", route.arg ? " " : "", route.arg ? route.arg : "");
    for (char *p = text; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (y < in.y + in.h) tui_put_str(sf, kv, kx, y++, text, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (route.note && y < in.y + in.h) tui_put_str(sf, kv, kx + 7, y++, route.note, LS_ATTR_DIM);
    char place[48] = "", dist[24] = "", bw[16] = "", line[24];
    if (e->lat_e5 != LS_FREQ_NOWHERE) {
        char lat[16], lon[16];
        ls_freqd_field_get(e, LS_FF_LAT, lat, sizeof(lat));
        ls_freqd_field_get(e, LS_FF_LON, lon, sizeof(lon));
        snprintf(place, sizeof(place), "%s, %s", lat, lon);
        distance_text(e, dist, sizeof(dist));
        if (e->radius_km) { const size_t k = strlen(dist); snprintf(dist + k, sizeof(dist) - k, ", reach %u", e->radius_km); }
    }
    ls_freqd_field_get(e, LS_FF_BW, bw, sizeof(bw));
    if (bw[0]) { const size_t k = strlen(bw); snprintf(bw + k, sizeof(bw) - k, " kHz"); }
    snprintf(line, sizeof(line), "%s%s", ls_freqs_bank(e->bank), "");
    if (e->line) { const size_t k = strlen(line); snprintf(line + k, sizeof(line) - k, " line %u", e->line); }
    const char *label[7] = { "tone", "bw", "tags", "notes", "place", "from", "bank" };
    const char *value[7] = { e->tone, bw, e->tags, e->notes, place, dist, line };
    const uint8_t ink[7] = { TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK), TUI_ATTR(TUI_WHITE, TUI_BLACK), TUI_ATTR(TUI_WHITE, TUI_BLACK),
                             TUI_ATTR(TUI_WHITE, TUI_BLACK), TUI_ATTR(TUI_WHITE, TUI_BLACK),
                             s_fix ? TUI_ATTR(HUE, TUI_BLACK) : LS_ATTR_DIM, TUI_ATTR(TUI_GREEN, TUI_BLACK) };
    for (int i = 0; i < 7 && y < in.y + in.h; i++) {
        if (!value[i][0]) continue;
        tui_put_str(sf, kv, kx, y, label[i], LS_ATTR_FAINT);
        /* A note longer than the pane goes on to the next lines. */
        const char *p = value[i];
        const int room = kw - 7;
        do {
            snprintf(text, sizeof(text), "%.*s", room > 0 ? room : 1, p);
            tui_put_str(sf, kv, kx + 7, y++, text, ink[i]);
            p += strlen(text);
        } while (*p && i == 3 && y < in.y + in.h);
    }
    if (ls_dui_bit(s_dup, s_idx[s_sel]) && y < in.y + in.h)
        tui_put_str(sf, kv, kx, y, "listed more than once in this view", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
}

static void draw_status(tui_surface *sf)
{
    const tui_rect r = s_r_status;
    char text[160];
    size_t k = (size_t)snprintf(text, sizeof(text), " %d of %d", s_n, ls_freqs_count());
    if (s_npick && k < sizeof(text)) k += (size_t)snprintf(text + k, sizeof(text) - k, "  %d selected", s_npick);
    if (ls_freqs_undo_count() && k < sizeof(text)) k += (size_t)snprintf(text + k, sizeof(text) - k, "  undo %d", ls_freqs_undo_count());
    if (k >= sizeof(text)) k = sizeof(text) - 1;
    tui_put_str(sf, r, r.x, r.y, text, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    const int x = r.x + (int)k + 2;
    const bool fresh = s_say[0] && esp_timer_get_time() - s_said_us < 8000000;
    const ls_freq_t *e = selected();
    const char *idle = s_drag.on && s_drag.drop >= 0 ? "Let go to move them into this bank"
                     : s_form ? "Tab or the arrows move between fields; Enter saves, Esc leaves"
                     : s_focus == F_FIND ? "Type to filter: name, tag, group, mode or MHz; Enter keeps it, Esc clears"
                     : s_focus == F_BANKS ? "Up and Down, Enter opens the bank; Tab back to the list"
                     : e && ls_freq_route(e->mode, e->hz).note ? ls_freq_route(e->mode, e->hz).note
                     : "Enter tunes  F2 edits  Ins adds  Del deletes  Space ticks  right button for more";
    tui_put_str(sf, tui_rect_make(x, r.y, r.x + r.w - x, 1), x, r.y, fresh && !s_drag.on ? s_say : idle, fresh && !s_drag.on ? AMBER : LS_ATTR_DIM);
}

static void draw_dialog(tui_surface *sf)
{
    const tui_rect a = s_area;
    int w = (int)strlen(s_dlg_text[0]), w1 = (int)strlen(s_dlg_text[1]);
    if (w1 > w) w = w1;
    w += 6;
    if (w < 40) w = 40;
    if (w > a.w - 2) w = a.w - 2;
    const int h = s_dlg_prompt ? 8 : 7;
    const tui_rect r = tui_rect_make(a.x + (a.w - w) / 2, a.y + (a.h - h) / 2, w, h);
    ls_dialog1_frame(sf, r, "FREQS", HUE);
    tui_put_str(sf, r, r.x + 3, r.y + 2, s_dlg_text[0], WHITE);
    tui_put_str(sf, r, r.x + 3, r.y + 3, s_dlg_text[1], LS_ATTR_DIM);
    int y = r.y + 4;
    s_dlg_field = tui_rect_make(0, 0, 0, 0);
    if (s_dlg_prompt) {
        s_dlg_field = tui_rect_make(r.x + 3, y, r.w - 6, 1);
        ls_dui_edit_draw(sf, r, r.x + 3, y, r.w - 6, &s_dlg_edit, true, HUE);
        y++;
    }
    y++;
    int x = r.x + 3;
    x += ls_dui_key(sf, r, x, y, s_dlg_prompt ? "OK  Enter" : "Yes  Enter", false, false, TUI_GREEN, &s_dlg_ok) + 2;
    ls_dui_key(sf, r, x, y, s_dlg_prompt ? "Cancel  Esc" : "No  Esc", false, false, TUI_RED, &s_dlg_no);
}

/* A row carried with the button down. */
static void drag_track(void)
{
    int sc, sr, c, r;
    const bool held = ls_tui_touch_held(&sc, &sr, &c, &r);
    if (!held) {
        if (s_drag.on) {
            s_drag.on = false;
            if (s_drag.drop >= 0 && s_drag.drop < s_ntree && s_tree[s_drag.drop].kind == T_BANK && targets())
                send_to(ls_freqs_bank(s_tree[s_drag.drop].arg), true);
        }
        return;
    }
    if (s_pop.open || s_dlg || s_form || !s_n) return;
    if (!s_drag.on) {
        /* Only a hold that began on a row and has left its cell. */
        if (!tui_rect_contains(s_t.body, sc, sr) || (sc == c && sr == r)) return;
        const int k = s_t.first + sr - s_t.body.y;
        if (k < 0 || k >= s_n) return;
        s_drag.on = true;
        s_drag.from = k;
        s_drag.drop = -1;
        s_focus = F_TABLE;
        if (!picked(k)) { pick_none(); s_sel = s_anchor = k; }
    }
    s_drag.col = c;
    s_drag.row = r;
    s_drag.drop = -1;
    if (tui_rect_contains(s_r_banks, c, r)) {
        const int row = r - s_tree_y;
        if (row >= 0 && row < s_ntree && s_tree[row].kind == T_BANK && s_tree[row].arg != ls_freqs_which()) s_drag.drop = row;
    } else if (s_npick <= 1 || !picked(s_drag.from) || s_anchor == s_drag.from) {
        /* Over the table it sweeps out a run of rows, the list following at
           its edges. */
        int k = s_t.first + r - s_t.body.y;
        if (r < s_t.body.y) k = s_t.first - 1;
        if (r >= s_t.body.y + s_t.body.h) k = s_t.first + s_t.rows;
        if (k < 0) k = 0;
        if (k >= s_n) k = s_n - 1;
        if (k != s_drag.from || s_npick) {
            s_anchor = s_drag.from;
            s_sel = k;
            s_free = false;
            pick_range(s_anchor, k);
        }
    }
}

static bool context(int col, int row);

void ls_freqs_desk_draw(tui_surface *sf, tui_rect a)
{
    s_area = a;
    s_nhit = 0;
    ls_dui_context_claim(context);
    if (a.w < 56 || a.h < 12) {
        ls_panel_notice(sf, a, "FREQS", "Enlarge the window", "the list keeps its place");
        return;
    }
    refresh();
    layout(a);
    drag_track();
    draw_menu(sf);
    draw_tools(sf);
    draw_filter(sf);
    draw_ruler(sf);
    draw_banks(sf);
    draw_table(sf);
    if (s_r_detail.w) draw_detail(sf, s_r_detail);
    else if (s_form) {
        /* No pane for it in a window this small: the form over the list. */
        int w = a.w - 4, h = LS_FF__COUNT + 4;
        if (w > 60) w = 60;
        if (h > a.h - 2) h = a.h - 2;
        const tui_rect r = tui_rect_make(a.x + (a.w - w) / 2, a.y + (a.h - h) / 2, w, h);
        tui_fill(sf, r, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        draw_detail(sf, r);
    }
    draw_status(sf);
    if (s_dlg) draw_dialog(sf);
    ls_dui_popup_draw(sf, a, &s_pop);
}

/* ----------------------------------------------------------------- input -- */

static void move_to(int k, bool extend)
{
    if (k < 0) k = 0;
    if (k >= s_n) k = s_n ? s_n - 1 : 0;
    s_free = false;
    s_sel = k;
    if (extend) pick_range(s_anchor, s_sel);
    else { s_anchor = s_sel; }
}

static void load_tree(int row)
{
    if (row < 0 || row >= s_ntree) return;
    const tree_t t = s_tree[row];
    if (t.kind == T_GROUP) {
        if (s_f.group[0] && !strcmp(s_f.group, s_group[t.arg])) s_f.group[0] = 0;
        else snprintf(s_f.group, sizeof(s_f.group), "%s", s_group[t.arg]);
        changed();
        return;
    }
    if (t.kind == T_NONE) return;
    ls_freqs_scan();
    ls_freqs_load(t.kind == T_ALL ? LS_FREQS_ALL : t.kind == T_BUILTIN ? LS_FREQS_BUILTIN : t.arg);
    want(NULL);
    s_sel = 0;
    changed();
}

static void tree_step(int d)
{
    for (int n = 0; n < s_ntree; n++) {
        s_bank_cur += d;
        if (s_bank_cur < 0) s_bank_cur = s_ntree - 1;
        if (s_bank_cur >= s_ntree) s_bank_cur = 0;
        if (s_tree[s_bank_cur].kind != T_NONE) return;
    }
}

/* The keyboard goes to the tree, starting on the bank that is open. */
static void tree_focus(void)
{
    const int which = ls_freqs_which();
    s_focus = F_BANKS;
    s_bank_cur = 0;
    for (int i = 0; i < s_ntree; i++)
        if ((s_tree[i].kind == T_BANK && s_tree[i].arg == which) || (s_tree[i].kind == T_BUILTIN && which == LS_FREQS_BUILTIN))
            s_bank_cur = i;
}

static void wheel(int d)
{
    int c, r;
    const bool known = ls_tui_pointer_cell(&c, &r);
    if (known && tui_rect_contains(s_r_ruler, c, r)) { move_to(s_sel + d, false); return; }
    /* The list goes by, three rows a notch; the cursor stays on its entry. */
    s_free = true;
    s_t.first += d * 3;
    ls_dui_table_clamp(&s_t, s_n);
}

static void popup_result(int r)
{
    if (r == LS_DUI_PREV) open_menu(s_pop.bar - 1);
    else if (r == LS_DUI_NEXT) open_menu(s_pop.bar + 1);
    else if (r >= 0) chosen(s_pop_kind, r);
}

bool ls_freqs_desk_key(ls_tk_t k, char ch)
{
    const uint8_t mods = mods_now();
    const bool ctrl = mods & LS_IMOD_CTRL, shift = mods & LS_IMOD_SHIFT, alt = mods & LS_IMOD_ALT;
    if (s_pop.open) { popup_result(ls_dui_popup_key(&s_pop, k, ch)); return true; }
    /* A chord is not text for a field. */
    if (k == LS_TK_CHAR && (ctrl || alt) && (s_dlg || s_form)) return true;
    if (s_dlg) {
        if (k == LS_TK_ESC) dialog_done(false);
        else if (k == LS_TK_ENTER) dialog_done(true);
        else if (s_dlg_prompt) ls_dui_edit_key(&s_dlg_edit, k, ch);
        else if (k == LS_TK_CHAR && (ch == 'y' || ch == 'Y')) dialog_done(true);
        else if (k == LS_TK_CHAR && (ch == 'n' || ch == 'N')) dialog_done(false);
        return true;
    }
    if (s_form) return form_key(k, ch);
    if (k == LS_TK_WHEEL_UP || k == LS_TK_WHEEL_DOWN) { wheel(k == LS_TK_WHEEL_UP ? -1 : 1); return true; }
    char low = ch;
    if (low >= 'A' && low <= 'Z') low = (char)(low + 32);
    if (k == LS_TK_CHAR && alt) {
        const char *p = strchr("fevt", low);
        if (p && low) { open_menu((int)(p - "fevt")); return true; }
        return false;
    }
    if (k == LS_TK_CHAR && ctrl) {
        switch (low) {
        case 'f': run(C_FIND); break;
        case 'a': run(C_ALL); break;
        case 'n': run(C_NEW); break;
        case 'd': run(C_DUP); break;
        case 'z': run(C_UNDO); break;
        case 'x': run(C_CUT); break;
        case 'c': run(C_COPY); break;
        case 'v': run(C_PASTE); break;
        case 'e': run(C_EXP_CSV); break;
        case 'r': run(C_RELOAD); break;
        default: return false;
        }
        return true;
    }
    if (s_focus == F_FIND) {
        if (k == LS_TK_ESC) { s_f.find[0] = 0; s_focus = F_TABLE; changed(); return true; }
        if (k == LS_TK_ENTER || k == LS_TK_DOWN || k == LS_TK_TAB) { s_focus = F_TABLE; return true; }
        if (ls_dui_edit_key(&s_find, k, ch)) {
            if (strcmp(s_f.find, s_find.text)) { snprintf(s_f.find, sizeof(s_f.find), "%s", s_find.text); want(NULL); s_sel = 0; changed(); }
            return true;
        }
        return true;
    }
    if (s_focus == F_BANKS) {
        if (k == LS_TK_UP) { tree_step(-1); return true; }
        if (k == LS_TK_DOWN) { tree_step(1); return true; }
        if (k == LS_TK_ENTER) { load_tree(s_bank_cur); return true; }
        if (k == LS_TK_TAB || k == LS_TK_ESC || k == LS_TK_RIGHT) { s_focus = F_TABLE; return true; }
    }
    const int page = s_t.rows > 1 ? s_t.rows - 1 : 1;
    switch (k) {
    case LS_TK_UP: move_to(s_sel - 1, shift); return true;
    case LS_TK_DOWN: move_to(s_sel + 1, shift); return true;
    case LS_TK_PGUP: move_to(s_sel - page, shift); return true;
    case LS_TK_PGDN: move_to(s_sel + page, shift); return true;
    case LS_TK_HOME: move_to(0, shift); return true;
    case LS_TK_END: move_to(s_n - 1, shift); return true;
    case LS_TK_ENTER: tune(); return true;
    case LS_TK_DELETE: run(C_DELETE); return true;
    case LS_TK_INSERT: run(C_NEW); return true;
    case LS_TK_F2: run(C_EDIT); return true;
    case LS_TK_F5: run(C_RELOAD); return true;
    case LS_TK_TAB: case LS_TK_LEFT:
        if (!s_r_banks.w) return k == LS_TK_TAB;
        tree_focus();
        return true;
    case LS_TK_ESC:
        if (s_npick) { pick_none(); return true; }
        if (s_f.find[0]) { s_f.find[0] = 0; changed(); return true; }
        return false;
    case LS_TK_CHAR:
        switch (low) {
        case ' ':
            if (selected()) { ls_dui_bit_set(s_pick, s_idx[s_sel], !picked(s_sel)); count_picks(); move_to(s_sel + 1, false); }
            return true;
        case '*': run(C_STAR); return true;
        case '/': case 'f': run(C_FIND); return true;
        case 't': tune(); return true;
        case 's': run(C_NEW); return true;
        case 'e': run(C_EDIT); return true;
        case 'k':
            if (s_r_banks.w) tree_focus();
            else open_chooser();
            return true;
        case 'o': {
            int y = s_t.body.y + s_sel - s_t.first + 1;
            if (y < s_t.body.y || y > s_t.body.y + s_t.body.h) y = s_t.body.y + 1;
            if (selected()) row_menu(s_t.x[LS_FC_NAME] > 0 ? s_t.x[LS_FC_NAME] : s_t.body.x + 2, y);
            else open_menu(0);
            return true;
        }
        case 'b': run(C_BAND); return true;
        case 'g': run(C_GROUP); return true;
        case 'n': run(C_NEAR); return true;
        default: return false;
        }
    default: return false;
    }
}

/* The entry of the view nearest a cell of the strip. */
static void ruler_pick(int col)
{
    int best = -1, least = 1 << 30;
    for (int k = 0; k < s_n; k++) {
        const ls_freq_t *e = at(k);
        if (!e) continue;
        const int d = abs(rule_x((float)e->hz * 1e-6f, s_rule_w) - (col - s_r_ruler.x));
        if (d < least) { least = d; best = k; }
    }
    if (best >= 0) { pick_none(); move_to(best, false); }
}

bool ls_freqs_desk_touch(int col, int row)
{
    const uint8_t mods = mods_now();
    const bool ctrl = mods & LS_IMOD_CTRL, shift = mods & LS_IMOD_SHIFT;
    if (s_pop.open) {
        const int bar = s_pop.bar;
        const int r = ls_dui_popup_tap(&s_pop, col, row);
        if (r == LS_DUI_CLOSED && row == s_r_menu.y) {
            /* On the bar: another title opens, the same one shuts. */
            const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
            if (i >= 0 && i != bar) open_menu(i);
        } else popup_result(r);
        return true;
    }
    if (s_dlg) {
        if (tui_rect_contains(s_dlg_ok, col, row)) dialog_done(true);
        else if (tui_rect_contains(s_dlg_no, col, row)) dialog_done(false);
        else if (tui_rect_contains(s_dlg_field, col, row)) ls_dui_edit_click(&s_dlg_edit, s_dlg_field.x, s_dlg_field.w, col);
        return true;
    }
    if (row == s_r_menu.y) {
        const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
        if (i >= 0) open_menu(i);
        return true;
    }
    if (s_form) { form_tap(col, row); return true; }
    if (tui_rect_contains(s_r_find, col, row)) {
        find_focus();
        ls_dui_edit_click(&s_find, s_r_find.x, s_r_find.w, col);
        return true;
    }
    for (int i = 0; i < s_nhit; i++)
        if (tui_rect_contains(s_hit[i].r, col, row)) { if (s_focus == F_FIND) s_focus = F_TABLE; run(s_hit[i].cmd); return true; }
    if (s_focus == F_FIND) s_focus = F_TABLE;
    if (tui_rect_contains(s_r_ruler, col, row)) { ruler_pick(col); return true; }
    if (tui_rect_contains(s_r_banks, col, row)) {
        const int t = row - s_tree_y;
        if (t >= 0 && t < s_ntree && s_tree[t].kind != T_NONE) { s_bank_cur = t; load_tree(t); }
        return true;
    }
    int what = 0;
    switch (ls_dui_table_hit(&s_t, LS_FC__COUNT, col, row, &what)) {
    case LS_DUI_T_HEAD: sort_by(what); break;
    case LS_DUI_T_STRIP: {
        const int page = s_t.rows > 1 ? s_t.rows - 1 : 1;
        s_free = true;
        s_t.first += what * page;
        ls_dui_table_clamp(&s_t, s_n);
        break;
    }
    case LS_DUI_T_ROW: {
        s_focus = F_TABLE;
        if (what >= s_n) { if (!ctrl && !shift) pick_none(); break; }
        const int64_t now = esp_timer_get_time();
        const bool twice = !ctrl && !shift && what == s_tap_row &&
                           (ls_tui_input_clicks() >= 2 || now - s_tap_us < 450000);
        s_tap_row = twice ? -1 : what;
        s_tap_us = now;
        s_free = false;
        if (s_t.x[LS_FC_STAR] >= 0 && col == s_t.x[LS_FC_STAR] && !ctrl && !shift) {
            /* The star itself: this row's, whatever is ticked. */
            pick_none();
            s_sel = s_anchor = what;
            star();
            s_tap_row = -1;
        } else if (shift) { s_sel = what; pick_range(s_anchor, what); }
        else if (ctrl) {
            /* The row the cursor was on joins in: it was the selection. */
            if (!s_npick && s_sel != what && selected()) ls_dui_bit_set(s_pick, s_idx[s_sel], true);
            ls_dui_bit_set(s_pick, s_idx[what], !picked(what));
            count_picks();
            s_sel = s_anchor = what;
        } else {
            pick_none();
            s_sel = s_anchor = what;
            if (twice) tune();
        }
        break;
    }
    default: break;
    }
    return true;
}

/* The right button: a menu for what is under it. False leaves the desk's
   own window menu to come up. */
static bool context(int col, int row)
{
    if (s_dlg || s_form) return true;
    s_pop.open = false;
    int what = 0;
    const int part = ls_dui_table_hit(&s_t, LS_FC__COUNT, col, row, &what);
    if (part == LS_DUI_T_ROW && what < s_n) {
        s_focus = F_TABLE;
        s_free = false;
        if (!picked(what)) { pick_none(); s_anchor = what; }
        s_sel = what;
        row_menu(col, row);
        return true;
    }
    if (part == LS_DUI_T_HEAD || (row == s_t.head.y && tui_rect_contains(s_r_table, col, row))) { head_menu(col, row + 1); return true; }
    if (tui_rect_contains(s_r_banks, col, row)) {
        const int t = row - s_tree_y;
        if (t >= 0 && t < s_ntree && s_tree[t].kind != T_NONE && s_tree[t].kind != T_GROUP) {
            const int which = s_tree[t].kind == T_ALL ? LS_FREQS_ALL : s_tree[t].kind == T_BUILTIN ? LS_FREQS_BUILTIN : s_tree[t].arg;
            if (which != ls_freqs_which()) { s_bank_cur = t; load_tree(t); }
        }
        bank_menu(col, row);
        return true;
    }
    return false;
}

void ls_freqs_desk_enter(void)
{
    static bool restored;
    s_pop.open = false;
    s_dlg = D_NONE;
    s_drag.on = false;
    s_tap_row = -1;
    if (s_focus == F_FIND) s_focus = F_TABLE;
    if (!restored) {
        restored = true;
        for (int b = 0; b < LS_FREQ_BANKS; b++) s_count[b] = -1;
        const uint32_t v = settings_get_freqs_view();
        s_f.band = (uint8_t)((v & 7) < LS_FB__COUNT ? (v & 7) : 0);
        const uint32_t sort = (v >> 3) & 3;
        s_sort = sort == LS_FS_FREQ ? LS_FC_FREQ : sort == LS_FS_NAME ? LS_FC_NAME : sort == LS_FS_NEAR ? LS_FC_DIST : LS_FC_LISTED;
        s_f.near = (v >> 5) & 1;
    }
    s_stale = true;
}

#endif /* LS_TUI_HAS_COMPACT */
