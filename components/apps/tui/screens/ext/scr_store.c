/* STORE: browse the app catalog, install, update and remove signed SD apps.
   A shop front over the shelves (the list), the picked app's card below,
   and a crate that fills as a download really arrives: nothing moves unless
   bytes do. State is in PSRAM; the backend is ls_store.c. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_deskui.h"
#include "../../ls_drag_scroll.h"
#include "../../ls_text.h"
#include "../../ls_app.h"
#include "../../ls_store.h"
#include "../../ls_lap.h"
#include "../../ls_icons.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

enum { C_ALL, C_UPD, C_MINE, C_RADIO, C_FIELD, C_SYS, C_USER, C_N };

EXT_RAM_BSS_ATTR static struct {
    int cat, sel, top, rows;        /* sel indexes vis[] */
    int vis[LS_STORE_MAX], nvis;
    int tab_x[C_N], tab_w[C_N], tab_y;
    tui_rect list;
    int64_t rm_until;               /* REMOVE asks twice */
    char rm_id[32];
    ls_drag_scroll_t drag;
    char lines[8][72], btn_have[16], btn_all[12];   /* card text and button values */
    char focus[32];                 /* store.open: the card to show once the catalog has it */
    bool free;                      /* a finger scrolled the list: the pick may be off screen until a key moves it */
} S;

#define A(fg) TUI_ATTR((fg), TUI_BLACK)
static const uint8_t GROUP_HUE[5] = {TUI_WHITE, TUI_GREEN, TUI_YELLOW, TUI_CYAN, TUI_MAGENTA};

/* The shop: an awning, shelves of boxes, crates by the door. */
static const char *const SHOP[7] = {
    " .-^-.-^-.-^-.-^-.-^-. ",
    "(_)(_)(_)(_)(_)(_)(_)(_)",
    " |  .-------.  .---. | ",
    " |  |[]{}[]<>|  |   | | ",
    " |  |<>[]{}[]|  | o | | ",
    " |  '-------'  |   | | ",
    "[x][x]=========|___|==",
};

static ls_store_state_t state_of(int i, char *have) { return ls_store_state(i, have, 16); }

static bool in_cat(int i)
{
    char have[16];
    const ls_store_app_t *a = ls_store_at(i);
    const ls_store_state_t st = state_of(i, have);
    switch (S.cat) {
    case C_UPD:  return st == LS_ST_UPDATE;
    case C_MINE: return have[0] != 0;
    case C_ALL:  return true;
    default:     return a->group == S.cat - C_RADIO + 1;
    }
}

static void build(void)
{
    S.nvis = 0;
    for (int i = 0; i < ls_store_count(); i++)
        if (in_cat(i)) S.vis[S.nvis++] = i;
    if (S.sel >= S.nvis) S.sel = S.nvis - 1;
    if (S.sel < 0) S.sel = 0;
}

static const ls_store_app_t *picked(int *idx)
{
    if (S.sel < 0 || S.sel >= S.nvis) return NULL;
    if (idx) *idx = S.vis[S.sel];
    return ls_store_at(S.vis[S.sel]);
}

static void enter(void)
{
    if (S.rows < 1) S.rows = 8;
    ls_store_open();
}

/* -------------------------------------------------------------- action -- */

typedef enum { ACT_NONE, ACT_GET, ACT_UPDATE } primary_t;

static primary_t primary(void)
{
    int i;
    char have[16];
    if (!picked(&i)) return ACT_NONE;
    const ls_store_state_t st = state_of(i, have);
    return st == LS_ST_NEW ? ACT_GET : st == LS_ST_UPDATE ? ACT_UPDATE : ACT_NONE;
}

static bool can_remove(void)
{
    int i = -1;
    char have[16];
    return picked(&i) && (state_of(i, have), have[0]);
}

