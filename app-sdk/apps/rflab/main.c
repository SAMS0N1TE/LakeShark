/* RF LAB: a bench instrument on whatever radio is to hand.
 *
 * The screen answers "what is on this frequency, and how good is my receiver
 * at it" in one look: the spectrum as a braille trace with its floor, peak,
 * channel and 99% bandwidth marked on it, the SNR in big figures, and the
 * readings an analyser's marker table would give - with an I/Q ellipse drawn
 * from the measured gain and phase error. Everything moves only when a new
 * reading lands (about four a second); a held or idle screen is still.
 *
 * It opens the radio picked under SOURCE (an RTL-SDR, a HackRF or the
 * LR2021) itself, tunes it to TUNE, collects 16384 unbroken pairs, measures
 * them (rflab_meas.h) and starts again. HOLD keeps the last reading and gives
 * the radio back. Channel width, offset, the expected carrier, the DC guard
 * and the gain live in OPTIONS. A clean carrier says how far the dongle's
 * reference is out, as the PPM to add in its FRONT END.
 *
 * Keys: r source, t tune, c channel, h hold, o options. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "rflab_meas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A(fg) LAP_ATTR((fg), LAP_BLACK)

/* A bench screen: a spectrum with a carrier standing out, a floor line,
   and the marker over the peak. */
LAP_ICON_PX("....................",
            ".bbbbbbbbbbbbbbbbbb.",
            ".b................b.",
            ".b.........b......b.",
            ".b........bbb.....b.",
            ".b........b.b.....b.",
            ".b.......b...b....b.",
            ".b......3.....3...b.",
            ".b.....3.......3..b.",
            ".b.333.........33.b.",
            ".b3..............3b.",
            ".b4444444444444444b.",
            ".b................b.",
            ".bbbbbbbbbbbbbbbbbb.",
            "....................",
            "...bb..bb..bb..bb...",
            "...bb..bb..bb..bb...",
            "....................");
#define HI(fg) A((fg) | LAP_BRIGHT)
#define CAP_PAIRS   16384u      /* one reading: 32 FFTs                      */
#define READ_PAIRS  2048        /* the SDK's block                           */
#define SPEC_COLS   128         /* the spectrum kept for drawing             */
#define HIST        96          /* readings of SNR history                   */
#define RATE_SDR    1024000u    /* what an SDR is opened at                  */
#define READING_MS  250u        /* at most four readings a second            */

static struct {
    lap_radio_info_t radios[LAP_RADIOS_MAX];
    int nradios;
    int src;                    /* the picked radio, -1 for the first usable */
    int radio, h;               /* the one open, and its session             */
    uint64_t hz;
    uint32_t chan_bw;
    int32_t chan_off, expect_off;
    int dc_guard;
    int gain_tenths;            /* 0 automatic                               */
    bool hold;
    /* capture */
    int16_t *cap, *scratch;
    uint32_t fill, next_ms;
    uint64_t cap_center;
    uint32_t cap_rate;
    /* Handed to the SDK, so in the app's memory, not on the stack: the board
       checks every pointer an app passes belongs to the app. */
    lap_rx_config_t cfg;
    lap_iq_meta_t meta;
    lap_rx_status_t st;
    /* the latest reading */
    rflab_t *lab;
    rflab_result_t r;
    uint64_t r_center;
    uint32_t r_rate, seq;
    bool have, r_sdr;
    float spec[SPEC_COLS];
    float hist[HIST];
    int hist_n;
    char why[64];
    char key_text[3][16];
    lap_btn_t keys[5];
} s;

static int iround(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ---- the radio ------------------------------------------------------- */

static bool usable(const lap_radio_info_t *r) { return r->present && !r->busy && (r->features & LAP_RXF_IQ); }
static bool is_sdr(const lap_radio_info_t *r)
{
    return r->kind == LAP_RADIO_KIND_RTL || r->kind == LAP_RADIO_KIND_HACKRF;
}
/* The picked radio, or for ANY the first usable one, SDRs before the LR2021. */
static int choose(void)
{
    if (s.src >= 0) return s.src < s.nradios ? s.src : -1;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < s.nradios; ++i)
            if (usable(&s.radios[i]) && is_sdr(&s.radios[i]) == (pass == 0)) return i;
    return -1;
}

static void close_rx(void)
{
    if (s.h > 0) lap_rx_close(s.h);
    s.h = 0;
    s.radio = -1;
    s.fill = 0;
}

