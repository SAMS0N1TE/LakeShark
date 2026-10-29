/* SPECTRE: a small spectrum analyser on any radio the board has.
 *
 * SDK 2.0's radio calls and SDK 2.1's shared controls in one screen:
 * lap_radio_list and lap_rx_open with LAP_RX_SPECTRUM_ONLY (the board does
 * the FFT; the app never handles I/Q), lap_rx_spectrum for the averaged
 * lines. Its own face is the braille trace with peak hold; everything else
 * is the board's: the waterfall under it (lap_wf_*, with the board's
 * palettes and DISPLAY settings), the annunciator rail, the ASCII meter,
 * the tap keys along the bottom, the RADIO list, the TUNE keypad and one
 * layered OPTIONS list. The trace moves only when a new line arrives, so a
 * stopped radio is a still screen.
 *
 * Taps: the trace or the waterfall centres that frequency; the keys pick the
 * radio, step the tuning, type a frequency, hold peaks and open OPTIONS.
 * Keys: r radio, , . step (or LEFT/RIGHT), t tune, h hold, o options,
 * UP/DOWN gain. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include <stdio.h>
#include <string.h>

#define A(fg) LAP_ATTR((fg), LAP_BLACK)
#define BINS_MAX 256

LAP_ICON_PX("....................",
            "....................",
            "..e.................",
            "..e.......b.........",
            "..e......bbb........",
            "..e......b.b........",
            "..e.....bb.bb...b...",
            "..e....b.....b.bb...",
            "..e.bbb.......bb.b..",
            "..ebb.............bb",
            "..eeeeeeeeeeeeeeeeee",
            "....................",
            "..1199e9e1199e911999",
            "..19e9eee9e1999e1191",
            "..9eeeeeeeee9e9e9e1e",
            "..1999e999e1191e9119",
            "....................",
            "....................");

static struct {
    lap_radio_info_t radios[LAP_RADIOS_MAX];
    int nradios, radio, h;
    uint64_t center;
    int gain;                        /* tenths of a dB, or LAP_GAIN_AUTO */
    bool hold;
    int step;                        /* STEP choice */
    int range_db;                    /* the trace's height in dB */
    uint32_t seq;
    float floor_db;
    char why[48];
    lap_rx_config_t cfg;
    lap_rx_status_t st;
    float *line, *peak;              /* BINS_MAX each */
    lap_wf_line_t wf;
    int bins;
    float peak_db;
    uint64_t peak_hz;
    char key_text[3][16];            /* values under RADIO, TUNE, HOLD */
    lap_btn_t keys[6];
    /* the last layout, for taps */
    int sx, sy, sw, sh, wy, wh, ky, kh;
} s;

static void fmt_hz(char *out, size_t cap, uint64_t hz)
{
    const uint32_t mhz = (uint32_t)(hz / 1000000u), khz = (uint32_t)(hz / 1000u % 1000u), rest = (uint32_t)(hz % 1000u);
    snprintf(out, cap, "%lu.%03lu.%03lu", (unsigned long)mhz, (unsigned long)khz, (unsigned long)rest);
}
static uint32_t rate(void) { return s.st.rate_hz ? s.st.rate_hz : s.cfg.rate_hz; }

/* ---- the radio ------------------------------------------------------- */

static void close_rx(void)
{
    if (s.h > 0) lap_rx_close(s.h);
    s.h = 0;
    s.seq = 0;
}
static bool usable(const lap_radio_info_t *r) { return r->present && !r->busy && (r->features & LAP_RXF_SPECTRUM); }
static void clear_peaks(void) { for (int i = 0; s.peak && i < BINS_MAX; i++) s.peak[i] = -200.0f; }

static void open_rx(void)
{
    close_rx();
    if (s.radio < 0 || s.radio >= s.nradios) return;
    const lap_radio_info_t *r = &s.radios[s.radio];
    if (!usable(r)) {
        snprintf(s.why, sizeof(s.why), "%s: %s", r->name, r->why[0] ? r->why : "no spectrum");
        return;
    }
    if (s.center < r->min_hz || s.center > r->max_hz)
        s.center = 433920000u >= r->min_hz && 433920000u <= r->max_hz ? 433920000u : (r->min_hz + r->max_hz) / 2;
    uint32_t want = 1024000;
    if (want > r->max_rate) want = r->max_rate;
    if (want < r->min_rate) want = r->min_rate;
    s.cfg = (lap_rx_config_t){ s.center, want, s.gain, LAP_RX_SPECTRUM_ONLY };
    s.h = lap_rx_open(s.radio, &s.cfg);
    if (s.h < 0) snprintf(s.why, sizeof(s.why), "%s would not open (%d)", r->name, s.h);
    else s.why[0] = 0;
    clear_peaks();
}

