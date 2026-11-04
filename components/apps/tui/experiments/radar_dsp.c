#include "radar_dsp.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

/* In-place radix-2 complex FFT, n a power of two. */
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float a = -2.0f * PI_F / (float)len;
        const float wr = cosf(a), wi = sinf(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                const int p = i + k, q = p + len / 2;
                const float xr = re[q] * cr - im[q] * ci, xi = re[q] * ci + im[q] * cr;
                re[q] = re[p] - xr; im[q] = im[p] - xi;
                re[p] += xr; im[p] += xi;
                const float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

float radar_bin_hz(int k)
{
    return (float)(k < RADAR_N / 2 ? k : k - RADAR_N) * (float)RADAR_RATE / (float)RADAR_N;
}

float radar_speed_mps(float doppler_hz, uint32_t carrier_hz)
{
    return carrier_hz ? doppler_hz * 299792458.0f / (2.0f * (float)carrier_hz) : 0.0f;
}

#define ROW_INPUT (RADAR_HOP * RADAR_D1 * RADAR_D2)   /* input samples a row */

/* The oscillator at `hz`. Between rows it also moves on by the learned
   drift every 256 samples (NCO_TICK): a correction only four times a second
   leaves a sawtooth of phase that shows as a skirt round the carrier. The
   per-sample sweep itself is below float resolution, hence the ticks, with
   the frequency kept in double. */
#define NCO_TICK 256
static void set_nco(radar_dsp_t *s, double hz)
{
    s->nco_hz = hz;
    const double a = -2.0 * 3.14159265358979323846 * hz / RADAR_IN_RATE;
    s->step[0] = (float)cos(a); s->step[1] = (float)sin(a);
}

static void stages_reset(radar_dsp_t *s)
{
    s->rot[0] = 1.0f; s->rot[1] = 0.0f; s->rot_n = 0;
    s->s1[0] = s->s1[1] = 0.0f; s->s1_n = 0;
    memset(s->hist, 0, sizeof(s->hist)); s->hist_at = s->d2_n = 0;
    s->win_at = s->since_row = s->filled = 0;
    s->have_prev = false;
    s->fll_re = s->fll_im = 0;
    s->drift_hz = 0;
}

void radar_dsp_reset(radar_dsp_t *s)
{
    s->locked = false;
    s->acq_acc[0] = s->acq_acc[1] = 0.0f; s->acq_n = s->acq_fill = 0;
    s->lost_rows = 0;
    s->bg_rows = 0;
    stages_reset(s);
}

void radar_dsp_init(radar_dsp_t *s)
{
    memset(s, 0, sizeof(*s));
    s->subtract = true;
    /* 5 kHz -> 500 Hz: windowed sinc, cut at 220 Hz, unity at DC. */
    const float fc = 220.0f / (RADAR_IN_RATE / RADAR_D1);
    float sum = 0;
    for (int i = 0; i < RADAR_FIR_TAPS; i++) {
        const float x = (float)i - (RADAR_FIR_TAPS - 1) / 2.0f;
        const float h = fabsf(x) < 1e-6f ? 2 * fc : sinf(2 * PI_F * fc * x) / (PI_F * x);
        const float w = 0.42f - 0.5f * cosf(2 * PI_F * i / (RADAR_FIR_TAPS - 1)) +
                        0.08f * cosf(4 * PI_F * i / (RADAR_FIR_TAPS - 1));
        s->fir[i] = h * w; sum += s->fir[i];
    }
    for (int i = 0; i < RADAR_FIR_TAPS; i++) s->fir[i] /= sum;
    /* 4-term Blackman-Harris: sidelobes 92 dB down, so a -6 dBFS carrier
       leaves nothing over a clean floor a few hertz out. */
    for (int i = 0; i < RADAR_N; i++) {
        const float a = 2 * PI_F * i / RADAR_N;
        s->hann[i] = 0.35875f - 0.48829f * cosf(a) + 0.14128f * cosf(2 * a) - 0.01168f * cosf(3 * a);
    }
    radar_dsp_reset(s);
}

void radar_dsp_set_background(radar_dsp_t *s, bool on)
{
    s->subtract = on;
    s->bg_rows = 0;
}

/* The strongest line of the 25 kHz search, away from the LR's own DC. */
static void acquire(radar_dsp_t *s)
{
    fft(s->acq_re, s->acq_im, RADAR_ACQ_N);
    const float bin_hz = (float)RADAR_IN_RATE / RADAR_ACQ_DECIM / RADAR_ACQ_N;
    int best = -1; float best_p = 0, total = 0;
    for (int k = 0; k < RADAR_ACQ_N; k++) {
        const float p = s->acq_re[k] * s->acq_re[k] + s->acq_im[k] * s->acq_im[k];
        total += p;
        const float hz = (float)(k < RADAR_ACQ_N / 2 ? k : k - RADAR_ACQ_N) * bin_hz;
        if (fabsf(hz) > RADAR_DC_GUARD_HZ && p > best_p) { best_p = p; best = k; }
    }
    s->acq_fill = 0;
    /* A carrier is a line, not noise: most of the power, far over the average
       bin, and strong enough to be the other board's, not something faint
       elsewhere in the band (amplitude sqrt(p) / N with no window). */
    const float min_amp = RADAR_ACQ_N * powf(10.0f, RADAR_MIN_DBFS / 20.0f);
    if (best < 0 || best_p < 200.0f * total / RADAR_ACQ_N || best_p < min_amp * min_amp) return;
    /* Centre it between bins from its neighbours: Jacobsen's estimate,
       Re((X[l] - X[r]) / (2 X[c] - X[l] - X[r])), right to a few hundredths
       of a 30 Hz bin with no window, so the loop starts within a hertz. */
    const int l = (best + RADAR_ACQ_N - 1) % RADAR_ACQ_N, r = (best + 1) % RADAR_ACQ_N;
    const float nr = s->acq_re[l] - s->acq_re[r], ni = s->acq_im[l] - s->acq_im[r];
    const float dr = 2 * s->acq_re[best] - s->acq_re[l] - s->acq_re[r];
    const float di = 2 * s->acq_im[best] - s->acq_im[l] - s->acq_im[r];
    const float den = dr * dr + di * di;
    const float frac = den > 1e-24f ? (nr * dr + ni * di) / den : 0.0f;
    const float k = (float)(best < RADAR_ACQ_N / 2 ? best : best - RADAR_ACQ_N) + frac;
    set_nco(s, k * bin_hz);
    stages_reset(s);
    s->locked = true;
    s->lost_rows = 0;
}

static float median(float *v, int n)
{
    /* insertion sort: n is a few hundred, four times a second */
    for (int i = 1; i < n; i++) {
        const float x = v[i]; int j = i - 1;
        for (; j >= 0 && v[j] > x; j--) v[j + 1] = v[j];
        v[j + 1] = x;
    }
    return n ? v[n / 2] : 0.0f;
}

static void make_row(radar_dsp_t *s, radar_row_t *row)
{
    for (int i = 0; i < RADAR_N; i++) {
        const int at = (s->win_at + i) % RADAR_N;
        s->re[i] = s->win[at][0] * s->hann[i];
        s->im[i] = s->win[at][1] * s->hann[i];
    }
    fft(s->re, s->im, RADAR_N);
    /* pc: the carrier's peak bin, what every bin is shown against; ptot: all
       of its main lobe, what the motion figure is a share of. */
    float pc = 0; double ptot = 0;
    for (int k = -4; k <= 4; k++) {
        const int b = (k + RADAR_N) % RADAR_N;
        const float p = s->re[b] * s->re[b] + s->im[b] * s->im[b];
        if (p > pc) pc = p;
        ptot += p;
    }
    if (pc <= 0) pc = 1e-30f;
    if (ptot <= 0) ptot = 1e-30;
    static float tmp[RADAR_N];
    int nt = 0;
    for (int k = 0; k < RADAR_N; k++) {
        const float p = s->re[k] * s->re[k] + s->im[k] * s->im[k];
        s->re[k] = p;                        /* the power, from here on */
        row->db[k] = 10.0f * log10f(p / pc + 1e-20f);
        const float hz = fabsf(radar_bin_hz(k));
        if (hz >= 30.0f && hz <= 245.0f) tmp[nt++] = row->db[k];
    }
    row->floor_db = median(tmp, nt);

    /* What changed: each bin against the still room, learned over about 12 s
       (RADAR_BG_ROWS). Skirt, hum and the walls stay in the background; a
       hand, a step or a door stands out until it has been there that long.
       The room is kept in dB against the carrier: a linear mean is pulled up
       by a strong new echo within a second, and a hand near an antenna that
       moves the whole level does not read as motion. Off, each bin is shown
       against the floor instead. Motion counts only bins clearly over the
       room, so noise flicker adds nothing. */
    const bool learning = s->subtract && s->bg_rows < RADAR_WARM_ROWS;
    const float a = s->bg_rows < RADAR_BG_ROWS ? 1.0f / (float)(s->bg_rows + 1) : 1.0f / RADAR_BG_ROWS;
    double motion = 0; float peak = -1e9f, peak_hz = 0;
    for (int k = 0; k < RADAR_N; k++) {
        const float p = s->re[k], db = row->db[k];
        const float bg = s->bg_rows ? s->bg[k] : db;
        row->ex[k] = s->subtract ? db - bg : db - row->floor_db;
        s->bg[k] = bg + a * (db - bg);
        const float hz = fabsf(radar_bin_hz(k));
        if (hz < RADAR_MIN_HZ || hz > 245.0f) continue;
        if (!s->subtract) motion += p;
        else if (row->ex[k] >= RADAR_SHOW_DB) motion += p - pc * powf(10.0f, bg / 10.0f);
        if (row->ex[k] > peak) { peak = row->ex[k]; peak_hz = radar_bin_hz(k); }
    }
    if (s->bg_rows < 0x7fffffff) s->bg_rows++;
    row->learning = learning;
    row->motion_db = learning ? -120.0f : 10.0f * log10f((float)(motion / ptot) + 1e-20f);
    row->peak_db = peak;
    row->peak_hz = !learning && peak >= RADAR_ECHO_DB ? peak_hz : 0.0f;
    const float amp = sqrtf(pc) / (RADAR_N * 0.35875f);
    row->carrier_dbfs = 20.0f * log10f(amp + 1e-12f);
    row->offset_hz = (float)s->nco_hz;
    /* Too faint to be the other board's tone, at once: a burst that passed
       one search is dropped before any row of it is shown. */
    if (row->carrier_dbfs < RADAR_MIN_DBFS) { radar_dsp_reset(s); return; }

    /* The loop: the mean phase step left over is the error, at 500 Hz. Two
       crystals warming up drift in a ramp, which a plain loop trails; the
       second term learns the ramp (Hz per row) so it is followed exactly. */
    const double err = atan2(s->fll_im, s->fll_re) * RADAR_RATE / (2.0 * 3.14159265358979323846);
    s->fll_re = s->fll_im = 0;
    s->drift_hz += 0.08 * err;
    set_nco(s, s->nco_hz + 0.5 * err);
    /* No line over the floor for two seconds: look again. */
    s->lost_rows = row->floor_db > -20.0f ? s->lost_rows + 1 : 0;
    if (s->lost_rows > 8) radar_dsp_reset(s);
}

int radar_dsp_feed(radar_dsp_t *s, const uint8_t *iq16, size_t pairs, radar_row_t *rows, int max)
{
    int made = 0;
    for (size_t n = 0; n < pairs; n++) {
        const float i = (float)(int16_t)(iq16[4 * n] | iq16[4 * n + 1] << 8) / 32768.0f;
        const float q = (float)(int16_t)(iq16[4 * n + 2] | iq16[4 * n + 3] << 8) / 32768.0f;
        if (!s->locked) {
            s->acq_acc[0] += i; s->acq_acc[1] += q;
            if (++s->acq_n < RADAR_ACQ_DECIM) continue;
            s->acq_re[s->acq_fill] = s->acq_acc[0]; s->acq_im[s->acq_fill] = s->acq_acc[1];
            s->acq_acc[0] = s->acq_acc[1] = 0.0f; s->acq_n = 0;
            if (++s->acq_fill == RADAR_ACQ_N) acquire(s);
            continue;
        }
        /* mix the carrier to 0 Hz */
        const float mi = i * s->rot[0] - q * s->rot[1], mq = i * s->rot[1] + q * s->rot[0];
        const float r0 = s->rot[0] * s->step[0] - s->rot[1] * s->step[1];
        s->rot[1] = s->rot[0] * s->step[1] + s->rot[1] * s->step[0]; s->rot[0] = r0;
        if (++s->rot_n == NCO_TICK) {
            const float m = 1.0f / sqrtf(s->rot[0] * s->rot[0] + s->rot[1] * s->rot[1]);
            s->rot[0] *= m; s->rot[1] *= m; s->rot_n = 0;
            if (s->drift_hz != 0) set_nco(s, s->nco_hz + s->drift_hz * NCO_TICK / ROW_INPUT);
        }
        /* 125k -> 5k */
        s->s1[0] += mi; s->s1[1] += mq;
        if (++s->s1_n < RADAR_D1) continue;
        s->hist[s->hist_at][0] = s->s1[0] / RADAR_D1;
        s->hist[s->hist_at][1] = s->s1[1] / RADAR_D1;
        s->hist_at = (s->hist_at + 1) % RADAR_FIR_TAPS;
        s->s1[0] = s->s1[1] = 0.0f; s->s1_n = 0;
        /* 5k -> 500 Hz */
        if (++s->d2_n < RADAR_D2) continue;
        s->d2_n = 0;
        float y[2] = { 0, 0 };
        for (int t = 0; t < RADAR_FIR_TAPS; t++) {
            const float *h = s->hist[(s->hist_at + t) % RADAR_FIR_TAPS];
            y[0] += s->fir[t] * h[0]; y[1] += s->fir[t] * h[1];
        }
        if (s->have_prev) {
            s->fll_re += (double)(y[0] * s->prev[0] + y[1] * s->prev[1]);
            s->fll_im += (double)(y[1] * s->prev[0] - y[0] * s->prev[1]);
        }
        s->prev[0] = y[0]; s->prev[1] = y[1]; s->have_prev = true;
        s->win[s->win_at][0] = y[0]; s->win[s->win_at][1] = y[1];
        s->win_at = (s->win_at + 1) % RADAR_N;
        if (s->filled < RADAR_N) s->filled++;
        if (++s->since_row < RADAR_HOP || s->filled < RADAR_N) continue;
        s->since_row = 0;
        radar_row_t *row = &rows[made < max ? made : max - 1];
        make_row(s, row);
        if (!s->locked) break;   /* lost: no row, and the rest is the next search's */
        if (made < max) made++;
    }
    return made;
}

int radar_row_text(const radar_row_t *r, float span_hz, int cols, char *out)
{
    static const char RAMP[] = " .:-=+*#%@";
    /* A noise bin against its own log average passes 11 dB 0.08% of the
       time, so of the ten a column holds about one row in a hundred shows a
       dot: the ramp starts there. */
    static const float STEP[] = { RADAR_SHOW_DB, 14, 17, 21, 25, 29, 33, 38, 43 };
    const int half = cols / 2;
    const float w = span_hz / (float)half;
    for (int c = 0; c < cols; c++) {
        if (c == half) { out[c] = '|'; continue; }
        const float lo = ((float)(c - half) - 0.5f) * w, hi = lo + w;
        float best = -1e9f;
        for (int k = 0; k < RADAR_N; k++) {
            const float hz = radar_bin_hz(k);
            if (hz >= lo && hz < hi && r->ex[k] > best) best = r->ex[k];
        }
        const float over = best;
        int lvl = 0;
        while (lvl < 9 && over >= STEP[lvl]) lvl++;
        out[c] = RAMP[lvl];
    }
    out[cols] = 0;
    return cols;
}
