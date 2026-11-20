/* SEND REPORT: the sheet DIAG and SETTINGS open over themselves, and the offer after a crash.
   It says what goes up before anything does, takes an optional note from the keyboard, keeps
   location off unless asked, and flies a paper plane from the board to the hub as the bytes
   really leave. The report id comes back on the sheet. The network side is core/ls_hub.h. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_notify.h"
#include "../../ls_app.h"
#include "../../ls_netui.h"
#include "../../ls_text.h"
#include "ls_hub.h"
#include "ls_errlog.h"
#include "ls_version.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

enum { P_EDIT, P_SENDING, P_SENT, P_FAILED };

EXT_RAM_BSS_ATTR static struct {
    bool open, gps, bg;             /* bg: sent from the crash notice, its answer is a notice too */
    uint8_t phase;
    uint32_t seq, size;
    char note[100], id[28], why[72];
    tui_rect note_hit, gps_hit;
} R;

#define A(fg) TUI_ATTR((fg), TUI_BLACK)
#define BIG (96 * 1024)

/* A bug folded into an envelope. */
static const char *const ENV[7] = {
    ".-----------------.",
    "|\\      _ _      /|",
    "| \\   _(o o)_   / |",
    "|  \\ ( /|=|\\ ) /  |",
    "|  /'-'/ ^ \\'-'\\  |",
    "| /   '-----'   \\ |",
    "'-----------------'",
};

static void measure(void)
{
    char *b = heap_caps_malloc(BIG, MALLOC_CAP_SPIRAM);
    R.size = b ? (uint32_t)ls_report_preview(b, BIG, R.gps) : 0;
    heap_caps_free(b);
}

void ls_report_sheet_open(void)
{
    R.open = true;
    if (R.phase != P_SENDING) R.phase = P_EDIT;
    measure();
}

bool ls_report_sheet_active(void) { return R.open; }

static void note_done(const char *t) { snprintf(R.note, sizeof(R.note), "%s", t); }

static void send(void)
{
    if (R.phase == P_SENDING) return;
    if (ls_report_send(R.note, R.gps)) {
        ls_hub_status_t st;
        ls_hub_status(&st);
        R.seq = st.seq;
        R.phase = P_SENDING;
        R.id[0] = R.why[0] = 0;
    } else {
        R.phase = P_FAILED;
        snprintf(R.why, sizeof(R.why), "the hub is busy, try again");
    }
}

/* The hub's answer for our job, once it has one. */
static void follow(void)
{
    ls_hub_status_t st;
    ls_hub_status(&st);
    if ((R.phase != P_SENDING && !R.bg) || st.seq == R.seq || strcmp(st.what, "report") || st.state == LS_HUB_BUSY) return;
    const bool ok = st.state == LS_HUB_OK;
    if (ok) {
        snprintf(R.id, sizeof(R.id), "%.27s", !strncmp(st.text, "sent ", 5) ? st.text + 5 : st.text);
        R.note[0] = 0;
    } else snprintf(R.why, sizeof(R.why), "%s", st.text);
    if (R.phase == P_SENDING) R.phase = ok ? P_SENT : P_FAILED;
    if (R.bg) {
        R.bg = false;
        ls_notice_t n = { .accent = ok ? TUI_GREEN : TUI_YELLOW, .screen = -1, .quiet = true };
        snprintf(n.title, sizeof(n.title), "REPORT");
        snprintf(n.body, sizeof(n.body), ok ? "Sent, thank you. Report id %s" : "Not sent: %s. DIAG > P tries again.",
                 ok ? R.id : R.why);
        ls_notify_post_quiet(&n);
    }
}

/* ---------------------------------------------------------------- draw -- */

