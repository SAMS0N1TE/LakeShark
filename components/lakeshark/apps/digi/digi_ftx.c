/* FT8 / FT4 coding and slot decoder. See digi_ftx.h for the references. */
#include "digi_ftx.h"
#include "digi_ftx_tables.h"
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
/* A long slot decode on the digi worker (core 1) gives IDLE1 a tick every
   200 ms, so its watchdog feed and the task reaper are not starved. */
static void yield_every(uint32_t *last)
{
    if (now_us() - *last < 200000u) return;
    vTaskDelay(1);
    *last = now_us();
}
#else
#include <time.h>
#define EXT_RAM_BSS_ATTR
static uint32_t now_us(void) { return (uint32_t)((double)clock() * 1e6 / CLOCKS_PER_SEC); }
static void yield_every(uint32_t *last) { (void)last; }
#endif

#define TWO_PI 6.283185307179586f
#define CODE_REF 96          /* waterfall code for ref_db                    */
#ifndef MAX_CANDS
#define MAX_CANDS 200        /* sync candidates handed to LDPC per slot      */
#endif
/* Sync ratio x100 below which a cell is not worth an LDPC attempt. */
#ifndef DIGI_FTX_MIN_SYNC
#define DIGI_FTX_MIN_SYNC 150
#endif
#define LDPC_ITERS 30
/* Retry the four neighbouring grid points when BP ends this close. */
#ifndef DIGI_FTX_RETRY
#define DIGI_FTX_RETRY 24
#endif
/* ... for the best-synced candidates only, which bounds the cost. */
#ifndef DIGI_FTX_RETRY_TOP
#define DIGI_FTX_RETRY_TOP 20
#endif
/* Average loss of the half-symbol / half-tone grid against a perfectly
   aligned matched filter, added back to the reported SNR. */
#ifndef DIGI_FTX_SNR_TRIM
#define DIGI_FTX_SNR_TRIM 1.0f
#endif

static const digi_ftx_spec_t SPEC[2] = {
    {.sym = 1920, .nn = 79, .ntones = 8, .slot_ms = 15000, .tone_hz = 6.25f, .bt = 2.0f},
    {.sym = 576, .nn = 105, .ntones = 4, .slot_ms = 7500, .tone_hz = 12000.0f / 576.0f, .bt = 1.0f},
};

const digi_ftx_spec_t *digi_ftx_spec(digi_ftx_mode_t m) { return &SPEC[m == DIGI_FT4]; }

/* ---- CRC-14 ------------------------------------------------------------- */

uint16_t digi_crc14(const uint8_t *bytes, int nbits)
{
    uint16_t r = 0;
    for (int i = 0; i < nbits; ++i) {
        int bit = (bytes[i / 8] >> (7 - i % 8)) & 1;
        int top = (r >> 13) & 1;
        r = (uint16_t)((r << 1) & 0x3FFF);
        if (top ^ bit) r ^= 0x2757;
    }
    return r;
}

/* The CRC covers the message zero-extended to 82 bits; the register form
   above (feed bit, xor on top) equals the long division over those bits
   followed by 14 zero bits. */
static uint16_t crc_of(const uint8_t a91[12])
{
    uint8_t t[12];
    memcpy(t, a91, 12);
    t[9] &= 0xF8; t[10] = 0; t[11] = 0;
    /* 82 message bits then 14 zeros = 96 bits through the shift register. */
    uint16_t r = 0;
    for (int i = 0; i < 96; ++i) {
        int bit = (t[i / 8] >> (7 - i % 8)) & 1;
        int top = (r >> 13) & 1;
        r = (uint16_t)(((r << 1) | (uint16_t)bit) & 0x3FFF);
        if (top) r ^= 0x2757;
    }
    return r;
}

void digi_ftx_add_crc(const uint8_t payload[10], uint8_t a91[12])
{
    memset(a91, 0, 12);
    memcpy(a91, payload, 10);
    a91[9] &= 0xF8;
    uint16_t c = crc_of(a91);
    a91[9] |= (uint8_t)(c >> 11);
    a91[10] = (uint8_t)(c >> 3);
    a91[11] = (uint8_t)(c << 5);
}

