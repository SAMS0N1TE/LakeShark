/* DIGITAL MODES: FT8, FT4 and WSPR heard right now, slotted by GPS time.

   One look answers "what is on the band": the Costas emblem fills column by
   column with the real slot clock, the band strip is the live audio
   spectrum in braille with the last slot's callsigns pinned where they
   were heard, and the decode list grows from the top as each slot is
   decoded, every new line lit for a moment when it lands. Nothing moves
   while nothing is heard: no audio leaves the spectrum flat and the list
   still. Settings live in one OPTIONS tree; the runtime is digi_engine. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../ls_app.h"
#include "../../ls_braille.h"
#include "../../ls_glyph.h"
#include "../../ls_options.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "digi_engine.h"
#include "digi_ftx_tables.h"
#include "esp_timer.h"
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#endif

#define LAND_MS 2200            /* how long a new decode stays lit */
#define ATTR(f) TUI_ATTR((f), TUI_BLACK)

EXT_RAM_BSS_ATTR static digi_view_t s_v;
static uint32_t s_seen;
static int s_scroll;

static int64_t now_us(void) { return esp_timer_get_time(); }

/* How long ago a decode landed (the engine stamps it on the same clock). */
static int land_age_ms(const digi_row_t *d)
{
    return (int)((uint32_t)(now_us() / 1000) - d->at_ms);
}

/* New decodes since the last frame keep a scrolled reader's place. */
static void note_arrivals(void)
{
    if (!s_v.n) return;
    uint32_t top = s_v.row[0].seq;
    if (s_seen && s_scroll && top > s_seen) s_scroll += (int)(top - s_seen);
    s_seen = top;
}

/* ---- OPTIONS -------------------------------------------------------------------- */

enum { O_MODE, O_BAND, O_TEST, O_LOG, O_TRIM, O_LAG, O_APPLY, O_CLEAR };
static const char *const MODE_NAMES[] = {"FT8", "FT4", "WSPR"};
static const char *const ONOFF[] = {"OFF", "ON"};

static int o_get(const ls_opt_t *o)
{
    switch (o->arg) {
    case O_MODE: return digi_mode();
    case O_BAND: return digi_band_index();
    case O_TEST: return digi_test_source();
    case O_LOG: return digi_logging();
    }
    return 0;
}
static void o_set(const ls_opt_t *o, int v)
{
    switch (o->arg) {
    case O_MODE: digi_set_mode((digi_mode_t)v); break;
    case O_BAND: digi_set_band(v); break;
    case O_TEST: digi_set_test_source(v != 0); break;
    case O_LOG: digi_set_logging(v != 0); break;
    }
}
static double o_num(const ls_opt_t *o) { return o->arg == O_TRIM ? digi_time_trim() : digi_gps_lag_ms(); }
static void o_set_num(const ls_opt_t *o, double v)
{
    if (o->arg == O_TRIM) digi_set_time_trim((float)v);
    else digi_set_gps_lag_ms((int)v);
}
static void o_act(const ls_opt_t *o)
{
    if (o->arg == O_CLEAR) { digi_clear(); s_scroll = 0; }
    /* Move the clock by the median DT, so the decodes centre on zero. */
    if (o->arg == O_APPLY && s_v.clock_off_n >= 3) digi_set_time_trim(digi_time_trim() - s_v.clock_off_s);
}
static void o_show(const ls_opt_t *o, char *out, size_t n)
{
    if (o->arg == O_APPLY) {
        if (s_v.clock_off_n >= 3) snprintf(out, n, "dT %+.2f s", (double)s_v.clock_off_s);
        else snprintf(out, n, "need 3 decodes");
    } else if (o->arg == O_CLEAR) snprintf(out, n, "%u heard", (unsigned)s_v.total);
}
static const char *o_apply_why(const ls_opt_t *o)
{
    (void)o;
    return s_v.clock_off_n >= 3 ? NULL : "no decodes to measure the clock from yet";
}

