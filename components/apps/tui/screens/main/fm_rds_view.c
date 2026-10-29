/* FM's STATION page in WFM.

   The station's name stands in stroke figures the way a car radio's
   display carries it, the RadioText runs underneath as a ticker whose
   characters light as their segments land, and under that the stereo
   river: the left channel's level rising above a centre line and the
   right's falling below it, scrolling one column per update from the
   multiplex decoder, so a stereo programme reads as two different shapes
   and a mono one as a mirror. The block strip at the bottom is RDS itself,
   a mark per update for what arrived. Everything moves only when the
   decoder publishes: no signal, nothing changes. */
#include "fm_rds_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../ls_braille.h"
#include "../../ls_font.h"
#include "../../ls_i18n.h"
#include "../../ls_motion.h"
#include "../../ls_quick.h"
#include "../../ls_rffe_ui.h"
#include "../../ls_tui_ui.h"
#include "../../ls_action.h"
#include "apps/fm/fm_bcast.h"
#include "apps/fm/fm_state.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_textpack.h"

static EXT_RAM_BSS_ATTR fm_wfm_status_t s_st;
/* The decoder's history, oldest first, fetched once a draw. */
static EXT_RAM_BSS_ATTR uint8_t s_hl[FM_WFM_HIST], s_hr[FM_WFM_HIST], s_hq[FM_WFM_HIST];
static EXT_RAM_BSS_ATTR bool s_hmono[FM_WFM_HIST];
static int s_hn;
static char s_rt_prev[RDS_RT_LEN + 1];
static EXT_RAM_BSS_ATTR int64_t s_rt_at[RDS_RT_LEN];
static ls_fresh_t s_ps_fresh, s_ct_fresh;

static void snap(void) { fm_wfm_status(&s_st); }

bool fm_rds_stereo(void) { snap(); return s_st.running && s_st.stereo; }
bool fm_rds_synced(void) { snap(); return s_st.running && s_st.rds.synced; }

void fm_rds_glass_line(char *out, int n)
{
    snap();
    out[0] = 0;
    if (!s_st.running || (!s_st.rds.ps[0] && !s_st.rds.has_pi)) return;
    const bool rbds = !fm_bcast_opt(FM_BC_PTY_EU);
    const char *pty = rds_pty_name(s_st.rds.pty, rbds);
    snprintf(out, (size_t)n, "%s%s%s", s_st.rds.ps[0] ? s_st.rds.ps : "--------",
             pty[0] && s_st.rds.pty ? " / " : "", s_st.rds.pty ? pty : "");
}

static void history_take(void)
{
    s_hn = fm_wfm_history(s_hl, s_hr, s_hq, s_hmono, FM_WFM_HIST);
}

/* Sample `back` columns ago, 0 the newest. */
static int hist_at(int back) { return s_hn - 1 - back; }

/* The station's name on a dot-matrix display, the way a car radio shows
   it: the 9x16 face's own pixels, one braille dot each, ten dots a letter.
   Eight letters are 40 cells by 4 rows. */
#define PS_PITCH 10
#define PS_CELLS (RDS_PS_LEN * PS_PITCH / 2)
#define PS_ROWS  4
static bool ps_pixel(const char *ps, int px, int py)
{
    const ls_font_t *f = &ls_font_mono_14;
    const int ci = px / PS_PITCH;
    if (ci >= RDS_PS_LEN || !ps[ci]) return false;
    const ls_font_glyph_t *g = ls_font_glyph(f, (uint8_t)ps[ci]);
    if (!g) return false;
    const int gx = px % PS_PITCH - g->ofs_x;
    const int top = f->cell_h - f->base_line - g->ofs_y - g->box_h;
    const int gy = py - top;
    if (gx < 0 || gx >= g->box_w || gy < 0 || gy >= g->box_h) return false;
    const int k = gy * g->box_w + gx;
    const uint8_t b = f->bitmap[g->bitmap_index + k / 2];
    return ((k & 1) ? (b & 0x0F) : (b >> 4)) >= 7;
}
static void draw_ps(tui_surface *sf, tui_rect a, int x, int y, const char *ps,
                    uint8_t at, uint8_t unlit)
{
    static const uint8_t BIT[4][2] = { {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80} };
    for (int cy = 0; cy < PS_ROWS; ++cy)
        for (int cx = 0; cx < PS_CELLS; ++cx) {
            unsigned bits = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    if (ps_pixel(ps, cx * 2 + dx, cy * 4 + dy)) bits |= BIT[dy][dx];
            /* The unlit matrix shows faintly, as a VFD's grid does. */
            if (bits) ls_braille_put(sf, a, x + cx, y + cy, (int16_t)(0x2800 | bits), at);
            else if (unlit) ls_braille_put(sf, a, x + cx, y + cy, (int16_t)(0x2800 | 0x24), unlit);
        }
}