static void open_rx(void)
{
    close_rx();
    const int i = choose();
    if (i < 0) {
        snprintf(s.why, sizeof(s.why), s.nradios ? "no radio free for I/Q" : "looking for radios");
        return;
    }
    const lap_radio_info_t *r = &s.radios[i];
    if (!usable(r)) {
        snprintf(s.why, sizeof(s.why), "%s: %s", r->name, r->why[0] ? r->why : "busy");
        return;
    }
    if (s.hz < r->min_hz || s.hz > r->max_hz) {
        snprintf(s.why, sizeof(s.why), "%s tunes %lu-%lu MHz", r->name,
                 (unsigned long)(r->min_hz / 1000000u), (unsigned long)(r->max_hz / 1000000u));
        return;
    }
    uint32_t rate = r->min_rate;
    if (r->features & LAP_RXF_RATE) {
        rate = RATE_SDR;
        if (rate > r->max_rate) rate = r->max_rate;
        if (rate < r->min_rate) rate = r->min_rate;
    }
    s.cfg = (lap_rx_config_t){ s.hz, rate, s.gain_tenths ? s.gain_tenths : LAP_GAIN_AUTO, 0 };
    s.h = lap_rx_open(i, &s.cfg);
    if (s.h < 0) {
        snprintf(s.why, sizeof(s.why), "%s would not open (%d)", r->name, s.h);
        s.h = 0;
        return;
    }
    s.radio = i;
    s.why[0] = 0;
    s.next_ms = 0;
}

/* Collect unbroken pairs; measure when there are enough; drain the rest. */
static void pump(void)
{
    if (s.h <= 0 || lap_rx_status(s.h, &s.st) != 0 || s.st.state != LAP_RX_RUNNING) return;
    for (int n = 0; n < 64; ++n) {
        const bool want = s.fill < CAP_PAIRS && lap_millis() >= s.next_ms;
        int16_t *to = want ? s.cap + 2 * s.fill : s.scratch;
        int room = want ? (int)(CAP_PAIRS - s.fill) : READ_PAIRS;
        if (room > READ_PAIRS) room = READ_PAIRS;
        const int got = lap_rx_read(s.h, to, room, &s.meta);
        if (got <= 0) break;
        if (!want) continue;
        if ((s.meta.flags & (LAP_IQ_GAP | LAP_IQ_RETUNED)) || !s.fill ||
            s.meta.center_hz != s.cap_center || s.meta.rate_hz != s.cap_rate) {
            /* A break: this block starts the capture again. */
            if (s.fill) memmove(s.cap, to, (size_t)got * 4);
            s.fill = 0;
            s.cap_center = s.meta.center_hz;
            s.cap_rate = s.meta.rate_hz;
        }
        s.fill += (uint32_t)got;
        if (s.fill >= CAP_PAIRS) break;
    }
    if (s.fill < CAP_PAIRS) return;
    const rflab_cfg_t cfg = {
        .rate_hz = s.cap_rate, .center_hz = s.cap_center, .chan_bw_hz = s.chan_bw,
        .chan_offset_hz = s.chan_off, .expect_offset_hz = s.expect_off,
        .dc_guard_bins = (uint8_t)s.dc_guard,
    };
    if (rflab_measure(s.lab, &cfg, s.cap, CAP_PAIRS, &s.r)) {
        rflab_spectrum_db(s.lab, s.spec, SPEC_COLS);
        s.r_center = s.cap_center;
        s.r_rate = s.cap_rate;
        s.r_sdr = s.radio >= 0 && is_sdr(&s.radios[s.radio]);
        s.have = true;
        s.seq++;
        memmove(s.hist, s.hist + 1, (HIST - 1) * sizeof(float));
        s.hist[HIST - 1] = s.r.snr_db;
        if (s.hist_n < HIST) s.hist_n++;
    }
    s.fill = 0;
    s.next_ms = lap_millis() + READING_MS;
}

/* ---- SOURCE, TUNE, CHANNEL --------------------------------------------- */