bool digi_ftx_crc_ok(const uint8_t a91[12])
{
    uint16_t got = (uint16_t)(((a91[9] & 0x07) << 11) | (a91[10] << 3) | (a91[11] >> 5));
    return got == crc_of(a91);
}

/* ---- LDPC --------------------------------------------------------------- */

static int parity8(uint8_t x) { x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return x & 1; }

void digi_ldpc_encode(const uint8_t a91[12], uint8_t cw[22])
{
    memset(cw, 0, 22);
    memcpy(cw, a91, 12);
    cw[11] &= 0xE0;   /* 91 bits: the top three of byte 11 */
    for (int i = 0; i < DIGI_LDPC_M; ++i) {
        int p = 0;
        for (int j = 0; j < DIGI_LDPC_K_BYTES; ++j) p ^= parity8(a91[j] & digi_ldpc_gen[i][j]);
        int b = DIGI_LDPC_K + i;
        if (p) cw[b / 8] |= (uint8_t)(0x80u >> (b % 8));
    }
}

/* Where bit n sits in each of its three checks' rows. */
EXT_RAM_BSS_ATTR static uint8_t s_edge[DIGI_LDPC_N][3];
static bool s_edge_ready;
static void edges_init(void)
{
    if (s_edge_ready) return;
    for (int n = 0; n < DIGI_LDPC_N; ++n)
        for (int j = 0; j < 3; ++j) {
            int m = digi_ldpc_mn[n][j] - 1;
            for (int k = 0; k < digi_ldpc_nrw[m]; ++k)
                if (digi_ldpc_nm[m][k] - 1 == n) s_edge[n][j] = (uint8_t)k;
        }
    s_edge_ready = true;
}

/* tanh by Lambert's continued fraction, exact to about 1e-6 below 4.97. */
static inline float fast_tanh(float x)
{
    if (x > 4.97f) return 1.0f;
    if (x < -4.97f) return -1.0f;
    float x2 = x * x;
    return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) /
           (135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f)));
}
static inline float safe_atanh(float y)
{
    if (y > 0.9999999f) y = 0.9999999f;
    if (y < -0.9999999f) y = -0.9999999f;
    return 0.5f * logf((1.0f + y) / (1.0f - y));
}

/* Sum-product belief propagation over the Tanner graph. Messages are in
   log(P0/P1) form internally. */
int digi_ldpc_decode(const float llr[174], int iters, uint8_t bits[174])
{
    edges_init();
    /* Worker-only scratch; one decoder runs at a time. */
    EXT_RAM_BSS_ATTR static float vc[DIGI_LDPC_M][7], cv[DIGI_LDPC_M][7];
    float L[DIGI_LDPC_N];
    for (int n = 0; n < DIGI_LDPC_N; ++n) L[n] = -llr[n];
    for (int m = 0; m < DIGI_LDPC_M; ++m)
        for (int k = 0; k < digi_ldpc_nrw[m]; ++k) { vc[m][k] = L[digi_ldpc_nm[m][k] - 1]; cv[m][k] = 0; }
    int fails = DIGI_LDPC_M;
    for (int it = 0; it <= iters; ++it) {
        /* Hard decision and check count from the current beliefs. */
        for (int n = 0; n < DIGI_LDPC_N; ++n) {
            float t = L[n];
            for (int j = 0; j < 3; ++j) t += cv[digi_ldpc_mn[n][j] - 1][s_edge[n][j]];
            bits[n] = t < 0;
        }
        fails = 0;
        for (int m = 0; m < DIGI_LDPC_M; ++m) {
            int p = 0;
            for (int k = 0; k < digi_ldpc_nrw[m]; ++k) p ^= bits[digi_ldpc_nm[m][k] - 1];
            fails += p;
        }
        if (!fails || it == iters) break;
        /* Check nodes. */
        for (int m = 0; m < DIGI_LDPC_M; ++m) {
            const int nr = digi_ldpc_nrw[m];
            float t[7];
            for (int k = 0; k < nr; ++k) t[k] = fast_tanh(0.5f * vc[m][k]);
            for (int k = 0; k < nr; ++k) {
                float prod = 1.0f;
                for (int q = 0; q < nr; ++q) if (q != k) prod *= t[q];
                cv[m][k] = 2.0f * safe_atanh(prod);
            }
        }
        /* Variable nodes. */
        for (int n = 0; n < DIGI_LDPC_N; ++n) {
            float tot = L[n];
            for (int j = 0; j < 3; ++j) tot += cv[digi_ldpc_mn[n][j] - 1][s_edge[n][j]];
            for (int j = 0; j < 3; ++j) {
                int m = digi_ldpc_mn[n][j] - 1, k = s_edge[n][j];
                vc[m][k] = tot - cv[m][k];
            }
        }
    }
    return fails;
}