static void tune_to(uint64_t hz)
{
    if (s.h <= 0) { s.center = hz; return; }
    const lap_radio_info_t *r = &s.radios[s.radio];
    if (hz < r->min_hz) hz = r->min_hz;
    if (hz > r->max_hz) hz = r->max_hz;
    if (lap_rx_tune(s.h, hz) == 0) {
        s.center = hz;
        clear_peaks();
    }
}
static void tune(int64_t delta) { tune_to((uint64_t)((int64_t)s.center + delta)); }
static int64_t step_hz(void)
{
    static const int64_t STEP[] = { 0, 100000, 25000, 12500 };
    return s.step ? STEP[s.step & 3] : (int64_t)rate() / 4;
}

static void set_gain(int g)
{
    if (s.h <= 0 || !(s.radios[s.radio].features & LAP_RXF_GAIN)) return;
    const lap_radio_info_t *r = &s.radios[s.radio];
    if (g != LAP_GAIN_AUTO) {
        if (g < r->gain_min) g = r->gain_min;
        if (g > r->gain_max) g = r->gain_max;
    }
    if (lap_rx_gain(s.h, g) == 0) s.gain = g;
}

/* ---- the RADIO list and the TUNE keypad: the board's own -------------- */

static void radio_picked(int index)
{
    s.radio = index;
    open_rx();
}
static void choose_radio(void)
{
    s.nradios = lap_radio_list(s.radios, LAP_RADIOS_MAX);
    if (s.nradios <= 0 || !lap_picker_open("RADIO", radio_picked)) return;
    for (int i = 0; i < s.nradios; i++) {
        const lap_radio_info_t *r = &s.radios[i];
        static char detail[40];               /* app memory: the list copies it */
        if (usable(r))
            snprintf(detail, sizeof(detail), "%lu-%lu MHz", (unsigned long)(r->min_hz / 1000000u),
                     (unsigned long)(r->max_hz / 1000000u));
        else
            snprintf(detail, sizeof(detail), "%s", r->why[0] ? r->why : "no spectrum");
        lap_picker_add(r->name, detail);
    }
    if (s.radio >= 0) lap_picker_select(s.radio);
}
static void typed(double mhz) { tune_to((uint64_t)(mhz * 1000000.0 + 0.5)); }
static void type_frequency(void) { lap_numpad_open("TUNE", "MHz", (double)(s.center / 1000u) / 1000.0, typed); }

/* ---- OPTIONS: one tree, a level below for the trace -------------------- */