static void source_picked(int index)
{
    s.src = index - 1;          /* row 0 is ANY */
    if (s.src >= 0 && s.src < s.nradios && s.radios[s.src].kind == LAP_RADIO_KIND_HACKRF && !s.dc_guard)
        s.dc_guard = 2;         /* the HackRF's DC spike sits on 0 Hz */
    if (!s.hold) open_rx();
}
static void choose_source(void)
{
    s.nradios = lap_radio_list(s.radios, LAP_RADIOS_MAX);
    if (s.nradios < 0) s.nradios = 0;
    if (!lap_picker_open("RF LAB SOURCE", source_picked)) return;
    lap_picker_add("ANY", "the first free: an SDR, else the LR2021");
    for (int i = 0; i < s.nradios; i++) {
        const lap_radio_info_t *r = &s.radios[i];
        static char detail[48];               /* app memory: the list copies it */
        if (usable(r))
            snprintf(detail, sizeof(detail), "%lu-%lu MHz, %.3f MS/s", (unsigned long)(r->min_hz / 1000000u),
                     (unsigned long)(r->max_hz / 1000000u),
                     ((r->features & LAP_RXF_RATE) && r->max_rate >= RATE_SDR ? RATE_SDR : r->min_rate) / 1e6);
        else
            snprintf(detail, sizeof(detail), "%s", r->why[0] ? r->why : !(r->features & LAP_RXF_IQ) ? "no I/Q" : "busy");
        lap_picker_add(r->name, detail);
    }
    lap_picker_select(s.src + 1);
}
static void tuned(double mhz)
{
    if (mhz < 1 || mhz > 6000) return;
    s.hz = (uint64_t)(mhz * 1e6 + 0.5);
    if (s.h > 0 && lap_rx_tune(s.h, s.hz) == 0) s.fill = 0;
    else if (!s.hold) open_rx();
}
static void chan_set(double khz)
{
    if (khz < 0.1 || khz > 2000) return;
    s.chan_bw = (uint32_t)(khz * 1e3 + 0.5);
}

/* ---- OPTIONS ----------------------------------------------------------- */

enum { O_CHAN, O_OFF, O_EXPECT, O_GUARD, O_GAIN };
static double opt_get(int id)
{
    switch (id) {
    case O_CHAN: return s.chan_bw / 1e3;
    case O_OFF: return s.chan_off / 1e3;
    case O_EXPECT: return s.expect_off / 1e3;
    case O_GUARD: return s.dc_guard;
    default: return s.gain_tenths / 10.0;
    }
}
static void opt_set(int id, double v)
{
    switch (id) {
    case O_CHAN: chan_set(v); break;
    case O_OFF: s.chan_off = (int32_t)(v * 1e3 + (v < 0 ? -0.5 : 0.5)); break;
    case O_EXPECT: s.expect_off = (int32_t)(v * 1e3 + (v < 0 ? -0.5 : 0.5)); break;
    case O_GUARD: s.dc_guard = v < 0 ? 0 : v > 8 ? 8 : (int)v; break;
    default:
        s.gain_tenths = (int)(v * 10 + 0.5);
        if (s.h > 0) lap_rx_gain(s.h, s.gain_tenths ? s.gain_tenths : LAP_GAIN_AUTO);
        break;
    }
}
static const lap_opt_t OPT_ROWS[] = {
    { .label = "CHANNEL WIDTH", .kind = LAP_OPT_NUMBER, .id = O_CHAN, .lo = 0.1, .hi = 2000,
      .unit = "kHz: channel power and SNR" },
    { .label = "CHANNEL OFFSET", .kind = LAP_OPT_NUMBER, .id = O_OFF, .lo = -1000, .hi = 1000,
      .unit = "kHz from the tuning" },
    { .label = "EXPECTED CARRIER", .kind = LAP_OPT_NUMBER, .id = O_EXPECT, .lo = -1000, .hi = 1000,
      .unit = "kHz from the tuning: frequency error" },
    { .label = "DC GUARD", .kind = LAP_OPT_LEVEL, .id = O_GUARD, .lo = 0, .hi = 8, .step = 1,
      .unit = "bins each side of 0 Hz the peak skips" },
    { .label = "GAIN", .kind = LAP_OPT_LEVEL, .id = O_GAIN, .lo = 0, .hi = 49.6, .step = 2,
      .unit = "dB; 0 automatic" },
};
static const lap_opt_list_t OPTIONS = { "RF LAB", OPT_ROWS, 5, opt_get, opt_set };

/* ---- drawing ---------------------------------------------------------- */