/* ---- tones -------------------------------------------------------------- */

static int cw_bit(const uint8_t cw[22], int i) { return (cw[i / 8] >> (7 - i % 8)) & 1; }

int digi_ftx_tones(digi_ftx_mode_t m, const uint8_t payload[10], uint8_t *tones)
{
    uint8_t p[10], a91[12], cw[22];
    memcpy(p, payload, 10);
    if (m == DIGI_FT4)
        for (int i = 0; i < 10; ++i) p[i] ^= digi_ft4_xor[i];
    digi_ftx_add_crc(p, a91);
    digi_ldpc_encode(a91, cw);
    int b = 0;
    if (m == DIGI_FT8) {
        for (int i = 0; i < 79; ++i) {
            if (i < 7) tones[i] = digi_ft8_costas[i];
            else if (i >= 36 && i < 43) tones[i] = digi_ft8_costas[i - 36];
            else if (i >= 72) tones[i] = digi_ft8_costas[i - 72];
            else {
                int v = cw_bit(cw, b) << 2 | cw_bit(cw, b + 1) << 1 | cw_bit(cw, b + 2);
                b += 3;
                tones[i] = digi_ft8_gray[v];
            }
        }
        return 79;
    }
    for (int i = 0; i < 105; ++i) {
        if (i == 0 || i == 104) tones[i] = 0;
        else if (i < 5) tones[i] = digi_ft4_costas[0][i - 1];
        else if (i >= 34 && i < 38) tones[i] = digi_ft4_costas[1][i - 34];
        else if (i >= 67 && i < 71) tones[i] = digi_ft4_costas[2][i - 67];
        else if (i >= 100) tones[i] = digi_ft4_costas[3][i - 100];
        else {
            int v = cw_bit(cw, b) << 1 | cw_bit(cw, b + 1);
            b += 2;
            tones[i] = digi_ft4_gray[v];
        }
    }
    return 105;
}

/* GFSK: each symbol's frequency step is a rectangle smoothed by a Gaussian
   (the standard GMSK frequency pulse), spread over three symbols. The pulse
   is tabulated once per mode, in PSRAM. */
EXT_RAM_BSS_ATTR static float *s_pulse[2];
static const float *pulse_table(digi_ftx_mode_t m)
{
    const int i = m == DIGI_FT4;
    if (s_pulse[i]) return s_pulse[i];
    const digi_ftx_spec_t *s = digi_ftx_spec(m);
    const int sps = s->sym;
    const float c = 3.14159265f * sqrtf(2.0f / logf(2.0f)) * s->bt;
    float *p = digi_alloc(sizeof(float) * (size_t)(3 * sps));
    if (!p) return NULL;
    for (int k = 0; k < 3 * sps; ++k) {
        float t = ((float)k - 1.5f * (float)sps) / (float)sps;
        p[k] = 0.5f * (erff(c * (t + 0.5f)) - erff(c * (t - 0.5f)));
    }
    s_pulse[i] = p;
    return p;
}

bool digi_ftx_tx_begin(digi_ftx_tx_t *tx, digi_ftx_mode_t m, const uint8_t payload[10],
                       float f0_hz, float amp, long start)
{
    memset(tx, 0, sizeof(*tx));
    tx->mode = m;
    tx->f0 = f0_hz;
    tx->amp = amp;
    tx->start = start;
    digi_ftx_tones(m, payload, tx->tones);
    return pulse_table(m) != NULL;
}