enum { OPT_AUTO, OPT_GAIN, OPT_STEP, OPT_FREQ, OPT_HOLD, OPT_RANGE };
static const char *const STEPS[] = { "1/4 SPAN", "100 kHz", "25 kHz", "12.5 kHz" };
static double opt_get(int id)
{
    switch (id) {
    case OPT_AUTO: return s.gain == LAP_GAIN_AUTO;
    case OPT_GAIN: return s.gain == LAP_GAIN_AUTO ? 0 : s.gain / 10.0;
    case OPT_STEP: return s.step;
    case OPT_FREQ: return (double)(s.center / 1000u) / 1000.0;
    case OPT_HOLD: return s.hold;
    default: return s.range_db;
    }
}
static void opt_set(int id, double v)
{
    const lap_radio_info_t *r = s.radio >= 0 ? &s.radios[s.radio] : NULL;
    switch (id) {
    case OPT_AUTO: set_gain(v > 0.5 ? LAP_GAIN_AUTO : r ? (r->gain_min + r->gain_max) / 2 : 0); break;
    case OPT_GAIN: set_gain((int)(v * 10.0 + (v < 0 ? -0.5 : 0.5))); break;
    case OPT_STEP: s.step = (int)v & 3; break;
    case OPT_FREQ: typed(v); break;
    case OPT_HOLD: s.hold = v > 0.5; if (!s.hold) clear_peaks(); break;
    default: s.range_db = (int)v; break;
    }
}
static const lap_opt_t TRACE_ROWS[] = {
    { .label = "PEAK HOLD", .kind = LAP_OPT_TOGGLE, .id = OPT_HOLD },
    { .label = "RANGE", .kind = LAP_OPT_LEVEL, .id = OPT_RANGE, .lo = 40, .hi = 100, .step = 10, .unit = "dB" },
};
static const lap_opt_list_t TRACE = { "TRACE", TRACE_ROWS, 2, NULL, NULL };
static lap_opt_t rows[6];
static lap_opt_list_t options;
static void open_options(void)
{
    const lap_radio_info_t *r = s.radio >= 0 && s.radio < s.nradios ? &s.radios[s.radio] : NULL;
    int n = 0;
    if (r && (r->features & LAP_RXF_GAIN)) {
        rows[n++] = (lap_opt_t){ .label = "AUTO GAIN", .kind = LAP_OPT_TOGGLE, .id = OPT_AUTO };
        rows[n++] = (lap_opt_t){ .label = "GAIN", .kind = LAP_OPT_LEVEL, .id = OPT_GAIN, .lo = r->gain_min / 10.0,
                                 .hi = r->gain_max / 10.0, .step = 3, .unit = "dB" };
    }
    rows[n++] = (lap_opt_t){ .label = "STEP", .kind = LAP_OPT_CYCLE, .id = OPT_STEP, .names = STEPS, .n = 4 };
    rows[n++] = (lap_opt_t){ .label = "FREQUENCY", .kind = LAP_OPT_NUMBER, .id = OPT_FREQ,
                             .lo = r ? r->min_hz / 1e6 : 1, .hi = r ? r->max_hz / 1e6 : 6000, .unit = "MHz" };
    rows[n++] = (lap_opt_t){ .label = "TRACE", .kind = LAP_OPT_MENU, .sub = &TRACE };
    rows[n++] = (lap_opt_t){ .label = "DISPLAY", .kind = LAP_OPT_DISPLAY };
    options = (lap_opt_list_t){ "SPECTRE", rows, n, opt_get, opt_set };
    lap_options_open(&options);
}

/* ---- a new line ------------------------------------------------------- */

static void take_line(void)
{
    float sum = 0, mx = -200.0f;
    int at = 0;
    for (int i = 0; i < s.bins; i++) {
        const float v = s.line[i];
        sum += v;
        if (v > mx) { mx = v; at = i; }
        if (v > s.peak[i]) s.peak[i] = v;
    }
    /* The mean in dB sits on the noise: a few carriers barely move it. */
    const float mean = sum / (float)s.bins;
    s.floor_db = s.seq <= 1 ? mean : s.floor_db * 0.9f + mean * 0.1f;
    s.peak_db = mx;
    s.peak_hz = s.center - rate() / 2 + (uint64_t)rate() * (uint64_t)(2 * at + 1) / (uint64_t)(2 * s.bins);
    /* The board's waterfall takes the same line, scaled the way the trace is. */
    s.wf = (lap_wf_line_t){ s.center, rate(), s.floor_db - 4.0f, s.floor_db + (float)s.range_db * 0.6f, false };
    lap_wf_push(s.line, s.bins, &s.wf);
}

/* ---- the screen ------------------------------------------------------- */

static void layout(void)
{
    const int W = lap_cols(), H = lap_rows();
    s.kh = lap_ui_keys_rows(6);
    s.ky = H - s.kh;
    s.sx = 5; s.sw = W - s.sx - 1; s.sy = 4;
    const int body = s.ky - 1 - s.sy - 1;        /* the axis row, the status row */
    s.sh = body / 2 < 4 ? 4 : body / 2;
    s.wy = s.sy + s.sh + 1;
    s.wh = body - s.sh;
    if (s.wh < 3) s.wh = 3;
}

static int level_dot(float db, int dots)
{
    float t = (db - (s.floor_db - 5.0f)) / (float)s.range_db;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return dots - 1 - (int)(t * (float)(dots - 1) + 0.5f);   /* 0 is the top dot row */
}