static const ls_opt_t OPT_TIME[] = {
    {.label = "TIME TRIM", .kind = LS_OPT_LEVEL, .arg = O_TRIM, .lo = -2, .hi = 2, .step = 0.1,
     .unit = "seconds added to the clock", .num = o_num, .set_num = o_set_num},
    {.label = "APPLY DT", .kind = LS_OPT_ACTION, .arg = O_APPLY, .act = o_act, .show = o_show,
     .why_not = o_apply_why},
    {.label = "GPS LAG", .kind = LS_OPT_LEVEL, .arg = O_LAG, .lo = 0, .hi = 900, .step = 50,
     .unit = "ms the GPS sentence trails its second", .num = o_num, .set_num = o_set_num},
};
static const ls_opt_ctx_t CTX_TIME = {.name = "TIME", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_TIME)};
static const ls_opt_t OPT_DIGI[] = {
    {.label = "MODE", .kind = LS_OPT_CYCLE, .arg = O_MODE, .names = MODE_NAMES, .n = 3, .step = 1,
     .get = o_get, .set = o_set},
    {.label = "BAND", .kind = LS_OPT_CYCLE, .arg = O_BAND, .names = digi_band_names, .n = DIGI_BAND_COUNT, .step = 1,
     .get = o_get, .set = o_set},
    {.label = "TEST SIGNAL", .kind = LS_OPT_TOGGLE, .arg = O_TEST, .names = ONOFF, .get = o_get, .set = o_set},
    {.label = "LOG TO SD", .kind = LS_OPT_TOGGLE, .arg = O_LOG, .names = ONOFF, .get = o_get, .set = o_set},
    {.label = "TIME", .kind = LS_OPT_MENU, .sub = &CTX_TIME},
    {.label = "CLEAR LIST", .kind = LS_OPT_ACTION, .arg = O_CLEAR, .act = o_act, .show = o_show},
};
static const ls_opt_ctx_t CTX_DIGI = {.name = "DIGI", .job = -1, .radio = LS_RSEL_NONE,
                                      LS_OPT_ROWS(OPT_DIGI), .tag = "DIGI", .refresh_ms = 500};

/* ---- actions -------------------------------------------------------------------- */

enum { A_RUN, A_BAND, A_MODE, A_TEST, A_OPT, A_UP, A_DOWN };

static void act(int a)
{
    switch (a) {
    case A_RUN: if (digi_running()) digi_stop(); else digi_start(); break;
    case A_BAND: digi_set_band((digi_band_index() + 1) % digi_band_count()); break;
    case A_MODE: digi_set_mode((digi_mode_t)((digi_mode() + 1) % DIGI_MODE_COUNT)); break;
    case A_TEST:
        if (!digi_running()) digi_start();
        digi_set_test_source(!digi_test_source());
        break;
    case A_OPT: ls_opt_open(&CTX_DIGI); break;
    case A_UP: if (s_scroll > 0) s_scroll -= s_scroll > 3 ? 3 : s_scroll; break;
    case A_DOWN: if (s_scroll < s_v.n - 1) s_scroll += 3; break;
    }
}

static void enter(void)
{
    s_scroll = 0;
    /* Opening the app is asking what is being heard: start listening. */
    if (!digi_running()) digi_start();
}

/* ---- drawing --------------------------------------------------------------------- */

static uint8_t snr_ink(int snr)
{
    return snr >= -5 ? (TUI_YELLOW | TUI_BRIGHT) : snr >= -12 ? (TUI_GREEN | TUI_BRIGHT)
         : snr >= -18 ? TUI_GREEN : TUI_CYAN;
}

/* The app's mark: the FT8 Costas array, 7 symbols across, 8 tones up. The
   columns fill left to right with the real slot clock and the whole array
   flares when a decode lands. */
static void emblem(tui_surface *sf, tui_rect clip, int x, int y, float slot_frac, bool flare, bool live)
{
    const int lit_cols = live ? (int)(slot_frac * 7.0f + 0.999f) : 0;
    for (int t = 0; t < 8; ++t)
        for (int c = 0; c < 7; ++c) {
            bool on = digi_ft8_costas[c] == 7 - t;
            uint8_t at;
            char ch;
            if (on) {
                /* A lit tone is the emblem's one solid accent. */
                ch = c < lit_cols || flare ? LS_TUI_BLOCK_FULL : 'o';
                at = flare ? ATTR(TUI_YELLOW | TUI_BRIGHT) : c < lit_cols ? ATTR(TUI_CYAN | TUI_BRIGHT) : ATTR(TUI_BLUE | TUI_BRIGHT);
            } else {
                ch = c < lit_cols ? ':' : '.';
                at = c < lit_cols ? ATTR(TUI_BLUE | TUI_BRIGHT) : LS_ATTR_FAINT;
            }
            tui_put_char(sf, clip, x + 2 * c, y + t, ch, at);
        }
}