/* Add the signal's samples for absolute indices at .. at+n-1. Calls must
   move forward in time; the phase carries from one call to the next. */
void digi_ftx_tx_add(digi_ftx_tx_t *tx, float *out, int n, long at)
{
    const digi_ftx_spec_t *s = digi_ftx_spec(tx->mode);
    const float *pulse = pulse_table(tx->mode);
    if (!pulse) return;
    const int sps = s->sym, nsym = s->nn;
    const long total = (long)nsym * sps, ramp = sps / 8;
    for (int i = 0; i < n; ++i) {
        const long t = at + i - tx->start;
        if (t < 0) continue;
        if (t >= total) break;
        if (t < tx->t) continue;   /* already produced */
        const int k0 = (int)(t / sps);
        float dev = 0.0f;
        /* Symbols beyond the ends repeat the end tones, keeping the edges flat. */
        for (int k = k0 - 1; k <= k0 + 1; ++k) {
            int kk = k < 0 ? 0 : k >= nsym ? nsym - 1 : k;
            long idx = t - ((long)k * sps - sps);  /* pulse[] is centred on 1.5 sps */
            if (idx >= 0 && idx < 3 * sps) dev += (float)tx->tones[kk] * pulse[idx];
        }
        tx->phase += (double)TWO_PI * (tx->f0 + s->tone_hz * dev) / DIGI_FTX_RATE;
        if (tx->phase > 1e6) tx->phase = fmod(tx->phase, 6.283185307179586);
        float env = 1.0f;
        if (t < ramp) env = 0.5f * (1.0f - cosf(3.14159265f * (float)t / (float)ramp));
        else if (t >= total - ramp) env = 0.5f * (1.0f - cosf(3.14159265f * (float)(total - 1 - t) / (float)ramp));
        out[i] += tx->amp * env * (float)sin(tx->phase);
        tx->t = t + 1;
    }
}

void digi_ftx_synth(digi_ftx_mode_t m, const uint8_t *tones, float f0_hz,
                    float amp, float *out, int n, int start)
{
    digi_ftx_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.mode = m; tx.f0 = f0_hz; tx.amp = amp; tx.start = start;
    memcpy(tx.tones, tones, (size_t)digi_ftx_spec(m)->nn);
    digi_ftx_tx_add(&tx, out, n, 0);
}

/* ---- decoder: waterfall ------------------------------------------------- */

float digi_ftx_dec_bin_hz(const digi_ftx_dec_t *d) { return (float)DIGI_FTX_RATE / (float)d->nfft; }

bool digi_ftx_dec_init(digi_ftx_dec_t *d, digi_ftx_mode_t m, float fmin, float fmax)
{
    memset(d, 0, sizeof(*d));
    d->mode = m;
    d->spec = digi_ftx_spec(m);
    d->nfft = 2 * d->spec->sym;
    d->tosr = DIGI_FTX_TOSR;
    d->step = d->spec->sym / d->tosr;
    float bin_hz = (float)DIGI_FTX_RATE / (float)d->nfft;
    if (fmin < 50) fmin = 50;
    if (fmax > 5800) fmax = 5800;
    d->bin_lo = (int)(fmin / bin_hz);
    d->bin_hi = (int)(fmax / bin_hz);
    d->bins = d->bin_hi + 2 * d->spec->ntones + 2;
    if (d->bins > d->nfft / 2) d->bins = d->nfft / 2;
    const int slot_samples = d->spec->slot_ms * (DIGI_FTX_RATE / 1000);
    d->frames_max = (slot_samples - d->spec->sym) / d->step + 2;
    d->wf = digi_alloc((size_t)d->frames_max * (size_t)d->bins);
    d->fin = digi_alloc(sizeof(digi_cpx) * (size_t)d->nfft);
    d->fout = digi_alloc(sizeof(digi_cpx) * (size_t)d->nfft);
    d->buf = digi_alloc(sizeof(float) * (size_t)(d->spec->sym + d->step + 1024));
    if (!d->wf || !d->fin || !d->fout || !d->buf || !digi_fft_init(&d->fft, d->nfft)) {
        digi_ftx_dec_free(d);
        return false;
    }
    return true;
}

