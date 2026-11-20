/* See ls_here.h. The radar is the screen's own art: you at the centre, north
   up, each system its kind's letter at its real bearing, the distance on a
   square-root scale so the near ones spread out. It changes only when the
   list or the position does. */
#include "ls_here.h"
#include "ls_location.h"
#include "ls_action.h"
#include "ls_app.h"
#include "ls_quick.h"
#include "ls_tui_ui.h"
#include "ls_textpack.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NMAX     24
#define FMAX     48
#define RANGE_KM 250.0f
#define A(f, b)  TUI_ATTR(f, b)
#define BR(c)    ((uint8_t)((c) | TUI_BRIGHT))

static const ls_here_cfg_t *s_cfg;
static EXT_RAM_BSS_ATTR ls_pack_sys_t s_sys[NMAX], s_cur;
static EXT_RAM_BSS_ATTR ls_pack_freq_t s_fq[FMAX];
static EXT_RAM_BSS_ATTR char s_msg[96];
static int s_n, s_sel, s_first, s_nf, s_fsel, s_ffirst, s_per = 2, s_rows;
static bool s_detail;
static tui_rect s_list;
int ls_packs_back = -1;

const char LS_HERE_GLYPH[] = "PDNFAWMHB";
const uint8_t LS_HERE_HUE[LS_PK_COUNT] = { BR(TUI_GREEN), BR(TUI_CYAN), BR(TUI_BLUE), BR(TUI_YELLOW),
                                           BR(TUI_MAGENTA), BR(TUI_WHITE), TUI_CYAN, BR(TUI_RED), TUI_YELLOW };

static void put(tui_surface *sf, tui_rect c, int x, int y, char ch, uint8_t at)
{
    if (tui_rect_contains(c, x, y)) tui_put_char(sf, c, x, y, ch, at);
}