static void fmt_clock(char *out, size_t n, int64_t utc_ms)
{
    int64_t s = utc_ms / 1000;
    snprintf(out, n, "%02d:%02d:%02dZ", (int)(s / 3600 % 24), (int)(s / 60 % 60), (int)(s % 60));
}

static const char *source_text(uint8_t *ink)
{
    if (!s_v.running) { *ink = LS_ATTR_DIM; return "STOPPED"; }
    if (s_v.source == DIGI_SRC_TEST) { *ink = ATTR(TUI_MAGENTA | TUI_BRIGHT); return "TEST SIGNAL"; }
    if (s_v.source == DIGI_SRC_RADIO) { *ink = ATTR(TUI_GREEN | TUI_BRIGHT); return s_v.tuner[0] ? s_v.tuner : "SSB AUDIO"; }
    *ink = ATTR(TUI_RED | TUI_BRIGHT);
    return "NO AUDIO";
}

static void head(tui_surface *sf, tui_rect r)
{
    char title[32];
    const digi_band_t *b = digi_band(s_v.band);
    snprintf(title, sizeof(title), "DIGI / %s %s", digi_mode_name(s_v.mode), b ? b->band : "");
    ls_panel_box(sf, r, title, TUI_CYAN);
    const bool live = s_v.running && s_v.source != DIGI_SRC_NONE;
    const float frac = s_v.slot_ms ? (float)s_v.slot_pos_ms / (float)s_v.slot_ms : 0;
    const bool flare = s_v.n && land_age_ms(&s_v.row[0]) < 600;
    int x = r.x + 2, y = r.y + 1;
    emblem(sf, r, x, y, frac, flare, live);

    /* Mode in block letters, then the dial and the source beside it. */
    int gx = x + 15;
    const char *mn = digi_mode_name(s_v.mode);
    for (int i = 0; mn[i]; ++i)
        ls_glyph_draw(sf, tui_rect_make(gx + 4 * i, y, 3, 5), mn[i], ATTR(TUI_GREEN | TUI_BRIGHT));
    int tx = gx + 4 * (int)strlen(mn) + 1;
    char line[48];
    snprintf(line, sizeof(line), "%.6f", s_v.dial_hz / 1e6);
    tui_put_str(sf, r, tx, y, line, ATTR(TUI_WHITE | TUI_BRIGHT));
    tui_put_str(sf, r, tx, y + 1, "MHz USB dial", LS_ATTR_DIM);
    uint8_t ink;
    const char *src = source_text(&ink);
    tui_put_str(sf, r, tx, y + 3, src, ink);
    if (s_v.running && s_v.source != DIGI_SRC_NONE) {
        snprintf(line, sizeof(line), "%+.0f dBFS", (double)s_v.level_db);
        tui_put_str(sf, r, tx, y + 4, line, LS_ATTR_DIM);
    }

    /* The slot clock: UTC, its source, the slot bar and the clock offset. */
    int cy = y + 6;
    char clk[16];
    fmt_clock(clk, sizeof(clk), s_v.utc_ms);
    tui_put_str(sf, r, gx, cy, clk, ATTR(TUI_WHITE | TUI_BRIGHT));
    static const uint8_t TS_INK[] = {TUI_RED | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT};
    snprintf(line, sizeof(line), "[%s]", digi_time_name(s_v.tsrc));
    tui_put_str(sf, r, gx + 11, cy, line, ATTR(TS_INK[s_v.tsrc & 3]));
    int bar_w = r.x + r.w - 2 - gx;
    if (bar_w > 8) {
        ls_meter_ascii(sf, r, gx, cy + 1, bar_w, live || s_v.running ? frac : 0,
                       s_v.decoding ? (TUI_YELLOW | TUI_BRIGHT) : (TUI_CYAN | TUI_BRIGHT));
    }
    /* Slot seconds, right-aligned on the clock row. */
    if (s_v.decoding) snprintf(line, sizeof(line), "decoding");
    else snprintf(line, sizeof(line), "%4.1f/%ds", s_v.slot_pos_ms / 1000.0, s_v.slot_ms / 1000);
    int sx = r.x + r.w - 2 - (int)strlen(line);
    if (sx > gx + 17)
        tui_put_str(sf, r, sx, cy, line, s_v.decoding ? ATTR(TUI_YELLOW | TUI_BRIGHT) : ATTR(TUI_CYAN));
    const int ly = y + 9 < r.y + r.h - 1 ? y + 9 : y + 8;
    if (s_v.clock_off_n >= 3)
        LS_SNPRINTF(t_scr_digi_dt_2fs_trim_1f, line, sizeof(line), "dT %+.2fs  trim %+.1f", (double)s_v.clock_off_s, (double)s_v.trim_s);
    else
        LS_SNPRINTF(t_scr_digi_dt_trim_1f, line, sizeof(line), "dT --     trim %+.1f", (double)s_v.trim_s);
    tui_put_str(sf, r, x, ly, line,
                fabsf(s_v.clock_off_s) > 1.0f && s_v.clock_off_n >= 3 ? ATTR(TUI_RED | TUI_BRIGHT) : ATTR(TUI_CYAN));
    if (s_v.have_pos) {
        snprintf(line, sizeof(line), "me %s", s_v.my_grid);
        tui_put_str(sf, r, r.x + r.w - 2 - (int)strlen(line), ly, line, ATTR(TUI_YELLOW | TUI_BRIGHT));
    } else {
        tui_put_str(sf, r, r.x + r.w - 8, ly, "no fix", LS_ATTR_FAINT);
    }
}