void digi_ftx_dec_free(digi_ftx_dec_t *d)
{
    digi_free(d->wf); digi_free(d->fin); digi_free(d->fout); digi_free(d->buf);
    digi_fft_free(&d->fft);
    memset(d, 0, sizeof(*d));
}

void digi_ftx_dec_reset(digi_ftx_dec_t *d)
{
    /* Re-centre the code range on the noise floor the last slot saw. */
    if (d->frames > 4 && d->ref_set) {
        int hist[256] = {0}, cnt = 0;
        for (int t = 0; t < d->frames; t += 4)
            for (int b = d->bin_lo; b < d->bin_hi; b += 3) { hist[d->wf[t * d->bins + b]]++; cnt++; }
        int acc = 0, med = CODE_REF;
        for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc * 2 >= cnt) { med = i; break; } }
        d->ref_db += (float)(med - CODE_REF) * 0.5f;
    }
    d->frames = 0;
    d->nbuf = 0;
    d->samples = 0;
}

static inline uint8_t to_code(const digi_ftx_dec_t *d, float p)
{
    float db = 10.0f * log10f(p + 1e-30f);
    float c = 2.0f * (db - d->ref_db) + CODE_REF;
    return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c + 0.5f);
}

/* Two frames per FFT: frame A in the real part, B in the imaginary. */
static void frames_pair(digi_ftx_dec_t *d, const float *a, const float *b)
{
    const int N = d->nfft, sym = d->spec->sym;
    for (int k = 0; k < sym; ++k) d->fin[k] = (digi_cpx){a[k], b ? b[k] : 0.0f};
    for (int k = sym; k < N; ++k) d->fin[k] = (digi_cpx){0, 0};
    digi_fft_run(&d->fft, d->fin, d->fout);
    const digi_cpx *Z = d->fout;
    if (!d->ref_set) {
        /* First frame ever: put the median bin at CODE_REF. */
        float tmp[64];
        int n = 0;
        for (int k = d->bin_lo; k < d->bin_hi && n < 64; k += (d->bin_hi - d->bin_lo) / 64 + 1) {
            const digi_cpx z = Z[k], w = Z[N - k];
            float re = z.re + w.re, im = z.im - w.im;
            tmp[n++] = 10.0f * log10f(0.25f * (re * re + im * im) + 1e-30f);
        }
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && tmp[j] < tmp[j - 1]; --j) { float t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t; }
        d->ref_db = n ? tmp[n / 2] : 0.0f;
        d->ref_set = true;
    }
    for (int pass = 0; pass < (b ? 2 : 1); ++pass) {
        if (d->frames >= d->frames_max) return;
        uint8_t *row = d->wf + (size_t)d->frames * (size_t)d->bins;
        for (int k = 0; k < d->bins; ++k) {
            const digi_cpx z = Z[k], w = Z[k ? N - k : 0];
            float re, im;
            if (!pass) { re = z.re + w.re; im = z.im - w.im; }
            else { re = z.im + w.im; im = w.re - z.re; }
            row[k] = to_code(d, 0.25f * (re * re + im * im));
        }
        d->frames++;
    }
}

void digi_ftx_dec_feed(digi_ftx_dec_t *d, const float *x, int n)
{
    if (!d->wf) return;
    const int sym = d->spec->sym, step = d->step, cap = sym + step + 1024;
    while (n > 0) {
        int take = cap - d->nbuf;
        if (take > n) take = n;
        memcpy(d->buf + d->nbuf, x, sizeof(float) * (size_t)take);
        d->nbuf += take; x += take; n -= take; d->samples += take;
        while (d->nbuf >= sym + step) {
            frames_pair(d, d->buf, d->buf + step);
            d->nbuf -= 2 * step;
            memmove(d->buf, d->buf + 2 * step, sizeof(float) * (size_t)d->nbuf);
        }
    }
}