static int buttons(ls_btn_t *b, int upd)
{
    int n = 0;
    const primary_t p = primary();
    const ls_store_app_t *a = picked(NULL);
    const bool asking = a && S.rm_until > esp_timer_get_time() && !strcmp(S.rm_id, a->id);
    if (p != ACT_NONE || !can_remove())
        b[n++] = (ls_btn_t){.label = p == ACT_UPDATE ? "U UPDATE" : "I GET", .value = a ? a->ver : "-",
                            .key = p == ACT_UPDATE ? 'u' : 'i', .dim = p == ACT_NONE};
    char *have = S.btn_have, *all = S.btn_all;
    if (can_remove()) {
        int i = -1;
        picked(&i);
        state_of(i, have);
        b[n++] = (ls_btn_t){.label = "X REMOVE", .value = asking ? "AGAIN" : have, .key = 'x', .on = asking};
    }
    snprintf(all, sizeof(S.btn_all), "%d UPD", upd);
    b[n++] = (ls_btn_t){.label = "A ALL", .value = upd ? all : "NONE", .key = 'a', .dim = !upd, .badge = upd > 0};
    b[n++] = (ls_btn_t){.label = "R RELOAD", .value = "LIST", .key = 'r'};
    return n;
}

static bool act(char k)
{
    const ls_store_app_t *a = picked(NULL);
    switch (k) {
    case 'i': case 'u': case '\r':
        if (a) ls_store_install(a->id);   /* when it cannot, the worker says why */
        return true;
    case 'x':
        if (!a || !can_remove()) return true;
        if (S.rm_until > esp_timer_get_time() && !strcmp(S.rm_id, a->id)) {
            S.rm_until = 0;
            ls_store_remove(a->id);
        } else {
            S.rm_until = esp_timer_get_time() + 4000000;
            snprintf(S.rm_id, sizeof(S.rm_id), "%s", a->id);
        }
        return true;
    case 'a': ls_store_update_all(); return true;
    case 'r': ls_store_refresh(); return true;
    default: return false;
    }
}

/* ---------------------------------------------------------------- draw -- */

static void draw_shop(tui_surface *sf, tui_rect r)
{
    static const uint8_t AWN[2] = {TUI_RED | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT};
    for (int y = 0; y < 7 && y < r.h; y++) {
        const char *l = SHOP[y];
        for (int x = 0; l[x] && x < r.w; x++) {
            const char c = l[x];
            uint8_t at = A(TUI_YELLOW);
            if (y < 2) at = A(AWN[(x / (y ? 3 : 4)) & 1]);                    /* striped awning */
            else if (y == 3 || y == 4) at = (x >= 5 && x <= 12) ? A(GROUP_HUE[1 + ((x - 5) / 2) % 4]) : A(TUI_YELLOW);
            else if (y == 6 && x < 6) at = A(TUI_YELLOW | TUI_BRIGHT);       /* crates */
            else if (c == '|' || c == '=' || c == '-' || c == '\'' || c == '.') at = A(TUI_YELLOW);
            if (c == 'o') at = A(TUI_CYAN | TUI_BRIGHT);
            tui_put_char(sf, r, r.x + x, r.y + y, c, ls_ink_hue(at));
        }
    }
}

/* A crate packed from the bottom as the bytes arrive; closed when idle. */
static void draw_crate(tui_surface *sf, tui_rect r, const ls_store_status_t *st)
{
    const bool dl = st->busy && st->phase == LS_SJ_DOWNLOAD && st->total;
    const int inner = r.h - 2, w = r.w - 2;
    if (inner < 1 || w < 2) return;
    const int fill = dl ? (int)((uint64_t)st->got * (uint64_t)(inner * w) / st->total) : 0;
    const uint8_t wood = A(TUI_YELLOW), lid = A(dl ? TUI_GREEN | TUI_BRIGHT : TUI_YELLOW);
    for (int x = 0; x < r.w; x++) {
        tui_put_char(sf, r, r.x + x, r.y, x == 0 || x == r.w - 1 ? '.' : dl ? ' ' : '-', lid);
        tui_put_char(sf, r, r.x + x, r.y + r.h - 1, x == 0 || x == r.w - 1 ? '\'' : '-', wood);
    }
    for (int y = 0; y < inner; y++) {
        tui_put_char(sf, r, r.x, r.y + 1 + y, '|', wood);
        tui_put_char(sf, r, r.x + r.w - 1, r.y + 1 + y, '|', wood);
        for (int x = 0; x < w; x++) {
            const int cell = (inner - 1 - y) * w + x;       /* bottom row first */
            const int l = inner > 1 ? y * (w - 1) / (inner - 1) : 0, lx = (w - 4) / 2;
            char c;
            uint8_t at = A(TUI_YELLOW);
            if (dl) { c = cell < fill ? (y & 1 ? '=' : '#') : ' '; at = A(y & 1 ? TUI_GREEN : TUI_GREEN | TUI_BRIGHT); }
            else if (y == inner / 2 && x >= lx && x < lx + 4) { c = "APPS"[x - lx]; at = A(TUI_CYAN | TUI_BRIGHT); }
            else c = x == l ? '\\' : x == w - 1 - l ? '/' : ' ';   /* the brace across a crate */
            tui_put_char(sf, r, r.x + 1 + x, r.y + 1 + y, c, at);
        }
    }
}