/* Live audio spectrum 0..3 kHz in braille, the last slot's calls under it. */
static void band(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "BAND 0-3 kHz", TUI_BLUE | TUI_BRIGHT);
    const int x0 = r.x + 2, w = r.w - 4, rows = r.h >= 9 ? 4 : 3;
    if (w < 8) return;
    const int base = r.y + 1 + rows - 1;    /* bottom braille row */
    const bool live = s_v.running && s_v.source != DIGI_SRC_NONE;
    for (int c = 0; c < w; ++c) {
        int lv[2], pk[2];
        for (int s = 0; s < 2; ++s) {
            int i = (2 * c + s) * DIGI_SPEC_CELLS / (2 * w);
            lv[s] = live ? s_v.spec[i] * (rows * 4) / 256 : 0;
            pk[s] = live ? s_v.spec_peak[i] * (rows * 4) / 256 : 0;
        }
        for (int k = 0; k < rows; ++k) {
            uint8_t bits = 0;
            for (int s = 0; s < 2; ++s) {
                int h = lv[s] - 4 * k;
                bits |= ls_braille_column((unsigned)(h < 0 ? 0 : h > 4 ? 4 : h), s);
                int p = pk[s] - 4 * k - 1;
                if (p >= 0 && p < 4 && pk[s] > lv[s]) bits |= ls_braille_dot((unsigned)p, s);
            }
            int top = lv[0] > lv[1] ? lv[0] : lv[1];
            uint8_t ink = top > rows * 3 ? (TUI_YELLOW | TUI_BRIGHT) : top > rows * 2 ? (TUI_GREEN | TUI_BRIGHT)
                        : top > rows ? TUI_CYAN : TUI_BLUE;
            if (!bits) { tui_put_char(sf, r, x0 + c, base - k, k == 0 ? '_' : ' ', LS_ATTR_FAINT); continue; }
            ls_braille_put(sf, r, x0 + c, base - k, (int16_t)(0x2800 | bits), ATTR(ink));
        }
    }
    /* Axis: a tick every 500 Hz. */
    int ay = base + 1;
    for (int c = 0; c < w; ++c) {
        int hz = c * 3000 / w, hz1 = (c + 1) * 3000 / w;
        bool tick = hz / 500 != hz1 / 500 || c == 0;
        tui_put_char(sf, r, x0 + c, ay, tick ? '+' : '-', LS_ATTR_FAINT);
    }
    for (int k = 0; k <= 6 && w > 30; k += 2) {
        char t[8];
        snprintf(t, sizeof(t), "%dk", k / 2);
        int cx = x0 + k * 500 * w / 3000 - (k == 6 ? 2 : 0);
        tui_put_str(sf, r, cx, ay + 1, k ? t : "0", LS_ATTR_DIM);
    }
    if (s_v.mode == DIGI_MODE_WSPR) {
        /* WSPR lives in 1400..1600 Hz: bracket it. */
        int a = x0 + 1400 * w / 3000, b = x0 + 1600 * w / 3000;
        tui_put_char(sf, r, a, ay, '[', ATTR(TUI_MAGENTA | TUI_BRIGHT));
        tui_put_char(sf, r, b, ay, ']', ATTR(TUI_MAGENTA | TUI_BRIGHT));
    }
    /* The last slot's senders, pinned at their audio frequency. */
    if (!s_v.n) return;
    const uint32_t slot = s_v.row[0].slot_utc;
    int used_until[2] = {-1, -1};
    for (int i = 0; i < s_v.n && s_v.row[i].slot_utc == slot; ++i) {
        const digi_row_t *d = &s_v.row[i];
        const char *who = d->call[0] ? d->call : d->text;
        bool again = false;
        for (int k = 0; k < i; ++k) if (!strcmp(s_v.row[k].call, d->call) && d->call[0]) again = true;
        if (again) continue;
        char tag[8];
        snprintf(tag, sizeof(tag), "%.6s", who);
        int cx = x0 + (int)(d->freq * w / 3000.0f);
        if (cx < x0) cx = x0;
        int len = (int)strlen(tag);
        if (cx + len > x0 + w) cx = x0 + w - len;
        for (int line = 0; line < 2; ++line) {
            if (cx <= used_until[line]) continue;
            int yy = ay + 2 + line;
            if (yy >= r.y + r.h - 1) break;
            bool fresh = land_age_ms(d) < LAND_MS;
            tui_put_char(sf, r, cx, ay, '^', ATTR(snr_ink(d->snr)));
            tui_put_str(sf, r, cx, yy, tag, fresh ? TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT) : ATTR(snr_ink(d->snr)));
            used_until[line] = cx + len;
            break;
        }
    }
}

