/* Capture level and receiver-rate history.
   Use the nonblocking hub status; probe storage only on entry.
   Keep line charts so steady rates do not fill the plot. */
#include "../../ls_tui_screen.h"

#include "esp_timer.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "apps/rec/rec_state.h"

static uint64_t s_free_bytes;
static bool     s_free_known;

/* Sample at 320 ms intervals; reset across missing data or retunes. */
#define HIST      128
#define SAMPLE_US 320000

static uint16_t s_mag[HIST];
static uint16_t s_thresh[HIST];
static int s_level_top = 64;
static int s_scale_quiet;
static uint16_t s_rate[HIST];
static int      s_head = -1;      /* -1 until the first sample */
static int      s_filled;
static int64_t s_last_sample;
static uint32_t s_freq;

/* Blink the recording indicator while capturing. */
static uint32_t s_blink;

static void on_enter(void)
{
    /* Filesystem call, once, off the draw path. */
    s_free_bytes = rec_dir_free_bytes();
    s_free_known = true;

    s_head = -1;
    s_filled = 0;
    s_last_sample = 0;
    s_level_top = 64;
    s_scale_quiet = 0;
}

static bool sample(const rec_hub_status_t *st)
{
    const int64_t now = esp_timer_get_time();
    if (!st->receiver_streaming || st->freq_hz != s_freq ||
        (s_filled && now - s_last_sample > SAMPLE_US * 2)) {
        s_head = -1;
        s_filled = 0;
        s_last_sample = 0;
        s_level_top = 64;
        s_scale_quiet = 0;
    }
    s_freq = st->freq_hz;
    if (!st->receiver_streaming) return false;
    if (s_filled && now - s_last_sample < SAMPLE_US) return false;
    s_last_sample = now;
    s_head = (s_head + 1) % HIST;
    s_mag[s_head]  = (uint16_t)(st->mag_now > 65535 ? 65535 :
                                st->mag_now < 0 ? 0 : st->mag_now);
    /* Keep threshold and scale on the same clock as the trace. */
    s_thresh[s_head] = (uint16_t)(st->mag_thresh > 65535 ? 65535 :
                                 st->mag_thresh < 0 ? 0 : st->mag_thresh);
    /* Rate in KiB/s, so the ring stays 16 bit up to 64 MB/s - far past what
       an SD card on this bus will ever take. */
    s_rate[s_head] = (uint16_t)(st->bytes_sec / 1024u);
    if (s_filled < HIST) s_filled++;
    return true;
}

static void row(tui_surface *sf, tui_rect a, int y, const char *label,
                const char *value, uint8_t va)
{
    tui_put_str(sf, a, a.x + 2, a.y + y, label,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, a, a.x + 14, a.y + y, value, va);
}

static const char *phase_name(rec_phase_t p)
{
    switch (p) {
    case REC_IDLE:      return "idle";
    case REC_ARMED:     return "armed";
    case REC_CAPTURING: return "CAPTURING";
    case REC_DONE:      return "done";
    default:            return "?";
    }
}

/* Newest sample at right; leave missing columns empty.
   Draw the threshold before the trace and label the scale at the call site. */