/* A channel meter in line characters: '|' lit up to the level, '-' past
   it, the colour stepping up as it nears full deviation. */
static void draw_channel(tui_surface *sf, tui_rect a, int x, int y, int w,
                         const char *name, float level)
{
    tui_put_str(sf, a, x, y, name, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    const int bw = w - 2;
    if (bw < 4) return;
    const int lit = (int)(fm_wfm_level_byte(level) * bw / 255);
    for (int i = 0; i < bw; ++i) {
        const bool on = i < lit;
        const uint8_t hue = !on ? (TUI_BLACK | TUI_BRIGHT)
                          : i > bw * 5 / 6 ? (TUI_RED | TUI_BRIGHT)
                          : i > bw * 2 / 3 ? (TUI_YELLOW | TUI_BRIGHT) : (TUI_GREEN | TUI_BRIGHT);
        tui_put_char(sf, a, x + 2 + i, y, on ? '|' : '-', TUI_ATTR(hue, TUI_BLACK));
    }
}

/* The river: L above the centre line, R below, newest at the right, two
   history columns a cell. */
static void draw_river(tui_surface *sf, tui_rect a, int x, int y, int w, int h)
{
    if (w < 8 || h < 2) return;
    const int half = h / 2;                 /* rows each side of the line   */
    const int dots = half * 4;
    for (int cx = 0; cx < w; ++cx) {
        for (int side = 0; side < 2; ++side) {
            for (int row = 0; row < half; ++row) {
                unsigned bits = 0;
                bool mono = true;
                for (int sub = 0; sub < 2; ++sub) {
                    const int back = (w - 1 - cx) * 2 + (1 - sub);
                    if (back >= s_hn) continue;
                    const int i = hist_at(back);
                    mono = s_hmono[i];
                    const int lv = (side == 0 ? s_hl[i] : s_hr[i]) * dots / 255;
                    /* Rows count away from the centre line. */
                    const int base = row * 4;
                    for (int k = 0; k < 4; ++k) {
                        if (lv <= base + k) break;
                        /* Above the line the fill grows upward: dot row 3 is
                           the one nearest the centre. Below, downward. */
                        bits |= ls_braille_dot(side == 0 ? (unsigned)k : (unsigned)(3 - k), sub);
                    }
                }
                const int cy = side == 0 ? y + half - 1 - row : y + half + 1 + row;
                const uint8_t hue = mono ? (TUI_WHITE) : side == 0 ? (TUI_GREEN | TUI_BRIGHT)
                                                                 : (TUI_CYAN | TUI_BRIGHT);
                if (bits) ls_braille_put(sf, a, x + cx, cy, (int16_t)(0x2800 | bits), TUI_ATTR(hue, TUI_BLACK));
            }
        }
        tui_put_char(sf, a, x + cx, y + half, '-', LS_ATTR_FAINT);
    }
    tui_put_str(sf, a, x - 2, y + half - 1, "L", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, a, x - 2, y + half + 1, "R", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
}

/* What RDS delivered, a mark per update, newest at the right. */
static void draw_blocks(tui_surface *sf, tui_rect a, int x, int y, int w)
{
    for (int cx = 0; cx < w; ++cx) {
        const int back = w - 1 - cx;
        char ch = ' ';
        uint8_t at = LS_ATTR_FAINT;
        if (back < s_hn) {
            switch (s_hq[hist_at(back)]) {
            case 1: ch = '|'; at = TUI_ATTR(TUI_GREEN, TUI_BLACK); break;
            case 2: ch = ':'; at = TUI_ATTR(TUI_YELLOW, TUI_BLACK); break;
            case 3: ch = 'x'; at = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK); break;
            default: ch = '.'; break;
            }
        }
        tui_put_char(sf, a, x + cx, y, ch, at);
    }
}

static void line(tui_surface *sf, tui_rect a, int y, const char *text, uint8_t at)
{
    ls_safe_line(sf, tui_rect_make(a.x + 2, a.y, a.w - 4, a.h - 1), y, text, at);
}

/* The RadioText, wrapped at spaces across up to `rows` lines. A character
   that changed in the last second and a half is drawn white: that is a
   segment arriving. */
static int draw_rt(tui_surface *sf, tui_rect a, int x, int y, int w, int rows, const char *rt)
{
    const int64_t now = esp_timer_get_time();
    const int len = (int)strlen(rt);
    for (int i = 0; i < len && i < RDS_RT_LEN; ++i)
        if (s_rt_prev[i] != rt[i]) s_rt_at[i] = now;
    memcpy(s_rt_prev, rt, sizeof(s_rt_prev) - 1);
    s_rt_prev[len < RDS_RT_LEN ? len : RDS_RT_LEN] = 0;
    int pos = 0, row = 0;
    while (pos < len && row < rows) {
        int take = len - pos < w ? len - pos : w;
        if (pos + take < len) {
            int cut = take;
            while (cut > 0 && rt[pos + cut] != ' ') --cut;
            if (cut > w / 3) take = cut;
        }
        for (int i = 0; i < take; ++i) {
            const bool hot = now - s_rt_at[pos + i] < 1500000;
            tui_put_char(sf, a, x + i, y + row, rt[pos + i],
                         TUI_ATTR(hot ? TUI_WHITE | TUI_BRIGHT : TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        }
        pos += take;
        while (pos < len && rt[pos] == ' ') ++pos;
        ++row;
    }
    return y + (row ? row : 1);
}

/* Call letters, PI, the programme type as a chip, the flags. */
static void draw_who(tui_surface *sf, tui_rect a, int x, int y, int right, bool rbds)
{
    const rds_info_t *r = &s_st.rds;
    char buf[32], cs[5] = "";
    if (r->has_pi && rbds) rds_callsign(r->pi, cs);
    if (cs[0]) { tui_put_str(sf, a, x, y, cs, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)); x += 6; }
    snprintf(buf, sizeof(buf), r->has_pi ? "PI %04X" : "PI ----", r->pi);
    tui_put_str(sf, a, x, y, buf, r->has_pi ? TUI_ATTR(TUI_WHITE, TUI_BLACK) : LS_ATTR_DIM);
    x += (int)strlen(buf) + 2;
    const char *pty = rds_pty_name(r->pty, rbds);
    if (r->has_pi && r->pty && pty[0] && x + (int)strlen(pty) + 2 < right) {
        snprintf(buf, sizeof(buf), "[%s]", pty);
        tui_put_str(sf, a, x, y, buf, TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
        x += (int)strlen(buf) + 2;
    }
    if (r->tp && x + 3 < right) { tui_put_str(sf, a, x, y, "TP", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)); x += 3; }
    if (r->ta && x + 3 < right) { tui_put_str(sf, a, x, y, "TA", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)); x += 3; }
    if (r->has_pi && x + 6 < right) tui_put_str(sf, a, x, y, r->music ? "MUSIC" : "SPEECH", LS_ATTR_DIM);
}

static int draw_clock_af(tui_surface *sf, tui_rect a, int x, int y, int right)
{
    const rds_info_t *r = &s_st.rds;
    char buf[64];
    if (r->ct_valid) {
        const int64_t local = r->ct_unix + (int64_t)r->ct_offset_half * 1800;
        const int um = (int)((r->ct_unix / 60) % 1440), lm = (int)(((local / 60) % 1440 + 1440) % 1440);
        const int off = r->ct_offset_half * 30, ao = off < 0 ? -off : off;
        LS_SNPRINTF(t_fm_rds_view_clock_02d_02d_utc, buf, sizeof(buf), "CLOCK %02d:%02d UTC  local %02d:%02d (%c%d:%02d)",
                 um / 60, um % 60, lm / 60, lm % 60, off < 0 ? '-' : '+', ao / 60, ao % 60);
        tui_put_str(sf, a, x, y++, buf,
                    ls_fresh_attr(ls_fresh(&s_ct_fresh, r->ct_seq, 2000),
                                  TUI_WHITE | TUI_BRIGHT, TUI_GREEN, TUI_BLACK));
    }
    if (r->af_n) {
        tui_put_str(sf, a, x, y, "ALSO ON", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        int ax = x + 8;
        for (int i = 0; i < r->af_n && ax + 6 < right; ++i) {
            const uint32_t khz = rds_af_khz(r->af[i]);
            snprintf(buf, sizeof(buf), "%u.%u", (unsigned)(khz / 1000), (unsigned)(khz % 1000 / 100));
            tui_put_str(sf, a, ax, y, buf, TUI_ATTR(TUI_YELLOW, TUI_BLACK));
            ax += (int)strlen(buf) + 1;
        }
        y++;
    }
    return y;
}

void fm_rds_draw(tui_surface *sf, tui_rect a)
{
    snap();
    history_take();
    const rds_info_t *r = &s_st.rds;
    const bool rbds = !fm_bcast_opt(FM_BC_PTY_EU);
    const uint8_t dim = LS_ATTR_DIM;
    char buf[96];

    ls_panel_box(sf, a, "WFM / STATION", TUI_MAGENTA);
    if (a.w < 24 || a.h < 6) return;
    const int x0 = a.x + 2, iw = a.w - 4, last = a.y + a.h - 2;
    int y = a.y + 1;

    /* The pilot lamp on the frame: doubled rings when the decoder holds the
       19 kHz pilot, one dim ring when it does not. */
    const char *lamp = s_st.stereo ? " ((o)) STEREO " : s_st.pilot ? " ( o ) MONO " : " ( . ) MONO ";
    tui_put_str(sf, a, a.x + a.w - 2 - (int)strlen(lamp), a.y, lamp,
                s_st.stereo ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK) : dim);

    if (!s_st.running) {
        line(sf, a, y + 1, LS_TEXT(t_fm_rds_view_tune_wfm_to_a, "Tune WFM to a broadcast station."), dim);
        line(sf, a, y + 3, LS_TEXT(t_fm_rds_view_stereo_locks_on_the, "Stereo locks on the 19 kHz pilot; the station's"), dim);
        line(sf, a, y + 4, LS_TEXT(t_fm_rds_view_name_and_text_arrive, "name and text arrive on the 57 kHz RDS carrier."), dim);
        return;
    }

    /* Two columns when the pane is wide (landscape): the card on the left,
       the river on the right. */
    const bool split = iw >= 90;
    const int cw = split ? iw * 11 / 20 : iw;
    const int right = x0 + cw;

    /* NAME. */
    const bool have_ps = r->ps[0] != 0;
    const uint8_t ps_at = have_ps ? ls_fresh_attr(ls_fresh(&s_ps_fresh, (uint32_t)(r->ps[0] | r->ps[3] << 8 | r->ps[7] << 16), 1200),
                                                   TUI_WHITE | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                                  : dim;
    if (cw >= PS_CELLS && a.h >= 14) {
        draw_ps(sf, a, x0 + (cw - PS_CELLS) / 2, y + 1, have_ps ? r->ps : "--------", ps_at,
                TUI_ATTR(TUI_YELLOW, TUI_BLACK) == ps_at ? 0 : LS_ATTR_FAINT);
        y += PS_ROWS + 2;
    } else {
        tui_put_str(sf, a, x0, y++, have_ps ? r->ps : "--------", ps_at);
    }

    if (y <= last) { draw_who(sf, a, x0, y, right, rbds); y += 2; }

    /* TEXT. */
    if (y <= last) {
        tui_put_str(sf, a, x0, y, "~", TUI_ATTR(TUI_CYAN, TUI_BLACK));
        if (r->rt[0]) y = draw_rt(sf, a, x0 + 2, y, cw - 2, split ? 4 : 3, r->rt) + 1;
        else {
            tui_put_str(sf, a, x0 + 2, y, r->synced ? "no RadioText from this station"
                                                     : "listening for RDS on 57 kHz", dim);
            y += 2;
        }
    }
    if (split) y = draw_clock_af(sf, a, x0, y, right);

    /* RIVER and meters: the right column in landscape, the rest of the page
       in portrait less the rows the clock, the meters and the strip need. */
    const int rx = split ? right + 4 : x0 + 2;
    const int rw = split ? x0 + iw - rx : iw - 2;
    int ry = split ? a.y + 2 : y;
    const int tail = split ? 4 : 4 + (r->ct_valid ? 1 : 0) + (r->af_n ? 1 : 0);
    int rh = last - ry - tail;
    if (rh > 22) rh = 22;
    rh &= ~1;
    if (rh >= 4) {
        draw_river(sf, a, rx, ry, rw, rh + 1);
        ry += rh + 2;
    }
    if (ry + 1 <= last) {
        const int mw = rw > 34 ? rw - 14 : rw;
        draw_channel(sf, a, rx - 2, ry, mw + 2, "L", s_st.lvl_l);
        draw_channel(sf, a, rx - 2, ry + 1, mw + 2, "R", s_st.lvl_r);
        if (rw > 34) {
            snprintf(buf, sizeof(buf), "PILOT %2d%%", (int)(s_st.pilot_level * 100.0f + 0.5f));
            tui_put_str(sf, a, rx + mw + 2, ry, buf, s_st.pilot ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK) : dim);
            tui_put_str(sf, a, rx + mw + 2, ry + 1,
                        fm_bcast_opt(FM_BC_MONO) ? "MONO SET" : s_st.stereo ? "SEPARATED" : "SUM ONLY", dim);
        }
        ry += 3;
    }
    if (!split && ry <= last - 1) ry = draw_clock_af(sf, a, x0, ry, x0 + iw);

    /* RDS itself, against the frame. */
    const uint32_t total = r->blocks_ok + r->blocks_fixed + r->blocks_bad;
    if (total)
        snprintf(buf, sizeof(buf), "RDS %s %lu%%", r->synced ? "LOCK" : "hunt",
                 (unsigned long)((r->blocks_ok + r->blocks_fixed) * 100u / total));
    else snprintf(buf, sizeof(buf), "RDS %s", fm_bcast_opt(FM_BC_RDS_OFF) ? "OFF" : "hunt");
    const int bx = x0 + (int)strlen(buf) + 1;
    tui_put_str(sf, a, x0, last, buf, r->synced ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK) : dim);
    snprintf(buf, sizeof(buf), " %lu groups", (unsigned long)r->groups);
    const int gx = x0 + iw - (int)strlen(buf);
    if (gx - bx > 8) {
        draw_blocks(sf, a, bx, last, gx - bx - 1);
        tui_put_str(sf, a, gx, last, buf, dim);
    }
}