static void draw_head(tui_surface *sf, tui_rect r, const ls_store_status_t *st)
{
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    const bool art = r.w >= 44;
    if (art) draw_shop(sf, tui_rect_make(r.x, r.y, 24, r.h));
    const int cw = r.w >= 52 ? 10 : 0;
    if (cw) draw_crate(sf, tui_rect_make(r.x + r.w - cw, r.y, cw, r.h < 7 ? r.h : 7), st);
    const tui_rect info = tui_rect_make(r.x + (art ? 25 : 0), r.y, r.w - (art ? 25 : 0) - cw - 1, r.h);
    char t[64];
    int y = info.y;
    tui_put_str(sf, info, info.x, y++, "terminalbay.com", ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT)));
    snprintf(t, sizeof(t), "%s%s", st->online ? "ONLINE" : "OFFLINE", st->loaded && !st->live ? " (kept)" : "");
    tui_put_str(sf, info, info.x, y++, t, ls_ink_hue(A(st->online ? TUI_GREEN | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT)));
    snprintf(t, sizeof(t), "%d apps", st->n);
    tui_put_str(sf, info, info.x, y++, t, ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
    snprintf(t, sizeof(t), "%d update%s", st->updates, st->updates == 1 ? "" : "s");
    tui_put_str(sf, info, info.x, y++, t, st->updates ? ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)) : LS_ATTR_DIM);
    snprintf(t, sizeof(t), "fw %.12s", ls_store_fw());
    tui_put_str(sf, info, info.x, y++, t, LS_ATTR_DIM);
    if (st->busy) {
        static const char *const PH[] = {"", "reading catalog", "downloading", "checking", "removing"};
        snprintf(t, sizeof(t), "%s", PH[st->phase]);
        tui_put_str(sf, info, info.x, y++, t, ls_ink_hue(A(TUI_GREEN | TUI_BRIGHT)));
        if (st->phase == LS_SJ_DOWNLOAD && st->total) {
            snprintf(t, sizeof(t), "%lu/%lu KB", (unsigned long)(st->got / 1024), (unsigned long)((st->total + 1023) / 1024));
            tui_put_str(sf, info, info.x, y++, t, A(TUI_GREEN));
        }
    }
}

static void draw_tabs(tui_surface *sf, tui_rect r, int upd)
{
    static const char *const LONG[C_N] = {"ALL", "UPDATES", "INSTALLED", "RADIO", "FIELD", "SYSTEM", "USER"};
    static const char *const SHORT[C_N] = {"ALL", "UPD", "MINE", "RADIO", "FIELD", "SYS", "USER"};
    static const uint8_t HUE[C_N] = {TUI_WHITE, TUI_YELLOW, TUI_BLUE, TUI_GREEN, TUI_YELLOW, TUI_CYAN, TUI_MAGENTA};
    int need = 0;
    for (int c = 0; c < C_N; c++) need += (int)strlen(LONG[c]) + 3;
    const char *const *name = need + (upd ? 2 : 0) <= r.w ? LONG : SHORT;
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    S.tab_y = r.y;
    int x = r.x;
    for (int c = 0; c < C_N; c++) {
        char t[16];
        snprintf(t, sizeof(t), c == C_UPD && upd ? " %s %d " : " %s ", name[c], upd);
        const int w = (int)strlen(t);
        const bool on = c == S.cat;
        tui_put_str(sf, r, x, r.y, t, on ? ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, HUE[c] | TUI_BRIGHT)) : A(HUE[c]));
        S.tab_x[c] = x;
        S.tab_w[c] = w;
        x += w + 1;
    }
}