static void draw_env(tui_surface *sf, tui_rect c, int x0, int y0)
{
    for (int y = 0; y < 7; y++)
        for (int x = 0; ENV[y][x]; x++) {
            const char ch = ENV[y][x];
            uint8_t at = A(TUI_YELLOW);
            if (y == 0 || y == 6 || x == 0 || x == 18) at = A(TUI_YELLOW | TUI_BRIGHT);
            else if (ch == 'o') at = A(TUI_RED | TUI_BRIGHT);
            else if (ch == '=') at = A(TUI_CYAN | TUI_BRIGHT);
            else if (x >= 5 && x <= 13 && y >= 1 && y <= 5) at = A(y < 3 ? TUI_GREEN | TUI_BRIGHT : TUI_GREEN);
            tui_put_char(sf, c, x0 + x, y0 + y, ch, ls_ink_hue(at));
        }
}

/* The board, the way up and the hub: the plane is where the upload is. */
static void draw_flight(tui_surface *sf, tui_rect c, int x0, int y, int w, float f)
{
    static const char *const BOARD[3] = {".--.", "|##|", "'--'"};
    static const char *const CLOUD[3] = {" .-~~-. ", "(  HUB )", " '-~~-' "};
    const bool done = R.phase == P_SENT, bad = R.phase == P_FAILED;
    for (int r = 0; r < 3; r++) {
        tui_put_str(sf, c, x0, y + r, BOARD[r], A(r == 1 ? TUI_CYAN | TUI_BRIGHT : TUI_CYAN));
        tui_put_str(sf, c, x0 + w - 8, y + r, CLOUD[r],
                    ls_ink_hue(A(done ? TUI_GREEN | TUI_BRIGHT : r == 1 ? TUI_WHITE | TUI_BRIGHT : TUI_WHITE)));
    }
    const int a = x0 + 5, n = w - 15;          /* the path's cells */
    if (n < 4) return;
    const int at = (int)(f * (float)(n - 1) + 0.5f);
    for (int i = 0; i < n; i++) {
        const bool past = R.phase != P_EDIT && (i < at || done);
        tui_put_char(sf, c, a + i, y + 1, past ? '=' : (i & 1 ? ' ' : '-'),
                     past ? ls_ink_hue(A(bad ? TUI_RED : TUI_GREEN | TUI_BRIGHT)) : ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    }
    if (!done) tui_put_char(sf, c, a + at, y + 1, bad ? 'x' : '>', ls_ink_hue(A(bad ? TUI_RED | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT)));
    if (R.phase == P_SENDING) tui_put_char(sf, c, a + at, y, '^', A(TUI_YELLOW));   /* the wing */
}

static int buttons(ls_btn_t *b)
{
    static char kb[12];
    const bool busy = R.phase == P_SENDING;
    snprintf(kb, sizeof(kb), "%lu KB", (unsigned long)((R.size + 1023) / 1024));
    int n = 0;
    b[n++] = (ls_btn_t){.label = "S SEND", .value = R.phase == P_SENT ? "SENT" : busy ? "..." : kb, .key = 's',
                        .dim = busy || R.phase == P_SENT};
    b[n++] = (ls_btn_t){.label = "N NOTE", .value = R.note[0] ? "EDIT" : "ADD", .key = 'n', .dim = busy};
    b[n++] = (ls_btn_t){.label = "G PLACE", .value = R.gps ? "ON" : "OFF", .key = 'g', .on = R.gps, .dim = busy};
    b[n++] = (ls_btn_t){.label = "X CLOSE", .value = R.phase == P_SENT ? "DONE" : "BACK", .key = 'x'};
    return n;
}

void ls_report_sheet_draw(tui_surface *sf, tui_rect area)
{
    follow();
    tui_fill(sf, area, ' ', TUI_DEFAULT_ATTR);
    ls_hub_status_t st;
    ls_hub_status(&st);
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int bar_h = ls_btn_raised_height(area, nb);
    const tui_rect bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    const int cw = area.w > 66 ? 66 : area.w, x0 = area.x + (area.w - cw) / 2;
    const tui_rect c = tui_rect_make(x0, area.y, cw, bar.y - area.y);
    const int bottom = bar.y - 1;
    char t[96];
    int y = area.y;

    /* the envelope and what this is */
    const bool art = area.h >= 44 && cw >= 44;
    if (art) {
        draw_env(sf, c, x0 + 1, y);
        const tui_rect r = tui_rect_make(x0 + 22, y, cw - 22, 7);
        tui_put_str(sf, r, r.x, y + 1, "SEND A REPORT", ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
        tui_put_str(sf, r, r.x, y + 2, "to the LakeShark team", ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT)));
        tui_put_str(sf, r, r.x, y + 4, "How the board was doing,", LS_ATTR_DIM);
        tui_put_str(sf, r, r.x, y + 5, "never names or keys.", LS_ATTR_DIM);
        y += 8;
    } else {
        tui_put_str(sf, c, x0 + 1, y++, "SEND A REPORT to the LakeShark team", ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
        y++;
    }

    /* what goes */
    tui_hline(sf, c, x0, y, cw, '-', ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    tui_put_str(sf, c, x0 + 2, y++, " WHAT GOES ", ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT)));
    const tui_rect kv = tui_rect_make(x0 + 1, y, cw - 2, 8);
    ls_version_info_t v;
    ls_version_get(&v);
    snprintf(t, sizeof(t), "%.20s %.16s", v.version ? v.version : "?", v.board ? v.board : "");
    ls_kv(sf, kv, 0, "firmware", t, A(TUI_WHITE | TUI_BRIGHT));
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 60000000);
    snprintf(t, sizeof(t), "%luh %02lum, %luK free", (unsigned long)(up / 60), (unsigned long)(up % 60),
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ls_kv(sf, kv, 1, "running", t, A(TUI_WHITE));
    const int ne = ls_errlog_count();
    const ls_errlog_rec_t *e = ls_errlog_newest();
    if (ne && e) snprintf(t, sizeof(t), "%d saved, last %.18s", ne, e->reason);
    else snprintf(t, sizeof(t), "nothing saved");
    ls_kv(sf, kv, 2, "error log", t, ls_ink_hue(A(ne ? TUI_YELLOW | TUI_BRIGHT : TUI_GREEN)));
    ls_kv(sf, kv, 3, "boot log", "start and end of this run", A(TUI_WHITE));
    ls_kv(sf, kv, 4, "taken out", "Wi-Fi names, keys, MAC, IP", ls_ink_hue(A(TUI_GREEN)));
    ls_kv(sf, kv, 5, "location", R.gps ? "rounded to about 1 km" : "not sent", ls_ink_hue(A(R.gps ? TUI_YELLOW | TUI_BRIGHT : TUI_GREEN)));
    snprintf(t, sizeof(t), "about %lu KB", (unsigned long)((R.size + 1023) / 1024));
    ls_kv(sf, kv, 6, "size", t, LS_ATTR_DIM);
    y += 7;
    /* the newest saved errors themselves, when there is room to show them */
    for (int i = 0; i < ne && i < 3 && area.h >= 50; i++) {
        ls_errlog_rec_t r;
        char when[32];
        if (!ls_errlog_get(i, &r)) break;
        ls_errlog_when(&r, when, sizeof(when));
        snprintf(t, sizeof(t), "%.11s  %.30s", r.wall > 0 ? when + 5 : "no clock", r.reason);
        tui_put_str(sf, c, x0 + 15, y++, t, ls_ink_hue(A(TUI_YELLOW)));
    }
    y++;

    /* the note: three lines, tap to type */
    if (y + 5 < bottom - 6) {
        const tui_rect nr = tui_rect_make(x0, y, cw, 5);
        snprintf(t, sizeof(t), "%d/99", (int)strlen(R.note));
        ls_panel_box(sf, nr, "NOTE", TUI_MAGENTA);
        tui_put_str(sf, nr, nr.x + nr.w - 2 - (int)strlen(t), nr.y + 4, t, LS_ATTR_FAINT);
        char lines[3][72];
        const int ww = cw - 4 < 71 ? cw - 4 : 71;
        const int k = R.note[0] ? ls_wrap_text(R.note, ww, lines[0], sizeof(lines[0]), 3) : 0;
        if (!k) tui_put_str(sf, nr, nr.x + 2, nr.y + 1, "Tap to say what you were doing (optional).", LS_ATTR_FAINT);
        for (int i = 0; i < k; i++) tui_put_str(sf, nr, nr.x + 2, nr.y + 1 + i, lines[i], ls_ink(LS_INK_TEXT, A(TUI_WHITE | TUI_BRIGHT)));
        R.note_hit = nr;
        y += 6;
    } else R.note_hit = tui_rect_make(0, -1, 0, 0);

    /* location, off unless asked */
    R.gps_hit = tui_rect_make(x0, y, cw, 2);
    tui_put_str(sf, c, x0 + 1, y, R.gps ? "[x]" : "[ ]", ls_ink_hue(A(R.gps ? TUI_YELLOW | TUI_BRIGHT : TUI_WHITE)));
    tui_put_str(sf, c, x0 + 5, y, "INCLUDE LOCATION", ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
    tui_put_str(sf, c, x0 + 5, y + 1, R.gps ? "Helps with map and GPS bugs." : "Off: no position leaves the board.", LS_ATTR_DIM);
    y += 3;

    /* the flight, then what came of it, down by the buttons that start it */
    const int room = bottom - y - 7;
    if (room > 0) y += room;
    float f = 0;
    if (R.phase == P_SENDING && !strcmp(st.what, "report") && st.total) f = (float)st.done / (float)st.total;
    if (R.phase == P_SENT) f = 1;
    if (y + 3 <= bottom) draw_flight(sf, c, x0 + 1, y, cw - 2, f);
    y += 4;
    const tui_rect sr = tui_rect_make(x0 + 1, y, cw - 2, bottom - y);
    switch (R.phase) {
    case P_SENDING:
        if (st.total) snprintf(t, sizeof(t), "sending %lu / %lu KB", (unsigned long)(st.done / 1024), (unsigned long)((st.total + 1023) / 1024));
        else snprintf(t, sizeof(t), "packing the report...");
        tui_put_str(sf, sr, sr.x, y, t, ls_ink_hue(A(TUI_GREEN | TUI_BRIGHT)));
        break;
    case P_SENT:
        tui_put_str(sf, sr, sr.x, y, "SENT  report id", ls_ink_hue(A(TUI_GREEN | TUI_BRIGHT)));
        tui_put_str(sf, sr, sr.x + 16, y, R.id, ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
        tui_put_str(sf, sr, sr.x, y + 1, "Quote the id if you write to us.", LS_ATTR_DIM);
        break;
    case P_FAILED:
        snprintf(t, sizeof(t), "Not sent: %s", R.why);
        tui_put_str(sf, sr, sr.x, y, t, ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)));
        tui_put_str(sf, sr, sr.x, y + 1, "SEND tries again.", LS_ATTR_DIM);
        break;
    default:
        if (!ls_hub_online()) tui_put_str(sf, sr, sr.x, y, "No Wi-Fi yet: connect in Settings first.", ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)));
        else tui_put_str(sf, sr, sr.x, y, "Nothing leaves until you press SEND.", A(TUI_CYAN));
        break;
    }
    ls_btn_bar_raised_row(sf, bar, btn, nb, -1);
}

