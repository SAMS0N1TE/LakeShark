/* FM's AIRBAND page in AM: the channel scanner.

   The whole band is one braille strip, every channel's last measured level
   standing up from the baseline, with the window being measured bracketed
   under it: the bracket walking across the band IS the scan. The channel
   being heard stands out in white, the ones that have talked recently in
   amber, lockouts in red. Under it what the scanner is doing now, then the
   hit list, newest first, each with how long it has talked. Idle, the page
   offers the plans one tap each. */
#include "fm_air_view.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_braille.h"
#include "../../ls_motion.h"
#include "../../ls_tui_ui.h"
#include "apps/fm/fm_bcast.h"
#include "apps/fm/fm_state.h"
#include "scan_engine.h"
#include "esp_attr.h"
#include "ls_textpack.h"

static EXT_RAM_BSS_ATTR fm_air_status_t s_st;
static EXT_RAM_BSS_ATTR uint8_t s_lv[AIR_MAX_CH];
static int s_nlv;
static int s_sel;
static int s_order[AIR_HITS], s_norder;
static tui_rect s_buttons, s_list;
static int s_list_first;

static const ls_btn_t START[] = {
    { .label = "AIR 25k" }, { .label = "AIR 8.33k" }, { .label = "CB AM" },
    { .label = "CB USB" }, { .label = "CB LSB" },
};
static const int START_PRESET[] = {
    AIR_PRESET_AIR25, AIR_PRESET_AIR833, AIR_PRESET_CB_AM, AIR_PRESET_CB_USB, AIR_PRESET_CB_LSB,
};
#define N_START ((int)(sizeof(START) / sizeof(START[0])))

static void snap(void)
{
    fm_air_status(&s_st);
    s_nlv = fm_air_levels(s_lv, AIR_MAX_CH);
    /* Hits newest first. */
    s_norder = 0;
    for (int i = 0; i < s_st.nhits && i < AIR_HITS; ++i) {
        int j = s_norder++;
        while (j > 0 && s_st.hits[s_order[j - 1]].last_us < s_st.hits[i].last_us) {
            s_order[j] = s_order[j - 1];
            --j;
        }
        s_order[j] = i;
    }
    if (s_sel >= s_norder) s_sel = s_norder - 1;
    if (s_sel < 0) s_sel = 0;
}

const char *fm_air_tab(void)
{
    fm_air_status(&s_st);
    const air_preset_t *p = air_preset(s_st.preset);
    return p && !strcmp(p->band, "CB") ? "CB" : "AIRBAND";
}

static void fmt_hz(char *out, size_t n, uint32_t hz)
{
    snprintf(out, n, "%u.%04u", (unsigned)(hz / 1000000u), (unsigned)(hz % 1000000u / 100u));
}

static void fmt_age(char *out, size_t n, int64_t us)
{
    const long s = (long)(us / 1000000);
    if (s < 60) snprintf(out, n, "%lds", s < 0 ? 0 : s);
    else if (s < 3600) snprintf(out, n, "%ldm", s / 60);
    else snprintf(out, n, "%ldh", s / 3600);
}

static bool recent_hit(int ch)
{
    for (int i = 0; i < s_st.nhits; ++i)
        if (s_st.hits[i].channel == ch && s_st.now_us - s_st.hits[i].last_us < 120000000LL) return true;
    return false;
}

/* What a channel shows on the band: its last measurement, or for one that
   has talked, its peak fading over five minutes, so the strip keeps where
   the activity is between the moments it is on the air. */
static int shown_level(int ch)
{
    int v = ch < s_nlv ? s_lv[ch] : 0;
    for (int i = 0; i < s_st.nhits; ++i) {
        if (s_st.hits[i].channel != ch) continue;
        const int64_t age = s_st.now_us - s_st.hits[i].last_us;
        const int64_t fade = 300000000LL;
        if (age < fade) {
            const int mem = (int)((int64_t)s_st.hits[i].peak_db * (fade - age) / fade);
            if (mem > v) v = mem;
        }
    }
    return v;
}

/* The band, `rows` tall: two braille columns a cell, each the loudest of
   the channels it covers, 40 dB to the top. */