/* Braille dots of one cell column, counted from the bottom. */
static const uint8_t COL_BITS[2][4] = { { 0x40, 0x04, 0x02, 0x01 }, { 0x80, 0x20, 0x10, 0x08 } };
static uint8_t column_bits(int n, int c)
{
    uint8_t b = 0;
    for (int k = 0; k < n && k < 4; ++k) b |= COL_BITS[c][k];
    return b;
}
static uint8_t level_hue(float frac)
{
    return frac < 0.34f ? LAP_BLUE | LAP_BRIGHT : frac < 0.67f ? LAP_GREEN | LAP_BRIGHT : LAP_YELLOW | LAP_BRIGHT;
}
static int col_of(float hz, int px, int pw, float rate)
{
    const int x = px + (int)floorf((hz / rate + 0.5f) * (float)pw);
    return x < px ? px : x >= px + pw ? px + pw - 1 : x;
}

/* The trace: spectrum as filled braille columns over a dotted dB graticule,
   the floor ruled across in blue, the peak flagged, and under it the channel
   bracket and the 99% bandwidth span. */
static void draw_trace(int bx, int by, int bw, int bh)
{
    lap_ui_bezel(bx, by, bw, bh, " SPECTRUM / dBFS ");
    const int axis = 4, px = bx + 1 + axis, py = by + 1, pw = bw - 2 - axis, ph = bh - 5;
    if (pw < 8 || ph < 3) return;
    const rflab_result_t *r = &s.r;
    float top = -200;
    for (int i = 0; i < SPEC_COLS; ++i) if (s.spec[i] > top) top = s.spec[i];
    float hi = ceilf((top + 3) / 10) * 10, lo = floorf((r->noise_dbfs_bin - 8) / 10) * 10;
    if (hi > 0) hi = 0;
    if (hi - lo < 30) lo = hi - 30;
    const float span = hi - lo;
    char t[16];
    for (float g = hi; g >= lo; g -= 10) {                  /* graticule and scale */
        const int y = py + iround((hi - g) / span * (ph - 1));
        snprintf(t, sizeof(t), "%4.0f", g);
        lap_put_str(bx + 1, y, t, LAP_UI_FAINT);
        for (int x = px; x < px + pw; x += 3) lap_put_cell(x, y, '.', LAP_UI_FAINT);
    }
    const int fy = py + iround((hi - r->noise_dbfs_bin) / span * (ph - 1));   /* floor */
    if (fy >= py && fy < py + ph)
        for (int x = px; x < px + pw; x += 2) lap_put_cell(x, fy, '-', A(LAP_BLUE));

    /* Two dot columns a cell: the floor as a line of dots, what stands above
       it filled, the crest bright and the body under it dimmed. */
    const int dots_h = ph * 4, dots_w = pw * 2;
    const int floor_cell = (int)((r->noise_dbfs_bin + 3 - lo) / span * ph);
    for (int cx = 0; cx < pw; ++cx) {
        int h[2];
        for (int half = 0; half < 2; ++half) {
            const int d = cx * 2 + half;
            const int a = d * SPEC_COLS / dots_w, b = (d + 1) * SPEC_COLS / dots_w;
            float v = s.spec[a];
            for (int k = a + 1; k < b && k < SPEC_COLS; ++k) if (s.spec[k] > v) v = s.spec[k];
            const int hh = iround((v - lo) / span * dots_h);
            h[half] = hh < 0 ? 0 : hh > dots_h ? dots_h : hh;
        }
        for (int cy = 0; cy < ph; ++cy) {
            const int l0 = h[0] - cy * 4, l1 = h[1] - cy * 4;
            uint8_t bits = column_bits(l0, 0) | column_bits(l1, 1);
            if (cy < floor_cell)          /* in the floor: the top dot alone */
                bits = (l0 > 0 && l0 <= 4 ? COL_BITS[0][l0 - 1] : 0) | (l1 > 0 && l1 <= 4 ? COL_BITS[1][l1 - 1] : 0);
            if (!bits) continue;
            const bool crest = (l0 > 0 && l0 <= 4) || (l1 > 0 && l1 <= 4);
            if (!crest && cy < floor_cell) continue;
            const uint8_t hue = level_hue((float)cy / (float)ph);
            lap_put_cell(px + cx, py + ph - 1 - cy, LAP_BRAILLE(bits), A(crest ? hue : hue & 7));
        }
    }

    const float rate = (float)(s.r_rate ? s.r_rate : 1);
    if (r->carrier) {                                        /* peak flag */
        const int x = col_of(r->peak_offset_hz, px, pw, rate);
        int y = py + iround((hi - r->peak_dbfs) / span * (ph - 1)) - 1;
        if (y < py) y = py;
        lap_put_cell(x, y, 'v', HI(LAP_YELLOW));
        snprintf(t, sizeof(t), "%+.2fk", r->peak_offset_hz / 1e3);
        const int lx = x + 2 + (int)strlen(t) < px + pw ? x + 2 : x - 1 - (int)strlen(t);
        lap_put_str(lx, y, t, HI(LAP_YELLOW));
    }
    if (s.expect_off)                                        /* expected carrier */
        lap_put_cell(col_of((float)s.expect_off, px, pw, rate), py, ':', HI(LAP_MAGENTA));

    const int yc = py + ph, yo = yc + 1, ya = yo + 1;        /* channel and OBW */
    int xa = col_of((float)s.chan_off - s.chan_bw / 2.0f, px, pw, rate);
    int xb = col_of((float)s.chan_off + s.chan_bw / 2.0f, px, pw, rate);
    for (int x = xa + 1; x < xb; ++x) lap_put_cell(x, yc, '=', A(LAP_CYAN));
    lap_put_cell(xa, yc, '[', HI(LAP_CYAN));
    lap_put_cell(xb > xa ? xb : xa + 1, yc, ']', HI(LAP_CYAN));
    lap_put_str(bx + 1, yc, "CH", A(LAP_CYAN));
    xa = col_of(r->obw_lo_hz, px, pw, rate); xb = col_of(r->obw_hi_hz, px, pw, rate);
    for (int x = xa + 1; x < xb; ++x) lap_put_cell(x, yo, '-', A(LAP_MAGENTA));
    lap_put_cell(xa, yo, '<', HI(LAP_MAGENTA));
    lap_put_cell(xb > xa ? xb : xa + 1, yo, '>', HI(LAP_MAGENTA));
    lap_put_str(bx + 1, yo, "99", A(LAP_MAGENTA));
    snprintf(t, sizeof(t), "-%.0fk", rate / 2e3);           /* frequency axis */
    lap_put_str(px, ya, t, LAP_UI_DIM);
    lap_put_cell(px + pw / 2, ya, '0', LAP_UI_DIM);
    snprintf(t, sizeof(t), "+%.0fk", rate / 2e3);
    lap_put_str(px + pw - (int)strlen(t), ya, t, LAP_UI_DIM);
}