static void chart(tui_surface *sf, tui_rect a, int x, int y, int w, int h,
                  const uint16_t *ring, const uint16_t *thresholds,
                  int lo, int top, int mark,
                  uint8_t trace_at, uint8_t hot_at, uint8_t mark_at)
{
    const int span = top - lo;
    if (w < 4 || h < 2 || span <= 0) return;

    /* The threshold first, so the trace draws over it where they meet. */
    if (mark >= lo && mark <= top) {
        const int my = y + h - 1 - ((mark - lo) * (h - 1) / span);
        for (int i = 0; i < w; i++)
            tui_put_char(sf, a, x + i, my, (i & 1) ? '-' : ' ', mark_at);
    }

    if (s_head < 0) return;

    int prev_cell = -1;
    for (int i = 0; i < w; i++) {
        const int back = w - 1 - i;
        if (back >= s_filled) { prev_cell = -1; continue; }
        const int idx = (s_head - back + HIST * 2) % HIST;

        int v = ring[idx];
        if (v > top) v = top;
        if (v < lo)  v = lo;

        const int eighths = (v - lo) * (h * 8) / span;
        int cell = eighths / 8;
        int rem  = eighths % 8;
        if (cell >= h) { cell = h - 1; rem = 8; }
        if (!rem) rem = 1;      /* a sample at the baseline is still a sample */

        /* Historical colors use the threshold at acquisition. */
        const int threshold = thresholds ? thresholds[idx] : mark;
        const uint8_t at = (threshold > 0 && ring[idx] >= threshold)
                           ? hot_at : trace_at;

        /* Join it to the sample before it.

           Without this the chart is a scatter of single marks and the eye
           has to do the joining, which it does badly when the gaps are
           large - a burst forty cells above the floor read as two unrelated
           specks. The run is drawn first and the sample's own glyph over
           it, so the endpoint keeps its sub-cell position. */
        if (prev_cell >= 0 && prev_cell != cell) {
            const int from = (prev_cell < cell) ? prev_cell + 1 : cell + 1;
            const int to   = (prev_cell < cell) ? cell : prev_cell;
            for (int c = from; c <= to; c++)
                tui_put_char(sf, a, x + i, y + h - 1 - c, '|', at);
        }

        tui_put_char(sf, a, x + i, y + h - 1 - cell, LS_TUI_TRACE(rem), at);
        prev_cell = cell;
    }
}

/* The range the samples actually occupy.

   Both ends, because scaling from zero is what turned a steady write rate
   into a filled rectangle. A trace of 497..503 against a 0..503 scale is a
   flat line at the top; against 495..505 it is the shape it really has. The
   baseline is only lifted when the samples sit well clear of zero, so a
   chart of something that genuinely idles at nothing still shows that. */
static void ring_range(const uint16_t *ring, int *out_lo, int *out_top)
{
    int lo = 65535, hi = 0;
    for (int i = 0; i < s_filled; i++) {
        if (ring[i] < lo) lo = ring[i];
        if (ring[i] > hi) hi = ring[i];
    }
    if (s_filled == 0) { lo = 0; hi = 0; }

    int base = 0;
    if (lo > hi / 4 && hi > 0) {
        const int pad = (hi - lo) / 4 + 1;
        base = lo - pad;
        if (base < 0) base = 0;
    }
    *out_lo  = base;
    *out_top = hi;
}

static void human_bytes(char *buf, size_t len, uint64_t n)
{
    if (n >= 1024ull * 1024 * 1024) snprintf(buf, len, "%.1f GB", (double)n / (1024.0 * 1024 * 1024));
    else if (n >= 1024ull * 1024)   snprintf(buf, len, "%.1f MB", (double)n / (1024.0 * 1024));
    else if (n >= 1024ull)          snprintf(buf, len, "%.1f kB", (double)n / 1024.0);
    else                            snprintf(buf, len, "%u B", (unsigned)n);
}

/* ------------------------------------------------------------------ parts */

static void draw_facts(tui_surface *sf, tui_rect a, const rec_hub_status_t *st)
{
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t hot  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48];

    row(sf, a, 1, "state", phase_name(st->phase),
        st->phase == REC_CAPTURING ? hot : st->phase == REC_ARMED ? val : dim);
    /* The recording light. One cell, immediately before the word it
       is redundant with as text and not redundant with as motion - see the
       note by s_blink. Drawn after the row itself so it sits on top of
       whatever blank column CAPTURING's left edge leaves there. */
    if (st->phase == REC_CAPTURING && ((s_blink / 12) & 1))
        tui_put_char(sf, a, a.x + 13, a.y + 1, LS_TUI_SHADE_FULL, hot);

    snprintf(buf, sizeof(buf), "%.4f MHz", (double)st->freq_hz / 1e6);
    row(sf, a, 2, "frequency", buf, val);

    row(sf, a, 3, "receiver", st->receiver_streaming ? "streaming" : "stopped",
        st->receiver_streaming ? good : dim);

    snprintf(buf, sizeof(buf), "%u", (unsigned)st->captures);
    row(sf, a, 4, "captures", buf, st->captures ? good : dim);

    if (a.h > 6) {
        snprintf(buf, sizeof(buf), "%d", st->edges);
        row(sf, a, 5, "edges", buf, st->edges ? val : dim);
    }

    if (a.h > 7 && s_free_known) {

        const bool known = (s_free_bytes != UINT64_MAX);
        if (known) human_bytes(buf, sizeof(buf), s_free_bytes);
        else       snprintf(buf, sizeof(buf), "no card");
        row(sf, a, 6, "free", buf,
            !known                               ? hot
            : s_free_bytes < 32ull * 1024 * 1024 ? hot : dim);
    }
}