/* ------------------------------------------------------------- OPTIONS -- */

static int o_bit(const ls_opt_t *o) { return fm_bcast_opt((uint32_t)o->arg) ? 1 : 0; }
static void o_set_bit(const ls_opt_t *o, int v) { fm_bcast_set_opt((uint32_t)o->arg, v != 0); }
/* Rows whose bit means "off": shown the other way round. */
static int o_not_bit(const ls_opt_t *o) { return !o_bit(o); }
static void o_set_not_bit(const ls_opt_t *o, int v) { o_set_bit(o, !v); }
static double o_gain(const ls_opt_t *o) { (void)o; return FM.gain_tenths / 10.0; }
static void o_set_gain(const ls_opt_t *o, double db)
{
    (void)o;
    ls_args_t args = {.n = 1}; ls_val_t out;
    args.v[0].kind = LS_VAL_FLOAT; args.v[0].f = (float)db;
    ls_action_call("fm.gain", &args, &out, ls_quick_grant_builtin());
}
static void o_show_gain(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    if (FM.gain_tenths <= 0) snprintf(out, n, "AUTO");
    else snprintf(out, n, "%d.%d dB", FM.gain_tenths / 10, FM.gain_tenths % 10);
}
static const char *const STEREO_NAMES[] = { "AUTO", "MONO" };
static const char *const PTY_NAMES[] = { "RBDS (AMERICAS)", "RDS (EUROPE)" };
static const ls_opt_t ROWS[] = {
    { .label = "STEREO", .kind = LS_OPT_CYCLE, .arg = FM_BC_MONO, .names = STEREO_NAMES, .n = 2,
      .get = o_bit, .set = o_set_bit },
    { .label = "RDS", .kind = LS_OPT_TOGGLE, .arg = FM_BC_RDS_OFF, .get = o_not_bit, .set = o_set_not_bit },
    { .label = "PROGRAMME TYPES", .kind = LS_OPT_CYCLE, .arg = FM_BC_PTY_EU, .names = PTY_NAMES, .n = 2,
      .get = o_bit, .set = o_set_bit },
    { .label = "SET CLOCK FROM RDS", .kind = LS_OPT_TOGGLE, .arg = FM_BC_CLOCK_OFF,
      .get = o_not_bit, .set = o_set_not_bit },
    { .label = "GAIN", .kind = LS_OPT_LEVEL, .step = 2, .radios = LS_OPT_SDR,
      .num = o_gain, .set_num = o_set_gain, .lo = 0, .hi = 49.6,
      .unit = "dB, 0 is automatic", .show = o_show_gain },
    /* FRONT END (bias-T and the rest), as on every SDR app's list. */
    LS_RFFE_MENUS,
};
const ls_opt_ctx_t fm_wfm_options = {
    .name = "WFM", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(ROWS)
};