/* SNR in figures a room away can read; the decimal and unit small. Returns
   the width used. */
static int draw_snr(int x0, int y)
{
    const float snr = s.r.snr_db;
    const uint8_t hue = snr >= 20 ? LAP_GREEN : snr >= 10 ? LAP_YELLOW : LAP_RED;
    char t[12];
    snprintf(t, sizeof(t), "%d", (int)(snr < 0 ? 0 : snr > 99 ? 99 : snr));
    int x = x0;
    for (const char *c = t; *c; ++c, x += 4) lap_big_char(x, y, *c, HI(hue));
    snprintf(t, sizeof(t), ".%d dB", (int)(fabsf(snr) * 10) % 10);
    lap_put_str(x, y + 4, t, HI(hue));
    lap_put_str(x, y, "SNR", A(LAP_CYAN));
    snprintf(t, sizeof(t), "%.1fk", s.chan_bw / 1e3);
    lap_put_str(x, y + 1, t, LAP_UI_DIM);
    return x + 7 - x0;
}

/* The receiver's I/Q error as a shape: the ellipse its gain and phase
   imbalance make of a circle, magnified five times, over a faint true
   circle. Round is perfect. */
static void draw_ellipse(int ax, int ay, int aw, int ah)
{
    if (aw < 6 || ah < 3) return;
    const float g = powf(10.0f, -s.r.iq_gain_db / 20.0f);        /* Q over I */
    const float gv = 1.0f + (g - 1.0f) * 5.0f;
    float ph = s.r.iq_phase_deg * 5.0f * 3.14159265f / 180.0f;
    ph = clampf(ph, -1.2f, 1.2f);
    enum { MAXW = 16, MAXH = 8 };
    const int w = aw < MAXW ? aw : MAXW, h = ah < MAXH ? ah : MAXH;
    uint8_t ring[MAXH][MAXW], shape[MAXH][MAXW];
    memset(ring, 0, sizeof(ring));
    memset(shape, 0, sizeof(shape));
    static const uint8_t BIT[4][2] = { {0, 3}, {1, 4}, {2, 5}, {6, 7} };
    const float rx = w - 0.6f, ry = h * 2 - 0.6f, scale = 1.0f / (gv > 1 ? gv : 1);
    for (int k = 0; k < 96; ++k) {
        const float t = k * 6.2831853f / 96;
        const float x0 = cosf(t), y0 = sinf(t);
        const float x1 = cosf(t) * scale, y1 = gv * sinf(t + ph) * scale;
        int dx = iround(rx + x0 * rx * 0.9f), dy = iround(ry - y0 * ry * 0.9f);
        if (dx >= 0 && dx < w * 2 && dy >= 0 && dy < h * 4) ring[dy / 4][dx / 2] |= 1u << BIT[dy % 4][dx % 2];
        dx = iround(rx + x1 * rx * 0.9f); dy = iround(ry - y1 * ry * 0.9f);
        if (dx >= 0 && dx < w * 2 && dy >= 0 && dy < h * 4) shape[dy / 4][dx / 2] |= 1u << BIT[dy % 4][dx % 2];
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (shape[y][x]) lap_put_cell(ax + x, ay + y, LAP_BRAILLE(shape[y][x]), HI(LAP_MAGENTA));
            else if (ring[y][x]) lap_put_cell(ax + x, ay + y, LAP_BRAILLE(ring[y][x]), A(LAP_BLUE));
        }
}