static void badge(char *t, size_t n, uint8_t *at, ls_store_state_t st, const ls_store_app_t *a, const char *have,
                  const ls_store_status_t *s)
{
    if (s->busy && !strcmp(s->job_id, a->id)) {
        if (s->phase == LS_SJ_DOWNLOAD && s->total) snprintf(t, n, "%3lu%%", (unsigned long)((uint64_t)s->got * 100 / s->total));
        else snprintf(t, n, "%s", s->phase == LS_SJ_REMOVE ? "REMOVING" : "CHECKING");
        *at = A(TUI_GREEN | TUI_BRIGHT);
        return;
    }
    switch (st) {
    case LS_ST_INSTALLED: snprintf(t, n, "INSTALLED"); *at = A(TUI_GREEN); break;
    case LS_ST_UPDATE:    snprintf(t, n, "%.6s>%.6s", have, a->ver); *at = A(TUI_YELLOW | TUI_BRIGHT); break;
    case LS_ST_NEEDS_FW:  snprintf(t, n, "NEEDS %.8s", a->minfw); *at = A(TUI_RED | TUI_BRIGHT); break;
    case LS_ST_BUILTIN:   snprintf(t, n, "BUILT-IN"); *at = LS_ATTR_FAINT; break;
    default:              snprintf(t, n, "GET"); *at = A(TUI_CYAN | TUI_BRIGHT); break;
    }
    *at = ls_ink_hue(*at);
}

static void draw_list(tui_surface *sf, tui_rect r, const ls_store_status_t *st)
{
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    S.list = r;
    S.rows = r.h / 2 > 0 ? r.h / 2 : 1;
    if (ls_drag_scroll(&S.drag, r, 2, &S.top, S.nvis, S.rows)) S.free = true;
    if (!S.free) {
        if (S.sel < S.top) S.top = S.sel;
        if (S.sel >= S.top + S.rows) S.top = S.sel - S.rows + 1;
    }
    if (S.top > S.nvis - S.rows) S.top = S.nvis - S.rows;
    if (S.top < 0) S.top = 0;
    if (!S.nvis) {
        if (!st->loaded)
            ls_panel_notice(sf, r, st->online ? "STORE" : "NO WI-FI",
                            st->online ? (st->busy ? "Reading the catalog..." : "No catalog yet.")
                                       : "Connect Wi-Fi in Settings,",
                            st->online ? "" : "or install apps from the desktop.");
        else
            ls_panel_notice(sf, r, "NOTHING HERE", S.cat == C_UPD ? "Every app is up to date." : "No apps on this shelf.", "");
        return;
    }
    const int rw = r.w - (S.nvis > S.rows ? 2 : 0);   /* room left for the shelf marker */
    for (int k = 0; k < S.rows && S.top + k < S.nvis; k++) {
        const int i = S.vis[S.top + k];
        const ls_store_app_t *a = ls_store_at(i);
        char have[16], t[40];
        const ls_store_state_t s = state_of(i, have);
        const bool on = S.top + k == S.sel;
        const int y = r.y + k * 2;
        const tui_rect row = tui_rect_make(r.x, y, r.w, 1);
        if (on) tui_fill(sf, row, ' ', ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, TUI_CYAN)));
        const uint8_t name_at = on ? ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, TUI_CYAN)) : A(TUI_WHITE | TUI_BRIGHT);
        tui_put_char(sf, row, r.x, y, '*', on ? name_at : A(GROUP_HUE[a->group <= 4 ? a->group : 0]));
        tui_put_str(sf, row, r.x + 2, y, a->name, name_at);
        tui_put_str(sf, row, r.x + 19, y, a->ver, on ? name_at : A(TUI_CYAN));
        uint8_t bat;
        badge(t, sizeof(t), &bat, s, a, have, st);
        const int bx = r.x + rw - (int)strlen(t);
        tui_put_str(sf, row, bx, y, t, on ? name_at : bat);
        /* second line: what it is, its size, where it is from */
        const tui_rect l2 = tui_rect_make(r.x, y + 1, r.w, 1);
        char kb[16];
        snprintf(kb, sizeof(kb), "%lu KB", (unsigned long)((a->size + 1023) / 1024));
        tui_put_str(sf, l2, r.x + 2, y + 1, a->sub[0] ? a->sub : a->cat, LS_ATTR_DIM);
        if (a->src) tui_put_str(sf, l2, r.x + rw - (int)strlen(kb) - 10, y + 1, "community", A(TUI_MAGENTA));
        tui_put_str(sf, l2, r.x + rw - (int)strlen(kb), y + 1, kb, LS_ATTR_FAINT);
    }
    if (S.nvis > S.rows) {    /* where the window is on the shelf */
        const int h = r.h, y0 = S.top * h / S.nvis, y1 = (S.top + S.rows) * h / S.nvis;
        for (int y = 0; y < h; y++)
            tui_put_char(sf, r, r.x + r.w - 1, r.y + y, y >= y0 && y <= y1 ? '|' : ':',
                         y >= y0 && y <= y1 ? A(TUI_CYAN | TUI_BRIGHT) : LS_ATTR_FAINT);
    }
}