/* SPECTRE's face: the trace as a braille line joined column to column, a
   dotted floor under it and the held peaks above it in amber. */
static void draw_trace(void)
{
    const int dots_h = s.sh * 4;
    static const uint8_t BIT[2][4] = { { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 } };
    int prev = -1;
    for (int cx = 0; cx < s.sw; cx++) {
        int top[2], hold[2], from[2];
        for (int k = 0; k < 2; k++) {
            const int i = (cx * 2 + k) * s.bins / (s.sw * 2);
            top[k] = s.seq ? level_dot(s.line[i], dots_h) : dots_h;
            hold[k] = s.hold && s.seq ? level_dot(s.peak[i], dots_h) : dots_h;
            from[k] = prev < 0 ? top[k] : prev;
            prev = top[k];
        }
        for (int cy = 0; cy < s.sh; cy++) {
            uint8_t trace = 0, fill = 0, held = 0;
            for (int k = 0; k < 2; k++)
                for (int r = 0; r < 4; r++) {
                    const int d = cy * 4 + r;
                    const int lo = from[k] < top[k] ? from[k] : top[k], hi = from[k] > top[k] ? from[k] : top[k];
                    if (d >= lo && d <= hi + 1) trace |= BIT[k][r];
                    else if (d > hi + 1 && !(d & 3) && ((d / 4 + cx + k) & 1)) fill |= BIT[k][r];
                    if (d == hold[k] && d < lo) held |= BIT[k][r];
                }
            const uint8_t dots = trace | fill | held;
            const uint8_t col = trace ? LAP_CYAN | LAP_BRIGHT : held ? LAP_YELLOW | LAP_BRIGHT : LAP_BLUE;
            lap_put_cell(s.sx + cx, s.sy + cy, dots ? LAP_BRAILLE(dots) : ' ', A(col));
        }
    }
    for (int q = 0; q <= 4; q++) {           /* the dB scale, ticks every quarter */
        const int row = s.sy + (s.sh - 1) * q / 4;
        char t[8];
        snprintf(t, sizeof(t), "%4d", (int)(s.floor_db - 5.0f + (float)s.range_db * (float)(4 - q) / 4.0f));
        lap_put_str(0, row, t, LAP_UI_FAINT);
    }
    lap_vline(4, s.sy, s.sh, '|', A(LAP_BLUE));
    for (int q = 0; q <= 4; q++) lap_put_cell(4, s.sy + (s.sh - 1) * q / 4, '+', A(LAP_BLUE | LAP_BRIGHT));
    const int y = s.sy + s.sh;               /* the span along the bottom */
    lap_hline(s.sx, y, s.sw, '-', A(LAP_BLUE));
    for (int q = 0; q <= 4; q++) lap_put_cell(s.sx + (s.sw - 1) * q / 4, y, '+', A(LAP_BLUE | LAP_BRIGHT));
    char t[24];
    snprintf(t, sizeof(t), "-%luk", (unsigned long)(rate() / 2000u));
    lap_put_str(s.sx, y, t, A(LAP_CYAN));
    snprintf(t, sizeof(t), "+%luk", (unsigned long)(rate() / 2000u));
    lap_put_str(s.sx + s.sw - (int)strlen(t), y, t, A(LAP_CYAN));
    lap_put_cell(s.sx + s.sw / 2, y, '^', A(LAP_YELLOW | LAP_BRIGHT));
}

