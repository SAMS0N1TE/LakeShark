/* SSTV receive. See sstv.h. */
#include "sstv.h"

#include <math.h>
#include <string.h>

#define FS        ((double)SSTV_RATE)
#define MS        (FS / 1000.0)
#define TWO_PI_F  6.28318530718f
#define CENTRE_HZ 1700.0f

/* Scan line structures. Martin: sync first, G B R. Scottie: G B, sync, R,
   with one extra sync before the first line. Robot 36: Y and one chroma a
   line, the separator tone saying which. Robot 72: Y, R-Y, B-Y. PD: a
   sync period carries two rows, Y0 R-Y B-Y Y1. */
#define SEG(k, ms) { SSTV_SEG_##k, (ms) }
static const sstv_mode_t MODES[] = {
    { "ROBOT 36", 8, SSTV_ROBOT36, 320, 240, 0.0f, 6,
      { SEG(SYNC, 9.0f), SEG(GAP, 3.0f), SEG(Y, 88.0f), SEG(SEP, 4.5f), SEG(GAP, 1.5f), SEG(C, 44.0f) } },
    { "ROBOT 72", 12, SSTV_YUV, 320, 240, 0.0f, 9,
      { SEG(SYNC, 9.0f), SEG(GAP, 3.0f), SEG(Y, 138.0f), SEG(GAP, 4.5f), SEG(GAP, 1.5f), SEG(RY, 69.0f),
        SEG(GAP, 4.5f), SEG(GAP, 1.5f), SEG(BY, 69.0f) } },
    { "MARTIN 1", 44, SSTV_RGB, 320, 256, 0.0f, 8,
      { SEG(SYNC, 4.862f), SEG(GAP, 0.572f), SEG(G, 146.432f), SEG(GAP, 0.572f), SEG(B, 146.432f),
        SEG(GAP, 0.572f), SEG(R, 146.432f), SEG(GAP, 0.572f) } },
    { "MARTIN 2", 40, SSTV_RGB, 320, 256, 0.0f, 8,
      { SEG(SYNC, 4.862f), SEG(GAP, 0.572f), SEG(G, 73.216f), SEG(GAP, 0.572f), SEG(B, 73.216f),
        SEG(GAP, 0.572f), SEG(R, 73.216f), SEG(GAP, 0.572f) } },
    { "SCOTTIE 1", 60, SSTV_RGB, 320, 256, 9.0f, 7,
      { SEG(GAP, 1.5f), SEG(G, 138.24f), SEG(GAP, 1.5f), SEG(B, 138.24f), SEG(SYNC, 9.0f),
        SEG(GAP, 1.5f), SEG(R, 138.24f) } },
    { "SCOTTIE 2", 56, SSTV_RGB, 320, 256, 9.0f, 7,
      { SEG(GAP, 1.5f), SEG(G, 88.064f), SEG(GAP, 1.5f), SEG(B, 88.064f), SEG(SYNC, 9.0f),
        SEG(GAP, 1.5f), SEG(R, 88.064f) } },
    { "SCOTTIE DX", 76, SSTV_RGB, 320, 256, 9.0f, 7,
      { SEG(GAP, 1.5f), SEG(G, 345.6f), SEG(GAP, 1.5f), SEG(B, 345.6f), SEG(SYNC, 9.0f),
        SEG(GAP, 1.5f), SEG(R, 345.6f) } },
    { "PD 50", 93, SSTV_PD, 320, 128, 0.0f, 6,
      { SEG(SYNC, 20.0f), SEG(GAP, 2.08f), SEG(Y, 91.52f), SEG(RY, 91.52f), SEG(BY, 91.52f), SEG(Y2, 91.52f) } },
    { "PD 90", 99, SSTV_PD, 320, 128, 0.0f, 6,
      { SEG(SYNC, 20.0f), SEG(GAP, 2.08f), SEG(Y, 170.24f), SEG(RY, 170.24f), SEG(BY, 170.24f), SEG(Y2, 170.24f) } },
    { "PD 120", 95, SSTV_PD, 640, 248, 0.0f, 6,
      { SEG(SYNC, 20.0f), SEG(GAP, 2.08f), SEG(Y, 121.6f), SEG(RY, 121.6f), SEG(BY, 121.6f), SEG(Y2, 121.6f) } },
    { "PD 160", 98, SSTV_PD, 512, 200, 0.0f, 6,
      { SEG(SYNC, 20.0f), SEG(GAP, 2.08f), SEG(Y, 195.584f), SEG(RY, 195.584f), SEG(BY, 195.584f), SEG(Y2, 195.584f) } },
    { "PD 180", 96, SSTV_PD, 640, 248, 0.0f, 6,
      { SEG(SYNC, 20.0f), SEG(GAP, 2.08f), SEG(Y, 183.04f), SEG(RY, 183.04f), SEG(BY, 183.04f), SEG(Y2, 183.04f) } },
    { "PD 240", 97, SSTV_PD, 640, 248, 0.0f, 6,
      { SEG(SYNC, 20.0f), SEG(GAP, 2.08f), SEG(Y, 244.48f), SEG(RY, 244.48f), SEG(BY, 244.48f), SEG(Y2, 244.48f) } },
};
#define N_MODES ((int)(sizeof(MODES) / sizeof(MODES[0])))