static void notice(tui_surface *sf, tui_rect r, int y, const char *a, const char *b, uint8_t ink)
{
    int x = r.x + (r.w - (int)strlen(a)) / 2;
    tui_put_str(sf, r, x < r.x + 2 ? r.x + 2 : x, y, a, ink);
    if (b) {
        x = r.x + (r.w - (int)strlen(b)) / 2;
        tui_put_str(sf, r, x < r.x + 2 ? r.x + 2 : x, y + 1, b, LS_ATTR_DIM);
    }
}

static void list(tui_surface *sf, tui_rect r)
{
    char title[40];
    snprintf(title, sizeof(title), "HEARD %u", (unsigned)s_v.total);
    ls_panel_box(sf, r, title, TUI_GREEN);
    /* Decodes per recent slot, as a line along the top edge. */
    {
        int hx = r.x + r.w - 2 - DIGI_SLOT_HIST;
        if (hx > r.x + (int)strlen(title) + 4)
            for (int i = 0; i < DIGI_SLOT_HIST; ++i) {
                int n = s_v.slot_hist[i];
                char ch = n == 0 ? '_' : n < 3 ? '.' : n < 6 ? ':' : n < 10 ? '|' : '#';
                tui_put_char(sf, r, hx + i, r.y, ch, ATTR(n ? (TUI_GREEN | TUI_BRIGHT) : TUI_GREEN));
            }
    }
    const int x = r.x + 2, w = r.w - 4;
    int y = r.y + 1;
    const int y_end = r.y + r.h - 1;
    if (!s_v.running) {
        notice(sf, r, r.y + r.h / 2 - 1, "receiver stopped", LS_TEXT(t_scr_digi_start_listens_on_the, "START listens on the dial above"), ATTR(TUI_WHITE));
        return;
    }
    if (!s_v.n) {
        if (s_v.source == DIGI_SRC_NONE)
            notice(sf, r, r.y + r.h / 2 - 2, LS_TEXT(t_scr_digi_waiting_for_ssb_audio, "waiting for SSB audio"),
                   LS_TEXT(t_scr_digi_tune_hf_to_the, "tune HF to the dial in USB, or tap TEST"), ATTR(TUI_YELLOW | TUI_BRIGHT));
        else {
            char t[48];
            int left = (s_v.slot_ms - s_v.slot_pos_ms + 999) / 1000;
            LS_SNPRINTF(t_scr_digi_first_decodes_in_d, t, sizeof(t), "first decodes in %d s", left);
            notice(sf, r, r.y + r.h / 2 - 2, "listening", t, ATTR(TUI_GREEN | TUI_BRIGHT));
        }
        return;
    }
    const bool wide = w >= 56;
    char hdr[96];
    snprintf(hdr, sizeof(hdr), wide ? " dB   DT    Hz  %-*s    km BRG" : " dB   DT   Hz  %-*s  km  BRG",
             w - (wide ? 30 : 29), "MESSAGE");
    tui_put_str(sf, r, x, y++, hdr, LS_ATTR_FAINT);
    if (s_scroll > s_v.n - 1) s_scroll = s_v.n - 1;
    if (s_scroll < 0) s_scroll = 0;
    uint32_t prev_slot = s_scroll ? s_v.row[s_scroll - 1].slot_utc : 0;
    for (int i = s_scroll; i < s_v.n && y < y_end; ++i) {
        const digi_row_t *d = &s_v.row[i];
        if (d->slot_utc != prev_slot) {
            /* A slot divider: its time, and how many it held. */
            int cnt = 0;
            for (int k = i; k < s_v.n && s_v.row[k].slot_utc == d->slot_utc; ++k) cnt++;
            char t[48], clk[16];
            fmt_clock(clk, sizeof(clk), (int64_t)d->slot_utc * 1000);
            snprintf(t, sizeof(t), "-- %s %s ", clk, digi_mode_name(d->mode));
            tui_hline(sf, r, x, y, w, '-', ATTR(TUI_BLUE));
            tui_put_str(sf, r, x, y, t, ATTR(TUI_BLUE | TUI_BRIGHT));
            snprintf(t, sizeof(t), " %d ", cnt);
            tui_put_str(sf, r, x + w - (int)strlen(t) - 1, y, t, ATTR(TUI_BLUE | TUI_BRIGHT));
            prev_slot = d->slot_utc;
            if (++y >= y_end) break;
        }
        const int age = land_age_ms(d);
        const bool fresh = age < LAND_MS;
        char dist[16] = "";
        if (d->has_pos) {
            static const char *const C8[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
            snprintf(dist, sizeof(dist), "%5.0f %-2s", (double)d->km, C8[((int)lroundf(d->bearing / 45.0f)) & 7]);
        } else if (d->grid[0]) snprintf(dist, sizeof(dist), "%8s", d->grid);
        int msg_w = w - (wide ? 17 : 16) - 9;
        if (msg_w < 8) msg_w = 8;
        char num[24];
        snprintf(num, sizeof(num), wide ? "%3d %+5.1f %5.0f " : "%3d %+4.1f %4.0f ", d->snr, (double)d->dt, (double)d->freq);
        uint8_t row_ink = d->cq ? ATTR(TUI_GREEN | TUI_BRIGHT) : ATTR(TUI_WHITE);
        if (fresh) {
            /* Just landed: the row is lit, then settles to its colours. */
            row_ink = age < LAND_MS / 2 ? TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT) : ATTR(TUI_YELLOW | TUI_BRIGHT);
            tui_fill(sf, tui_rect_make(x, y, w, 1), ' ', row_ink);
            tui_put_char(sf, r, x - 1, y, '>', ATTR(TUI_YELLOW | TUI_BRIGHT));
        }
        tui_put_str(sf, r, x, y, num, fresh ? row_ink : ATTR(snr_ink(d->snr)));
        int nx = x + (int)strlen(num);
        char msg[48];
        snprintf(msg, sizeof(msg), "%-*.*s", msg_w, msg_w, d->text);
        tui_put_str(sf, r, nx, y, msg, row_ink);
        tui_put_str(sf, r, nx + msg_w + 1, y, dist, fresh ? row_ink : d->has_pos ? ATTR(TUI_YELLOW) : LS_ATTR_FAINT);
        ++y;
    }
    if (s_scroll) {
        char t[24];
        snprintf(t, sizeof(t), " %d newer ^ ", s_scroll);
        tui_put_str(sf, r, r.x + r.w - 2 - (int)strlen(t), r.y + r.h - 1, t, ATTR(TUI_YELLOW | TUI_BRIGHT));
    }
}