void digi_ftx_dec_feed_i16(digi_ftx_dec_t *d, const int16_t *x, int n)
{
    float tmp[256];
    while (n > 0) {
        int k = n < 256 ? n : 256;
        for (int i = 0; i < k; ++i) tmp[i] = (float)x[i] * (1.0f / 32768.0f);
        digi_ftx_dec_feed(d, tmp, k);
        x += k; n -= k;
    }
}

const uint8_t *digi_ftx_dec_line(const digi_ftx_dec_t *d, int back)
{
    if (!d->wf || d->frames <= back) return NULL;
    return d->wf + (size_t)(d->frames - 1 - back) * (size_t)d->bins;
}

/* ---- decoder: sync and bits --------------------------------------------- */

typedef struct { int16_t t0, f0; int16_t score; } cand_t;

/* Bit metrics use tone amplitude relative to the noise floor, which tracks
   the noncoherent FSK likelihood far better at low SNR than log power. */
EXT_RAM_BSS_ATTR static float s_amp[256], s_pow[256];
static void amp_init(void)
{
    if (s_amp[255] > 0) return;
    for (int i = 0; i < 256; ++i) {
        s_amp[i] = (float)pow(10.0, (i - CODE_REF) / 40.0);
        s_pow[i] = s_amp[i] * s_amp[i];
    }
}

static inline int wfc(const digi_ftx_dec_t *d, int t, int b) { return d->wf[(size_t)t * (size_t)d->bins + (size_t)b]; }

/* Sync: power in the Costas tones over the mean power of the other tones of
   the same symbols (the ratio WSJT-X describes for its sync search), x100. */
static int sync_score(const digi_ftx_dec_t *d, int t0, int f0)
{
    const bool ft8 = d->mode == DIGI_FT8;
    const int ngroups = ft8 ? 3 : 4, glen = ft8 ? 7 : 4, nt = d->spec->ntones;
    float on = 0, all = 0;
    int cnt = 0;
    for (int g = 0; g < ngroups; ++g) {
        const int s0 = ft8 ? g * 36 : 1 + g * 33;
        for (int k = 0; k < glen; ++k) {
            const int t = t0 + d->tosr * (s0 + k);
            if (t < 0 || t >= d->frames) continue;
            const uint8_t *row = d->wf + (size_t)t * (size_t)d->bins + f0;
            const int tone = ft8 ? digi_ft8_costas[k] : digi_ft4_costas[g][k];
            for (int q = 0; q < nt; ++q) all += s_pow[row[2 * q]];
            on += s_pow[row[2 * tone]];
            cnt++;
        }
    }
    if (cnt < glen) return 0;
    float off = (all - on) / (float)(nt - 1);
    float r = off > 0 ? on / off : 0;
    return r > 300 ? 30000 : (int)(r * 100.0f);
}

/* Median waterfall code over the band around a candidate and the frames it
   spans: the local noise floor. */
static int floor_code(const digi_ftx_dec_t *d, const cand_t *c)
{
    int hist[256] = {0}, cnt = 0;
    const int span = 40;
    for (int s = 0; s < d->spec->nn; s += 2) {
        const int t = c->t0 + d->tosr * s;
        if (t < 0 || t >= d->frames) continue;
        for (int b = c->f0 - span; b < c->f0 + 2 * d->spec->ntones + span; b += 3)
            if (b >= 0 && b < d->bins) { hist[wfc(d, t, b)]++; cnt++; }
    }
    int acc = 0;
    for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc * 2 >= cnt) return i; }
    return CODE_REF;
}

/* Bit LLRs: for each bit, the strongest tone amplitude among the tones that
   carry a 1 minus the strongest among those carrying a 0 (max-log), scaled
   to a fixed variance. Measured against an exact Rician log-likelihood on
   the host bench: no better, and this needs no SNR estimate. */