int sstv_mode_count(void) { return N_MODES; }
const sstv_mode_t *sstv_mode_at(int i) { return i >= 0 && i < N_MODES ? &MODES[i] : NULL; }

const sstv_mode_t *sstv_mode_by_vis(uint8_t vis)
{
    for (int i = 0; i < N_MODES; ++i) if (MODES[i].vis == vis) return &MODES[i];
    return NULL;
}

float sstv_line_ms(const sstv_mode_t *m)
{
    float t = 0.0f;
    for (int i = 0; i < m->nseg; ++i) t += m->seg[i].ms;
    return t;
}

int sstv_height(const sstv_mode_t *m) { return m->family == SSTV_PD ? m->lines * 2 : m->lines; }

/* ---- front end --------------------------------------------------------- */

void sstv_reset(sstv_t *d)
{
    if (d->phase == SSTV_IMAGE && d->sink.end) d->sink.end(d->sink.user, false, d->rows);
    d->phase = SSTV_HUNT;
    d->mode = NULL;
    d->vis_block_until = 0;
}

void sstv_init(sstv_t *d, const sstv_sink_t *sink)
{
    memset(d, 0, sizeof(*d));
    if (sink) d->sink = *sink;
    d->nco_c = 1.0f;
    d->rot_c = cosf(TWO_PI_F * CENTRE_HZ / (float)SSTV_RATE);
    d->rot_s = sinf(TWO_PI_F * CENTRE_HZ / (float)SSTV_RATE);
    /* 1500 Hz low-pass on the mixed-down tone: 200 to 3200 Hz of audio. */
    float sum = 0.0f;
    const float fc = 1500.0f / (float)SSTV_RATE;
    for (int k = 0; k < 25; ++k) {
        const float n = (float)k - 12.0f;
        const float v = n != 0.0f ? sinf(TWO_PI_F * fc * n) / (3.14159265f * n) : 2.0f * fc;
        d->fir[k] = v * (0.54f - 0.46f * cosf(TWO_PI_F * (float)k / 24.0f));
        sum += d->fir[k];
    }
    for (int k = 0; k < 25; ++k) d->fir[k] /= sum;
}

static inline float ring_hz(const sstv_t *d, int64_t i)
{
    return (float)d->ring[(uint64_t)i % SSTV_RING] * (1.0f / 16.0f);
}

static float ms_mean(const sstv_t *d, int64_t a, int64_t b)
{
    float s = 0.0f;
    int n = 0;
    for (int64_t i = a; i < b; ++i, ++n) s += (float)d->ms[(uint64_t)i & 1023];
    return n ? s / (16.0f * (float)n) : 0.0f;
}

/* ---- the picture ---------------------------------------------------------- */