/* WHERE: every station with a grid, around us by true bearing, distance on
   a log scale (100 km at the centre, 20 000 km at the rim). */
static void ring(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, s_v.have_pos ? "WHERE" : "WHERE / no GPS fix", TUI_YELLOW);
    const int R = (r.h - 5) / 2;
    const int cx = r.x + 3 + 2 * R + 1, cy = r.y + 2 + R;
    if (R < 3) return;
    /* A ring at 3 000 km, and the rim at 20 000. */
    static const float RK[] = {3000.0f, 20000.0f};
    for (int k = 0; k < 2; ++k) {
        float rr = log10f(RK[k] / 100.0f) / log10f(200.0f) * (float)R;
        const int pts = k == 0 ? 36 : 72;
        for (int a = 0; a < pts; ++a) {
            float t = (float)a * 6.2831853f / (float)pts;
            int px = cx + (int)lroundf(sinf(t) * rr * 2.0f), py = cy - (int)lroundf(cosf(t) * rr);
            tui_put_char(sf, r, px, py, '.', k == 1 ? ATTR(TUI_BLUE | TUI_BRIGHT) : LS_ATTR_FAINT);
        }
    }
    tui_put_char(sf, r, cx, cy - R - 1, 'N', ATTR(TUI_RED | TUI_BRIGHT));
    tui_put_char(sf, r, cx + 2 * R + 2, cy, 'E', ATTR(TUI_WHITE));
    tui_put_char(sf, r, cx - 2 * R - 2, cy, 'W', ATTR(TUI_WHITE));
    tui_put_char(sf, r, cx, cy + R + 1, 'S', ATTR(TUI_WHITE));
    tui_put_char(sf, r, cx, cy, '+', ATTR(TUI_YELLOW | TUI_BRIGHT));
    /* Stations, oldest first so the newest draw on top; keep the farthest. */
    int far[4] = {-1, -1, -1, -1};
    for (int i = s_v.n - 1; i >= 0; --i) {
        const digi_row_t *d = &s_v.row[i];
        if (!d->has_pos) continue;
        float km = d->km < 100 ? 100 : d->km > 20000 ? 20000 : d->km;
        float rr = log10f(km / 100.0f) / log10f(200.0f) * (float)R;
        float t = d->bearing * 0.01745329f;
        int px = cx + (int)lroundf(sinf(t) * rr * 2.0f), py = cy - (int)lroundf(cosf(t) * rr);
        bool fresh = land_age_ms(d) < LAND_MS;
        tui_put_char(sf, r, px, py, fresh ? '@' : d->cq ? 'o' : '*',
                     fresh ? TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT) : ATTR(snr_ink(d->snr)));
        bool dup = false;
        for (int k = 0; k < 4; ++k) if (far[k] >= 0 && !strcmp(s_v.row[far[k]].call, d->call)) dup = true;
        if (dup) continue;
        for (int k = 0; k < 4; ++k)
            if (far[k] < 0 || s_v.row[far[k]].km < d->km) {
                for (int m = 3; m > k; --m) far[m] = far[m - 1];
                far[k] = i;
                break;
            }
    }
    /* Beside the ring: the farthest heard. */
    int tx = cx + 2 * R + 6, ty = r.y + 2;
    if (tx + 20 > r.x + r.w) return;
    tui_put_str(sf, r, tx, ty++, "FARTHEST", ATTR(TUI_YELLOW | TUI_BRIGHT));
    static const char *const C8[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    for (int k = 0; k < 4 && far[k] >= 0 && ty < r.y + r.h - 1; ++k) {
        const digi_row_t *d = &s_v.row[far[k]];
        char t[40];
        snprintf(t, sizeof(t), "%-7.7s", d->call);
        tui_put_str(sf, r, tx, ty, t, ATTR(TUI_WHITE | TUI_BRIGHT));
        snprintf(t, sizeof(t), "%5.0fkm %-2s", (double)d->km, C8[((int)lroundf(d->bearing / 45.0f)) & 7]);
        tui_put_str(sf, r, tx + 7, ty, t, ATTR(TUI_YELLOW));
        snprintf(t, sizeof(t), "%+d", d->snr);
        tui_put_str(sf, r, tx + 18, ty++, t, ATTR(snr_ink(d->snr)));
    }
    if (far[0] < 0)
        tui_put_str(sf, r, tx, ty, s_v.have_pos ? "no grids heard yet" : "needs a GPS fix", LS_ATTR_DIM);
    else if (ty + 1 < r.y + r.h - 1)
        tui_put_str(sf, r, tx, ty + 1, "rings 3k / 20k km", LS_ATTR_FAINT);
}