static void bits_llr(const digi_ftx_dec_t *d, const cand_t *c, float llr[174])
{
    const bool ft8 = d->mode == DIGI_FT8;
    const int nt = d->spec->ntones, nb = ft8 ? 3 : 2;
    static const uint8_t inv8[8] = {0, 1, 3, 2, 6, 4, 5, 7};   /* tone -> bits */
    static const uint8_t inv4[4] = {0, 1, 3, 2};
    int bi = 0;
    for (int s = 0; s < d->spec->nn && bi < 174; ++s) {
        bool data = ft8 ? !(s < 7 || (s >= 36 && s < 43) || s >= 72)
                        : !(s < 5 || (s >= 34 && s < 38) || (s >= 67 && s < 71) || s >= 100);
        if (!data) continue;
        const int t = c->t0 + d->tosr * s;
        if (t < 0 || t >= d->frames) { for (int j = 0; j < nb; ++j) llr[bi++] = 0; continue; }
        float v[8];
        for (int k = 0; k < nt; ++k) v[k] = s_amp[wfc(d, t, c->f0 + 2 * k)];
        for (int j = 0; j < nb; ++j) {
            float m1 = -1e30f, m0 = -1e30f;
            for (int k = 0; k < nt; ++k) {
                int bitsv = ft8 ? inv8[k] : inv4[k];
                if ((bitsv >> (nb - 1 - j)) & 1) { if (v[k] > m1) m1 = v[k]; }
                else if (v[k] > m0) m0 = v[k];
            }
            llr[bi++] = m1 - m0;
        }
    }
    float s1 = 0, s2 = 0;
    for (int i = 0; i < 174; ++i) { s1 += llr[i]; s2 += llr[i] * llr[i]; }
    float var = (s2 - s1 * s1 / 174.0f) / 174.0f;
    float k = var > 1e-12f ? sqrtf(24.0f / var) : 1.0f;
    for (int i = 0; i < 174; ++i) llr[i] *= k;
}

/* SNR in 2500 Hz: mean tone power over the noise floor near the signal.
   The floor is the median bin of the frames the signal spans, +1.6 dB (the
   median of exponentially distributed noise power sits ln 2 below its mean). */
static float snr_of(const digi_ftx_dec_t *d, const cand_t *c, const uint8_t *tones)
{
    double sig = 0;
    int ns = 0;
    for (int s = 0; s < d->spec->nn; ++s) {
        if (d->mode == DIGI_FT4 && (s == 0 || s == 104)) continue;
        const int t = c->t0 + d->tosr * s;
        if (t < 0 || t >= d->frames) continue;
        sig += s_pow[wfc(d, t, c->f0 + 2 * tones[s])];
        ns++;
    }
    if (!ns) return -30.0f;
    double pn = s_pow[floor_code(d, c)] / 0.6931, ps = sig / ns - pn;
    if (ps < pn * 1e-4) ps = pn * 1e-4;
    float snr = (float)(10.0 * log10(ps / pn) + 10.0 * log10(d->spec->tone_hz / 2500.0)) + DIGI_FTX_SNR_TRIM;
    return snr < -30.0f ? -30.0f : snr;
}

