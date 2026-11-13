/* P25 as a desktop program: the talkgroups in a table, the call beside it.

       [ VFO ] [ WATERFALL ] [ SCANNER ] [ CALLS ] ...        scr_p25.c's bar
       Receiver  View  Talkgroup              CONTROL LOCKED  851.0125 MHz
       [Follow ON] [Hold 1041] [Lock out 1041] [Skip encrypted]  Find [...]
       +TALKGROUPS 7---------------------------+ +NOW PLAYING------------+
       |    TG   NAME      HITS LAST  16 MIN   | | ON AIR   P1 CLEAR     |
       | H  1041 FIRE DISP  312   2s  .:|::.|  | | 1041 FIRE DISPATCH    |
       | rows, sorted by any column            | +RECEIVER HEALTH--------+
       |                                       | | sync, voice, errors   |
       +---------------------------------------+ +-----------------------+
       status
       TUNE [ STEP ] [<<] [<] [>] [>>]   VOLUME   GAIN        scr_p25.c's deck

   The receiver, the views, tuning and the deck are the handheld's
   (scr_p25.c); the card is CALLS' (p25_calls_live.c); what was heard is the
   receiver's own store (p25_tg_observed.h) and a row's order and text are
   ls_p25_desk.c. Hold, lock out and the allow list go through the same
   calls the handheld's H and L do. */
#include "scr_p25_desk.h"

#if LS_TUI_HAS_COMPACT

#include "../../ls_deskui.h"
#include "../../ls_p25_desk.h"
#include "../../ls_box.h"
#include "../../ls_calls.h"
#include "../../ls_input_ev.h"
#include "../../ls_tui_touch.h"
#include "p25_calls_live.h"
#include "p25_state.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32                   /* the bench on Windows */
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0775)
#endif

#define HUE TUI_CYAN
#define WHITE TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define INK(h) TUI_ATTR((h) | TUI_BRIGHT, TUI_BLACK)
#define SOFT(h) TUI_ATTR((h), TUI_BLACK)

static EXT_RAM_BSS_ATTR p25_tg_observed_snapshot_t s_snap;
static EXT_RAM_BSS_ATTR ls_p25d_row_t s_row[LS_P25D_ROWS];
static EXT_RAM_BSS_ATTR uint8_t s_order[LS_P25D_ROWS], s_view[LS_P25D_ROWS];
static EXT_RAM_BSS_ATTR int s_nrow, s_n, s_sel;
static EXT_RAM_BSS_ATTR uint16_t s_sel_tg, s_now_tg;
static EXT_RAM_BSS_ATTR int64_t s_read_us, s_said_us;
static EXT_RAM_BSS_ATTR uint32_t s_now_ms, s_shown;
static EXT_RAM_BSS_ATTR bool s_have, s_on_air, s_desc, s_free, s_classic, s_find_focus, s_show_card, s_show_health, s_only_heard;
static EXT_RAM_BSS_ATTR int s_sort;
static EXT_RAM_BSS_ATTR ls_dui_edit_t s_find;
static EXT_RAM_BSS_ATTR ls_dui_table_t s_t;
static EXT_RAM_BSS_ATTR ls_dui_menu_t s_menu;
static EXT_RAM_BSS_ATTR ls_dui_hits_t s_hits;
static EXT_RAM_BSS_ATTR tui_rect s_area, s_r_menu, s_r_tools, s_r_table, s_r_card, s_r_health, s_r_status, s_r_find;
static EXT_RAM_BSS_ATTR int16_t s_bar_x[6];
static EXT_RAM_BSS_ATTR char s_say[96], s_dir[96];

static const ls_dui_col_t COLS[LS_PT__COUNT] = {
    [LS_PT_FLAGS] = { "",        4,  4, 0, 0 },
    [LS_PT_TG]    = { "TG",      5,  5, LS_DUI_RIGHT, 0 },
    [LS_PT_NAME]  = { "NAME",    20, 8, LS_DUI_FLEX, 0 },
    [LS_PT_HITS]  = { "HITS",    6,  6, LS_DUI_RIGHT, 2 },
    [LS_PT_LAST]  = { "LAST",    4,  4, LS_DUI_RIGHT, 0 },
    [LS_PT_BUSY]  = { "16 MIN",  16, 16, 0, 1 },
    [LS_PT_CHAN]  = { "CHANNEL", 9,  9, LS_DUI_RIGHT, 3 },
    [LS_PT_NAC]   = { "NAC",     3,  3, 0, 5 },
    [LS_PT_VIA]   = { "SEEN IN", 13, 13, 0, 4 },
};

enum {
    C_FOLLOW, C_HOLD, C_LOCK, C_ALLOW, C_ALLOW_ONLY, C_SKIP_ENC, C_FIND, C_EXPORT, C_CLEAR_TABLE, C_ONLY_HEARD,
    C_RADIO, C_OPTIONS, C_CALLS, C_PROFILE, C_CLASSIC, C_PANE_CARD, C_PANE_HEALTH, C_COPY_TG, C_COPY_CHAN, C_COPY_ROW,
    C_COL = 100, C_SORT = 120,
};

