/* The one waterfall. See ls_waterfall.h for why there is only one. */
#include "ls_waterfall.h"
#include "ls_motion.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "ls_tui_ui.h"
/* ls_tui_daylight: which way round the ground is. */
#include "ls_theme.h"

/* ------------------------------------------------------------------ state */

static uint8_t (*s_hist)[LS_WF_BINS_MAX];
static int      s_head;                  /* newest row index                */
static int      s_filled;                /* rows written since the claim    */
static int      s_nbins;                 /* bins the source actually gives  */

static ls_wf_owner_t s_owner;
static char          s_label[16];

static ls_wf_feed_t  s_feed;
static bool          s_have_feed;

/* Averaging is a RUNNING average, and it does not slow the scroll. */

static uint16_t s_acc[LS_WF_BINS_MAX];
static bool     s_acc_primed;
static uint8_t  s_decim_n;

static uint8_t  s_peak[LS_WF_BINS_MAX];

static int      s_marker = -1;           /* display column, -1 for none     */

static ls_wf_cfg_t s_cfg = {
    .split_pct = 50, .ref = 0, .range = 16, .palette = LS_WF_PAL_HEAT,

    .avg = 1, .decim = 1, .grain = LS_WF_GRAIN_ASCII, .paused = false,
};

static ls_wf_stats_t s_st;
static int64_t       s_last_push_us;
static uint32_t      s_row_ms_n;

/* Where the control bar landed, so a tap outside it can be handled as a tap
   on the plot instead of being swallowed. */
static tui_rect s_bar_rect, s_plot_rect;
/* Landscape only: the button strip is put away. Off by default, so
   the controls are there until somebody asks for the room. */
static bool s_bar_hidden;
static int      s_focus = -1;

/* ------------------------------------------------------------------ colour */

static const uint8_t PAL[LS_WF_PAL__COUNT][16] = {
    [LS_WF_PAL_HEAT] = {
        TUI_BLACK, TUI_BLUE, TUI_BLUE, TUI_BLUE | TUI_BRIGHT,
        TUI_BLUE | TUI_BRIGHT, TUI_CYAN, TUI_CYAN, TUI_CYAN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW,
        TUI_YELLOW | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
        TUI_RED | TUI_BRIGHT, TUI_RED | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT },
    [LS_WF_PAL_ICE] = {
        TUI_BLACK, TUI_BLUE, TUI_BLUE, TUI_BLUE,
        TUI_BLUE | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT, TUI_CYAN, TUI_CYAN,
        TUI_CYAN, TUI_CYAN | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT,
        TUI_CYAN | TUI_BRIGHT, TUI_WHITE, TUI_WHITE,
        TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT },
    [LS_WF_PAL_PHOSPHOR] = {
        TUI_BLACK, TUI_GREEN, TUI_GREEN, TUI_GREEN, TUI_GREEN, TUI_GREEN,
        TUI_GREEN, TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT,
        TUI_WHITE, TUI_WHITE, TUI_WHITE | TUI_BRIGHT },
    [LS_WF_PAL_NEON] = {
        TUI_BLACK, TUI_MAGENTA, TUI_MAGENTA, TUI_MAGENTA,
        TUI_MAGENTA | TUI_BRIGHT, TUI_MAGENTA | TUI_BRIGHT,
        TUI_BLUE | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT, TUI_CYAN,
        TUI_CYAN | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
        TUI_YELLOW | TUI_BRIGHT, TUI_WHITE, TUI_WHITE | TUI_BRIGHT },
};

static const char *const PAL_NAME[LS_WF_PAL__COUNT] = {
    "HEAT", "ICE", "PHOS", "NEON"
};

/* The same four scales for a white ground. */

static const uint8_t PAL_DAY[LS_WF_PAL__COUNT][16] = {
    [LS_WF_PAL_HEAT] = {
        TUI_BLACK, TUI_BLACK | TUI_BRIGHT, TUI_BLACK | TUI_BRIGHT, TUI_BLUE,
        TUI_BLUE, TUI_CYAN, TUI_CYAN, TUI_GREEN,
        TUI_GREEN, TUI_YELLOW, TUI_YELLOW, TUI_RED,
        TUI_YELLOW | TUI_BRIGHT, TUI_RED | TUI_BRIGHT, TUI_RED | TUI_BRIGHT,
        TUI_WHITE | TUI_BRIGHT },
    [LS_WF_PAL_ICE] = {
        TUI_BLACK, TUI_BLACK | TUI_BRIGHT, TUI_BLACK | TUI_BRIGHT,
        TUI_BLACK | TUI_BRIGHT, TUI_BLUE, TUI_BLUE, TUI_CYAN, TUI_CYAN,
        TUI_WHITE, TUI_WHITE, TUI_BLUE | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT,
        TUI_CYAN | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT,
        TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT },
    [LS_WF_PAL_PHOSPHOR] = {
        TUI_BLACK, TUI_BLACK | TUI_BRIGHT, TUI_BLACK | TUI_BRIGHT,
        TUI_GREEN, TUI_GREEN, TUI_GREEN, TUI_GREEN, TUI_GREEN,
        TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT,
        TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT },
    [LS_WF_PAL_NEON] = {
        TUI_BLACK, TUI_BLACK | TUI_BRIGHT, TUI_BLACK | TUI_BRIGHT, TUI_BLUE,
        TUI_CYAN, TUI_GREEN, TUI_YELLOW, TUI_MAGENTA,
        TUI_MAGENTA, TUI_BLUE | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
        TUI_MAGENTA | TUI_BRIGHT, TUI_MAGENTA | TUI_BRIGHT,
        TUI_WHITE | TUI_BRIGHT },
};