/* The app's tile picture: its colour icon when the catalog carries one, else its character art. */
static void draw_art(tui_surface *sf, tui_rect in, const ls_store_app_t *a)
{
    const uint8_t hue = a->color & 7 ? a->color : TUI_CYAN;
    if (a->px[0][0]) {
        static EXT_RAM_BSS_ATTR uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2];
        for (int y = 0; y < LS_ICON_ROWS * 3; y++)
            for (int x = 0; x < LS_ICON_COLS * 2; x++) {
                const char c = a->px[y][x];
                px[y][x] = c >= '0' && c <= '9' ? (uint8_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint8_t)(c - 'a' + 10) : 0xFF;
            }
        ls_icon_draw_px(sf, tui_rect_make(in.x, in.y, in.w, in.h < LS_ICON_ROWS ? in.h : LS_ICON_ROWS), in.x + 1, in.y, px, A(hue));
        return;
    }
    for (int y = 0; y < 6 && y < in.h; y++) tui_put_str(sf, in, in.x + 1, in.y + y, a->art[y], A(hue));
}

static void draw_card(tui_surface *sf, tui_rect r, const ls_store_status_t *st)
{
    int i;
    const ls_store_app_t *a = picked(&i);
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    if (!a || r.h < 4) return;
    char have[16], t[96], caps[32];
    const ls_store_state_t s = state_of(i, have);
    ls_panel_box(sf, r, a->name, GROUP_HUE[a->group <= 4 ? a->group : 0]);
    const tui_rect in = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    int x0 = in.x;
    if (in.w >= 40) {      /* the app's own tile picture */
        draw_art(sf, in, a);
        x0 = in.x + 13;
    }
    const tui_rect kv = tui_rect_make(x0, in.y, in.x + in.w - x0, in.h);
    int y = 0;
    snprintf(t, sizeof(t), have[0] ? "%s  (have %s)" : "%s", a->ver, have);
    ls_kv_col(sf, kv, y++, "VERSION", t, s == LS_ST_UPDATE ? ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)) : A(TUI_WHITE | TUI_BRIGHT), 10);
    snprintf(t, sizeof(t), "%lu KB  %s", (unsigned long)((a->size + 1023) / 1024), a->cat);
    ls_kv_col(sf, kv, y++, "SIZE", t, A(TUI_WHITE), 10);
    if (a->src && a->publisher[0]) snprintf(t, sizeof(t), "%s (community)", a->publisher);
    else snprintf(t, sizeof(t), "%s", a->author[0] ? a->author : "LakeShark");
    ls_kv_col(sf, kv, y++, "BY", t, a->src ? A(TUI_MAGENTA | TUI_BRIGHT) : A(TUI_CYAN), 10);
    uint8_t cb = a->caps;
    const ls_lap_info_t *l = cb == 0xFF ? ls_lap_by_id(a->id) : NULL;
    if (l && l->installed) cb = l->manifest.flags;     /* an older catalog: the copy on the card says */
    ls_store_caps_text(cb, caps, sizeof(caps));
    ls_kv_col(sf, kv, y++, "USES", caps, cb != 0xFF && (cb & LAP_CAP_RADIO_TX) ? ls_ink_hue(A(TUI_RED | TUI_BRIGHT)) : A(TUI_GREEN), 10);
    snprintf(t, sizeof(t), "%s%s", a->minfw[0] ? a->minfw : "any", s == LS_ST_NEEDS_FW ? "  NEWER THAN THIS" : "");
    ls_kv_col(sf, kv, y++, "FIRMWARE", t, s == LS_ST_NEEDS_FW ? ls_ink_hue(A(TUI_RED | TUI_BRIGHT)) : A(TUI_GREEN), 10);
    if (a->license[0]) ls_kv_col(sf, kv, y++, "LICENSE", a->license, LS_ATTR_DIM, 10);
    y = y < 6 && in.w >= 40 ? 6 : y;
    char (*lines)[72] = S.lines;
    const int ww = in.w - 2 < 71 ? in.w - 2 : 71;
    if (s == LS_ST_NEEDS_FW) {
        snprintf(t, sizeof(t), "Needs firmware %s; this board runs %.16s. Update it in UPDATE first.", a->minfw, ls_store_fw());
        const int n = ls_wrap_text(t, ww, lines[0], sizeof(lines[0]), 2);
        for (int k = 0; k < n && y < in.h; k++) tui_put_str(sf, in, in.x + 1, in.y + y++, lines[k], ls_ink_hue(A(TUI_RED | TUI_BRIGHT)));
    }
    const int room = in.h - y - (a->credits[0] ? 3 : 0);
    int n = room > 0 ? ls_wrap_text(a->desc, ww, lines[0], sizeof(lines[0]), room < 8 ? room : 8) : 0;
    for (int k = 0; k < n; k++) tui_put_str(sf, in, in.x + 1, in.y + y++, lines[k], ls_ink(LS_INK_TEXT, A(TUI_WHITE)));
    if (a->credits[0] && y + 1 < in.h) {     /* third-party work the app carries */
        tui_put_str(sf, in, in.x + 1, in.y + ++y, "CREDITS", A(TUI_YELLOW));
        n = ls_wrap_text(a->credits, ww - 8, lines[0], sizeof(lines[0]), in.h - y < 4 ? in.h - y : 4);
        for (int k = 0; k < n; k++) tui_put_str(sf, in, in.x + 9, in.y + y++, lines[k], LS_ATTR_DIM);
    }
    (void)st;
}