/* --------------------------------------------------------------- input -- */

static bool act(char k)
{
    const bool busy = R.phase == P_SENDING;
    switch (k) {
    case 's': case '\r': if (R.phase != P_SENT) send(); return true;
    case 'n': if (!busy) ls_keyboard_open_plain("NOTE FOR THE REPORT", R.note, (int)sizeof(R.note) - 1, note_done); return true;
    case 'g': if (!busy) { R.gps = !R.gps; measure(); } return true;
    case 'x': R.open = false; if (R.phase == P_SENT) R.phase = P_EDIT; return true;
    default: return false;
    }
}

bool ls_report_sheet_key(ls_tk_t k, char ch)
{
    if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) return act('x');
    if (k == LS_TK_ENTER) return act('\r');
    if (k == LS_TK_CHAR) act((char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
    return true;                        /* the page underneath is not on the glass */
}

bool ls_report_sheet_touch(int col, int row)
{
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int b = ls_btn_hit(col, row);
    if (b >= 0 && b < nb) return act(btn[b].key);
    if (tui_rect_contains(R.note_hit, col, row)) return act('n');
    if (tui_rect_contains(R.gps_hit, col, row)) return act('g');
    return true;
}

/* ------------------------------------------- after a crash, at boot -- */

static void offer_act(uint32_t item, int a)
{
    (void)item;
    if (a == 1) { ls_report_offer_done(); return; }            /* NOT NOW: asked and answered */
    if (a == 0) {                                               /* SEND: as it is, answer as a notice */
        ls_report_offer_done();
        if (ls_report_send("sent from the after-crash notice", false)) {
            ls_hub_status_t st;
            ls_hub_status(&st);
            R.seq = st.seq;
            R.bg = true;
        }
        return;
    }
    const ls_app_t *d = ls_app_by_id("diag");                   /* the notice itself: the sheet, to look first */
    const int i = d ? ls_tui_screen_index_of(d->screen) : -1;
    if (i >= 0) ls_tui_screen_show(i);
    ls_report_offer_done();
    ls_report_sheet_open();
}

void ls_netui_tick(void)
{
    static bool asked;
    extern void ls_chat_tile_tick(void);
    if (R.bg) follow();
    ls_chat_tile_tick();
    if (asked || esp_timer_get_time() < 8000000) return;       /* HOME first, then the question */
    asked = true;
    if (!ls_report_offer_pending()) return;
    ls_notice_t n = { .accent = TUI_YELLOW, .screen = -1, .steady = true, .activate = offer_act };
    snprintf(n.title, sizeof(n.title), "AFTER A CRASH");
    snprintf(n.body, sizeof(n.body), "LakeShark restarted after a crash. Send the report so it gets fixed?");
    snprintf(n.actions[0], sizeof(n.actions[0]), "SEND");
    snprintf(n.actions[1], sizeof(n.actions[1]), "NOT NOW");
    ls_notify_post(&n);
}

/* ------------------------------------------------------ on HOME -- */

#include "../../ls_icons.h"
#include "../../ls_app_docs.h"

static bool netui_probe(ls_notice_t *out) { (void)out; ls_netui_tick(); return false; }

void ls_netui_register(void)
{
    extern const ls_tui_screen_t ls_scr_locker, ls_scr_chat;
    extern const char *ls_chat_tile_line(void);
    static const char LOCKER_ART[LS_ICON_ROWS][LS_ICON_COLS + 1] = {
        ".---.---. ", "|=- |=- | ", "|  o|o  | ", "|## |   | ", "|## |## | ", "'---'---' ",
    };
    static const char CHAT_ART[LS_ICON_ROWS][LS_ICON_COLS + 1] = {
        " .----.   ", "( hi! )   ", " '-.-'.--.", "   ' (...)", "      '-' ", "  #global ",
    };
    static ls_app_t apps[2];
    apps[0] = (ls_app_t){ .id = "locker", .name = "LOCKER", .sub = "files", .hue = TUI_CYAN, .cat = LS_APP_EXTRA,
                          .icon = ls_icon_art(LS_ICON_SLOT_LOCKER, LOCKER_ART, LS_ICON_UPDATE),
                          .screen = &ls_scr_locker, .doc = &ls_doc_locker };
    apps[1] = (ls_app_t){ .id = "chat", .name = "CHAT", .sub = ls_chat_tile_line(), .hue = TUI_MAGENTA, .cat = LS_APP_EXTRA,
                          .icon = ls_icon_art(LS_ICON_SLOT_CHAT, CHAT_ART, LS_ICON_WIRELESS),
                          .screen = &ls_scr_chat, .doc = &ls_doc_chat };
    for (int i = 0; i < 2; i++) ls_app_register(&apps[i]);
    ls_notify_add_probe(netui_probe);
}