static bool s_light;

static inline uint8_t level_colour(int palette, int level, bool light)
{
    if (palette < 0 || palette >= LS_WF_PAL__COUNT) palette = 0;
    return (light ? PAL_DAY : PAL)[palette][level & 0x0F];
}

uint8_t ls_wf_level_colour(int palette, int level, bool light_ground)
{
    return level_colour(palette, level, light_ground);
}

/* What the sixteen levels are stretched between. */

static int s_auto_lo = 0, s_auto_hi = 255;
static int s_auto_top = 255;   /* the loudest recent bin, smoothed */
static bool s_auto_seeded;     /* the first row sets these outright */

/* Each row keeps the automatic window it was written with. */

static uint8_t s_row_lo[LS_WF_ROWS_MAX], s_row_hi[LS_WF_ROWS_MAX];

static inline int level_between(uint8_t raw, int lo, int hi)
{
    if (hi - lo < 8) hi = lo + 8;                   /* never divide by ~0   */

    int v = ((int)raw - lo) * 15 / (hi - lo);

    v -= (int)s_cfg.ref;
    const int span = s_cfg.range ? s_cfg.range : 16;
    v = v * 16 / span;

    if (v < 0)  v = 0;
    if (v > 15) v = 15;
    return v;
}

static inline int level_of(uint8_t raw)
{
    return level_between(raw, s_auto_lo, s_auto_hi);
}

/* A history row through its own window. A row past what has been
   written has none of its own, and takes the current one. */
static inline int level_in_row(uint8_t raw, int row_back)
{
    if (row_back < 0 || row_back >= s_filled) return level_of(raw);
    const int idx = (s_head - row_back + LS_WF_ROWS_MAX * 2) % LS_WF_ROWS_MAX;
    return level_between(raw, s_row_lo[idx], s_row_hi[idx]);
}

/* How TALL a spectrum bar is, which is a different question. */

static inline int height_of(uint8_t raw)
{
    int lo = s_auto_lo, hi = s_auto_top;
    if (hi - lo < 16) hi = lo + 16;
    int v = ((int)raw - lo) * 15 / (hi - lo);
    if (v < 0)  v = 0;
    if (v > 15) v = 15;
    return v;
}

/* The floor and the ceiling of one row, as a quarter-scale histogram: the
   25th percentile is the floor and the loudest occupied bucket is the top.

   A percentile and not the minimum, because one dead bin - a DC spike
   notched to zero, a bin the resampler never wrote - would peg the floor at
   nothing and flatten everything above it back into two levels, which is the
   fault this exists to fix, arriving by another road. */
static void track_scale(const uint8_t *row, int n)
{
    if (n <= 0) return;

    int hist[64] = { 0 };
    for (int i = 0; i < n; i++) hist[row[i] >> 2]++;

    const int q1 = n / 4, q3 = (n * 3) / 4;
    int seen = 0, b25 = 0, b75 = 0;
    for (int b = 0; b < 64; b++) {
        seen += hist[b];
        if (seen <= q1) b25 = b;
        if (seen <= q3) b75 = b;
    }
    const int p25 = b25 * 4, p75 = b75 * 4 + 3;

    int lo = p25 - 6;
    if (lo < 0) lo = 0;

    int span = (p75 - p25) * 6;
    if (span < 40)  span = 40;
    if (span > 180) span = 180;
    int hi = lo + span;
    if (hi > 255) hi = 255;

    int top = 0;
    for (int b = 63; b >= 0; b--) if (hist[b]) { top = b * 4 + 3; break; }
    if (top < lo + 16) top = lo + 16;

    if (!s_auto_seeded) {
        s_auto_seeded = true;
        s_auto_lo = lo; s_auto_hi = hi; s_auto_top = top;
    } else {
        /* Integer smoothing has a dead zone: a difference under eight
           divides to zero and the estimate stops moving. That is acceptable
           - it means "within eight raw units", which is a fifth of the
           narrowest window - and it is why the seed above matters, because
           from 255 the dead zone is reached long after the picture has been
           wrong for a while. */
        s_auto_lo  += (lo  - s_auto_lo)  / 8;
        s_auto_hi  += (hi  - s_auto_hi)  / 8;
        s_auto_top += (top - s_auto_top) / 8;
    }
    if (s_auto_lo < 0) s_auto_lo = 0;
    if (s_auto_hi > 255) s_auto_hi = 255;
    if (s_auto_top > 255) s_auto_top = 255;
}

static inline uint8_t colour_of(int level)
{
    return level_colour(s_cfg.palette, level, s_light);
}

/* ------------------------------------------------------------------- claim */

/* `ready` and `hist_rows` say what the instrument holds, so they are kept where what it holds changes. */

static bool ensure_hist(void)
{
    if (s_hist) { s_st.ready = true; return true; }
    s_hist = heap_caps_malloc(sizeof(*s_hist) * LS_WF_ROWS_MAX,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hist) return false;
    memset(s_hist, 0, sizeof(*s_hist) * LS_WF_ROWS_MAX);
    s_st.ready = true;
    return true;
}