static void draw_header(void)
{
    const int W = lap_cols();
    lap_put_str(0, 0, " /\\  ", A(LAP_CYAN | LAP_BRIGHT));
    lap_put_str(0, 1, "/  \\/", A(LAP_CYAN));
    lap_put_str(6, 0, "SPECTRE", A(LAP_MAGENTA | LAP_BRIGHT));
    char hz[24];
    fmt_hz(hz, sizeof(hz), s.center);
    lap_put_str(6, 1, hz, LAP_UI_TEXT);
    lap_put_str(6 + (int)strlen(hz) + 1, 1, "MHz", LAP_UI_DIM);
    /* The annunciators: the board's chips, lit by what is true now. */
    const bool live = s.h > 0 && s.st.state == LAP_RX_RUNNING && s.seq;
    static char gain[12], span[12];          /* app memory, not the stack: the board reads them */
    if (s.gain == LAP_GAIN_AUTO) snprintf(gain, sizeof(gain), "AGC");
    else snprintf(gain, sizeof(gain), "G%d", s.gain / 10);
    snprintf(span, sizeof(span), "%lu.%luM", (unsigned long)(rate() / 1000000u), (unsigned long)(rate() / 100000u % 10u));
    static lap_chip_t left[3], right[2];
    const lap_chip_t l[] = {
        { live ? "LIVE" : s.h > 0 ? "OPEN" : "IDLE", live, live ? LAP_UI_LIVE : LAP_UI_WARN },
        { gain, s.h > 0, LAP_CYAN },
        { "HOLD", s.hold, LAP_UI_VALUE },
    };
    const lap_radio_info_t *r = s.radio >= 0 && s.radio < s.nradios ? &s.radios[s.radio] : NULL;
    const lap_chip_t rr[] = { { span, s.h > 0, LAP_MAGENTA }, { r ? r->name : "NO RADIO", r != NULL, LAP_MAGENTA } };
    memcpy(left, l, sizeof(left));
    memcpy(right, rr, sizeof(right));
    lap_ui_rail(0, 2, W, left, 3, right, 2);
}

static void draw_status(void)
{
    const int y = s.ky - 1;
    char t[96], hz[24];
    if (s.h <= 0 || s.st.state < 0) {
        snprintf(t, sizeof(t), " %s", s.why[0] ? s.why : "tap RADIO to choose a radio");
        lap_put_str(0, y, t, A(LAP_RED | LAP_BRIGHT));
        return;
    }
    if (s.st.state == LAP_RX_OPENING || !s.seq) {
        lap_put_str(1, y, "opening the radio...", A(LAP_CYAN));
        return;
    }
    fmt_hz(hz, sizeof(hz), s.peak_hz);
    snprintf(t, sizeof(t), " PEAK %s %4d dB ", hz, (int)s.peak_db);
    lap_put_str(0, y, t, A(LAP_YELLOW | LAP_BRIGHT));
    /* How far the peak stands over the floor, on the board's ASCII meter. */
    const int x = (int)strlen(t);
    if (lap_cols() - x >= 8) lap_ui_meter(x, y, lap_cols() - x - 1, (s.peak_db - s.floor_db) / 60.0f, LAP_GREEN | LAP_BRIGHT);
}

static void draw_keys(void)
{
    const lap_radio_info_t *r = s.radio >= 0 && s.radio < s.nradios ? &s.radios[s.radio] : NULL;
    snprintf(s.key_text[0], sizeof(s.key_text[0]), "%s", r ? r->name : "none");
    snprintf(s.key_text[1], sizeof(s.key_text[1]), "%lu.%03lu", (unsigned long)(s.center / 1000000u),
             (unsigned long)(s.center / 1000u % 1000u));
    snprintf(s.key_text[2], sizeof(s.key_text[2]), "%s", s.hold ? "on" : "off");
    s.keys[0] = (lap_btn_t){ "RADIO", s.key_text[0], 'r', false, false, false };
    s.keys[1] = (lap_btn_t){ "<<", NULL, ',', false, s.h <= 0, false };
    s.keys[2] = (lap_btn_t){ ">>", NULL, '.', false, s.h <= 0, false };
    s.keys[3] = (lap_btn_t){ "TUNE", s.key_text[1], 't', false, false, false };
    s.keys[4] = (lap_btn_t){ "HOLD", s.key_text[2], 'h', s.hold, false, false };
    s.keys[5] = (lap_btn_t){ "OPTIONS", NULL, 'o', false, false, false };
    lap_ui_keys(0, s.ky, lap_cols(), s.kh, s.keys, 6, -1);
}