static const char *const MENUS[] = { "Receiver", "View", "Talkgroup" };
#define NMENUS 3

static void say(const char *text) { snprintf(s_say, sizeof(s_say), "%s", text); s_said_us = esp_timer_get_time(); }

bool ls_p25_desk_classic(void) { return s_classic; }
void ls_p25_desk_export_dir(const char *dir) { snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : ""); }

/* ------------------------------------------------------------------ data -- */

static const ls_p25d_row_t *at(int k) { return k >= 0 && k < s_n ? &s_row[s_view[k]] : NULL; }
static const ls_p25d_row_t *selected(void) { return at(s_sel); }

static bool in_list(const uint16_t *list, unsigned n, uint16_t tg)
{
    for (unsigned i = 0; i < n && i < 64; i++)
        if (list[i] == tg) return true;
    return false;
}

/* The talkgroup a toolbar key acts on: the row chosen, else the call. */
static uint16_t target(void)
{
    const ls_p25d_row_t *r = selected();
    return r ? r->tg : s_now_tg;
}

static void refresh(void)
{
    const int64_t now = esp_timer_get_time();
    s_now_ms = (uint32_t)(now / 1000);
    /* The store moves on decoded frames; once a second is finer than an age is read. */
    if (!s_read_us || now < s_read_us || now - s_read_us >= 1000000) {
        if (p25_tg_observed_read(&s_snap)) { s_have = true; s_read_us = now; }
    }
    s_nrow = s_have ? ls_p25d_merge(s_row, LS_P25D_ROWS, &s_snap, s_now_ms) : 0;
    uint32_t unit = 0, elapsed = 0;
    s_now_tg = 0;
    s_on_air = false;
    (void)p25_calls_live_now(&s_now_tg, &unit, &s_on_air, &elapsed);
    /* What only a list knows: held, locked out or allowed and not heard yet. */
    if (!s_only_heard) {
        ls_p25d_row(s_row, &s_nrow, LS_P25D_ROWS, P25.hold_tg);
        for (unsigned i = 0; i < P25.lockout_count && i < 64; i++) ls_p25d_row(s_row, &s_nrow, LS_P25D_ROWS, P25.lockout[i]);
        for (unsigned i = 0; i < P25.allow_count && i < 64; i++) ls_p25d_row(s_row, &s_nrow, LS_P25D_ROWS, P25.allow[i]);
    }
    for (int i = 0; i < s_nrow; i++) {
        ls_p25d_row_t *r = &s_row[i];
        const char *name = p25_calls_alias(r->tg);
        snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
        r->flags = 0;
        if (P25.hold_tg == r->tg) r->flags |= LS_PF_HELD;
        if (in_list(P25.lockout, P25.lockout_count, r->tg)) r->flags |= LS_PF_LOCKED;
        if (in_list(P25.allow, P25.allow_count, r->tg)) r->flags |= LS_PF_ALLOW;
        for (unsigned e = 0; e < P25.p25_enc_tg_count && e < P25_STATE_ENC_TG_MAX; e++)
            if (P25.p25_enc_tg[e].talkgroup == r->tg) r->flags |= LS_PF_ENC;
        if (s_on_air && s_now_tg == r->tg) r->flags |= LS_PF_NOW;
    }
    ls_p25d_sort(s_order, s_row, s_nrow, s_sort, s_desc);
    s_n = 0;
    for (int i = 0; i < s_nrow; i++)
        if (ls_p25d_match(&s_row[s_order[i]], s_find.text)) s_view[s_n++] = s_order[i];
    /* The cursor stays on its talkgroup while the order moves under it. */
    s_sel = -1;
    for (int k = 0; k < s_n && s_sel < 0; k++)
        if (s_sel_tg && s_row[s_view[k]].tg == s_sel_tg) s_sel = k;
}

static void move_to(int k)
{
    if (!s_n) return;
    if (k < 0) k = 0;
    if (k >= s_n) k = s_n - 1;
    s_free = false;
    s_sel = k;
    s_sel_tg = s_row[s_view[k]].tg;
}

/* ----------------------------------------------------------------- menus -- */

static void tg_items(uint16_t tg)
{
    char text[44];
    const int none = tg ? 0 : LS_DUI_OFF;
    const bool held = tg && P25.hold_tg == tg, locked = tg && in_list(P25.lockout, P25.lockout_count, tg),
               allowed = tg && in_list(P25.allow, P25.allow_count, tg);
    snprintf(text, sizeof(text), held ? "Release the hold" : "Hold: follow only this one");
    ls_dui_menu_item(&s_menu, text, "H", C_HOLD, none);
    ls_dui_menu_item(&s_menu, locked ? "Unlock" : "Lock out", "L", C_LOCK, none);
    ls_dui_menu_item(&s_menu, allowed ? "Take off the allow list" : "Add to the allow list", "A", C_ALLOW, none);
    ls_dui_menu_rule(&s_menu);
    ls_dui_menu_item(&s_menu, "Copy talkgroup", "Ctrl+C", C_COPY_TG, none);
    const ls_p25d_row_t *r = selected();
    ls_dui_menu_item(&s_menu, "Copy channel", NULL, C_COPY_CHAN, r && r->tg == tg && r->heard && r->hz ? 0 : LS_DUI_OFF);
    ls_dui_menu_item(&s_menu, "Copy the row as CSV", NULL, C_COPY_ROW, r && r->tg == tg ? 0 : LS_DUI_OFF);
}