static tui_rect s_list_r;

static void draw(tui_surface *sf, tui_rect area)
{
    digi_view(&s_v);
    note_arrivals();
    const bool wide = area.w > area.h * 2;
    tui_rect bar = tui_rect_make(area.x, area.y + area.h - 5, area.w, 5);
    tui_rect body = tui_rect_make(area.x, area.y, area.w - (wide ? 8 : 0), area.h - 5);
    tui_rect h, b, l;
    if (wide) {
        int lw = body.w * 2 / 5;
        if (lw < 48) lw = 48;
        h = tui_rect_make(body.x, body.y, lw, 11);
        b = tui_rect_make(body.x, body.y + 11, lw, body.h - 11);
        l = tui_rect_make(body.x + lw, body.y, body.w - lw, body.h);
    } else {
        h = tui_rect_make(body.x, body.y, body.w, 11);
        b = tui_rect_make(body.x, body.y + 11, body.w, 9);
        l = tui_rect_make(body.x, body.y + 20, body.w, body.h - 20);
    }
    /* Tall enough: the WHERE ring between the band and the list. */
    if (!wide && body.h >= 20 + 15 + 14) {
        ring(sf, tui_rect_make(body.x, body.y + 20, body.w, 15));
        l = tui_rect_make(body.x, body.y + 35, body.w, body.h - 35);
    }
    head(sf, h);
    band(sf, b);
    list(sf, l);
    s_list_r = l;
    const digi_band_t *bd = digi_band(s_v.band);
    ls_btn_t btn[5] = {
        {.label = s_v.running ? "STOP" : "START", .value = "RX", .key = 's', .on = s_v.running},
        {.label = "BAND", .value = bd ? bd->band : "?", .key = 'b'},
        {.label = "MODE", .value = digi_mode_name(s_v.mode), .key = 'm'},
        {.label = "TEST", .value = digi_test_source() ? "ON" : "OFF", .key = 't', .on = digi_test_source()},
        ls_opt_button(&CTX_DIGI),
    };
    ls_btn_bar_raised_row(sf, bar, btn, 5, -1);
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_UP || k == LS_TK_WHEEL_UP) { act(A_UP); return true; }
    if (k == LS_TK_DOWN || k == LS_TK_WHEEL_DOWN) { act(A_DOWN); return true; }
    if (k != LS_TK_CHAR) return false;
    if (ls_opt_key(&CTX_DIGI, ch)) return true;
    switch (ch) {
    case 's': case 'S': act(A_RUN); return true;
    case 'b': act(A_BAND); return true;
    case 'B': digi_set_band((digi_band_index() + digi_band_count() - 1) % digi_band_count()); return true;
    case 'm': case 'M': act(A_MODE); return true;
    case 't': case 'T': act(A_TEST); return true;
    }
    return false;
}