/* Bearing from here in degrees, and its eight-point name. */
static const char *bearing(double la0, double lo0, double la1, double lo1, float *deg)
{
    static const char *const P[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    const float k = 0.0174533f;
    float a = atan2f((float)(lo1 - lo0) * cosf((float)la0 * k), (float)(la1 - la0)) / k;
    if (a < 0) a += 360.0f;
    *deg = a;
    return P[(int)((a + 22.5f) / 45.0f) % 8];
}

static void km_text(float km, char *out, size_t n)
{
    snprintf(out, n, km < 10 ? "%.1f" : "%.0f", (double)km);
}

void ls_here_open(const ls_here_cfg_t *cfg)
{
    s_cfg = cfg;
    s_detail = false;
    s_sel = s_first = 0;
    s_msg[0] = 0;
    ls_location_want();
}
void ls_here_close(void) { s_cfg = NULL; }
bool ls_here_up(const ls_here_cfg_t *cfg) { return s_cfg && s_cfg == cfg; }

const char *ls_here_count(const ls_here_cfg_t *cfg)
{
    static char t[16];
    ls_packs_status_t st;
    ls_packs_status(&st);
    const int n = ls_packs_nearest(cfg->kinds, RANGE_KM, s_sys, NMAX);
    if (n) snprintf(t, sizeof(t), "%d%s near", n, n == NMAX ? "+" : "");
    else snprintf(t, sizeof(t), "%s", st.have ? "NONE" : "NO PACK");
    return t;
}

void ls_packs_app_open(void)
{
    for (int i = 0; i < ls_app_count(); i++)
        if (!strcmp(ls_app_at(i)->id, "packs")) { ls_packs_back = ls_tui_screen_current(); ls_app_open(i); return; }
}

/* ------------------------------------------------------------- drawing -- */

static void radar(tui_surface *sf, tui_rect r, double lat, double lon)
{
    if (r.w < 9 || r.h < 5) return;
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const float rx = (float)(r.w - 1) / 2.0f, ry = (float)(r.h - 1) / 2.0f, k = 0.0174533f;
    const uint8_t rule = ls_ink(LS_INK_RULE, LS_ATTR_FAINT);
    /* Two rings: half the scale and the scale. */
    for (int ring = 1; ring <= 2; ring++)
        for (int a = 0; a < 360; a += ring == 2 ? 4 : 10)
            put(sf, r, cx + (int)lroundf(sinf((float)a * k) * rx * (float)ring / 2.0f),
                cy - (int)lroundf(cosf((float)a * k) * ry * (float)ring / 2.0f), '.', rule);
    for (int x = r.x; x < r.x + r.w; x += 2) put(sf, r, x, cy, '-', rule);
    for (int y = r.y; y < r.y + r.h; y++) put(sf, r, cx, y, ':', rule);
    put(sf, r, cx, r.y, 'N', A(BR(TUI_WHITE), TUI_BLACK));
    /* Scale: the farthest shown, rounded up to a figure a person says. */
    float far = 5.0f;
    for (int i = 0; i < s_n; i++) if (s_sys[i].dist_km > far) far = s_sys[i].dist_km;
    static const float NICE[] = { 5, 10, 25, 50, 100, 250 };
    for (unsigned i = 0; i < sizeof(NICE) / sizeof(NICE[0]); i++) if (far <= NICE[i]) { far = NICE[i]; break; }
    for (int pass = 0; pass < 2; pass++)
        for (int i = s_n - 1; i >= 0; i--) {
            const bool sel = i == s_sel && !s_detail ? true : s_detail && !strcmp(s_sys[i].id, s_cur.id) && !strcmp(s_sys[i].pack, s_cur.pack);
            if (sel != (pass == 1)) continue;
            float deg;
            bearing(lat, lon, s_sys[i].lat, s_sys[i].lon, &deg);
            const float f = sqrtf(s_sys[i].dist_km / far);
            const int x = cx + (int)lroundf(sinf(deg * k) * rx * f), y = cy - (int)lroundf(cosf(deg * k) * ry * f);
            const uint8_t h = LS_HERE_HUE[s_sys[i].kind % LS_PK_COUNT];
            put(sf, r, x, y, LS_HERE_GLYPH[s_sys[i].kind % LS_PK_COUNT], sel ? A(TUI_BLACK, h) : A(h, TUI_BLACK));
        }
    put(sf, r, cx, cy, '+', A(BR(TUI_WHITE), TUI_BLACK));
    char t[16];
    snprintf(t, sizeof(t), "%.0f km", (double)far);
    tui_put_str(sf, r, r.x + r.w - (int)strlen(t), r.y + r.h - 1, t, LS_ATTR_DIM);
}

static void line(tui_surface *sf, tui_rect c, int y, const char *s, uint8_t at)
{
    if (y >= c.y && y < c.y + c.h) tui_put_str(sf, c, c.x, y, s, at);
}

static void scroll_to(int sel, int *first, int rows)
{
    if (rows < 1) rows = 1;
    if (sel < *first) *first = sel;
    if (sel >= *first + rows) *first = sel - rows + 1;
    if (*first < 0) *first = 0;
}

static void kinds_text(uint16_t kinds, char *out, size_t n)
{
    out[0] = 0;
    for (unsigned i = 0; i < LS_PK_COUNT; i++)
        if (kinds & (1u << i)) snprintf(out + strlen(out), n - strlen(out), "%s%s", out[0] ? " " : "", ls_pack_kind_name(i));
}

/* The empty list: why, and the one tap that fixes it. */
static void empty(tui_surface *sf, tui_rect r, const ls_packs_status_t *st, bool have)
{
    const uint8_t hue = s_cfg->hue;
    int y = r.y + 1;
    line(sf, r, y++, !have ? LS_TEXT(t_here_no_pos, "No position yet: the GPS key turns it on.")
                     : st->installed ? LS_TEXT(t_here_no_cover, "No installed pack covers here.")
                                     : LS_TEXT(t_here_no_packs, "No data packs on the card yet."), A(BR(hue), TUI_BLACK));
    line(sf, r, y++, LS_TEXT(t_here_offer, "INSTALL MY AREA gets every pack within"), LS_ATTR_DIM);
    line(sf, r, y++, LS_TEXT(t_here_offer2, "50 km of you; PACKS picks regions by hand."), LS_ATTR_DIM);
    y++;
    char t[48];
    if (st->state == LS_PACKS_FETCHING) {
        ls_packs_status_text(t, sizeof(t));
        line(sf, r, y, t, A(BR(TUI_YELLOW), TUI_BLACK));
        if (st->total) ls_meter_ascii(sf, r, r.x + 18, y, r.w - 20, (float)st->got / (float)st->total, TUI_YELLOW);
        y += 2;
    } else if (st->state == LS_PACKS_NO_WIFI || st->state == LS_PACKS_FAILED) {
        line(sf, r, y++, st->state == LS_PACKS_FAILED ? LS_TEXT(t_here_failed, "A download failed; it tries again in 10 min.")
                                                      : LS_TEXT(t_here_nowifi, "No Wi-Fi: connect it in Settings,"), A(BR(TUI_RED), TUI_BLACK));
        if (st->state == LS_PACKS_NO_WIFI) line(sf, r, y++, LS_TEXT(t_here_nowifi2, "or copy packs from the desktop."), LS_ATTR_DIM);
        y++;
    }
    const int bh = ls_tui_compact() ? 1 : 3;
    if (y + bh <= r.y + r.h) {
        const ls_btn_t b = { "INSTALL MY AREA", "50 km", 'a', false, st->state == LS_PACKS_FETCHING, false };
        ls_btn_bar_raised_slot(sf, tui_rect_make(r.x, y, r.w, bh), &b, 1, -1, LS_BTN_SLOT_QUICK);
    }
}

static void list(tui_surface *sf, tui_rect r, double lat, double lon)
{
    s_list = r;
    s_per = ls_tui_compact() || r.h < 8 ? 1 : 2;
    s_rows = r.h / s_per;
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    scroll_to(s_sel, &s_first, s_rows);
    for (int k = 0; k < s_rows && s_first + k < s_n; k++) {
        const int i = s_first + k, y = r.y + k * s_per;
        const ls_pack_sys_t *s = &s_sys[i];
        const bool sel = i == s_sel;
        const uint8_t hue = LS_HERE_HUE[s->kind % LS_PK_COUNT];
        const uint8_t at = sel ? ls_ink(LS_INK_SELECT, A(TUI_BLACK, s_cfg->hue)) : ls_ink(LS_INK_STRONG, A(BR(TUI_WHITE), TUI_BLACK));
        char d[12], row[96];
        float deg;
        km_text(s->dist_km, d, sizeof(d));
        const char *b = bearing(lat, lon, s->lat, s->lon, &deg);
        if (sel) tui_fill(sf, tui_rect_make(r.x, y, r.w, 1), ' ', at);
        snprintf(row, sizeof(row), "%5s km %-2s ", d, b);
        tui_put_str(sf, r, r.x, y, row, sel ? at : A(TUI_CYAN, TUI_BLACK));
        const int kx = r.x + r.w - 5;
        snprintf(row, sizeof(row), "%.*s", kx - r.x - 13 > 0 ? kx - r.x - 13 : 0, s->name);
        tui_put_str(sf, r, r.x + 12, y, row, at);
        tui_put_str(sf, r, kx, y, ls_pack_kind_name(s->kind), sel ? at : A(hue, TUI_BLACK));
        if (s_per < 2) continue;
        char n[8] = "", who[24] = "", where[32] = "";
        ls_pack_extra(s, "n", n, sizeof(n));
        if (!ls_pack_extra(s, "ident", who, sizeof(who))) ls_pack_extra(s, "lic", who, sizeof(who));
        who[strcspn(who, ",")] = 0;
        if (!ls_pack_extra(s, "city", where, sizeof(where))) ls_pack_extra(s, "own", where, sizeof(where));
        snprintf(row, sizeof(row), "%s%s%s  %s", n, n[0] ? " ch  " : "", who, where);
        tui_put_str(sf, r, r.x + 12, y + 1, row, LS_ATTR_DIM);
    }
}

static void detail(tui_surface *sf, tui_rect r, double lat, double lon)
{
    const uint8_t hue = LS_HERE_HUE[s_cur.kind % LS_PK_COUNT];
    char t[96], d[12], who[32] = "", where[32] = "";
    float deg;
    int y = r.y;
    line(sf, r, y++, s_cur.name, A(hue, TUI_BLACK));
    km_text(s_cur.dist_km, d, sizeof(d));
    snprintf(t, sizeof(t), "%s km %s  radius %.0f km  %s", d, bearing(lat, lon, s_cur.lat, s_cur.lon, &deg),
             (double)s_cur.radius_km, ls_pack_kind_name(s_cur.kind));
    line(sf, r, y++, t, A(TUI_CYAN, TUI_BLACK));
    if (!ls_pack_extra(&s_cur, "ident", who, sizeof(who))) ls_pack_extra(&s_cur, "lic", who, sizeof(who));
    if (!ls_pack_extra(&s_cur, "own", where, sizeof(where))) ls_pack_extra(&s_cur, "city", where, sizeof(where));
    snprintf(t, sizeof(t), "%.20s  %s", who, where);
    line(sf, r, y++, t, LS_ATTR_DIM);
    /* The site's own action as one big key: the one tap that sets it up. */
    const int bh = ls_tui_compact() ? 1 : 3;
    if (s_cfg->site && y + bh + 2 < r.y + r.h) {
        snprintf(t, sizeof(t), "%d frequencies", s_nf);
        const ls_btn_t b = { s_cfg->site_label, t, 's', true, !s_nf, false };
        ls_btn_bar_raised_slot(sf, tui_rect_make(r.x, y, r.w, bh), &b, 1, -1, LS_BTN_SLOT_QUICK);
        y += bh;
    }
    tui_hline(sf, r, r.x, y++, r.w, '-', ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    s_list = tui_rect_make(r.x, y, r.w, r.y + r.h - y);
    s_per = 1;
    s_rows = s_list.h;
    if (!s_nf) { line(sf, r, y, LS_TEXT(t_here_no_freqs, "No frequencies listed for this one."), LS_ATTR_DIM); return; }
    scroll_to(s_fsel, &s_ffirst, s_rows);
    for (int k = 0; k < s_rows && s_ffirst + k < s_nf; k++) {
        const ls_pack_freq_t *f = &s_fq[s_ffirst + k];
        const bool sel = s_ffirst + k == s_fsel;
        const uint8_t at = sel ? ls_ink(LS_INK_SELECT, A(TUI_BLACK, s_cfg->hue)) : ls_ink(LS_INK_STRONG, A(BR(TUI_WHITE), TUI_BLACK));
        if (sel) tui_fill(sf, tui_rect_make(r.x, y + k, r.w, 1), ' ', at);
        snprintf(t, sizeof(t), "%4lu.%05lu", (unsigned long)(f->hz / 1000000u), (unsigned long)(f->hz % 1000000u / 10u));
        tui_put_str(sf, r, r.x + 1, y + k, t, at);
        snprintf(t, sizeof(t), "%-5s %-4s %s", f->mode, f->role, f->label);
        tui_put_str(sf, r, r.x + 13, y + k, t, sel ? at : A(hue, TUI_BLACK));
    }
}

void ls_here_draw(tui_surface *sf, tui_rect a)
{
    if (!s_cfg) return;
    tui_fill(sf, a, ' ', A(TUI_WHITE, TUI_BLACK));
    double lat = 0, lon = 0;
    const bool have = ls_location_here(&lat, &lon) != LS_LOC_SRC_NONE;
    ls_packs_status_t st;
    ls_packs_status(&st);
    s_n = have ? ls_packs_nearest(s_cfg->kinds, RANGE_KM, s_sys, NMAX) : 0;

    /* The key row: the GPS key, PACKS, BACK. */
    char gps[24], cnt[16];
    const ls_loc_state_t ls = ls_location_key(gps, sizeof(gps));
    const char *gv = !strncmp(gps, "GPS ", 4) ? gps + 4 : gps;
    snprintf(cnt, sizeof(cnt), "%u on card", st.installed);
    const ls_btn_t keys[3] = {
        { "GPS", gv, 'g', ls == LS_LOC_FIX, false, false },
        { "PACKS", cnt, 'p', !s_n, false, false },
        { "BACK", NULL, 'b', false, false, false },
    };
    const int bh = ls_tui_compact() ? 1 : 3;
    ls_btn_bar_raised_slot(sf, tui_rect_make(a.x, a.y, a.w, bh), keys, 3, -1, LS_BTN_SLOT_SCREEN);
    tui_rect body = tui_rect_make(a.x, a.y + bh + 1, a.w, a.h - bh - 2);
    /* A short window keeps the radar and the position: the radar gives the info its rows. */
    const bool wide = a.w > a.h * 2 && a.w >= 76;
    tui_rect rad, info, rest;
    if (wide) {
        rad = tui_rect_make(body.x, body.y, 31, body.h < 22 ? body.h - 7 : 15);
        info = tui_rect_make(body.x, rad.y + rad.h + 1, 31, body.y + body.h - rad.y - rad.h - 1);
        rest = tui_rect_make(body.x + 33, body.y, body.w - 33, body.h);
        tui_rect hole;                   /* a landscape camera hole at the edge: the list stops short of it */
        if (ls_tui_cutout(&hole) && hole.x > rest.x && hole.x < rest.x + rest.w) rest.w = hole.x - 1 - rest.x;
    } else {
        const int rh = ls_tui_compact() ? 0 : body.h >= 40 ? 13 : body.h >= 24 ? 9 : 0;
        rad = tui_rect_make(body.x, body.y, rh ? rh * 2 + 1 : 0, rh);
        info = tui_rect_make(body.x + rad.w + 2, body.y, body.w - rad.w - 2, rh);
        rest = tui_rect_make(body.x, body.y + rh + (rh ? 1 : 0), body.w, body.h - rh - (rh ? 1 : 0));
    }
    radar(sf, rad, lat, lon);
    if (info.h > 0) {
        char t[48], kt[40];
        int y = info.y;
        line(sf, info, y++, s_detail ? "THIS SITE" : "NEAR HERE", A(BR(s_cfg->hue), TUI_BLACK));
        kinds_text(s_cfg->kinds, kt, sizeof(kt));
        line(sf, info, y++, kt, A(s_cfg->hue, TUI_BLACK));
        y++;
        snprintf(t, sizeof(t), "%d within %.0f km", s_n, (double)RANGE_KM);
        line(sf, info, y++, t, ls_ink(LS_INK_TEXT, A(TUI_WHITE, TUI_BLACK)));
        if (have) {
            snprintf(t, sizeof(t), "%.3f%c %.3f%c", fabs(lat), lat < 0 ? 'S' : 'N', fabs(lon), lon < 0 ? 'W' : 'E');
            line(sf, info, y++, t, LS_ATTR_DIM);
        } else line(sf, info, y++, LS_TEXT(t_here_nopos, "no position yet"), A(BR(TUI_YELLOW), TUI_BLACK));
        ls_packs_status_text(t, sizeof(t));
        line(sf, info, y++, t, LS_ATTR_DIM);
        for (int i = 0; i < LS_PK_COUNT && y < info.y + info.h; i++) {
            if (!(s_cfg->kinds & (1u << i))) continue;
            snprintf(t, sizeof(t), "%c %s", LS_HERE_GLYPH[i], ls_pack_kind_name(i));
            line(sf, info, y++, t, A(LS_HERE_HUE[i], TUI_BLACK));
        }
    }
    if (rest.h > 0) tui_hline(sf, a, rest.x, rest.y - 1, rest.w, '=', A(s_cfg->hue, TUI_BLACK));
    if (s_detail) detail(sf, rest, lat, lon);
    else if (!s_n) empty(sf, rest, &st, have);
    else list(sf, rest, lat, lon);
    ls_safe_line(sf, a, a.y + a.h - 1,
                 s_msg[0] ? s_msg : s_detail ? LS_TEXT(t_here_hint_f, "tap a frequency twice to tune it")
                                             : s_n ? LS_TEXT(t_here_hint_l, "tap a row twice to open it") : "",
                 s_msg[0] ? A(BR(TUI_YELLOW), TUI_BLACK) : LS_ATTR_DIM);
}

/* --------------------------------------------------------------- input -- */

static void open_row(int i)
{
    if (i < 0 || i >= s_n) return;
    s_cur = s_sys[i];
    s_nf = ls_packs_freqs(&s_cur, s_fq, FMAX);
    s_fsel = s_ffirst = 0;
    s_detail = true;
    s_msg[0] = 0;
}

static void tune(int i)
{
    if (i < 0 || i >= s_nf) return;
    const ls_pack_freq_t *f = &s_fq[i];
    char mhz[24], name[25];
    snprintf(mhz, sizeof(mhz), "%lu.%06lu", (unsigned long)(f->hz / 1000000u), (unsigned long)(f->hz % 1000000u));
    snprintf(name, sizeof(name), "%.24s", f->label[0] ? f->label : s_cur.name);
    const char *m = !strcmp(f->mode, "P25P2") ? "P25" : !strcmp(f->mode, "FM") ? "WFM" : f->mode;
    const char *v[3] = { mhz, m, name };
    ls_args_t a = { .n = 3 };
    for (int k = 0; k < 3; k++) { a.v[k].kind = LS_VAL_TEXT; a.v[k].s = v[k]; }
    ls_val_t out = { LS_VAL_NONE };
    const ls_act_status_t st = ls_action_call("freq.tune", &a, &out, ls_quick_grant_builtin());
    snprintf(s_msg, sizeof(s_msg), "%s", out.kind == LS_VAL_TEXT && out.s ? out.s : ls_act_status_str(st));
    if (st == LS_ACT_OK) ls_here_close();
}

static void site(void)
{
    if (!s_detail || !s_cfg->site) return;
    if (s_cfg->site(&s_cur, s_fq, s_nf, s_msg, sizeof(s_msg))) ls_here_close();
}

static void back(void)
{
    if (s_detail) { s_detail = false; s_msg[0] = 0; }
    else ls_here_close();
}

static void key_button(int b)
{
    if (b == 0) ls_location_key_tap();
    else if (b == 1) ls_packs_app_open();
    else if (b == 2) back();
}

bool ls_here_key(ls_tk_t k, char ch)
{
    if (!s_cfg) return false;
    if (k == LS_TK_WHEEL_UP) k = LS_TK_UP;
    if (k == LS_TK_WHEEL_DOWN) k = LS_TK_DOWN;
    int *sel = s_detail ? &s_fsel : &s_sel;
    const int n = s_detail ? s_nf : s_n;
    if (k == LS_TK_UP || k == LS_TK_DOWN) {
        *sel += k == LS_TK_UP ? -1 : 1;
        if (*sel < 0) *sel = 0;
        if (*sel >= n) *sel = n ? n - 1 : 0;
        return true;
    }
    if (k == LS_TK_ENTER) { if (s_detail) tune(s_fsel); else open_row(s_sel); return true; }
    if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { back(); return true; }
    if (k != LS_TK_CHAR) return k < LS_TK_F1;   /* the F-keys still switch apps */
    if (ch == 'g' || ch == 'G') key_button(0);
    else if (ch == 'p' || ch == 'P') key_button(1);
    else if (ch == 's' || ch == 'S') site();
    else if (ch == 'b' || ch == 'B') back();
    else if ((ch == 'a' || ch == 'A') && !s_n) ls_packs_install_around(NAN, NAN, 50.0f, 0);
    return true;
}

bool ls_here_touch(int col, int row)
{
    if (!s_cfg) return false;
    int b = ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
    if (b >= 0) { key_button(b); return true; }
    if (!s_detail && !s_n) {
        if (ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK) == 0) ls_packs_install_around(NAN, NAN, 50.0f, 0);
        return true;
    }
    if (s_detail && s_cfg->site && ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK) == 0) { site(); return true; }
    if (!tui_rect_contains(s_list, col, row)) return true;
    const int i = (s_detail ? s_ffirst : s_first) + (row - s_list.y) / (s_per ? s_per : 1);
    if (s_detail) { if (i == s_fsel) tune(i); else if (i < s_nf) s_fsel = i; }
    else if (i == s_sel) open_row(i);
    else if (i < s_n) s_sel = i;
    return true;
}