/* One reading: label, value, and where it helps a line meter. */
static void reading(int x, int y, int w, const char *label, const char *value, uint8_t hue, float meter)
{
    lap_put_str(x, y, label, A(LAP_CYAN));
    lap_put_str(x + 7, y, value, HI(hue));
    const int vx = x + 8 + (int)strlen(value), mw = x + w - vx - 1;
    if (meter >= 0 && mw >= 6) lap_ui_meter(vx + 1, y, mw > 16 ? 16 : mw, meter > 1 ? 1 : meter, hue | LAP_BRIGHT);
}

static int draw_readings(int x, int y, int w)
{
    const rflab_result_t *r = &s.r;
    char v[48];
    snprintf(v, sizeof(v), "%+.3f kHz  %.1f dBFS", r->peak_offset_hz / 1e3, r->peak_dbfs);
    reading(x, y++, w, "PEAK", v, r->carrier ? LAP_YELLOW : LAP_WHITE, -1);
    snprintf(v, sizeof(v), "%.1f dBFS/Hz", r->noise_dbfs_hz);
    reading(x, y++, w, "FLOOR", v, LAP_BLUE, -1);
    snprintf(v, sizeof(v), "%.1f dBFS", r->chan_dbfs);
    reading(x, y++, w, "CHAN", v, LAP_CYAN, clampf((r->chan_dbfs + 100) / 100, 0, 1));
    snprintf(v, sizeof(v), "%.2f kHz", r->obw_hz / 1e3);
    reading(x, y++, w, "OBW99", v, LAP_MAGENTA, clampf(r->obw_hz / (float)(s.r_rate ? s.r_rate : 1), 0, 1));
    y++;
    if (r->carrier) {
        snprintf(v, sizeof(v), "%+.0f Hz  %+.2f ppm", r->freq_err_hz, r->freq_err_ppm);
        reading(x, y++, w, "ERROR", v, fabsf(r->freq_err_ppm) < 2 ? LAP_GREEN : LAP_YELLOW, -1);
        if (s.r_sdr && r->fine) {
            /* A carrier heard err ppm high: the reference runs slow by that much. */
            const int adjust = -iround(r->freq_err_ppm);
            if (adjust) snprintf(v, sizeof(v), "%+d in FRONT END", adjust);
            else snprintf(v, sizeof(v), "right as it is");
            reading(x, y++, w, "PPM", v, adjust ? LAP_YELLOW : LAP_GREEN, -1);
        }
    } else {
        reading(x, y++, w, "ERROR", "no carrier over the floor", LAP_WHITE, -1);
    }
    y++;
    snprintf(v, sizeof(v), "%.2f %%", r->clip_pct);
    reading(x, y++, w, "CLIP", v, r->clip_pct > 0.1f ? LAP_RED : LAP_GREEN, clampf(r->clip_pct / 5, 0, 1));
    snprintf(v, sizeof(v), "%.0f dBFS", r->dc_dbfs);
    reading(x, y++, w, "DC", v, r->dc_dbfs > -30 ? LAP_YELLOW : LAP_GREEN, clampf((r->dc_dbfs + 80) / 80, 0, 1));
    snprintf(v, sizeof(v), "%+.2f dB %+.1f deg", r->iq_gain_db, r->iq_phase_deg);
    reading(x, y++, w, "I/Q", v, r->image_db < 25 ? LAP_YELLOW : LAP_GREEN, -1);
    snprintf(v, sizeof(v), "%.0f dB", r->image_db);
    reading(x, y++, w, "IMAGE", v, r->image_db < 25 ? LAP_YELLOW : LAP_GREEN, clampf(r->image_db / 60, 0, 1));
    return y;
}