static void draw_band(tui_surface *sf, tui_rect a, int x, int y, int w, int rows,
                      const air_preset_t *p)
{
    const int cols = w * 2, n = s_nlv;
    if (n <= 0 || cols <= 0) return;
    const int dots = rows * 4;
    for (int cx = 0; cx < w; ++cx) {
        int h[2] = {0, 0};
        uint8_t hue = TUI_GREEN;
        for (int sub = 0; sub < 2; ++sub) {
            const int c0 = (cx * 2 + sub) * n / cols, c1 = ((cx * 2 + sub + 1) * n / cols) - 1;
            int pk = 0;
            for (int ch = c0; ch <= c1 && ch < n; ++ch) {
                const int lv = shown_level(ch);
                if (lv > pk) pk = lv;
                if (ch == s_st.listen_ch) hue = TUI_WHITE | TUI_BRIGHT;
                else if (hue == TUI_GREEN && recent_hit(ch)) hue = TUI_YELLOW | TUI_BRIGHT;
            }
            h[sub] = pk * dots / 40;
            if (h[sub] > dots) h[sub] = dots;
            if (pk >= (int)s_st.thresh_db && hue == TUI_GREEN) hue = TUI_GREEN | TUI_BRIGHT;
        }
        for (int r = 0; r < rows; ++r) {
            /* r counts up from the baseline. */
            unsigned bits = 0;
            for (int sub = 0; sub < 2; ++sub) {
                int lv = h[sub] - r * 4;
                if (lv > 4) lv = 4;
                if (lv > 0) bits |= ls_braille_column((unsigned)lv, sub);
            }
            if (!bits && r == 0) bits = ls_braille_dot(0, 0) | ls_braille_dot(0, 1);
            ls_braille_put(sf, a, x + cx, y + rows - 1 - r, (int16_t)(0x2800 | bits),
                           TUI_ATTR(bits == (unsigned)(ls_braille_dot(0, 0) | ls_braille_dot(0, 1)) && r == 0
                                    ? (TUI_BLACK | TUI_BRIGHT) : hue, TUI_BLACK));
        }
    }
    /* The bracket: which part of the band this block was measured on. */
    const int ry = y + rows;
    for (int cx = 0; cx < w; ++cx) tui_put_char(sf, a, x + cx, ry, '-', LS_ATTR_FAINT);
    if (s_st.nch > 0 && s_st.phase == AIR_PH_HOP) {
        const int c0 = air_channel_find(p, s_st.win_lo_hz), c1 = air_channel_find(p, s_st.win_hi_hz);
        if (c0 >= 0 && c1 >= c0) {
            const int x0 = c0 * w / s_st.nch, x1 = c1 * w / s_st.nch;
            for (int cx = x0; cx <= x1 && cx < w; ++cx)
                tui_put_char(sf, a, x + cx, ry, cx == x0 ? '[' : cx == x1 ? ']' : '=',
                             TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        }
    }
    if (s_st.listen_ch >= 0 && s_st.nch > 0)
        tui_put_char(sf, a, x + s_st.listen_ch * w / s_st.nch, ry, '^',
                     TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    /* The scale: the band's ends, and the guard where it sits. */
    char lo[16], hi[16];
    snprintf(lo, sizeof(lo), "%.3f", air_channel_hz(p, 0) / 1e6);
    snprintf(hi, sizeof(hi), "%.3f MHz", air_channel_hz(p, air_channel_count(p) - 1) / 1e6);
    tui_put_str(sf, a, x, ry + 1, lo, LS_ATTR_DIM);
    tui_put_str(sf, a, x + w - (int)strlen(hi), ry + 1, hi, LS_ATTR_DIM);
    if (p->guard_hz && s_st.nch > 0) {
        const int g = air_channel_find(p, p->guard_hz);
        const int gx = g >= 0 ? x + g * w / s_st.nch : -1;
        if (gx - 2 > x + (int)strlen(lo)) {
            tui_put_char(sf, a, gx, ry, '|', TUI_ATTR(TUI_RED, TUI_BLACK));
            tui_put_str(sf, a, gx - 2, ry + 1, "121.5",
                        TUI_ATTR(s_st.guard_on ? TUI_RED | TUI_BRIGHT : TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
        }
    }
}

/* IN VIEW: the channels of the window the scanner last measured, a column
   each, standing against the line the carrier has to clear. */
static int draw_bank(tui_surface *sf, tui_rect a, int x, int y, int w, int rows,
                     const air_preset_t *p)
{
    int chs[32], n = 0;
    const int total = air_channel_count(p);
    /* Parked, the channels around the one being heard; scanning, the
       window measured last. */
    uint32_t lo = s_st.win_lo_hz, hi = s_st.win_hi_hz;
    if (s_st.listen_ch >= 0 && s_st.listen_hz) {
        lo = s_st.listen_hz - (s_st.listen_hz > AIR_SPAN_HZ / 2 ? AIR_SPAN_HZ / 2 : 0);
        hi = s_st.listen_hz + AIR_SPAN_HZ / 2;
    }
    for (int ch = 0; ch < total && n < 32; ++ch) {
        const uint32_t f = air_channel_hz(p, ch);
        if (f >= lo && f <= hi) chs[n++] = ch;
    }
    if (n == 0 || rows < 3) return y;
    /* In frequency order, which a list preset (CB) is not. */
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && air_channel_hz(p, chs[j]) < air_channel_hz(p, chs[j - 1]); --j) {
            const int t = chs[j]; chs[j] = chs[j - 1]; chs[j - 1] = t;
        }
    const int cw = w / n;
    if (cw < 1) return y;
    const int top = 40;
    const int line = rows - 1 - (int)(s_st.thresh_db * (rows - 1) / top);
    for (int i = 0; i < n; ++i) {
        const int ch = chs[i];
        const int lv = ch < s_nlv ? s_lv[ch] : 0;
        const int hgt = lv * (rows - 1) / top;
        const bool live = ch == s_st.listen_ch, over = lv >= (int)s_st.thresh_db;
        const uint8_t hue = live ? TUI_WHITE | TUI_BRIGHT : over ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN;
        const int bx = x + i * cw + (cw > 2 ? cw / 2 - (cw >= 4 ? 1 : 0) : 0);
        const int bw = cw >= 4 ? 2 : 1;
        for (int r = 0; r < rows; ++r) {
            const int yy = y + r;
            const bool lit = rows - 1 - r < hgt && r < rows;
            for (int k = 0; k < bw; ++k) {
                if (lit) tui_put_char(sf, a, bx + k, yy, '|', TUI_ATTR(hue, TUI_BLACK));
                else if (r == line) tui_put_char(sf, a, bx + k, yy, '-', TUI_ATTR(TUI_RED, TUI_BLACK));
                else if (r == rows - 1) tui_put_char(sf, a, bx + k, yy, '.', LS_ATTR_FAINT);
            }
        }
        if (cw >= 4) {
            char lab[8];
            const int num = air_channel_number(p, ch);
            if (num) snprintf(lab, sizeof(lab), "%d", num);
            else snprintf(lab, sizeof(lab), "%03u", (unsigned)(air_channel_hz(p, ch) % 1000000u / 1000u));
            tui_put_str(sf, a, x + i * cw + (cw - (int)strlen(lab)) / 2, y + rows, lab,
                        live ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
        }
    }
    return y + rows + 1;
}

static void draw_idle(tui_surface *sf, tui_rect a, int x0, int iw, int y, int last)
{
    static const char *const ART[] = {
        "      .   (( | ))   .     ",
        "   .        \\|/        .  ",
        "             |            ",
        "  __________/_\\__________ ",
    };
    for (int i = 0; i < 4 && y <= last; ++i, ++y)
        tui_put_str(sf, a, x0 + (iw - 26) / 2, y, ART[i], TUI_ATTR(i == 0 ? TUI_YELLOW | TUI_BRIGHT : TUI_CYAN, TUI_BLACK));
    y++;
    static const char *const TEXT[] = {
        "Airband 118-137 MHz in about a hundred hops:",
        "every channel of a 176 kHz window measured at",
        "once, parks on a carrier, moves on after the",
        "hang. 121.5 MHz is watched between hops.",
        "",
        "CB 1-40 in three. Pick a plan below.",
    };
    for (int i = 0; i < 6 && y <= last; ++i, ++y)
        tui_put_str(sf, a, x0, y, TEXT[i], LS_ATTR_DIM);
}

void fm_air_draw(tui_surface *sf, tui_rect area)
{
    snap();
    const air_preset_t *p = air_preset(s_st.preset);
    const bool running = p != NULL;
    const int bh = ls_btn_raised_height(area, running ? 4 : N_START);
    s_buttons = tui_rect_make(area.x, area.y + area.h - bh, area.w, bh);
    const tui_rect a = tui_rect_make(area.x, area.y, area.w, area.h - bh);
    char title[40], buf[96];
    snprintf(title, sizeof(title), "%s / %s", p ? p->band : "AIRBAND", p ? p->name : "CHANNEL SCAN");
    ls_panel_box(sf, a, title, TUI_CYAN);
    s_list = tui_rect_make(0, -1, 0, 0);
    if (a.w < 24 || a.h < 8) return;
    const int x0 = a.x + 2, iw = a.w - 4, last = a.y + a.h - 2;
    int y = a.y + 1;

    if (!running) {
        draw_idle(sf, a, x0, iw, y + 1, last);
        ls_btn_bar_raised_slot(sf, s_buttons, START, N_START, -1, LS_BTN_SLOT_WATERFALL);
        return;
    }

    /* The guard lamp on the frame: lit while it is being sampled or heard. */
    if (s_st.guard_on) {
        const bool live = s_st.guard_now || (s_st.listen_ch >= 0 && s_st.listen_hz == p->guard_hz);
        const char *g = live ? " GUARD 121.5 (!) " : " GUARD 121.5 ";
        tui_put_str(sf, a, a.x + a.w - 2 - (int)strlen(g), a.y, g,
                    live ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK) : TUI_ATTR(TUI_RED, TUI_BLACK));
    }

    /* THE BAND. */
    const int rows = a.h >= 30 ? 4 : 2;
    draw_band(sf, a, x0, y + 1, iw, rows, p);
    y += rows + 4;

    /* NOW. */
    if (y <= last) {
        if (s_st.phase == AIR_PH_LISTEN) {
            fmt_hz(buf, sizeof(buf), s_st.listen_hz);
            const bool up = s_st.carrier;
            tui_put_str(sf, a, x0, y, up ? "HEARING" : "HANG", TUI_ATTR(up ? TUI_GREEN | TUI_BRIGHT : TUI_YELLOW, TUI_BLACK));
            tui_put_str(sf, a, x0 + 8, y, buf, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            int x = x0 + 8 + (int)strlen(buf) + 1;
            const int num = air_channel_number(p, s_st.listen_ch);
            if (num) { snprintf(buf, sizeof(buf), "CH %d", num); tui_put_str(sf, a, x, y, buf, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)); x += (int)strlen(buf) + 1; }
            if (up && x + 8 < x0 + iw) {
                /* Level of the carrier in the channel, line characters. */
                const int lv = s_st.listen_ch < s_nlv ? s_lv[s_st.listen_ch] : 0;
                const int bw = 10;
                for (int i = 0; i < bw; ++i)
                    tui_put_char(sf, a, x + i, y, i * 40 / bw < lv ? '|' : '.',
                                 TUI_ATTR(i * 40 / bw < lv ? TUI_GREEN | TUI_BRIGHT : TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
                x += bw + 1;
            }
            if (s_st.held && x + 5 < x0 + iw) tui_put_str(sf, a, x, y, "HOLD", TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
        } else {
            char lo[16], hi[16];
            fmt_hz(lo, sizeof(lo), s_st.win_lo_hz);
            fmt_hz(hi, sizeof(hi), s_st.win_hi_hz);
            snprintf(buf, sizeof(buf), "%s %s-%s", s_st.guard_now ? "GUARD" : "SCAN", lo, hi);
            tui_put_str(sf, a, x0, y, buf, TUI_ATTR(s_st.guard_now ? TUI_RED | TUI_BRIGHT : TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        }
        y++;
    }
    if (y <= last) {
        LS_SNPRINTF(t_fm_air_view_sweep_lu_lu_lu, buf, sizeof(buf), "sweep %lu  %lu.%lu s  over %d dB  locked %d%s",
                 (unsigned long)s_st.sweeps, (unsigned long)(s_st.sweep_ms / 1000),
                 (unsigned long)(s_st.sweep_ms % 1000 / 100), (int)s_st.thresh_db, s_st.lockouts,
                 s_st.demod_as_am ? "  (SSB as AM)" : "");
        tui_put_str(sf, a, x0, y, buf, LS_ATTR_DIM);
        y += 2;
    }

    /* IN VIEW, where the page is tall enough to keep eight rows of hits. */
    if (last - y >= 22) {
        tui_put_str(sf, a, x0, y, "IN VIEW", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_hline(sf, a, x0 + 8, y, iw - 8, '-', LS_ATTR_FAINT);
        y = draw_bank(sf, a, x0, y + 1, iw, 9, p) + 1;
    }

    /* HITS. */
    if (y <= last) {
        tui_put_str(sf, a, x0, y, "HEARD", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_hline(sf, a, x0 + 6, y, iw - 6, '-', LS_ATTR_FAINT);
        y++;
    }
    s_list = tui_rect_make(x0, y, iw, last - y + 1);
    s_list_first = 0;
    if (s_sel >= s_list.h) s_list_first = s_sel - s_list.h + 1;
    uint32_t most = 1;
    for (int i = 0; i < s_st.nhits; ++i) if (s_st.hits[i].talk_ms > most) most = s_st.hits[i].talk_ms;
    if (!s_norder && y <= last) tui_put_str(sf, a, x0, y, "nothing yet", LS_ATTR_DIM);
    for (int r = 0; r < s_list.h && s_list_first + r < s_norder; ++r) {
        const int idx = s_list_first + r;
        const air_hit_t *h = &s_st.hits[s_order[idx]];
        const bool sel = idx == s_sel, live = s_st.listen_ch == h->channel;
        const uint8_t base = sel ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : TUI_ATTR(TUI_WHITE, TUI_BLACK);
        char f[16], age[12];
        fmt_hz(f, sizeof(f), h->hz);
        fmt_age(age, sizeof(age), s_st.now_us - h->last_us);
        const int num = air_channel_number(p, h->channel);
        if (num) snprintf(buf, sizeof(buf), "%c %s CH%-2d x%-3u", live ? '>' : ' ', f, num, h->count);
        else snprintf(buf, sizeof(buf), "%c %s%s x%-3u", live ? '>' : ' ', f, h->guard ? "!" : " ", h->count);
        const int yy = s_list.y + r;
        tui_put_str(sf, a, x0, yy, buf, live && !sel ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) :
                    h->guard && !sel ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK) : base);
        /* Talk time as a bar against the busiest. */
        int x = x0 + (int)strlen(buf) + 1;
        const int bw = iw - (x - x0) - 8;
        if (bw >= 4) {
            const int lit = (int)((uint64_t)h->talk_ms * (uint64_t)bw / most);
            for (int i = 0; i < bw; ++i)
                tui_put_char(sf, a, x + i, yy, i < lit ? '=' : '.',
                             TUI_ATTR(i < lit ? TUI_YELLOW | TUI_BRIGHT : TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
            x += bw + 1;
        }
        tui_put_str(sf, a, x, yy, age, LS_ATTR_DIM);
    }

    const ls_btn_t run[] = {
        { .label = "NEXT", .key = 'n' },
        { .label = "LOCKOUT", .key = 'l', .dim = s_st.phase != AIR_PH_LISTEN },
        { .label = s_st.held ? "RESUME" : "HOLD", .key = 'h', .on = s_st.held },
        { .label = "STOP", .key = 'x' },
    };
    ls_btn_bar_raised_slot(sf, s_buttons, run, 4, -1, LS_BTN_SLOT_WATERFALL);
}

static void listen_selected(void)
{
    if (s_sel < 0 || s_sel >= s_norder) return;
    fm_air_request(FM_AIR_REQ_LISTEN, s_st.hits[s_order[s_sel]].channel);
}

bool fm_air_key(ls_tk_t k, char ch)
{
    snap();
    const bool running = s_st.preset != AIR_PRESET_OFF;
    if (!running) return false;
    if (k == LS_TK_UP) { if (s_sel > 0) s_sel--; return true; }
    if (k == LS_TK_DOWN) { if (s_sel + 1 < s_norder) s_sel++; return true; }
    if (k == LS_TK_ENTER) { listen_selected(); return true; }
    if (k != LS_TK_CHAR) return false;
    switch (ch) {
    case 'n': case 'N': fm_air_request(FM_AIR_REQ_SKIP, 0); return true;
    case 'l': case 'L': fm_air_request(FM_AIR_REQ_LOCKOUT, 0); return true;
    case 'h': case 'H': fm_air_request(FM_AIR_REQ_HOLD, !s_st.held); return true;
    case 'x': case 'X': fm_air_request(FM_AIR_REQ_PRESET, 0); return true;
    default: return false;
    }
}

bool fm_air_touch(int col, int row)
{
    snap();
    const bool running = s_st.preset != AIR_PRESET_OFF;
    if (tui_rect_contains(s_buttons, col, row)) {
        const int slot = ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL);
        if (!running) {
            if (slot >= 0 && slot < N_START) {
                /* The stored-channel scanner owns the tuner too: one at a time. */
                scan_engine_stop();
                fm_air_request(FM_AIR_REQ_PRESET, START_PRESET[slot]);
            }
        } else if (slot == 0) fm_air_request(FM_AIR_REQ_SKIP, 0);
        else if (slot == 1) fm_air_request(FM_AIR_REQ_LOCKOUT, 0);
        else if (slot == 2) fm_air_request(FM_AIR_REQ_HOLD, !s_st.held);
        else if (slot == 3) fm_air_request(FM_AIR_REQ_PRESET, 0);
        return true;
    }
    if (running && tui_rect_contains(s_list, col, row)) {
        const int i = s_list_first + row - s_list.y;
        if (i >= 0 && i < s_norder) {
            /* First tap picks, a second on the same row listens. */
            if (i == s_sel) listen_selected();
            else s_sel = i;
        }
        return true;
    }
    return true;
}

/* ------------------------------------------------------------- OPTIONS -- */

static const char *const PLAN_NAMES[] = { "OFF", "AIR 25k", "AIR 8.33k", "CB AM", "CB USB", "CB LSB" };
static int o_plan(const ls_opt_t *o) { (void)o; fm_air_status(&s_st); return s_st.preset < 6 ? s_st.preset : 0; }
static void o_set_plan(const ls_opt_t *o, int v)
{
    (void)o;
    if (v < 0 || v >= 6) return;
    if (v) scan_engine_stop();
    fm_air_request(FM_AIR_REQ_PRESET, v);
}
static int o_guard(const ls_opt_t *o) { (void)o; return !fm_bcast_opt(FM_BC_GUARD_OFF); }
static void o_set_guard(const ls_opt_t *o, int v) { (void)o; fm_bcast_set_opt(FM_BC_GUARD_OFF, !v); }
static double o_thresh(const ls_opt_t *o) { (void)o; fm_air_status(&s_st); return s_st.thresh_db > 0 ? s_st.thresh_db : 10; }
static void o_set_thresh(const ls_opt_t *o, double v) { (void)o; fm_air_request(FM_AIR_REQ_THRESH, (int)(v + 0.5)); }
static void o_clear(const ls_opt_t *o) { (void)o; fm_air_request(FM_AIR_REQ_CLEAR_LOCKOUTS, 0); }
static void o_show_locks(const ls_opt_t *o, char *out, size_t n)
{
    (void)o; fm_air_status(&s_st); snprintf(out, n, "%d locked", s_st.lockouts);
}
static const ls_opt_t ROWS[] = {
    { .label = "PLAN", .kind = LS_OPT_CYCLE, .names = PLAN_NAMES, .n = 6, .get = o_plan, .set = o_set_plan },
    { .label = "GUARD 121.5", .kind = LS_OPT_TOGGLE, .get = o_guard, .set = o_set_guard },
    { .label = "OPENS ABOVE", .kind = LS_OPT_LEVEL, .step = 2, .num = o_thresh, .set_num = o_set_thresh,
      .lo = 4, .hi = 40, .unit = "dB over the window's noise floor" },
    { .label = "CLEAR LOCKOUTS", .kind = LS_OPT_ACTION, .act = o_clear, .show = o_show_locks },
};
const ls_opt_ctx_t fm_air_options = {
    .name = "CHANNEL SCAN", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(ROWS)
};

void fm_air_options_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    fm_air_status(&s_st);
    snprintf(out, n, "%s", PLAN_NAMES[s_st.preset < 6 ? s_st.preset : 0]);
}