static void begin_image(sstv_t *d, const sstv_mode_t *m, double vis_end)
{
    d->phase = SSTV_IMAGE;
    d->mode = m;
    d->nominal = d->period = sstv_line_ms(m) * MS;
    d->t0 = vis_end + m->pre_ms * MS;
    double at = 0.0;
    d->sync_at = 0.0;
    for (int i = 0; i < m->nseg; ++i) {
        d->seg_at[i] = at;
        if (m->seg[i].kind == SSTV_SEG_SYNC) d->sync_at = at;
        at += m->seg[i].ms * MS;
    }
    d->line = d->rows = d->misses = 0;
    d->sx = d->sy = d->sxx = d->sxy = 0.0;
    d->npts = d->found = d->missed = 0;
    d->slant_ppm = 0.0f;
    memset(d->ry, 128, sizeof(d->ry));
    memset(d->by, 128, sizeof(d->by));
    if (d->sink.begin) d->sink.begin(d->sink.user, m);
}

static void end_image(sstv_t *d, bool complete)
{
    d->phase = SSTV_HUNT;
    d->images++;
    if (d->sink.end) d->sink.end(d->sink.user, complete, d->rows);
    d->mode = NULL;
    d->vis_block_until = d->nms + 500;
}

/* The VIS header ending now, if there is one. On the 1 ms track: 300 ms of
   start bit, 8 bits and stop bit, with the second 1900 Hz leader before. */
static void vis_check(sstv_t *d)
{
    const int64_t e = d->nms;
    if (e < 560 || e < d->vis_block_until) return;
    const float stop = ms_mean(d, e - 26, e - 4);
    if (stop < 1130.0f || stop > 1270.0f) return;
    const float start = ms_mean(d, e - 296, e - 274);
    if (start < 1130.0f || start > 1270.0f) return;
    const float lead = ms_mean(d, e - 500, e - 305);
    if (lead < 1830.0f || lead > 1970.0f) return;
    unsigned code = 0;
    for (int i = 0; i < 8; ++i) {
        const float f = ms_mean(d, e - 270 + 30 * i + 6, e - 240 + 30 * i - 6);
        if (f > 1030.0f && f < 1170.0f) code |= 1u << i;
        else if (!(f > 1230.0f && f < 1370.0f)) return;
    }
    const unsigned data = code & 0x7F;
    if ((unsigned)__builtin_popcount(data) % 2 != ((code >> 7) & 1u)) return;
    const sstv_mode_t *m = sstv_mode_by_vis((uint8_t)data);
    d->vis_block_until = e + 400;
    if (!m) return;
    /* The leader-to-start-bit edge, to the sample: VIS ends 300 ms after it. */
    const int64_t guess = (e - 300) * 16;
    int64_t edge = guess;
    for (int64_t i = guess - 160; i < guess + 160; ++i) {
        if (i <= 0 || i >= d->n) continue;
        if (ring_hz(d, i - 1) >= 1550.0f && ring_hz(d, i) < 1550.0f) { edge = i; break; }
    }
    begin_image(d, m, (double)edge + 300.0 * MS);
}

/* Where line n's sync really starts: the window of the sync's length that
   holds the most low (sync) tone, within `win` samples of `near`. */
static bool find_sync(const sstv_t *d, double near, double len_ms, double win, double *at)
{
    const int len = (int)(len_ms * MS + 0.5);
    const int64_t a = (int64_t)(near - win), b = (int64_t)(near + win);
    if (a < d->n - SSTV_RING + 1 || b + len > d->n) return false;
    int run = 0;
    for (int64_t i = a; i < a + len; ++i) run += ring_hz(d, i) < 1350.0f;
    int best = run;
    int64_t best_at = a;
    for (int64_t p = a + 1; p <= b; ++p) {
        run += (ring_hz(d, p + len - 1) < 1350.0f) - (ring_hz(d, p - 1) < 1350.0f);
        /* The last of a tie: the first line's sync runs on from the VIS
           stop bit at the same tone, and only its end is the line's. */
        if (run >= best) { best = run; best_at = p; }
    }
    if (best < len * 7 / 10) return false;
    *at = (double)best_at;
    return true;
}