void ls_wf_claim(ls_wf_owner_t owner, const char *label)
{
    if (owner == s_owner) return;
    s_owner = owner;
    snprintf(s_label, sizeof(s_label), "%s", label ? label : "");
    s_head = 0;
    s_filled = 0;

    /* Not seeded to the extremes. */

    s_auto_seeded = false;
    s_nbins = 0;
    s_acc_primed = false;
    s_decim_n = 0;
    s_have_feed = false;
    s_marker = -1;
    memset(s_peak, 0, sizeof(s_peak));
    memset(&s_st, 0, sizeof(s_st));
    s_row_ms_n = 0;
    s_last_push_us = 0;
    if (s_hist) memset(s_hist, 0, sizeof(*s_hist) * LS_WF_ROWS_MAX);

    if (owner != LS_WF_OWNER_NONE) ensure_hist();
    s_st.ready = (s_hist != NULL);
}

ls_wf_owner_t ls_wf_owner(void) { return s_owner; }

/* -------------------------------------------------------------------- push */

static uint32_t s_seq;

/* Whether rows are arriving, as opposed to whether anything claims
   they are. Six hundred milliseconds of grace: a slow sweep pushes a row
   every few hundred, and a mark that stuttered between rows would report a
   fault that is not there. */
static ls_fresh_t s_feed_fresh;
#define FEED_FADE_MS 600

void ls_wf_push(ls_wf_owner_t owner, const float *bins, int n,
                const ls_wf_feed_t *feed)
{
    if (owner != s_owner || !bins || n <= 0) { s_st.dropped++; return; }
    if (s_cfg.paused)                        { s_st.dropped++; return; }
    if (!ensure_hist())                      { s_st.dropped++; return; }

    if (feed) { s_feed = *feed; s_have_feed = true; }

    /* Counted AFTER the refusals above, so it means "a row was
       accepted" and not "somebody called this". A paused waterfall or a
       claim by another owner must not look like a live feed. */
    s_seq++;

    const int dst_n = n < LS_WF_BINS_MAX ? n : LS_WF_BINS_MAX;

    /* A source that changes its resolution starts a new picture. */

    if (s_nbins && dst_n != s_nbins) {
        s_filled = 0;
        s_head = 0;
        s_acc_primed = false;
        memset(s_peak, 0, sizeof(s_peak));
        s_st.hist_rows = 0;
    }

    /* The running average. `want` is the smoothing depth; at 1 the
       new measurement replaces the old exactly, so AVG x1 is not an
       approximation of "off", it IS off. */
    const int32_t want = s_cfg.avg ? s_cfg.avg : 1;
    for (int i = 0; i < dst_n; i++) {
        const int lo = i * n / dst_n;
        int hi = (i + 1) * n / dst_n;
        if (hi <= lo) hi = lo + 1;
        float peak = 0.0f;
        for (int b = lo; b < hi && b < n; b++)
            if (bins[b] > peak) peak = bins[b];
        if (peak < 0.0f) peak = 0.0f;
        if (peak > 1.0f) peak = 1.0f;
        const int32_t target = (int32_t)(peak * 255.0f) << 8;
        if (!s_acc_primed || want == 1) {
            s_acc[i] = (uint16_t)target;
        } else {
            const int32_t cur = (int32_t)s_acc[i];
            s_acc[i] = (uint16_t)(cur + (target - cur) / want);
        }
    }
    s_nbins = dst_n;
    s_acc_primed = true;

    /* SPEED, and it is now the only thing that decides how often a
       row is drawn. */
    if (++s_decim_n < (s_cfg.decim ? s_cfg.decim : 1)) {
        s_st.dropped++;
        return;
    }
    s_decim_n = 0;

    s_head = (s_head + 1) % LS_WF_ROWS_MAX;
    uint8_t *row = s_hist[s_head];
    for (int i = 0; i < dst_n; i++) {
        const uint8_t v = (uint8_t)(s_acc[i] >> 8);
        row[i] = v;
        /* Peak decays one step a row: fast enough to follow a band that has
           gone quiet, slow enough to still be there when you look up. */
        if (v >= s_peak[i]) s_peak[i] = v;
        else if (s_peak[i]) s_peak[i]--;
    }
    if (dst_n < LS_WF_BINS_MAX)
        memset(row + dst_n, 0, (size_t)(LS_WF_BINS_MAX - dst_n));

    track_scale(row, dst_n);
    /* The window this row was written under travels with it. */
    s_row_lo[s_head] = (uint8_t)(s_auto_lo < 0 ? 0 : s_auto_lo > 255 ? 255 : s_auto_lo);
    s_row_hi[s_head] = (uint8_t)(s_auto_hi < 0 ? 0 : s_auto_hi > 255 ? 255 : s_auto_hi);

    if (s_filled < LS_WF_ROWS_MAX) s_filled++;
    s_st.hist_rows = s_filled;
    s_st.pushes++;

    const int64_t now = esp_timer_get_time();
    if (s_last_push_us) {
        /* From the second row, not the seventeenth. This published
           nothing until sixteen intervals had been averaged, and on an FM
           band sweep - one row a pass, measured on the board at about 18 s
           over VHF land - that is five minutes of "a row every 0 ms" on the
           console and "row 0 ms" on the readout, for a feed that was working.
           A plain average over the first sixteen intervals and a one-in-
           sixteen running average after that says the same thing about a
           fast source as before and something true about a slow one. */
        const int32_t dt = (int32_t)((now - s_last_push_us) / 1000);
        if (s_row_ms_n < 16) s_row_ms_n++;
        int32_t avg = (int32_t)s_st.row_ms;
        avg += (dt - avg) / (int32_t)s_row_ms_n;
        s_st.row_ms = (uint32_t)(avg < 0 ? 0 : avg);
    }
    s_last_push_us = now;
}