/* store.open: ALL, with that app picked. Waits for a refresh under way,
   which may bring it; not there once the catalog is in, the list stays. */
static void focus_apply(const ls_store_status_t *st)
{
    if (!S.focus[0]) return;
    const int i = ls_store_find(S.focus);
    if (i < 0) {
        if (st->loaded && !st->busy) S.focus[0] = 0;
        return;
    }
    S.cat = C_ALL;
    build();
    for (int k = 0; k < S.nvis; k++)
        if (S.vis[k] == i) { S.sel = k; S.top = k; S.free = false; break; }  /* draw_list keeps it in view */
    S.focus[0] = 0;
}

/* The line under everything: what is going on, else what the store is. */
static const char *message(const ls_store_status_t *st, uint8_t *at)
{
    const ls_store_app_t *a = picked(NULL);
    if (a && S.rm_until > esp_timer_get_time() && !strcmp(S.rm_id, a->id)) {
        *at = ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT));
        return ls_tui_compact() ? "Click REMOVE again to delete. Saved data stays." : "Tap REMOVE again to delete. Saved data stays.";
    }
    if (st->msg[0]) { *at = ls_ink_hue(A(st->msg_bad ? TUI_YELLOW | TUI_BRIGHT : TUI_GREEN | TUI_BRIGHT)); return st->msg; }
    if (!st->online) { *at = ls_ink_hue(A(TUI_YELLOW)); return "No Wi-Fi: connect in Settings, or use the desktop."; }
    *at = A(TUI_CYAN);
    return "Signed apps from terminalbay.com, checked before install.";
}