/* What is being measured: chips on a rail, then the tuning. */
static void draw_rail(int x, int y, int w)
{
    const lap_radio_info_t *r = s.radio >= 0 && s.radio < s.nradios ? &s.radios[s.radio] : NULL;
    const bool live = s.h > 0 && s.st.state == LAP_RX_RUNNING;
    static lap_chip_t left[4];
    const lap_chip_t l[] = {
        { r ? r->name : "NO RADIO", r != NULL, LAP_CYAN },
        { s.hold ? "HELD" : live ? "LIVE" : "OPENING", !s.hold && live, s.hold ? LAP_UI_WARN : LAP_UI_LIVE },
        { "CARRIER", s.r.carrier, LAP_YELLOW },
        { "CLIP", s.r.clip_pct > 0.1f, LAP_RED },
    };
    memcpy(left, l, sizeof(left));
    lap_ui_rail(x, y, w, left, 4, NULL, 0);
    char t[80];
    snprintf(t, sizeof(t), "%.6f MHz  %.3f MS/s  RBW %.0f Hz  %d avg%s", s.r_center / 1e6, s.r_rate / 1e6,
             s.r.rbw_hz, s.r.frames, s.hold ? "  HELD" : "");
    lap_put_str(x, y + 1, t, s.hold ? HI(LAP_YELLOW) : LAP_UI_DIM);
}

static void draw_history(int x, int y, int w, int h)
{
    if (h < 4 || w < 20 || s.hist_n < 1) return;
    lap_ui_bezel(x, y, w, h, " SNR HISTORY ");
    const int n = s.hist_n < (w - 2) * 2 ? s.hist_n : (w - 2) * 2;
    float lo = 1e9f, hi = -1e9f;
    for (int i = HIST - n; i < HIST; ++i) {
        if (s.hist[i] < lo) lo = s.hist[i];
        if (s.hist[i] > hi) hi = s.hist[i];
    }
    lo = floorf(lo / 5) * 5 - 5; hi = ceilf(hi / 5) * 5 + 5;
    const float last = s.hist[HIST - 1];
    lap_ui_history(x + 1, y + 1, w - 2, h - 2, s.hist + HIST - n, n, lo, hi,
                   (last >= 20 ? LAP_GREEN : last >= 10 ? LAP_YELLOW : LAP_RED) | LAP_BRIGHT);
    char t[12];
    snprintf(t, sizeof(t), "%.0f", hi);
    lap_put_str(x + w - 1 - (int)strlen(t), y, t, LAP_UI_DIM);
    snprintf(t, sizeof(t), "%.0f dB", lo);
    lap_put_str(x + w - 1 - (int)strlen(t), y + h - 1, t, LAP_UI_DIM);
}

static int draw_keys(void)
{
    const lap_radio_info_t *r = s.src >= 0 && s.src < s.nradios ? &s.radios[s.src] : NULL;
    snprintf(s.key_text[0], sizeof(s.key_text[0]), "%s", r ? r->name : "ANY");
    snprintf(s.key_text[1], sizeof(s.key_text[1]), "%.4f", s.hz / 1e6);
    snprintf(s.key_text[2], sizeof(s.key_text[2]), "%.1fk", s.chan_bw / 1e3);
    s.keys[0] = (lap_btn_t){ "SOURCE", s.key_text[0], 'r', false, false, false };
    s.keys[1] = (lap_btn_t){ "TUNE", s.key_text[1], 't', false, false, false };
    s.keys[2] = (lap_btn_t){ "CHANNEL", s.key_text[2], 'c', false, false, false };
    s.keys[3] = (lap_btn_t){ "HOLD", s.hold ? "HELD" : "LIVE", 'h', s.hold, false, false };
    s.keys[4] = (lap_btn_t)LAP_BTN_OPTIONS;
    const int kh = lap_ui_keys_rows(5);
    lap_ui_keys(0, 0, lap_cols(), kh, s.keys, 5, -1);
    return kh;
}