/* -------------------------------------------------------------------- draw */

/* Display column to history bin. The history holds whatever the source gave
   us; the panel has whatever columns it has. One mapping, used by the
   spectrum, the waterfall and the marker readout, so the three cannot
   disagree about which bin a column is. */
static inline int bin_for_col(int x, int w)
{
    if (w <= 1 || s_nbins <= 0) return 0;
    int b = x * s_nbins / w;
    return b < s_nbins ? b : s_nbins - 1;
}

static uint8_t sample(int row_back, int col, int w)
{
    if (!s_hist || s_filled <= 0) return 0;
    if (row_back >= s_filled) return 0;
    const int idx = (s_head - row_back + LS_WF_ROWS_MAX * 2) % LS_WF_ROWS_MAX;
    const int lo = bin_for_col(col, w);
    const int hi = bin_for_col(col + 1, w);
    const uint8_t *r = s_hist[idx];
    uint8_t v = r[lo];
    for (int b = lo + 1; b <= hi && b < s_nbins; b++)
        if (r[b] > v) v = r[b];
    return v;
}

static void draw_spectrum(tui_surface *sf, tui_rect r)
{
    if (r.h < 1 || r.w < 4) return;
    for (int x = 0; x < r.w; x++) {
        const uint8_t raw = sample(0, x, r.w);
        const int hgt = height_of(raw);
        const int eighths = hgt * (r.h * 8) / 15;
        const int whole = eighths / 8, rem = eighths % 8;
        /* Height says how strong, colour says the same thing the waterfall
           below is saying about that column - so a stripe in the history
           and the bar above it are the same colour and read as one signal. */
        const uint8_t c = colour_of(level_of(raw));

        for (int y = 0; y < whole && y < r.h; y++)
            tui_put_char(sf, r, r.x + x, r.y + r.h - 1 - y, LS_TUI_TRACE(8),
                         TUI_ATTR(c, TUI_BLACK));
        if (rem && whole < r.h)
            tui_put_char(sf, r, r.x + x, r.y + r.h - 1 - whole,
                         LS_TUI_TRACE(rem), TUI_ATTR(c, TUI_BLACK));

        if (s_cfg.peak_hold) {
            const int plvl = height_of(s_peak[bin_for_col(x, r.w)]);
            const int py = r.h - 1 - (plvl * (r.h - 1) / 15);
            if (py >= 0 && py < r.h && plvl > hgt)
                tui_put_char(sf, r, r.x + x, r.y + py, LS_TUI_TRACE(1),
                             TUI_ATTR(TUI_WHITE, TUI_BLACK));
        }
    }
    /* The marker is drawn last so it is never hidden by a strong bin. */
    if (s_marker >= 0 && s_marker < r.w)
        for (int y = 0; y < r.h; y++)
            tui_put_char(sf, r, r.x + s_marker, r.y + y, LS_TUI_BLOCK_LEFT,
                         TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

/* The ten-step ASCII density ramp, which is older than any of this. */

static const char ASCII_RAMP[10] = { '.', ',', ':', ';', '=', '+',
                                     '*', '%', '#', '@' };

static char grain_glyph(int level)
{
    switch (s_cfg.grain) {
    case LS_WF_GRAIN_ASCII: {
        int i = level * 10 / 16;
        if (i < 0) i = 0;
        if (i > 9) i = 9;
        return ASCII_RAMP[i];
    }
    case LS_WF_GRAIN_BARS:
        /* Eighth-height, so a cell fills from the bottom. Level 15 is a full
           cell, which is the same glyph the spectrum's tallest bar uses -
           deliberately, because they are saying the same thing. */
        return LS_TUI_TRACE(level >= 15 ? 8 : (level * 8 / 15) + 1);
    default:
        /* Five steps, not four starting at a quarter: the old ramp put
           levels 1..4 all on the 25% shade, so the bottom third of the
           scale was one flat wash. */
        return level >= 11 ? LS_TUI_SHADE_FULL
             : level >= 7  ? LS_TUI_SHADE_75
             : level >= 4  ? LS_TUI_SHADE_50
                           : LS_TUI_SHADE_25;
    }
}

/* A waterfall that is not scrolling says why. */

#define STALL_QUIET_MS 1500

/* Starved means late by the source's own clock, not by a constant. */

static uint32_t stall_after_ms(void)
{
    const uint64_t own = s_have_feed ? 2ull * s_feed.period_ms : 0;
    if (own <= STALL_QUIET_MS) return STALL_QUIET_MS;
    return own > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)own;
}

static void draw_stall(tui_surface *sf, tui_rect r)
{
    if (r.h < 1 || r.w < 12) return;

    char msg[48];
    uint8_t at;
    if (s_cfg.paused) {
        snprintf(msg, sizeof(msg), " HELD - press HOLD to run ");
        at = TUI_ATTR(TUI_BLACK, TUI_YELLOW);
    } else if (s_last_push_us &&
               (esp_timer_get_time() - s_last_push_us) / 1000 >
                   (int64_t)stall_after_ms()) {
        const uint32_t ms =
            (uint32_t)((esp_timer_get_time() - s_last_push_us) / 1000);
        snprintf(msg, sizeof(msg), " no rows for %u.%us ",
                 (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100));
        at = TUI_ATTR(TUI_BLACK, TUI_RED);
    } else {
        return;
    }

    const int len = (int)strlen(msg);
    if (len > r.w) return;
    tui_put_str(sf, r, r.x + (r.w - len) / 2, r.y + r.h / 2, msg, at);
}

static void draw_falls(tui_surface *sf, tui_rect r)
{
    if (r.h < 1 || r.w < 4) return;

    const int per_cell = (s_cfg.grain == LS_WF_GRAIN_DENSE) ? 2 : 1;

    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x++) {
            if (per_cell == 2) {
                const int a = level_in_row(sample(y * 2,     x, r.w), y * 2);
                const int b = level_in_row(sample(y * 2 + 1, x, r.w), y * 2 + 1);
                if (!a && !b) continue;
                tui_put_char(sf, r, r.x + x, r.y + y, LS_TUI_BLOCK_UPPER,
                             TUI_ATTR(colour_of(a), colour_of(b)));
            } else {
                /* Through the row's own window, not this frame's. */
                const int a = level_in_row(sample(y, x, r.w), y);
                if (a <= 1) continue;     /* the floor's floor stays black */
                tui_put_char(sf, r, r.x + x, r.y + y, grain_glyph(a),
                             TUI_ATTR(colour_of(a), TUI_BLACK));
            }
        }
    }

    if (s_marker >= 0 && s_marker < r.w)
        for (int y = 0; y < r.h; y += 2)
            tui_put_char(sf, r, r.x + s_marker, r.y + y, LS_TUI_BLOCK_LEFT,
                         TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

static void fmt_mhz(char *buf, size_t cap, uint32_t hz)
{
    snprintf(buf, cap, "%u.%03u", (unsigned)(hz / 1000000u),
             (unsigned)((hz % 1000000u) / 1000u));
}

static void draw_scale(tui_surface *sf, tui_rect r)
{
    if (!s_have_feed || !s_feed.span_hz || r.w < 24) return;
    const uint32_t half = s_feed.span_hz / 2;
    char lo[16], mid[16], hi[16];
    fmt_mhz(lo,  sizeof(lo),  s_feed.center_hz > half ? s_feed.center_hz - half : 0);
    fmt_mhz(mid, sizeof(mid), s_feed.center_hz);
    fmt_mhz(hi,  sizeof(hi),  s_feed.center_hz + half);
    const uint8_t a = LS_ATTR_DIM;
    tui_put_str(sf, r, r.x, r.y, lo, a);
    tui_put_str(sf, r, r.x + (r.w - (int)strlen(mid)) / 2, r.y, mid,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, r, r.x + r.w - (int)strlen(hi), r.y, hi, a);
}

uint32_t ls_wf_marker_hz(void)
{
    if (s_marker < 0 || !s_have_feed || !s_feed.span_hz) return 0;
    const int w = s_plot_rect.w > 0 ? s_plot_rect.w : 1;
    const int64_t off = (int64_t)s_feed.span_hz * (s_marker * 2 + 1) / (2 * w);
    const int64_t hz = (int64_t)s_feed.center_hz - s_feed.span_hz / 2 + off;
    return hz > 0 ? (uint32_t)hz : 0;
}

/* How strong the strongest thing on the band is, as a number. */

static uint8_t peak_now(void)
{
    if (!s_hist || s_filled <= 0 || s_plot_rect.w <= 0) return 0;
    uint8_t top = 0;
    for (int x = 0; x < s_plot_rect.w; x++) {
        const uint8_t v = sample(0, x, s_plot_rect.w);
        if (v > top) top = v;
    }
    return top;
}

/* Returns the columns used, so the caller can keep what follows clear of it. */
static int draw_level_meter(tui_surface *sf, tui_rect r, int x)
{
    if (r.w < 24) return 0;                 /* no room; the row has other jobs */
    const uint8_t raw = peak_now();

    char val[12];
    if (s_have_feed && s_feed.top_db > s_feed.floor_db) {
        const float db = s_feed.floor_db +
            (s_feed.top_db - s_feed.floor_db) * ((float)raw / 255.0f);
        snprintf(val, sizeof(val), "%d dB", (int)db);
    } else {
        snprintf(val, sizeof(val), "%d%%", raw * 100 / 255);
    }

    const uint8_t lab = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    tui_put_str(sf, r, x, r.y, "PK", lab);
    tui_put_str(sf, r, x + 3, r.y, val,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* Six rising traces, the same reading MESH's signal meter and P25's own
       gauge use, so a level looks like a level wherever it appears. */
    const int bar_x = x + 3 + 8;
    for (int i = 0; i < 6; i++) {
        const int h = 2 + i;                       /* 2..7 eighths */
        const bool lit = raw >= (uint8_t)((i + 1) * 255 / 7);
        const uint8_t hue = i >= 4 ? TUI_RED : i >= 2 ? TUI_YELLOW : TUI_GREEN;
        tui_put_char(sf, r, bar_x + i, r.y, LS_TUI_TRACE(h),
                    TUI_ATTR(lit ? (hue | TUI_BRIGHT) : (TUI_BLACK | TUI_BRIGHT),
                             TUI_BLACK));
    }
    return (bar_x + 6) - x;
}

static void draw_readout(tui_surface *sf, tui_rect r)
{
    char buf[96];
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t hot = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);

    if (s_marker >= 0) {
        const uint32_t hz = ls_wf_marker_hz();
        const uint8_t raw = sample(0, s_marker, s_plot_rect.w);
        /* Report the level in the source's own dB when it told us what its
           ends mean, and as a fraction when it did not. Inventing a dBm here
           would be the same class of claim the project already refuses. */
        char lvl[24];
        if (s_have_feed && s_feed.top_db > s_feed.floor_db) {
            const float db = s_feed.floor_db +
                (s_feed.top_db - s_feed.floor_db) * ((float)raw / 255.0f);
            snprintf(lvl, sizeof(lvl), "%d dB", (int)db);
        } else {
            snprintf(lvl, sizeof(lvl), "%d%%", raw * 100 / 255);
        }
        char mhz[16];
        fmt_mhz(mhz, sizeof(mhz), hz);
        snprintf(buf, sizeof(buf), "MARK %s MHz  %s", mhz, lvl);
        tui_put_str(sf, r, r.x, r.y, buf, hot);
    } else if (s_have_feed && s_feed.note) {
        tui_put_str(sf, r, r.x, r.y, s_feed.note, dim);
    }

    const int meter = draw_level_meter(sf, r, r.x + 24);
    const int stats_left = meter > r.x + 24 ? meter : r.x + 24;

    /* The instrument reports its own cost. A waterfall that starves the
       decoder is worse than no waterfall, and this is the number that says
       whether it is doing that. */

    const bool feeding = ls_fresh(&s_feed_fresh, s_seq, FEED_FADE_MS) > 0;
    snprintf(buf, sizeof(buf), "%c %s  %dx%d  %lu us  row %lu ms  %lu drop",
             ls_motion_pip(feeding),
             s_label[0] ? s_label : "-", s_st.bins, s_st.rows,
             (unsigned long)s_st.draw_us, (unsigned long)s_st.row_ms,
             (unsigned long)s_st.dropped);
    int x = r.x + r.w - (int)strlen(buf);
    /* Against where the meter actually ENDED, not against the
       column it started at. The end was computed and thrown away, so a wide
       meter and a long stats line could still overlap. */
    if (x > stats_left) tui_put_str(sf, r, x, r.y, buf, dim);
}

/* ---------------------------------------------------------------- controls */

static const char *split_name(void)
{
    switch (s_cfg.split_pct) {
    case 0:   return "FALL";
    case 25:  return "1/4";
    case 50:  return "1/2";
    case 75:  return "3/4";
    default:  return "SPEC";
    }
}

static const char *const GRAIN_NAME[LS_WF_GRAIN__COUNT] = {
    "shade", "ascii", "dense", "bars"
};

/* One knob for contrast, because it was already there twice and named neither time. */

typedef struct { const char *name; int8_t ref; uint8_t range; } wf_contrast_t;
static const wf_contrast_t CONTRAST[] = {
    { "soft", 0, 16 },
    { "norm", 0, 12 },
    { "hard", 2,  8 },
    { "max",  4,  4 },
};
#define N_CONTRAST ((int)(sizeof(CONTRAST) / sizeof(CONTRAST[0])))

static const char *contrast_name(void)
{
    for (int i = 0; i < N_CONTRAST; i++)
        if (s_cfg.ref == CONTRAST[i].ref && s_cfg.range == CONTRAST[i].range)
            return CONTRAST[i].name;
    return "cust";
}

static void contrast_step(void)
{

    int at = -1;
    for (int i = 0; i < N_CONTRAST; i++)
        if (s_cfg.ref == CONTRAST[i].ref && s_cfg.range == CONTRAST[i].range)
            at = i;
    const wf_contrast_t *next = &CONTRAST[(at + 1) % N_CONTRAST];
    s_cfg.ref = next->ref;
    s_cfg.range = next->range;
}

static void build_buttons(ls_btn_t *b, char v[LS_WF_BTNS][12])
{
    snprintf(v[0], 12, "%s", split_name());
    snprintf(v[1], 12, "%+d", s_cfg.ref);
    snprintf(v[2], 12, "%d", s_cfg.range);
    snprintf(v[3], 12, "%s", PAL_NAME[s_cfg.palette < LS_WF_PAL__COUNT
                                      ? s_cfg.palette : 0]);
    /* Two controls, two vocabularies, because they are two
       different things now. AVG counts sweeps blended into a row; SPEED is
       a fraction of full scroll, and "full" says so in a word. */
    if (s_cfg.avg <= 1) snprintf(v[4], 12, "off");
    else                snprintf(v[4], 12, "x%d", s_cfg.avg);
    if (s_cfg.decim <= 1) snprintf(v[5], 12, "full");
    else                  snprintf(v[5], 12, "1/%d", s_cfg.decim);
    snprintf(v[6], 12, "%s", s_cfg.peak_hold ? "ON" : "off");
    snprintf(v[7], 12, "%s", s_cfg.paused ? "HELD" : "run");
    snprintf(v[8], 12, "%s", GRAIN_NAME[s_cfg.grain < LS_WF_GRAIN__COUNT
                                        ? s_cfg.grain : 0]);
    snprintf(v[9], 12, "%s", contrast_name());

    b[0] = (ls_btn_t){ "SPLIT", v[0], 's', false, false };
    b[1] = (ls_btn_t){ "REF",   v[1], 'r', false, false };
    b[2] = (ls_btn_t){ "RANGE", v[2], 'g', false, false };
    b[3] = (ls_btn_t){ "COLOR", v[3], 'p', false, false };
    b[4] = (ls_btn_t){ "AVG",   v[4], 'a', false, false };
    b[5] = (ls_btn_t){ "SPEED", v[5], 'd', false, false };
    b[6] = (ls_btn_t){ "PEAK",  v[6], 'k', s_cfg.peak_hold, false };
    b[7] = (ls_btn_t){ "HOLD",  v[7], 'h', s_cfg.paused, false };

    b[8] = (ls_btn_t){ "DETAIL", v[8], 'f', false, false };

    b[9] = (ls_btn_t){ "CNTRST", v[9], 'c', false, false };
}

static void act(int i)
{
    switch (i) {
    case 0: s_cfg.split_pct = (uint8_t)((s_cfg.split_pct + 25) % 125); break;
    case 1: s_cfg.ref = (int8_t)(s_cfg.ref >= 8 ? -8 : s_cfg.ref + 2);  break;
    case 2: s_cfg.range = (uint8_t)(s_cfg.range <= 4 ? 16 : s_cfg.range - 4); break;
    case 3: s_cfg.palette = (uint8_t)((s_cfg.palette + 1) % LS_WF_PAL__COUNT); break;
    case 4: s_cfg.avg = (uint8_t)(s_cfg.avg >= 8 ? 1 : s_cfg.avg * 2); break;
    case 5: s_cfg.decim = (uint8_t)(s_cfg.decim >= 8 ? 1 : s_cfg.decim * 2); break;
    case 6: s_cfg.peak_hold = !s_cfg.peak_hold; memset(s_peak, 0, sizeof(s_peak)); break;
    case 7: s_cfg.paused = !s_cfg.paused; break;
    case 8: s_cfg.grain = (uint8_t)((s_cfg.grain + 1) % LS_WF_GRAIN__COUNT);
            break;
    case 9: contrast_step(); break;
    default: break;
    }
}

bool ls_wf_key(ls_tk_t key, char ch)
{
    ls_btn_t b[LS_WF_BTNS];
    char v[LS_WF_BTNS][12];
    build_buttons(b, v);

    if (key == LS_TK_CHAR) {

        const int i = ls_btn_key(ch, b, LS_WF_BTNS);
        if (i >= 0) { act(i); s_focus = i; return true; }
        if (ch == 'm' || ch == 'M') {
            s_marker = (s_marker < 0) ? s_plot_rect.w / 2 : -1;
            return true;
        }
        /* B puts the button strip away and brings it back. Not on
           the strip itself as an eleventh button - a control whose only
           route back is the thing it hides is a trap - and landscape only,
           because portrait has no keyboard to press it with. */
        if ((ch == 'b' || ch == 'B') && ls_tui_is_wide()) {
            /* No invalidate needed: the frame is blanked before every
               draw, so the cells the bar occupied are redrawn as spectrum
               and the diff carries them. */
            s_bar_hidden = !s_bar_hidden;
            return true;
        }
        return false;
    }
    switch (key) {
    case LS_TK_LEFT:
        if (s_marker < 0) s_marker = s_plot_rect.w / 2;
        else if (s_marker > 0) s_marker--;
        return true;
    case LS_TK_RIGHT:
        if (s_marker < 0) s_marker = s_plot_rect.w / 2;
        else if (s_marker < s_plot_rect.w - 1) s_marker++;
        return true;
    case LS_TK_UP:    s_cfg.ref = (int8_t)(s_cfg.ref < 8 ? s_cfg.ref + 1 : 8); return true;
    case LS_TK_DOWN:  s_cfg.ref = (int8_t)(s_cfg.ref > -8 ? s_cfg.ref - 1 : -8); return true;
    case LS_TK_ENTER: s_cfg.paused = !s_cfg.paused; return true;
    case LS_TK_ESC:   if (s_marker >= 0) { s_marker = -1; return true; } return false;
    default: return false;
    }
}

bool ls_wf_touch(int col, int row)
{
    if (s_bar_rect.h > 0 &&
        row >= s_bar_rect.y && row < s_bar_rect.y + s_bar_rect.h) {
        const int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL);
        if (i >= 0) { act(i); s_focus = i; return true; }
        return true;
    }

    if (row >= s_plot_rect.y && row < s_plot_rect.y + s_plot_rect.h &&
        col >= s_plot_rect.x && col < s_plot_rect.x + s_plot_rect.w) {
        const int c = col - s_plot_rect.x;
        s_marker = (s_marker == c) ? -1 : c;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------- assembling */

static void layout_and_draw(tui_surface *sf, tui_rect area, bool chrome)
{
    const int64_t t0 = esp_timer_get_time();
    /* Once per draw; every level below is coloured against it. */
    s_light = ls_tui_daylight();

    if (!s_hist || s_filled == 0) {
        const uint8_t dim = LS_ATTR_DIM;
        tui_put_str(sf, area, area.x + 2, area.y + 1,
                    s_cfg.paused ? "HELD - press HOLD to run" :
                    s_hist ? "waiting for the receiver"
                           : "no PSRAM for the waterfall", dim);
        if (!chrome) return;
    }

    tui_rect body = area;
    /* Landscape can put the buttons away. */

    if (chrome && ls_tui_is_wide() && s_bar_hidden && !s_cfg.paused) {
        chrome = false;
        s_bar_rect = tui_rect_make(0, -1, 0, 0);
        tui_put_str(sf, area, area.x + 2, area.y + area.h - 1,
                    "B shows the controls",
                    LS_ATTR_DIM);
        body = tui_rect_make(area.x, area.y, area.w, area.h - 1);
    }
    if (chrome) {
        const bool wide = ls_tui_is_wide();
        /* Portrait gets an eight-row bar and landscape two.

           Same eight buttons; in portrait they wrap onto two rows of four,
           which is what makes each one wide enough to hit. Four rows made
           each one two rows tall - 2.6 mm on a 334 dpi panel, against the
           seven a thumb wants - so the width was fixed and the height was
           left at the number that was already wrong. Eight rows makes them
           four each, and portrait has sixty-six. */
        int bar_h = wide ? 2 : 8;
        if (bar_h > area.h / 3) bar_h = area.h / 3;
        if (bar_h < 1) bar_h = 1;

        s_bar_rect = tui_rect_make(area.x, area.y + area.h - bar_h,
                                   area.w, bar_h);
        body = tui_rect_make(area.x, area.y, area.w, area.h - bar_h - 2);

        ls_btn_t b[LS_WF_BTNS];
        char v[LS_WF_BTNS][12];
        build_buttons(b, v);
        ls_btn_bar_slot(sf, s_bar_rect, b, LS_WF_BTNS, s_focus,
                        LS_BTN_SLOT_WATERFALL);

        draw_scale(sf, tui_rect_make(area.x + 1, s_bar_rect.y - 2,
                                     area.w - 2, 1));
        draw_readout(sf, tui_rect_make(area.x + 1, s_bar_rect.y - 1,
                                       area.w - 2, 1));
    } else {
        s_bar_rect = tui_rect_make(0, -1, 0, 0);
    }

    if (body.h < 2 || body.w < 6) { s_st.draw_us = 0; return; }

    int spec_h = body.h * s_cfg.split_pct / 100;
    if (s_cfg.split_pct > 0 && spec_h < 2) spec_h = 2;
    if (spec_h > body.h) spec_h = body.h;
    const int fall_h = body.h - spec_h;

    s_plot_rect = tui_rect_make(body.x + 1, body.y, body.w - 2, body.h);

    if (spec_h > 0)
        draw_spectrum(sf, tui_rect_make(s_plot_rect.x, body.y,
                                        s_plot_rect.w, spec_h));
    if (fall_h > 0) {
        const tui_rect fr = tui_rect_make(s_plot_rect.x, body.y + spec_h,
                                          s_plot_rect.w, fall_h);
        draw_falls(sf, fr);
        draw_stall(sf, fr);
    }

    s_st.scale_lo  = (uint8_t)s_auto_lo;
    s_st.scale_hi  = (uint8_t)s_auto_hi;
    s_st.scale_top = (uint8_t)s_auto_top;
    s_st.bins = s_plot_rect.w;
    s_st.rows = fall_h * ((s_cfg.grain == LS_WF_GRAIN_DENSE) ? 2 : 1);
    s_st.hist_rows = s_filled;
    s_st.ready = (s_hist != NULL);
    s_st.age_ms = s_last_push_us
        ? (uint32_t)((esp_timer_get_time() - s_last_push_us) / 1000) : 0;
    s_st.draw_us = (uint32_t)(esp_timer_get_time() - t0);
    if (s_st.draw_us > s_st.worst_us) s_st.worst_us = s_st.draw_us;
}

void ls_wf_draw(tui_surface *sf, tui_rect area)      { layout_and_draw(sf, area, true); }
void ls_wf_draw_mini(tui_surface *sf, tui_rect area) { layout_and_draw(sf, area, false); }

void ls_wf_stats(ls_wf_stats_t *out) { if (out) *out = s_st; }

uint32_t ls_wf_seq(void) { return s_seq; }

const char *ls_wf_idle_reason(void)
{
    /* Order matters: with no owner there is no buffer either, so asking
       about PSRAM first would report the wrong one of the two. */
    if (s_owner == LS_WF_OWNER_NONE)
        return "no receiver is producing a spectrum";
    if (!s_st.ready)
        return "no PSRAM for the history buffer";
    /* HELD before empty, because held is WHY it is empty. */

    if (s_cfg.paused)
        return "HELD - press HOLD to run";
    if (s_st.hist_rows == 0)
        return "waiting for the receiver";
    return NULL;
}
const ls_wf_cfg_t *ls_wf_cfg(void)   { return &s_cfg; }

void ls_wf_cfg_set(const ls_wf_cfg_t *cfg)
{
    if (!cfg) return;
    s_cfg = *cfg;
    if (s_cfg.avg   < 1) s_cfg.avg = 1;
    if (s_cfg.decim < 1) s_cfg.decim = 1;
    if (s_cfg.range < 4) s_cfg.range = 4;
    if (s_cfg.range > 16) s_cfg.range = 16;
    if (s_cfg.palette >= LS_WF_PAL__COUNT) s_cfg.palette = 0;
    if (s_cfg.grain >= LS_WF_GRAIN__COUNT) s_cfg.grain = 0;
    if (s_cfg.split_pct > 100) s_cfg.split_pct = 100;
}