static void draw_empty(void)
{
    static const char *const ART[] = {
        "    \\ | /    ",
        "  -- (o) --  ",
        "    / | \\    ",
        "      |      ",
        "     /_\\     ",
    };
    const int x = (lap_cols() - 13) / 2, y = s.sy + 1;
    for (int i = 0; i < 5; i++) lap_put_str(x, y + i, ART[i], A(i == 1 ? LAP_YELLOW | LAP_BRIGHT : LAP_BLUE | LAP_BRIGHT));
    const char *m = "NO RADIO WITH A SPECTRUM";
    lap_put_str((lap_cols() - (int)strlen(m)) / 2, y + 6, m, LAP_UI_TEXT);
    const char *n = "Plug in an RTL-SDR or HackRF, or use the LR2021.";
    lap_put_str((lap_cols() - (int)strlen(n)) / 2 > 0 ? (lap_cols() - (int)strlen(n)) / 2 : 0, y + 8, n, A(LAP_CYAN));
}

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    layout();
    if (!s.line || !s.peak) return;
    if (!s.nradios || !(lap_frame() % 25)) {
        const int n = lap_radio_list(s.radios, LAP_RADIOS_MAX);
        if (n >= 0) s.nradios = n;
        if (s.nradios && s.radio < 0) {
            for (int i = 0; i < s.nradios && s.radio < 0; i++) if (usable(&s.radios[i])) s.radio = i;
            if (s.radio >= 0) open_rx();
        }
    }
    if (s.h > 0) {
        lap_rx_status(s.h, &s.st);
        if (s.st.state < 0) snprintf(s.why, sizeof(s.why), "the radio stopped (%d): tap RADIO", s.st.state);
        s.bins = s.sw * 2 > BINS_MAX ? BINS_MAX : s.sw * 2;
        uint32_t seq = 0;
        if (s.st.state == LAP_RX_RUNNING && lap_rx_spectrum(s.h, s.line, s.bins, &seq) == s.bins && seq != s.seq) {
            s.seq = seq;
            take_line();
        }
    }
    draw_header();
    if (!s.nradios) draw_empty();
    else {
        draw_trace();
        lap_wf_draw(s.sx, s.wy, s.sw, s.wh, LAP_WF_MINI);
        lap_vline(4, s.wy, s.wh, ':', A(LAP_BLUE));
    }
    draw_status();
    draw_keys();
}

static bool key(lap_key_t k, char ch)
{
    if (k == LAP_KEY_LEFT || (k == LAP_KEY_CHAR && ch == ',')) { tune(-step_hz()); return true; }
    if (k == LAP_KEY_RIGHT || (k == LAP_KEY_CHAR && ch == '.')) { tune(step_hz()); return true; }
    if (k == LAP_KEY_UP) { set_gain(s.gain == LAP_GAIN_AUTO ? 0 : s.gain + 30); return true; }
    if (k == LAP_KEY_DOWN) { set_gain(s.gain == LAP_GAIN_AUTO ? 0 : s.gain - 30); return true; }
    if (k != LAP_KEY_CHAR) return false;
    switch (ch) {
    case 'r': choose_radio(); return true;
    case 't': type_frequency(); return true;
    case 'h': opt_set(OPT_HOLD, !s.hold); return true;
    case 'o': open_options(); return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    const int k = lap_ui_key_hit(col, row);
    if (k >= 0) return key(LAP_KEY_CHAR, s.keys[k].key);
    if (row >= s.ky) return true;                   /* a dim key: nothing to do now */
    /* On the trace or the waterfall: centre what was tapped. */
    if (col >= s.sx && col < s.sx + s.sw && row >= s.sy && row < s.wy + s.wh && s.h > 0) {
        tune(((int64_t)(col - s.sx) * 2 + 1) * (int64_t)rate() / (2 * s.sw) - (int64_t)rate() / 2);
        return true;
    }
    return false;
}

static void enter(void)
{
    memset(&s, 0, sizeof(s));
    s.radio = -1;
    s.gain = LAP_GAIN_AUTO;
    s.center = 433920000u;
    s.hold = true;
    s.range_db = 70;
    s.line = calloc(BINS_MAX, sizeof(float));
    s.peak = calloc(BINS_MAX, sizeof(float));
    clear_peaks();
}

static void leave(void)
{
    close_rx();                      /* the board would do it anyway */
    free(s.line); free(s.peak);
    memset(&s, 0, sizeof(s));
}

LAP_APP(.id = "spectre", .name = "SPECTRE", .sub = "spectrum on any radio", .version = "1.1",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_MAGENTA, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "  /\\      ", " /  \\  /\\ ", "/    \\/  \\", "----------", " SPECTRE  ", "          " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