/* Shrink only after ten samples fit a smaller visible range. */
static void level_scale(int width)
{
    int peak = 0;
    for (int back = 0; back < s_filled && back < width; back++) {
        const int i = (s_head - back + HIST) % HIST;
        if (s_mag[i] > peak) peak = s_mag[i];
        if (s_thresh[i] > peak) peak = s_thresh[i];
    }
    peak += peak / 4;
    int want = 64;
    while (want < peak && want < 65536) want *= 2;
    if (want > s_level_top || (want < s_level_top && ++s_scale_quiet >= 10)) {
        s_level_top = want;
        s_scale_quiet = 0;
    } else if (want == s_level_top) {
        s_scale_quiet = 0;
    }
}

static void draw_level(tui_surface *sf, tui_rect a, const rec_hub_status_t *st,
                       bool sampled)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t hot  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t on   = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, a, "LEVEL", TUI_CYAN);
    if (a.h < 7 || a.w < 24) return;

    if (sampled) level_scale(a.w - 2);
    /* A zero baseline and delayed scale changes keep the trace readable. */
    const int lo = 0, top = s_level_top;
    const int level = s_head >= 0 ? s_mag[s_head] : 0;
    const int threshold = s_head >= 0 ? s_thresh[s_head] : 0;

    const int h = a.h - 5;
    chart(sf, a, a.x + 1, a.y + 1, a.w - 2, h, s_mag, s_thresh, lo, top,
          threshold, on, hot, val);

    snprintf(buf, sizeof(buf), "level %d  trigger %d",
             level, threshold);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 4, buf,
                (threshold > 0 && level >= threshold) ? hot : dim);
    snprintf(buf, sizeof(buf), "scale %d-%d raw; yellow = trigger", lo, top);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 3, buf, dim);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 2,
                !st->receiver_streaming ? "RX stopped: no live level" :
                st->phase == REC_ARMED ? "Armed: waiting for trigger" :
                st->phase == REC_CAPTURING ? "Capturing: red >= trigger" :
                "Preview: press ARM to capture", dim);
}

static void draw_rate(tui_surface *sf, tui_rect a, const rec_hub_status_t *st)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t on  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, "RX RATE", TUI_CYAN);
    if (a.h < 7 || a.w < 24) return;

    int lo, top;
    ring_range(s_rate, &lo, &top);
    if (top < lo + 8) top = lo + 8;

    const int h = a.h - 5;
    chart(sf, a, a.x + 1, a.y + 1, a.w - 2, h, s_rate, NULL, lo, top, -1,
          on, on, dim);

    const int now = (s_head >= 0) ? s_rate[s_head] : 0;
    char cap[80];
    snprintf(cap, sizeof(cap), "%d KiB/s  auto %d-%d", now, lo, top);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 4, cap, dim);
    const int visible = s_filled < a.w - 2 ? s_filled : a.w - 2;
    snprintf(cap, sizeof(cap), "%.1fs ago <-- time --> now",
             visible > 1 ? (visible - 1) * (SAMPLE_US / 1000000.0) : 0.0);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 3, cap, dim);
    tui_put_str(sf, a, a.x + 2, a.y + a.h - 2,
                !st->receiver_streaming ? "RX stopped: no live samples" :
                !st->bytes_sec ? "Waiting for receiver samples" :
                "Receiver input, not disk writes", dim);
}

/* ------------------------------------------------------------------- draw */

#define REC_BTN_H 5

static tui_rect s_btn;

/* Whether a CAPTURE is running, which is not what rec_active() says. */