/* A desk window: the keys as the one toolbar, the shelves as tabs over a
   page holding the list (a white well) and the picked app's card (a group
   box); the line under it all goes to the window's status bar. */
static void draw_desk(tui_surface *sf, tui_rect area, const ls_store_status_t *st, const ls_btn_t *btn, int nb)
{
    static const char *const LONG[C_N] = {"ALL", "UPDATES", "INSTALLED", "RADIO", "FIELD", "SYSTEM", "USER"};
    static const char *const SHORT[C_N] = {"ALL", "UPD", "MINE", "RADIO", "FIELD", "SYS", "USER"};
    tui_fill(sf, area, ' ', TUI_DEFAULT_ATTR);
    ls_btn_bar_raised_row(sf, tui_rect_make(area.x, area.y, area.w, 1), btn, nb, -1);
    char names[C_N][16];
    const char *np[C_N];
    int need = 0;
    for (int c = 0; c < C_N; c++) need += (int)strlen(LONG[c]) + 4;
    for (int c = 0; c < C_N; c++) {
        const char *nm = need + 3 <= area.w ? LONG[c] : SHORT[c];
        if (c == C_UPD && st->updates) snprintf(names[c], sizeof(names[c]), "%s %d", nm, st->updates);
        else snprintf(names[c], sizeof(names[c]), "%s", nm);
        np[c] = names[c];
    }
    int16_t tx[C_N], tw[C_N];
    const tui_rect tabs = tui_rect_make(area.x, area.y + 1, area.w, area.h - 1);
    tui_rect page = ls_dui_tabs(sf, tabs, np, C_N, S.cat, tx, tw);
    S.tab_y = tabs.y;
    for (int c = 0; c < C_N; c++) { S.tab_x[c] = tx[c] >= 0 ? tx[c] : -100; S.tab_w[c] = tw[c]; }
    if (page.w < 20 || page.h < 6) return;
    page = tui_rect_make(page.x + 1, page.y, page.w - 2, page.h);
    if (page.h >= 30) {     /* tall: the shop front across the top */
        const tui_rect g = ls_dui_group(sf, tui_rect_make(page.x, page.y, page.w, 9), "Catalog");
        draw_head(sf, tui_rect_make(g.x + 1, g.y, g.w - 2, g.h), st);
        page.y += 9;
        page.h -= 9;
    }
    const int lw = page.w >= 90 ? page.w / 2 : page.w * 11 / 20;
    const tui_rect list = ls_dui_well(sf, tui_rect_make(page.x, page.y, lw, page.h), NULL);
    draw_list(sf, list, st);
    draw_card(sf, tui_rect_make(page.x + lw + 1, page.y, page.w - lw - 1, page.h), st);
    uint8_t at;
    ls_tui_status_set(message(st, &at), NULL);
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_store_status_t st;
    ls_store_status(&st);
    ls_store_lock();
    build();
    focus_apply(&st);
    ls_btn_t btn[4];
    const int nb = buttons(btn, st.updates);
    if (ls_tui_compact()) {
        draw_desk(sf, area, &st, btn, nb);
        ls_store_unlock();
        return;
    }
    const int bar_h = ls_btn_raised_height(area, nb);
    const tui_rect bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    const tui_rect msg = tui_rect_make(area.x, bar.y - 1, area.w, 1);
    const int head_h = area.h >= 40 ? 7 : 0;
    const tui_rect head = tui_rect_make(area.x, area.y, area.w, head_h);
    const tui_rect tabs = tui_rect_make(area.x, area.y + head_h + (head_h ? 1 : 0), area.w, 1);
    const int body_y = tabs.y + 2, body_h = msg.y - body_y;
    const bool wide = area.w >= 96;
    tui_rect list, card;
    if (wide) {
        list = tui_rect_make(area.x, body_y, area.w / 2 - 1, body_h);
        card = tui_rect_make(area.x + area.w / 2, body_y, area.w - area.w / 2, body_h);
        /* A landscape camera hole at the right edge: the card stops one cell short of it. */
        tui_rect hole;
        if (ls_tui_cutout(&hole) && hole.x > card.x && hole.x < card.x + card.w) card.w = hole.x - 1 - card.x;
    } else {
        const int ch = body_h * 2 / 5 > 14 ? body_h * 2 / 5 : (body_h > 22 ? 14 : 0);
        list = tui_rect_make(area.x, body_y, area.w, body_h - ch);
        card = tui_rect_make(area.x, body_y + body_h - ch, area.w, ch);
    }
    if (head_h) draw_head(sf, head, &st);
    draw_tabs(sf, tabs, st.updates);
    tui_hline(sf, area, area.x, tabs.y + 1, area.w, '-', ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    draw_list(sf, list, &st);
    draw_card(sf, card, &st);
    ls_store_unlock();

    tui_fill(sf, msg, ' ', LS_ATTR_DIM);
    uint8_t mat;
    const char *m = message(&st, &mat);
    ls_safe_line(sf, msg, msg.y, m, mat);
    ls_btn_bar_raised_row(sf, bar, btn, nb, -1);
}

/* --------------------------------------------------------------- input -- */

static void set_cat(int c)
{
    S.cat = (c + C_N) % C_N;
    S.sel = S.top = 0;
    S.free = false;
}

static bool key(ls_tk_t k, char ch)
{
    S.free = false;
    switch (k) {
    case LS_TK_UP:         S.sel--; break;
    case LS_TK_DOWN:       S.sel++; break;
    case LS_TK_WHEEL_UP:   S.sel -= 2; break;
    case LS_TK_WHEEL_DOWN: S.sel += 2; break;
    case LS_TK_PGUP:       S.sel -= S.rows; break;
    case LS_TK_PGDN:       S.sel += S.rows; break;
    case LS_TK_HOME:       S.sel = 0; break;
    case LS_TK_END:        S.sel = S.nvis - 1; break;
    case LS_TK_LEFT:       set_cat(S.cat - 1); return true;
    case LS_TK_RIGHT: case LS_TK_TAB: set_cat(S.cat + 1); return true;
    case LS_TK_ENTER:      return act('\r');
    case LS_TK_CHAR:
        if (ch >= '1' && ch <= '7') { set_cat(ch - '1'); return true; }
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        return act(ch);
    default: return false;
    }
    if (S.sel >= S.nvis) S.sel = S.nvis - 1;
    if (S.sel < 0) S.sel = 0;
    return true;
}

static bool touch(int col, int row)
{
    if (ls_drag_scroll_tap_is_lift(&S.drag)) return true;
    ls_btn_t btn[4];
    ls_store_status_t st;
    ls_store_status(&st);
    const int nb = buttons(btn, st.updates);
    const int b = ls_btn_hit(col, row);
    if (b >= 0 && b < nb) return act(btn[b].key);
    if (row == S.tab_y || row == S.tab_y + 1)     /* the rule under the shelf names takes a finger too */
        for (int c = 0; c < C_N; c++)
            if (col >= S.tab_x[c] && col < S.tab_x[c] + S.tab_w[c]) { set_cat(c); return true; }
    if (tui_rect_contains(S.list, col, row)) {
        const int k = S.top + (row - S.list.y) / 2;
        if (k < S.nvis) S.sel = k;
        return true;
    }
    return false;
}

const ls_tui_screen_t ls_scr_store = {
    .name = "STORE", .hint = "up/down pick  left/right shelf  i get  u update  x remove  a update all  r refresh",
    .enter = enter, .draw = draw, .key = key, .touch = touch, .diff_switch = true, .wheel = true,
};

bool ls_store_focus(const char *id)
{
    const int idx = ls_tui_screen_index_of(&ls_scr_store);
    if (idx < 0) return false;                  /* not registered in this build or mode */
    snprintf(S.focus, sizeof(S.focus), "%s", id ? id : "");
    ls_tui_screen_show_later(idx);              /* after the caller's callback returns */
    return true;
}