static uint8_t level_of(float hz)
{
    const float v = (hz - 1500.0f) * (255.0f / 800.0f);
    return (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v + 0.5f);
}

static void scan(const sstv_t *d, double start, double dur, int w, uint8_t *out)
{
    const double px = dur / w;
    for (int x = 0; x < w; ++x) {
        const int64_t a = (int64_t)(start + x * px + 0.5);
        int64_t b = (int64_t)(start + (x + 1) * px + 0.5);
        if (b <= a) b = a + 1;
        float s = 0.0f;
        for (int64_t i = a; i < b; ++i) s += ring_hz(d, i);
        out[x] = level_of(s / (float)(b - a));
    }
}

static inline uint8_t clip(float v) { return (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v + 0.5f); }

static void emit_yuv(sstv_t *d, int y, const uint8_t *Y)
{
    const int w = d->mode->width;
    for (int x = 0; x < w; ++x) {
        const float yy = Y[x], v = (float)d->ry[x] - 128.0f, u = (float)d->by[x] - 128.0f;
        d->rgb[3 * x]     = clip(yy + 1.402f * v);
        d->rgb[3 * x + 1] = clip(yy - 0.344136f * u - 0.714136f * v);
        d->rgb[3 * x + 2] = clip(yy + 1.772f * u);
    }
    if (d->sink.row) d->sink.row(d->sink.user, y, d->rgb, w);
    d->rows++;
}

static void decode_line(sstv_t *d, double start)
{
    const sstv_mode_t *m = d->mode;
    const int w = m->width;
    bool chroma_is_ry = true;
    for (int i = 0; i < m->nseg; ++i) {
        const double s = start + d->seg_at[i], dur = m->seg[i].ms * MS;
        switch (m->seg[i].kind) {
        case SSTV_SEG_R:  scan(d, s, dur, w, d->chan[0]); break;
        case SSTV_SEG_G:  scan(d, s, dur, w, d->chan[1]); break;
        case SSTV_SEG_B:  scan(d, s, dur, w, d->chan[2]); break;
        case SSTV_SEG_Y:  scan(d, s, dur, w, d->chan[0]); break;
        case SSTV_SEG_Y2: scan(d, s, dur, w, d->chan[3]); break;
        case SSTV_SEG_RY: scan(d, s, dur, w, d->ry); break;
        case SSTV_SEG_BY: scan(d, s, dur, w, d->by); break;
        case SSTV_SEG_SEP: {
            float f = 0.0f;
            const int64_t a = (int64_t)s, b = (int64_t)(s + dur);
            for (int64_t k = a; k < b; ++k) f += ring_hz(d, k);
            chroma_is_ry = f / (float)(b > a ? b - a : 1) < 1900.0f;
            break;
        }
        case SSTV_SEG_C: scan(d, s, dur, w, chroma_is_ry ? d->ry : d->by); break;
        default: break;
        }
    }
    if (m->family == SSTV_RGB) {
        for (int x = 0; x < w; ++x) {
            d->rgb[3 * x] = d->chan[0][x];
            d->rgb[3 * x + 1] = d->chan[1][x];
            d->rgb[3 * x + 2] = d->chan[2][x];
        }
        if (d->sink.row) d->sink.row(d->sink.user, d->line, d->rgb, w);
        d->rows++;
    } else if (m->family == SSTV_PD) {
        emit_yuv(d, 2 * d->line, d->chan[0]);
        emit_yuv(d, 2 * d->line + 1, d->chan[3]);
    } else {
        emit_yuv(d, d->line, d->chan[0]);
    }
}