static bool capture_running(void)
{
    rec_hub_status_t st;
    memset(&st, 0, sizeof(st));
    rec_get_hub_status(&st);
    return st.phase == REC_ARMED || st.phase == REC_CAPTURING;
}

static void capture_toggle(void)
{
    if (capture_running()) rec_disarm();
    else                   rec_arm_request();
}

static void draw_button(tui_surface *sf, tui_rect a, bool active)
{
    s_btn = a;
    if (a.w < 8 || a.h < 3) { s_btn.w = 0; return; }

    const uint8_t hue = active ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    ls_panel_box(sf, a, NULL, hue);

    tui_rect f = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (f.w <= 0 || f.h <= 0) return;
    ls_fill_dither(sf, f, active ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
    ls_dither_label(sf, f, (f.h - 1) / 2, active ? "STOP" : "ARM",
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48];

    rec_hub_status_t st;
    memset(&st, 0, sizeof(st));
    rec_get_hub_status(&st);
    const bool sampled = sample(&st);
    s_blink++;

    tui_box(sf, area, "CAPTURE", frame);

    tui_rect body = tui_rect_make(area.x + 1, area.y + 1,
                                  area.w - 2, area.h - 2);
    if (body.h < 6 || body.w < 14) return;

    const int foot = (body.h > 10) ? 1 : 0;
    int usable = body.h - foot;

    s_btn.w = 0;
    if (usable >= REC_BTN_H + 8) {
        draw_button(sf, tui_rect_make(body.x, body.y + usable - REC_BTN_H,
                                      body.w, REC_BTN_H),
                    st.phase == REC_ARMED || st.phase == REC_CAPTURING);
        usable -= REC_BTN_H;
    }

    if (ls_tui_is_wide()) {
        /* Landscape has width and not height: facts on the left, the two
           charts stacked on the right, both short. */
        tui_rect left, right;
        ls_tui_split(tui_rect_make(body.x, body.y, body.w, usable),
                     &left, &right);
        draw_facts(sf, left, &st);
        const int lh = (right.h * 3) / 5;
        draw_level(sf, tui_rect_make(right.x, right.y, right.w, lh), &st, sampled);
        if (right.h - lh >= 4)
            draw_rate(sf, tui_rect_make(right.x, right.y + lh, right.w,
                                        right.h - lh), &st);
    } else {
        /* Portrait has the height. The facts take what they need and the
           charts divide the rest, level getting the larger share because it
           is the one being read against a threshold. */
        const int facts_h = (usable > 22) ? 8 : 7;
        draw_facts(sf, tui_rect_make(body.x, body.y, body.w, facts_h), &st);

        const int rest = usable - facts_h;
        if (rest >= 8) {
            const int lh = (rest * 3) / 5;
            draw_level(sf, tui_rect_make(body.x, body.y + facts_h,
                                         body.w, lh), &st, sampled);
            draw_rate(sf, tui_rect_make(body.x, body.y + facts_h + lh,
                                        body.w, rest - lh), &st);
        } else if (rest >= 5) {
            draw_level(sf, tui_rect_make(body.x, body.y + facts_h,
                                         body.w, rest), &st, sampled);
        }
    }

    if (foot) {
        const char *d = rec_dir();
        snprintf(buf, sizeof(buf), "to %s", d ? d : "?");
        tui_put_str(sf, area, area.x + 2, area.y + area.h - 2, buf, dim);
    }
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    if (k == LS_TK_ENTER) {

        capture_toggle();
        return true;
    }
    return false;
}

/* The button, and nothing else. A tap anywhere else in this screen
   is claimed and ignored - see the note above draw_button for what it used
   to do instead. */
static bool touch(int col, int row)
{
    if (s_btn.w > 0 &&
        col >= s_btn.x && col < s_btn.x + s_btn.w &&
        row >= s_btn.y && row < s_btn.y + s_btn.h) {
        capture_toggle();
    }
    return true;
}

const ls_tui_screen_t ls_scr_rec = {
    /* the capture engine IS the receiver here. */
    .radio = "REC",
    .name = "REC",
    .hint = "TAP the button  ENTER arms and disarms",
    .enter = on_enter,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