int digi_ftx_dec_run(digi_ftx_dec_t *d, digi_ftx_result_t *out, int max, digi_hash_t *h)
{
    uint32_t t_start = now_us();
    if (!d->wf) return 0;
    amp_init();
    const int sym = d->spec->sym;
    if (d->nbuf >= sym) {           /* a last single frame */
        frames_pair(d, d->buf, NULL);
        d->nbuf = 0;
    }
    const int nn = d->spec->nn, nt = d->spec->ntones;
    const int tmin = -6 * d->tosr, tmax = d->frames - d->tosr * nn + 6 * d->tosr;
    const int fmax_b = d->bins - 2 * nt;
    int fhi = d->bin_hi < fmax_b ? d->bin_hi : fmax_b;
    cand_t cand[MAX_CANDS];
    int nc = 0;
    d->last_candidates = d->last_ldpc = d->last_decodes = 0;
    if (tmax < tmin || fhi <= d->bin_lo) return 0;

    /* Score every (time, frequency) cell; keep local maxima, best first. */
    const int nT = tmax - tmin + 1;
    int16_t *grid = digi_alloc(sizeof(int16_t) * (size_t)nT * 3);
    if (!grid) return 0;
    int16_t *prev = grid, *cur = grid + nT, *next = grid + 2 * nT;
    for (int t = 0; t < nT; ++t) { prev[t] = -32768; cur[t] = (int16_t)sync_score(d, tmin + t, d->bin_lo); }
    for (int f = d->bin_lo; f <= fhi; ++f) {
        for (int t = 0; t < nT; ++t)
            next[t] = f + 1 <= fhi ? (int16_t)sync_score(d, tmin + t, f + 1) : -32768;
        for (int t = 0; t < nT; ++t) {
            int s = cur[t];
            if (s < DIGI_FTX_MIN_SYNC) continue;
            if (s < prev[t] || s < next[t]) continue;
            if ((t > 0 && s < cur[t - 1]) || (t + 1 < nT && s < cur[t + 1])) continue;
            /* Insert into the bounded list, kept sorted by score. */
            if (nc == MAX_CANDS && s <= cand[nc - 1].score) continue;
            int pos = nc < MAX_CANDS ? nc++ : nc - 1;
            while (pos > 0 && cand[pos - 1].score < s) { cand[pos] = cand[pos - 1]; --pos; }
            cand[pos] = (cand_t){(int16_t)(tmin + t), (int16_t)f, (int16_t)s};
        }
        int16_t *tmp = prev; prev = cur; cur = next; next = tmp;
    }
    digi_free(grid);
    d->last_candidates = nc;

    int found = 0;
    float llr[174];
    uint8_t bits[174];
    uint32_t yielded = now_us();
    for (int i = 0; i < nc && found < max; ++i) {
        yield_every(&yielded);
        bits_llr(d, &cand[i], llr);
        d->last_ldpc++;
        int fails = digi_ldpc_decode(llr, LDPC_ITERS, bits);
#if DIGI_FTX_RETRY
        /* Nearly there: the grid point beside it may be the better fit. */
        static const int8_t NB[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
        for (int k = 0; fails && fails <= DIGI_FTX_RETRY && i < DIGI_FTX_RETRY_TOP && k < 4; ++k) {
            cand_t c2 = cand[i];
            c2.t0 = (int16_t)(c2.t0 + NB[k][0]);
            c2.f0 = (int16_t)(c2.f0 + NB[k][1]);
            if (c2.f0 < 0 || c2.f0 + 2 * d->spec->ntones >= d->bins) continue;
            bits_llr(d, &c2, llr);
            d->last_ldpc++;
            fails = digi_ldpc_decode(llr, LDPC_ITERS, bits);
            if (!fails) cand[i] = c2;
        }
#endif
        if (fails) continue;
        uint8_t a91[12] = {0};
        for (int b = 0; b < 91; ++b) if (bits[b]) a91[b / 8] |= (uint8_t)(0x80u >> (b % 8));
        if (!digi_ftx_crc_ok(a91)) continue;
        uint8_t p[10];
        memcpy(p, a91, 10);
        p[9] &= 0xF8;
        if (d->mode == DIGI_FT4) {
            for (int k = 0; k < 10; ++k) p[k] ^= digi_ft4_xor[k];
            p[9] &= 0xF8;
        }
        bool zero = true;
        for (int k = 0; k < 10; ++k) if (p[k]) zero = false;
        if (zero) continue;
        bool dup = false;
        for (int k = 0; k < found; ++k) if (!memcmp(out[k].payload, p, 10)) dup = true;
        if (dup) continue;
        digi_ftx_result_t *r = &out[found];
        memset(r, 0, sizeof(*r));
        memcpy(r->payload, p, 10);
        if (!digi_msg_unpack(p, r->text, sizeof(r->text), h, &r->f)) continue;
        if (!r->text[0]) continue;
        uint8_t tones[105];
        digi_ftx_tones(d->mode, p, tones);
        r->snr_db = snr_of(d, &cand[i], tones);
        r->dt_s = (float)cand[i].t0 * (float)d->step / DIGI_FTX_RATE - 0.5f;
        r->freq_hz = (float)cand[i].f0 * digi_ftx_dec_bin_hz(d);
        r->score = cand[i].score;
        found++;
    }
    d->last_decodes = found;
    d->last_us = now_us() - t_start;
    return found;
}