/* Lines whose samples (and the sync search past them) are all in. */
static void lines(sstv_t *d)
{
    const sstv_mode_t *m = d->mode;
    double sync_len = 9.0;
    for (int i = 0; i < m->nseg; ++i) if (m->seg[i].kind == SSTV_SEG_SYNC) sync_len = m->seg[i].ms;
    double win = d->nominal * 0.04;
    if (win > 25.0 * MS) win = 25.0 * MS;
    while (d->phase == SSTV_IMAGE) {
        const double start = d->t0 + d->line * d->period;
        if ((double)d->n < start + d->period + win + 64.0) return;
        double at;
        const double expect = start + d->sync_at;
        if (find_sync(d, expect, sync_len, win, &at) &&
            (d->npts < 4 || fabs(at - expect) < 2.5 * MS)) {
            const double x = d->line, y = at - d->sync_at;
            d->sx += x; d->sy += y; d->sxx += x * x; d->sxy += x * y;
            d->npts++;
            d->found++;
            d->misses = 0;
            if (d->npts >= 3) {
                const double nn = d->npts, den = nn * d->sxx - d->sx * d->sx;
                if (den > 0.0) {
                    double b = (nn * d->sxy - d->sx * d->sy) / den;
                    if (b < d->nominal * 0.99) b = d->nominal * 0.99;
                    if (b > d->nominal * 1.01) b = d->nominal * 1.01;
                    d->period = b;
                    d->t0 = (d->sy - b * d->sx) / nn;
                    d->slant_ppm = (float)((b / d->nominal - 1.0) * 1e6);
                }
            }
        } else {
            d->missed++;
            d->misses++;
        }
        decode_line(d, d->t0 + d->line * d->period);
        d->line++;
        if (d->line >= m->lines) { end_image(d, true); return; }
        if (d->misses >= 12) { end_image(d, false); return; }
    }
}

void sstv_push(sstv_t *d, const int16_t *pcm, int n)
{
    const float k = TWO_PI_F / (float)SSTV_RATE;
    for (int j = 0; j < n; ++j) {
        const float x = (float)pcm[j] * (1.0f / 32768.0f);
        d->lvl += 0.001f * (x * x - d->lvl);
        const float c = d->nco_c, s = d->nco_s;
        d->zi[d->zpos] = d->zi[d->zpos + 25] = x * c;
        d->zq[d->zpos] = d->zq[d->zpos + 25] = -x * s;
        if (++d->zpos == 25) d->zpos = 0;
        d->nco_c = c * d->rot_c - s * d->rot_s;
        d->nco_s = s * d->rot_c + c * d->rot_s;
        if (++d->nco_n >= 512) {
            d->nco_n = 0;
            const float g = 1.0f / sqrtf(d->nco_c * d->nco_c + d->nco_s * d->nco_s);
            d->nco_c *= g; d->nco_s *= g;
        }
        float fi = 0.0f, fq = 0.0f;
        const float *zi = &d->zi[d->zpos], *zq = &d->zq[d->zpos];
        for (int t = 0; t < 25; ++t) { fi += d->fir[t] * zi[t]; fq += d->fir[t] * zq[t]; }
        const float re = fi * d->pi + fq * d->pq, im = fq * d->pi - fi * d->pq;
        d->pi = fi; d->pq = fq;
        float hz = CENTRE_HZ + atan2f(im, re) / k;
        if (hz < 0.0f) hz = 0.0f;
        if (hz > 4000.0f) hz = 4000.0f;
        d->ring[(uint64_t)d->n % SSTV_RING] = (uint16_t)(hz * 16.0f);
        d->n++;
        d->ms_acc += hz;
        if (++d->ms_acc_n == 16) {
            d->ms[(uint64_t)d->nms & 1023] = (uint16_t)(d->ms_acc * (16.0f / 16.0f));
            d->nms++;
            d->ms_acc = 0.0f;
            d->ms_acc_n = 0;
            if (d->phase == SSTV_HUNT) vis_check(d);
        }
    }
    if (d->phase == SSTV_IMAGE) lines(d);
}

void sstv_status(const sstv_t *d, sstv_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->phase = d->phase;
    out->mode = d->mode;
    out->rows = d->rows;
    out->height = d->mode ? sstv_height(d->mode) : 0;
    out->slant_ppm = d->slant_ppm;
    out->syncs_found = d->found;
    out->syncs_missed = d->missed;
    out->tone_hz = d->n ? ring_hz(d, d->n - 1) : 0.0f;
    out->level = sqrtf(d->lvl);
    out->images = d->images;
}