static void draw_notice(int y)
{
    const char *a = s.why[0] ? s.why : s.h > 0 ? "measuring..." : "looking for a radio";
    const char *b = "SOURCE picks the radio; RF LAB tunes it to TUNE";
    lap_put_str((lap_cols() - 6) / 2, y + 2, "RF LAB", HI(LAP_YELLOW));
    lap_put_str((lap_cols() - (int)strlen(a)) / 2 > 0 ? (lap_cols() - (int)strlen(a)) / 2 : 0, y + 4, a, LAP_UI_TEXT);
    lap_put_str((lap_cols() - (int)strlen(b)) / 2 > 0 ? (lap_cols() - (int)strlen(b)) / 2 : 0, y + 6, b, A(LAP_CYAN));
}

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    if (!s.cap || !s.scratch || !s.lab) {
        lap_put_str(1, 1, "RF LAB: no memory", HI(LAP_RED));
        return;
    }
    if (!s.nradios || !(lap_frame() % 25)) {
        const int n = lap_radio_list(s.radios, LAP_RADIOS_MAX);
        if (n >= 0) s.nradios = n;
    }
    if (!s.hold) {
        if (s.h <= 0 && s.nradios && !(lap_frame() % 25)) open_rx();
        if (s.h > 0 && s.st.state < 0) {
            snprintf(s.why, sizeof(s.why), "the radio stopped (%d): pick SOURCE", s.st.state);
            close_rx();
        }
        pump();
    }

    const int W = lap_cols(), H = lap_rows();
    const int kh = draw_keys();
    int y = kh;
    if (!s.have) { draw_notice(y); return; }
    draw_rail(1, y, W - 2);
    y += 3;
    const int bh = H - y;
    if (W >= 90) {
        const int tw = W * 3 / 5;
        draw_trace(0, y, tw, bh);
        const int sx = tw + 2, sw = W - tw - 3;
        const int used = draw_snr(sx, y);
        draw_ellipse(sx + used + 2, y, sw - used - 2, 5);
        draw_readings(sx, y + 6, sw);
    } else {
        int th = bh * 9 / 20;
        if (th < 10) th = 10;
        draw_trace(0, y, W, th);
        const int ly = y + th + 1;
        const int used = draw_snr(1, ly);
        draw_ellipse(1 + used + 3, ly, W - 2 - used - 3, 5);
        const int ry = ly + 6, end = draw_readings(1, ry, W - 2);
        if (H - (end + 1) >= 5) draw_history(0, end + 1, W, H - (end + 1));
    }
}

/* ---- input ------------------------------------------------------------- */

static bool key(lap_key_t k, char ch)
{
    if (k != LAP_KEY_CHAR) return false;
    switch (ch | 0x20) {
    case 'r': choose_source(); return true;
    case 't': lap_numpad_open("TUNE", "MHz", s.hz / 1e6, tuned); return true;
    case 'c': lap_numpad_open("CHANNEL WIDTH", "kHz", s.chan_bw / 1e3, chan_set); return true;
    case 'h':
        s.hold = !s.hold;
        if (s.hold) close_rx(); else open_rx();
        return true;
    case 'o': lap_options_open(&OPTIONS); return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    const int i = lap_ui_key_hit(col, row);
    if (i >= 0) return key(LAP_KEY_CHAR, s.keys[i].key);
    return false;
}

static void enter(void)
{
    memset(&s, 0, sizeof(s));
    s.src = -1;
    s.radio = -1;
    s.hz = 162400000u;
    s.chan_bw = 12500;
    s.cap = malloc(CAP_PAIRS * 4);
    s.scratch = malloc(READ_PAIRS * 4);
    s.lab = malloc(sizeof(*s.lab));
    if (s.lab) rflab_init(s.lab);
}

static void leave(void)
{
    close_rx();                      /* the board would do it anyway */
    free(s.cap); free(s.scratch); free(s.lab);
    memset(&s, 0, sizeof(s));
}

LAP_APP(.id = "rflab", .name = "RF LAB", .sub = "measurements", .version = "2.0",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_YELLOW, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "  .----.  ", " | /\\/\\ | ", " |/    \\| ", " '------' ", "  RF LAB  ", "          " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