static void build_menu(int which)
{
    char text[44];
    ls_dui_menu_begin(&s_menu);
    switch (which) {
    case 0:
        ls_dui_menu_item(&s_menu, "Follow grants to voice", "F", C_FOLLOW, p25_get_auto_follow() ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Leave encrypted calls", NULL, C_SKIP_ENC, p25_get_leave_on_encrypted() ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Follow only the allow list", NULL, C_ALLOW_ONLY, P25.list_mode == 1 ? LS_DUI_CHECK : 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Radio...", "R", C_RADIO, 0);
        ls_dui_menu_item(&s_menu, "Options...", "O", C_OPTIONS, 0);
        ls_dui_menu_item(&s_menu, "Load a system profile...", "l", C_PROFILE, 0);
        ls_dui_menu_item(&s_menu, "Recorded calls", "5", C_CALLS, 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Save the table as CSV", "Ctrl+E", C_EXPORT, s_n ? 0 : LS_DUI_OFF);
        ls_dui_menu_item(&s_menu, "Forget what was heard", NULL, C_CLEAR_TABLE, s_have && s_snap.count ? 0 : LS_DUI_OFF);
        break;
    case 1:
        ls_dui_menu_item(&s_menu, "Most recent first", NULL, C_SORT + LS_PT_LAST, s_sort == LS_PT_LAST ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Busiest first", NULL, C_SORT + LS_PT_BUSY, s_sort == LS_PT_BUSY ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "By number", NULL, C_SORT + LS_PT_TG, s_sort == LS_PT_TG ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "By name", NULL, C_SORT + LS_PT_NAME, s_sort == LS_PT_NAME ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Held and locked first", NULL, C_SORT + LS_PT_FLAGS, s_sort == LS_PT_FLAGS ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Only what was heard", NULL, C_ONLY_HEARD, s_only_heard ? LS_DUI_CHECK : 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Now playing", NULL, C_PANE_CARD, s_show_card ? LS_DUI_CHECK : 0);
        ls_dui_menu_item(&s_menu, "Receiver health", NULL, C_PANE_HEALTH, s_show_health ? LS_DUI_CHECK : 0);
        ls_dui_menu_rule(&s_menu);
        ls_dui_menu_item(&s_menu, "Receiver view (the dial)", "Ctrl+L", C_CLASSIC, 0);
        break;
    default: {
        const uint16_t tg = target();
        if (tg) {
            const char *name = p25_calls_alias(tg);
            snprintf(text, sizeof(text), "%u %.30s", tg, name ? name : "");
        } else snprintf(text, sizeof(text), "NO TALKGROUP CHOSEN");
        ls_dui_menu_item(&s_menu, text, NULL, -1, LS_DUI_HEAD);
        tg_items(tg);
        break;
    }
    }
}

static void open_menu(int which)
{
    if (which < 0) which = NMENUS - 1;
    if (which >= NMENUS) which = 0;
    build_menu(which);
    ls_dui_menu_open(&s_menu, s_bar_x[which], s_r_menu.y + 1, which, HUE);
}

static void head_menu(int x, int y)
{
    ls_dui_menu_begin(&s_menu);
    ls_dui_menu_item(&s_menu, "COLUMNS", NULL, -1, LS_DUI_HEAD);
    for (int c = LS_PT_HITS; c < LS_PT__COUNT; c++)
        ls_dui_menu_item(&s_menu, COLS[c].title, NULL, C_COL + c,
                         ((s_shown >> c) & 1 ? LS_DUI_CHECK : 0) | (COLS[c].drop ? 0 : LS_DUI_OFF));
    ls_dui_menu_open(&s_menu, x, y, -1, HUE);
}

/* --------------------------------------------------------------- actions -- */

static void copy(const char *text)
{
    char line[96];
    ls_dui_clip_set(text);
    snprintf(line, sizeof(line), "Copied %.80s", text);
    say(line);
}

static void export_csv(void)
{
    char path[160], line[160];
    const char *dir = s_dir[0] ? s_dir : "/sdcard/p25";
    make_dir(dir);
    snprintf(path, sizeof(path), "%s/talkgroups-%lu.csv", dir, (unsigned long)time(NULL));
    FILE *f = fopen(path, "w");
    if (!f) { say("No card to save to"); return; }
    fprintf(f, "%s\n", LS_P25D_CSV_HEAD);
    for (int k = 0; k < s_n; k++) {
        ls_p25d_csv(at(k), s_now_ms, line, sizeof(line));
        fprintf(f, "%s\n", line);
    }
    fclose(f);
    snprintf(line, sizeof(line), "Saved %d talkgroup%s to %.40s", s_n, s_n == 1 ? "" : "s",
             strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    say(line);
}

/* A letter the handheld's own key handler takes: R, O, 5, l. */
extern const ls_tui_screen_t ls_scr_p25;
static EXT_RAM_BSS_ATTR bool s_pass;
static void handheld(char ch)
{
    /* Straight to the screen's key, with this file standing aside: L here
       is lock out, and the handheld's l is what was asked for. */
    s_pass = true;
    if (ls_scr_p25.key) ls_scr_p25.key(LS_TK_CHAR, ch);
    s_pass = false;
}

static void run(int cmd)
{
    char text[160];
    const uint16_t tg = target();
    const ls_p25d_row_t *r = selected();
    s_find_focus = false;
    if (cmd >= C_SORT) {
        const int c = cmd - C_SORT;
        if (c >= LS_PT__COUNT) return;
        if (s_sort == c) s_desc = !s_desc;
        else { s_sort = c; s_desc = false; }
        s_free = false;
        return;
    }
    if (cmd >= C_COL) { if (cmd - C_COL < LS_PT__COUNT) s_shown ^= 1u << (cmd - C_COL); return; }
    switch (cmd) {
    case C_FOLLOW: {
        const bool on = !p25_get_auto_follow();
        say(p25_set_auto_follow(on) ? (on ? "Following grants to voice" : "Staying on the control channel") : "The receiver refused");
        break;
    }
    case C_SKIP_ENC: {
        const bool on = !p25_get_leave_on_encrypted();
        say(p25_set_leave_on_encrypted(on) ? (on ? "Encrypted calls are left at once" : "Encrypted calls are sat through") : "The receiver refused");
        break;
    }
    case C_ALLOW_ONLY:
        (void)p25_ui_policy(P25_UI_LIST_MODE, P25.list_mode == 1 ? 0 : 1);
        say(P25.list_mode == 1 ? "Only talkgroups on the allow list are followed" : "Every talkgroup not locked out is followed");
        break;
    case C_HOLD:
        if (!tg && !P25.hold_tg) { say("Choose a talkgroup first"); break; }
        if (P25.hold_tg && (P25.hold_tg == tg || !tg)) { (void)p25_ui_policy(P25_UI_HOLD_SET, 0); say("Hold released"); }
        else if (p25_ui_policy(P25_UI_HOLD_SET, tg)) { snprintf(text, sizeof(text), "Holding on %u: nothing else is followed", tg); say(text); }
        break;
    case C_LOCK:
        if (!tg) { say("Choose a talkgroup first"); break; }
        if (in_list(P25.lockout, P25.lockout_count, tg)) { (void)p25_ui_lockout_remove(tg); snprintf(text, sizeof(text), "%u unlocked", tg); }
        else if (p25_ui_policy(P25_UI_LOCKOUT_ADD, tg)) snprintf(text, sizeof(text), "%u locked out: its calls are skipped", tg);
        else snprintf(text, sizeof(text), "The lockout list is full");
        say(text);
        break;
    case C_ALLOW:
        if (!tg) { say("Choose a talkgroup first"); break; }
        if (in_list(P25.allow, P25.allow_count, tg)) { (void)p25_ui_policy(P25_UI_ALLOW_REMOVE, tg); snprintf(text, sizeof(text), "%u taken off the allow list", tg); }
        else if (p25_ui_policy(P25_UI_ALLOW_ADD, tg)) snprintf(text, sizeof(text), "%u added to the allow list", tg);
        else snprintf(text, sizeof(text), "The allow list is full");
        say(text);
        break;
    case C_FIND: s_find_focus = true; s_find.caret = s_find.len; break;
    case C_EXPORT: export_csv(); break;
    case C_CLEAR_TABLE: if (p25_tg_observed_clear()) { s_read_us = 0; say("The table starts again from now"); } break;
    case C_ONLY_HEARD: s_only_heard = !s_only_heard; break;
    case C_RADIO: handheld('r'); break;
    case C_OPTIONS: handheld('o'); break;
    case C_CALLS: handheld('5'); break;
    case C_PROFILE: handheld('l'); break;
    case C_CLASSIC: s_classic = true; s_menu.pop.open = false; break;
    case C_PANE_CARD: s_show_card = !s_show_card; break;
    case C_PANE_HEALTH: s_show_health = !s_show_health; break;
    case C_COPY_TG: if (tg) { snprintf(text, sizeof(text), "%u", tg); copy(text); } break;
    case C_COPY_CHAN: if (r && r->heard && r->hz) { snprintf(text, sizeof(text), "%.4f", (double)r->hz / 1e6); copy(text); } break;
    case C_COPY_ROW: if (r) { ls_p25d_csv(r, s_now_ms, text, sizeof(text)); copy(text); } break;
    default: break;
    }
}

/* ---------------------------------------------------------------- layout -- */

static void layout(tui_rect a)
{
    const tui_rect none = tui_rect_make(0, 0, 0, 0);
    int y = a.y;
    s_r_menu = tui_rect_make(a.x, y++, a.w, 1);
    s_r_tools = tui_rect_make(a.x, y++, a.w, 1);
    s_r_status = tui_rect_make(a.x, a.y + a.h - 1, a.w, 1);
    const tui_rect body = tui_rect_make(a.x, y, a.w, a.y + a.h - 1 - y);
    int rw = 0;
    if ((s_show_card || s_show_health) && a.w >= 100) { rw = a.w * 2 / 5; if (rw > 72) rw = 72; if (rw < 44) rw = 44; }
    s_r_card = s_r_health = none;
    int th = body.h;
    if (rw) {
        const int rx = body.x + body.w - rw;
        const int ch = s_show_card && body.h >= 12 ? 12 : 0;
        if (ch) s_r_card = tui_rect_make(rx, body.y, rw, ch);
        if (s_show_health && body.h - ch >= 6) s_r_health = tui_rect_make(rx, body.y + ch, rw, body.h - ch);
        if (!ch && !s_r_health.h) rw = 0;
        else rw--;                      /* the table and the column share the line between them */
    } else if (s_show_card && body.h >= 22) {
        /* No room beside: the call under the table. */
        s_r_card = tui_rect_make(body.x, body.y + body.h - 12, body.w, 12);
        th = body.h - 12;
    }
    s_r_table = tui_rect_make(body.x, body.y, body.w - rw, th);
    ls_dui_table_layout(&s_t, tui_rect_make(s_r_table.x + 1, s_r_table.y + 1, s_r_table.w - 2, s_r_table.h - 2), COLS,
                        LS_PT__COUNT, s_shown | (1u << LS_PT_FLAGS) | (1u << LS_PT_TG) | (1u << LS_PT_NAME) | (1u << LS_PT_LAST));
}

/* ------------------------------------------------------------------ draw -- */

static void draw_menu(tui_surface *sf)
{
    ls_dui_bar_draw(sf, s_r_menu, MENUS, NMENUS, s_menu.pop.open ? s_menu.pop.bar : -1, HUE, s_bar_x);
    char text[80];
    snprintf(text, sizeof(text), "%.4f MHz", s_tune_freq_hz / 1e6);
    int x = s_r_menu.x + s_r_menu.w - (int)strlen(text) - 1;
    if (x <= s_bar_x[NMENUS] + 2) return;
    tui_put_str(sf, s_r_menu, x, s_r_menu.y, text, INK(TUI_YELLOW));
    const char *label = ls_scr_p25_receive_label();
    x -= (int)strlen(label) + 2;
    if (x > s_bar_x[NMENUS] + 2)
        tui_put_str(sf, s_r_menu, x, s_r_menu.y, label, P25.dsd_has_sync ? INK(TUI_GREEN) : LS_ATTR_DIM);
}

static void draw_tools(tui_surface *sf)
{
    const tui_rect r = s_r_tools;
    char label[32];
    const uint16_t tg = target();
    const bool held = P25.hold_tg != 0, locked = tg && in_list(P25.lockout, P25.lockout_count, tg);
    int x = r.x + 1;
    x += ls_dui_tool(sf, r, x, p25_get_auto_follow() ? "Follow ON" : "Follow OFF", C_FOLLOW, p25_get_auto_follow(), false, TUI_GREEN, &s_hits);
    if (held) snprintf(label, sizeof(label), "Release %u", P25.hold_tg);
    else if (tg) snprintf(label, sizeof(label), "Hold %u", tg);
    else snprintf(label, sizeof(label), "Hold");
    x += ls_dui_tool(sf, r, x, label, C_HOLD, held, !held && !tg, TUI_YELLOW, &s_hits);
    if (tg) snprintf(label, sizeof(label), locked ? "Unlock %u" : "Lock out %u", tg);
    else snprintf(label, sizeof(label), "Lock out");
    x += ls_dui_tool(sf, r, x, label, C_LOCK, locked, !tg, TUI_RED, &s_hits);
    x += ls_dui_tool(sf, r, x, "Skip encrypted", C_SKIP_ENC, p25_get_leave_on_encrypted(), false, TUI_MAGENTA, &s_hits);
    x += 1;
    s_r_find = tui_rect_make(0, 0, 0, 0);
    if (x + 24 < r.x + r.w) {
        tui_put_str(sf, r, x, r.y, "Find", s_find_focus ? INK(TUI_YELLOW) : LS_ATTR_DIM);
        s_r_find = tui_rect_make(x + 5, r.y, 16, 1);
        ls_dui_edit_draw(sf, r, x + 5, r.y, 16, &s_find, s_find_focus, TUI_YELLOW);
        x += 23;
    }
    x += ls_dui_tool(sf, r, x, "Save CSV", C_EXPORT, false, !s_n, TUI_GREEN, &s_hits);
}

static void draw_table(tui_surface *sf)
{
    char title[40], text[32];
    snprintf(title, sizeof(title), "TALKGROUPS %d", s_n);
    ls_dui_pane(sf, s_r_table, title, "H held  L locked out  A allowed  E encrypted", HUE, !s_find_focus);
    ls_dui_table_head(sf, &s_t, COLS, LS_PT__COUNT, s_sort, s_desc, HUE);
    const tui_rect b = s_t.body;
    if (!s_n) {
        const char *why = s_nrow ? "Nothing matches Find." : P25.dsd_has_sync ? "Synced, nothing heard yet." : "No sync: nothing to hear.";
        tui_put_str(sf, b, b.x + 2, b.y + 1, why, LS_ATTR_DIM);
        if (!s_nrow) {
            static const struct { const char *key, *what; } HOW[] = {
                { "< >", "step the channel, << >> seek" },
                { "T", "type a frequency" },
                { "SCANNER", "search a list of channels" },
                { "R", "pick the radio it listens with" },
                { "GAIN", "raise it until SIG moves, not past CLIP" },
            };
            for (int i = 0; i < 5 && b.y + 3 + i < b.y + b.h; i++) {
                tui_put_str(sf, b, b.x + 2, b.y + 3 + i, HOW[i].key, INK(TUI_YELLOW));
                tui_put_str(sf, b, b.x + 12, b.y + 3 + i, HOW[i].what, TUI_ATTR(TUI_WHITE, TUI_BLACK));
            }
        }
        return;
    }
    if (!s_free && s_sel >= 0) ls_dui_table_follow(&s_t, s_sel, s_n);
    else ls_dui_table_clamp(&s_t, s_n);
    /* One scale for every row's sixteen minutes, so a busy one looks it. */
    int most = 1;
    for (int k = 0; k < s_n; k++)
        for (int m = 0; m < P25_TG_OBSERVED_MINUTES; m++)
            if (at(k)->minute[m] > most) most = at(k)->minute[m];
    for (int row = 0; row < s_t.rows && s_t.first + row < s_n; row++) {
        const int k = s_t.first + row, y = b.y + row;
        const ls_p25d_row_t *r = at(k);
        const bool on = k == s_sel, live = r->flags & LS_PF_NOW, locked = r->flags & LS_PF_LOCKED;
        const uint8_t hue = ls_calls_tg_hue(r->tg);
        /* Brightness is freshness: on air, in the last minute, older, never. */
        const uint32_t ago = r->heard && s_now_ms >= r->last_ms ? (s_now_ms - r->last_ms) / 1000u : 99999u;
        const uint8_t ink = locked ? LS_ATTR_FAINT : live || ago < 60 ? INK(hue) : r->heard ? SOFT(hue) : LS_ATTR_DIM;
        if (on) ls_fill_dither(sf, tui_rect_make(b.x, y, b.w, 1), LS_DITHER_MEDIUM, HUE);
        else if (live) ls_fill_dither(sf, tui_rect_make(b.x, y, b.w, 1), LS_DITHER_LIGHT, hue);
        if (on) ls_box_put(sf, b, b.x, y, LS_SHAPE_RIGHT, WHITE);
        if (s_t.x[LS_PT_FLAGS] >= 0) {
            int x = s_t.x[LS_PT_FLAGS] + 1;
            if (r->flags & LS_PF_HELD) tui_put_char(sf, b, x++, y, 'H', INK(TUI_YELLOW));
            if (r->flags & LS_PF_LOCKED) tui_put_char(sf, b, x++, y, 'L', INK(TUI_RED));
            if (r->flags & LS_PF_ALLOW) tui_put_char(sf, b, x++, y, 'A', INK(TUI_GREEN));
            if ((r->flags & LS_PF_ENC) && x < s_t.x[LS_PT_FLAGS] + 4) tui_put_char(sf, b, x++, y, 'E', INK(TUI_MAGENTA));
        }
        ls_p25d_cell(r, LS_PT_TG, s_now_ms, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_TG, row, text, on ? WHITE : ink);
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_NAME, row, r->name[0] ? r->name : "-", r->name[0] ? ink : LS_ATTR_FAINT);
        ls_p25d_cell(r, LS_PT_HITS, s_now_ms, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_HITS, row, text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
        ls_p25d_cell(r, LS_PT_LAST, s_now_ms, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_LAST, row, live ? "now" : text, live ? INK(TUI_GREEN) : ago < 60 ? WHITE : LS_ATTR_DIM);
        if (s_t.x[LS_PT_BUSY] >= 0 && r->heard)
            for (int m = 0; m < P25_TG_OBSERVED_MINUTES; m++) {
                const int v = r->minute[m];
                if (!v) { ls_box_put(sf, b, s_t.x[LS_PT_BUSY] + m, y, LS_SHAPE_DOT, LS_ATTR_FAINT); continue; }
                tui_put_char(sf, b, s_t.x[LS_PT_BUSY] + m, y, LS_TUI_TRACE(1 + v * 7 / most),
                             m >= P25_TG_OBSERVED_MINUTES - 2 ? INK(hue) : SOFT(hue));
            }
        ls_p25d_cell(r, LS_PT_CHAN, s_now_ms, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_CHAN, row, text, SOFT(TUI_YELLOW));
        ls_p25d_cell(r, LS_PT_NAC, s_now_ms, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_NAC, row, text, LS_ATTR_DIM);
        ls_p25d_cell(r, LS_PT_VIA, s_now_ms, text, sizeof(text));
        ls_dui_table_cell(sf, &s_t, COLS, LS_PT_VIA, row, text, LS_ATTR_DIM);
    }
    ls_dui_table_strip(sf, &s_t, s_n, HUE);
}

static void draw_status(tui_surface *sf)
{
    const tui_rect r = s_r_status;
    char text[96];
    int k = snprintf(text, sizeof(text), " %d talkgroup%s", s_nrow, s_nrow == 1 ? "" : "s");
    if (P25.hold_tg) k += snprintf(text + k, sizeof(text) - (size_t)k, "  holding %u", P25.hold_tg);
    if (P25.lockout_count) k += snprintf(text + k, sizeof(text) - (size_t)k, "  %u locked out", P25.lockout_count);
    if (P25.list_mode == 1) k += snprintf(text + k, sizeof(text) - (size_t)k, "  allow list only (%u)", P25.allow_count);
    tui_put_str(sf, r, r.x, r.y, text, P25.hold_tg ? INK(TUI_YELLOW) : INK(HUE));
    const int x = r.x + k + 2;
    const bool mine = s_say[0] && esp_timer_get_time() - s_said_us < 8000000;
    const char *idle = s_find_focus ? "Type a talkgroup number or part of a name; Enter keeps it, Esc clears"
                     : "H hold  L lock out  A allow  F follow  click a header to sort  right button for more";
    tui_put_str(sf, tui_rect_make(x, r.y, r.x + r.w - x, 1), x, r.y, mine ? s_say : idle, mine ? INK(TUI_YELLOW) : LS_ATTR_DIM);
}

static bool context(int col, int row);

void ls_p25_desk_draw(tui_surface *sf, tui_rect a)
{
    s_area = a;
    s_hits.n = 0;
    ls_dui_context_claim(context);
    if (a.w < 50 || a.h < 9) {
        ls_panel_notice(sf, a, "P25", "Enlarge the window", "the table keeps its place");
        return;
    }
    refresh();
    layout(a);
    draw_menu(sf);
    draw_tools(sf);
    draw_table(sf);
    if (s_r_card.h) {
        const tui_rect in = ls_dui_pane(sf, s_r_card, "NOW PLAYING", ls_scr_p25_voice_status(), s_on_air ? TUI_GREEN : HUE, false);
        p25_calls_live_card(sf, in);
    }
    if (s_r_health.h) ls_scr_p25_health(sf, s_r_health);
    draw_status(sf);
    ls_dui_popup_draw(sf, a, &s_menu.pop);
}

/* ----------------------------------------------------------------- input -- */

static void popup_result(int r)
{
    if (r == LS_DUI_PREV) open_menu(s_menu.pop.bar - 1);
    else if (r == LS_DUI_NEXT) open_menu(s_menu.pop.bar + 1);
    else if (r >= 0) run(r);
}

bool ls_p25_desk_classic_key(ls_tk_t k, char ch)
{
    if (!s_classic) return false;
    if (k == LS_TK_CHAR && (ls_tui_input_mods() & LS_IMOD_CTRL) && (ch == 'l' || ch == 'L')) { s_classic = false; return true; }
    return false;
}

bool ls_p25_desk_key(ls_tk_t k, char ch)
{
    const uint8_t mods = ls_tui_input_mods();
    const bool ctrl = mods & LS_IMOD_CTRL, alt = mods & LS_IMOD_ALT;
    if (s_pass) return false;
    if (s_menu.pop.open) { popup_result(ls_dui_popup_key(&s_menu.pop, k, ch)); return true; }
    char low = ch;
    if (low >= 'A' && low <= 'Z') low = (char)(low + 32);
    if (k == LS_TK_CHAR && alt) {
        const char *p = low ? strchr("rvt", low) : NULL;
        if (p) open_menu((int)(p - "rvt"));
        return true;
    }
    if (k == LS_TK_CHAR && ctrl) {
        switch (low) {
        case 'f': run(C_FIND); break;
        case 'e': run(C_EXPORT); break;
        case 'c': run(C_COPY_TG); break;
        case 'l': run(C_CLASSIC); break;
        case 'v': if (s_find_focus) ls_dui_edit_paste(&s_find); break;
        default: break;
        }
        return true;
    }
    if (s_find_focus) {
        if (k == LS_TK_ESC) { ls_dui_edit_set(&s_find, "", 16); s_find_focus = false; return true; }
        if (k == LS_TK_ENTER || k == LS_TK_DOWN || k == LS_TK_TAB) { s_find_focus = false; return true; }
        ls_dui_edit_key(&s_find, k, ch);
        return true;
    }
    const int page = s_t.rows > 1 ? s_t.rows - 1 : 1;
    switch (k) {
    case LS_TK_UP: move_to(s_sel < 0 ? 0 : s_sel - 1); return true;
    case LS_TK_DOWN: move_to(s_sel + 1); return true;
    case LS_TK_PGUP: move_to(s_sel < 0 ? 0 : s_sel - page); return true;
    case LS_TK_PGDN: move_to(s_sel + page); return true;
    case LS_TK_HOME: move_to(0); return true;
    case LS_TK_END: move_to(s_n - 1); return true;
    case LS_TK_ENTER: run(C_HOLD); return true;
    case LS_TK_DELETE: run(C_LOCK); return true;
    case LS_TK_TAB: run(C_FIND); return true;
    case LS_TK_ESC:
        if (s_find.len) { ls_dui_edit_set(&s_find, "", 16); return true; }
        if (s_sel_tg) { s_sel_tg = 0; return true; }
        return false;
    case LS_TK_CHAR:
        switch (low) {
        case 'h': run(C_HOLD); return true;
        case 'a': run(C_ALLOW); return true;
        case 'f': run(C_FOLLOW); return true;
        case '/': run(C_FIND); return true;
        case 'l':
            /* L is lock out on the table, as in CALLS; Shift+L is the
               handheld's "load a profile". */
            if (ch == 'l') { run(C_LOCK); return true; }
            run(C_PROFILE);
            return true;
        default: return false;      /* tuning, the views, R, O, T, N: the handheld's */
        }
    default: return false;
    }
}

bool ls_p25_desk_touch(int col, int row)
{
    if (s_menu.pop.open) {
        const int bar = s_menu.pop.bar;
        const int r = ls_dui_popup_tap(&s_menu.pop, col, row);
        if (r == LS_DUI_CLOSED && row == s_r_menu.y) {
            const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
            if (i >= 0 && i != bar) open_menu(i);
        } else popup_result(r);
        return true;
    }
    if (!tui_rect_contains(s_area, col, row)) { s_find_focus = false; return false; }
    if (row == s_r_menu.y) {
        const int i = ls_dui_bar_hit(s_bar_x, NMENUS, col);
        if (i >= 0) open_menu(i);
        return true;
    }
    if (tui_rect_contains(s_r_find, col, row)) {
        s_find_focus = true;
        ls_dui_edit_click(&s_find, s_r_find.x, s_r_find.w, col);
        return true;
    }
    const int cmd = ls_dui_hits_at(&s_hits, col, row);
    if (cmd >= 0) { run(cmd); return true; }
    s_find_focus = false;
    int what = 0;
    switch (ls_dui_table_hit(&s_t, LS_PT__COUNT, col, row, &what)) {
    case LS_DUI_T_HEAD: run(C_SORT + what); break;
    case LS_DUI_T_STRIP:
        s_free = true;
        s_t.first += what * (s_t.rows > 1 ? s_t.rows - 1 : 1);
        ls_dui_table_clamp(&s_t, s_n);
        break;
    case LS_DUI_T_ROW:
        if (what < s_n) {
            const bool again = what == s_sel;
            move_to(what);
            /* Twice on a row holds on it, or lets it go. */
            if (again && ls_tui_input_clicks() >= 2) run(C_HOLD);
        } else s_sel_tg = 0;
        break;
    default: break;
    }
    return true;
}

static bool context(int col, int row)
{
    s_menu.pop.open = false;
    if (!tui_rect_contains(s_area, col, row)) return false;
    int what = 0;
    const int part = ls_dui_table_hit(&s_t, LS_PT__COUNT, col, row, &what);
    if (part == LS_DUI_T_ROW && what < s_n) {
        move_to(what);
        build_menu(2);
        ls_dui_menu_open(&s_menu, col, row, -1, HUE);
        return true;
    }
    if (part == LS_DUI_T_HEAD) { head_menu(col, row + 1); return true; }
    if (tui_rect_contains(s_r_card, col, row) && s_now_tg) {
        /* On the call: its talkgroup's menu. */
        s_sel_tg = s_now_tg;
        refresh();
        build_menu(2);
        ls_dui_menu_open(&s_menu, col, row, -1, HUE);
        return true;
    }
    return false;
}

void ls_p25_desk_enter(void)
{
    static EXT_RAM_BSS_ATTR bool once;
    s_menu.pop.open = false;
    s_find_focus = false;
    s_free = false;
    s_read_us = 0;
    if (!once) {
        once = true;
        /* Nothing here has an initialiser: all of it is in PSRAM, where
           the Nano has room, and is set the first time the screen opens. */
        ls_dui_edit_set(&s_find, "", 16);
        s_sort = LS_PT_LAST;
        s_shown = (1u << LS_PT__COUNT) - 1;
        s_show_card = s_show_health = true;
    }
}

#endif /* LS_TUI_HAS_COMPACT */