static bool touch(int col, int row)
{
    int i = ls_btn_hit(col, row);
    if (i >= 0) { act(i); return true; }
    /* The decode list scrolls by its top and bottom thirds. */
    if (tui_rect_contains(s_list_r, col, row) && s_v.n) {
        if (row < s_list_r.y + s_list_r.h / 3) { act(A_UP); return true; }
        if (row >= s_list_r.y + s_list_r.h * 2 / 3) { act(A_DOWN); return true; }
    }
    return false;
}

const ls_tui_screen_t ls_scr_digi = {
    .name = "DIGI",
    .hint = "s Start/Stop  b Band  m Mode  t Test  o Options  Up/Down or tap list ends to scroll",
    .enter = enter, .draw = draw, .key = key, .touch = touch,
};

const ls_app_doc_t ls_doc_digi = {
    .purpose = LS_TXT(doc_digi_purpose, "Receive FT8, FT4 and WSPR from SSB audio: who is on the band now, "
               "their signal, timing and distance. Slots follow GPS time, or NTP/RTC "
               "without a fix. Receive only; nothing is transmitted."),
    .records = LS_APP_RECORDS_AUTOMATIC,
    .record_note = LS_TXT(doc_digi_record_note, "Each decode is appended to /sdcard/lakeshark/digi/YYYYMMDD.csv: UTC, mode, "
                   "dial, audio offset, SNR, DT, message, call, grid, distance and bearing. "
                   "LOG TO SD in OPTIONS turns it off."),
    .gps = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_digi_gps_note, "GPS time slots the decoder; your fix gives distance and bearing to each "
                "station's grid square. Your position is not logged."),
};
