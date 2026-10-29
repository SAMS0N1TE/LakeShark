/* WSPR codec and decoder. See digi_wspr.h for the protocol references. */
#include "digi_wspr.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
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

#define POLY1 0xF2D05351u
#define POLY2 0xE4613C47u
#define NBITS 81            /* 50 message bits + 31 tail */
#define DECIM 32            /* 12000 -> 375 Hz           */
#define NTAPS 512
#define NFFT  256           /* one symbol at 375 Hz      */
#define HOP   64            /* a quarter symbol          */
#define CODE_REF 96

static const uint8_t SYNC[162] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,0,0,
    0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,0,0,1,0,
    1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,1,0,0,0,0,0,1,0,1,0,0,1,1,0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,0,0,
};

/* ---- message ------------------------------------------------------------ */

static int cval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 36;  /* space */
}

bool digi_wspr_pack(const char *call, const char *grid4, int dbm, uint8_t data[7])
{
    char c[7] = "      ";
    size_t n = strlen(call);
    if (n < 3 || n > 6) return false;
    char up[8];
    for (size_t i = 0; i <= n; ++i) up[i] = (char)toupper((unsigned char)call[i]);
    if (isdigit((unsigned char)up[2])) memcpy(c, up, n);
    else if (isdigit((unsigned char)up[1]) && n <= 5) memcpy(c + 1, up, n);
    else return false;
    if (cval(c[1]) > 35 || !isdigit((unsigned char)c[2])) return false;
    for (int i = 3; i < 6; ++i) if (c[i] != ' ' && !isupper((unsigned char)c[i])) return false;
    uint32_t nc = (uint32_t)cval(c[0]);
    nc = nc * 36 + (uint32_t)cval(c[1]);
    nc = nc * 10 + (uint32_t)cval(c[2]);
    for (int i = 3; i < 6; ++i) nc = nc * 27 + (uint32_t)(cval(c[i]) - 10);
    char g[5];
    for (int i = 0; i < 4; ++i) g[i] = (char)toupper((unsigned char)grid4[i]);
    if (strlen(grid4) < 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' ||
        !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;
    if (dbm < 0 || dbm > 60) return false;
    uint32_t loc = (uint32_t)((179 - 10 * (g[0] - 'A') - (g[2] - '0')) * 180 + 10 * (g[1] - 'A') + (g[3] - '0'));
    uint32_t m = loc * 128 + (uint32_t)(dbm + 64);
    memset(data, 0, 7);
    uint64_t v = ((uint64_t)nc << 22) | m;      /* 50 bits */
    v <<= 6;                                    /* left-align in 56 */
    for (int i = 0; i < 7; ++i) data[i] = (uint8_t)(v >> (48 - 8 * i));
    return true;
}

bool digi_wspr_unpack(const uint8_t data[7], char call[12], char grid[7], int *dbm)
{
    uint64_t v = 0;
    for (int i = 0; i < 7; ++i) v = (v << 8) | data[i];
    v >>= 6;
    uint32_t nc = (uint32_t)(v >> 22), m = (uint32_t)(v & 0x3FFFFF);
    static const char A37[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
    char c[7];
    c[6] = 0;
    for (int i = 5; i >= 3; --i) { c[i] = (char)(nc % 27 == 26 ? ' ' : 'A' + nc % 27); nc /= 27; }
    c[2] = (char)('0' + nc % 10); nc /= 10;
    c[1] = A37[nc % 36]; nc /= 36;
    if (nc > 36) { snprintf(call, 12, "?"); grid[0] = 0; *dbm = 0; return false; }
    c[0] = A37[nc];
    int k = 0;
    for (int i = 0; i < 6; ++i) if (c[i] != ' ') call[k++] = c[i];
    call[k] = 0;
    int p = (int)(m & 127) - 64;
    uint32_t loc = m >> 7;
    *dbm = p;
    if (loc >= 32400) { snprintf(grid, 7, "?"); return false; }
    uint32_t q = loc / 180, r = loc % 180;
    int l0 = (int)(179 - q) / 10, l2 = (int)(179 - q) % 10;
    grid[0] = (char)('A' + l0); grid[1] = (char)('A' + r / 10);
    grid[2] = (char)('0' + l2); grid[3] = (char)('0' + r % 10); grid[4] = 0;
    /* Type 1 powers end in 0, 3 or 7. Others mean a compound call or a
       six-character locator travels in a second message. */
    int last = p % 10;
    if (p < 0 || p > 60 || (last != 0 && last != 3 && last != 7)) return false;
    if (l0 > 17) return false;
    return true;
}

static int parity32(uint32_t x)
{
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(x & 1);
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* perm[p] = channel position of parity bit p. */
static void interleave_map(uint8_t perm[162])
{
    int p = 0;
    for (int k = 0; k < 256 && p < 162; ++k) {
        uint8_t j = rev8((uint8_t)k);
        if (j < 162) perm[p++] = j;
    }
}

void digi_wspr_encode(const uint8_t data[7], uint8_t sym[162])
{
    uint8_t par[162], perm[162];
    uint32_t reg = 0;
    for (int i = 0; i < NBITS; ++i) {
        int bit = i < 50 ? (data[i / 8] >> (7 - i % 8)) & 1 : 0;
        reg = (reg << 1) | (uint32_t)bit;
        par[2 * i] = (uint8_t)parity32(reg & POLY1);
        par[2 * i + 1] = (uint8_t)parity32(reg & POLY2);
    }
    interleave_map(perm);
    for (int p = 0; p < 162; ++p) sym[perm[p]] = par[p];
    for (int j = 0; j < 162; ++j) sym[j] = (uint8_t)(SYNC[j] + 2 * sym[j]);
}

void digi_wspr_synth(const uint8_t sym[162], float f_hz, float amp, float *out, long n, long start)
{
    double ph = 0;
    for (long t = 0; t < 162L * DIGI_WSPR_SPS; ++t) {
        double f = f_hz + DIGI_WSPR_TONE_HZ * sym[t / DIGI_WSPR_SPS];
        ph += 6.283185307179586 * f / DIGI_WSPR_RATE;
        long o = start + t;
        if (o >= 0 && o < n) out[o] += amp * (float)sin(ph);
    }
}

/* ---- sequential decoder -------------------------------------------------- */

/* Fano branch metric x10 for a bit hypothesis given a soft bit: log2 of the
   posterior over one half, less the code rate (Lin and Costello, ch. 13). */
EXT_RAM_BSS_ATTR static int s_met[2][256];
static void metric_init(void)
{
    if (s_met[0][0]) return;
    for (int r = 0; r < 256; ++r) {
        double L = (r - 127.5) / 16.0;          /* ln P(1)/P(0) */
        double p1 = 1.0 / (1.0 + exp(-L)), p0 = 1.0 - p1;
        s_met[0][r] = (int)lround(10.0 * (log2(2.0 * p0 + 1e-12) - 0.5));
        s_met[1][r] = (int)lround(10.0 * (log2(2.0 * p1 + 1e-12) - 0.5));
    }
}

typedef struct { uint32_t state; int gamma; int tm[2]; uint8_t best, i; } fano_node_t;

bool digi_wspr_fano(const uint8_t chan[162], uint8_t data[7], long max_cycles, long *cycles_out)
{
    metric_init();
    uint8_t perm[162], soft[162];
    interleave_map(perm);
    for (int p = 0; p < 162; ++p) soft[p] = chan[perm[p]];
    fano_node_t node[NBITS + 1];
    uint8_t path[NBITS];
    const int delta = 20;
    int T = 0, k = 0;
    long cyc = 0;
    /* Branch metrics of node k, best first. Tail nodes have one branch. */
#define BRANCHES(k) do {                                                        \
        int m_[2];                                                              \
        for (int b_ = 0; b_ < 2; ++b_) {                                        \
            uint32_t s_ = (node[k].state << 1) | (uint32_t)b_;                  \
            m_[b_] = s_met[parity32(s_ & POLY1)][soft[2 * (k)]] +               \
                     s_met[parity32(s_ & POLY2)][soft[2 * (k) + 1]];            \
        }                                                                       \
        if ((k) >= 50) { node[k].best = 0; node[k].tm[0] = m_[0]; node[k].tm[1] = -1000000; } \
        else if (m_[1] > m_[0]) { node[k].best = 1; node[k].tm[0] = m_[1]; node[k].tm[1] = m_[0]; } \
        else { node[k].best = 0; node[k].tm[0] = m_[0]; node[k].tm[1] = m_[1]; } \
        node[k].i = 0;                                                          \
    } while (0)
    node[0].state = 0;
    node[0].gamma = 0;
    BRANCHES(0);
    while (cyc++ < max_cycles) {
        int ng = node[k].gamma + node[k].tm[node[k].i];
        if (ng >= T) {
            /* First visit: tighten the threshold as far as it will go. */
            if (node[k].gamma < T + delta)
                while (ng >= T + delta) T += delta;
            uint8_t bit = (uint8_t)(node[k].i ? !node[k].best : node[k].best);
            path[k] = bit;
            node[k + 1].state = (node[k].state << 1) | bit;
            node[k + 1].gamma = ng;
            if (++k == NBITS) break;
            BRANCHES(k);
            continue;
        }
        for (;;) {   /* look back */
            if (k == 0 || node[k - 1].gamma < T) {
                T -= delta;
                node[k].i = 0;
                break;
            }
            --k;
            if (k < 50 && node[k].i == 0) { node[k].i = 1; break; }
        }
    }
#undef BRANCHES
    if (cycles_out) *cycles_out = cyc;
    if (k != NBITS) return false;
    memset(data, 0, 7);
    for (int i = 0; i < 50; ++i)
        if (path[i]) data[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    return true;
}

/* ---- decoder ------------------------------------------------------------- */

bool digi_wspr_dec_init(digi_wspr_dec_t *d)
{
    memset(d, 0, sizeof(*d));
    d->bb_max = 120L * DIGI_WSPR_BB_RATE;
    d->bb = digi_alloc(sizeof(digi_cpx) * (size_t)d->bb_max);
    if (!d->bb || !digi_fft_init(&d->fft, NFFT)) { digi_wspr_dec_free(d); return false; }
    /* Blackman-windowed sinc low-pass, 140 Hz, moved up to the 1500 Hz
       window: y = sum g[k] x[n-k] with g[k] = h[k] e^(j pi k / 4). */
    const double fc = 140.0 / DIGI_WSPR_RATE;
    double sum = 0, h[NTAPS];
    for (int k = 0; k < NTAPS; ++k) {
        double t = k - (NTAPS - 1) / 2.0;
        double s = fabs(t) < 1e-9 ? 2 * fc : sin(6.283185307179586 * fc * t) / (3.141592653589793 * t);
        double w = 0.42 - 0.5 * cos(6.283185307179586 * k / (NTAPS - 1)) + 0.08 * cos(12.566370614359172 * k / (NTAPS - 1));
        h[k] = s * w;
        sum += h[k];
    }
    for (int k = 0; k < NTAPS; ++k) {
        double a = 3.141592653589793 * k / 4.0;
        d->g[k] = (digi_cpx){(float)(h[k] / sum * cos(a)), (float)(h[k] / sum * sin(a))};
    }
    return true;
}

void digi_wspr_dec_free(digi_wspr_dec_t *d)
{
    digi_free(d->bb);
    digi_fft_free(&d->fft);
    d->bb = NULL;
}

void digi_wspr_dec_reset(digi_wspr_dec_t *d)
{
    d->nbb = 0;
    d->phase = 0;
}

void digi_wspr_dec_feed(digi_wspr_dec_t *d, const float *x, int n)
{
    for (int i = 0; i < n; ++i) {
        d->hist[d->hpos] = x[i];
        d->hpos = (d->hpos + 1) & (NTAPS - 1);
        if (++d->phase < DECIM) continue;
        d->phase = 0;
        if (d->nbb >= d->bb_max) continue;
        float re = 0, im = 0;
        int p = (d->hpos - 1) & (NTAPS - 1);
        for (int k = 0; k < NTAPS; ++k) {
            float v = d->hist[p];
            re += d->g[k].re * v; im += d->g[k].im * v;
            p = (p - 1) & (NTAPS - 1);
        }
        d->bb[d->nbb++] = (digi_cpx){re, im};
    }
}

void digi_wspr_dec_feed_i16(digi_wspr_dec_t *d, const int16_t *x, int n)
{
    float t[256];
    while (n > 0) {
        int k = n < 256 ? n : 256;
        for (int i = 0; i < k; ++i) t[i] = x[i] * (1.0f / 32768.0f);
        digi_wspr_dec_feed(d, t, k);
        x += k; n -= k;
    }
}

/* Spectra: frame q (hop 64), half-bin shift h, bin b (fftshifted, b = 128 is
   0 Hz), as 0.5 dB codes. */
typedef struct {
    uint8_t *c;
    int q;
    float pow_of[256];
} spec_t;
#define SPEC(s, q, fh) ((s)->c[((size_t)(q) * 2 + ((fh) & 1)) * NFFT + ((fh) >> 1)])

static bool spectra(digi_wspr_dec_t *d, spec_t *s)
{
    s->q = d->nbb >= NFFT ? (int)((d->nbb - NFFT) / HOP + 1) : 0;
    if (s->q < 4 * 161 + 1) return false;
    s->c = digi_alloc((size_t)s->q * 2 * NFFT);
    digi_cpx *in = digi_alloc(sizeof(digi_cpx) * NFFT), *out = digi_alloc(sizeof(digi_cpx) * NFFT);
    if (!s->c || !in || !out) { digi_free(s->c); digi_free(in); digi_free(out); s->c = NULL; return false; }
    float ref = 0;
    bool ref_set = false;
    digi_cpx rot[NFFT];   /* e^(-j pi n / NFFT): a half-bin shift */
    for (int n = 0; n < NFFT; ++n) rot[n] = (digi_cpx){cosf(-3.14159265f * n / NFFT), sinf(-3.14159265f * n / NFFT)};
    for (int q = 0; q < s->q; ++q)
        for (int h = 0; h < 2; ++h) {
            const digi_cpx *x = d->bb + (size_t)q * HOP;
            for (int n = 0; n < NFFT; ++n) {
                if (!h) { in[n] = x[n]; continue; }
                const float c = rot[n].re, sn = rot[n].im;
                in[n] = (digi_cpx){x[n].re * c - x[n].im * sn, x[n].re * sn + x[n].im * c};
            }
            digi_fft_run(&d->fft, in, out);
            if (!ref_set) {
                /* Median of the first frame's band as the code reference. */
                float v[NFFT];
                for (int b = 0; b < NFFT; ++b) v[b] = 10.0f * log10f(out[b].re * out[b].re + out[b].im * out[b].im + 1e-30f);
                for (int i = 1; i < NFFT; ++i)
                    for (int j = i; j > 0 && v[j] < v[j - 1]; --j) { float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
                ref = v[NFFT / 2];
                ref_set = true;
            }
            uint8_t *row = s->c + ((size_t)q * 2 + h) * NFFT;
            for (int b = 0; b < NFFT; ++b) {
                const digi_cpx z = out[(b + NFFT / 2) % NFFT];
                float db = 10.0f * log10f(z.re * z.re + z.im * z.im + 1e-30f);
                float c = 2.0f * (db - ref) + CODE_REF;
                row[b] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c + 0.5f);
            }
        }
    digi_free(in);
    digi_free(out);
    for (int i = 0; i < 256; ++i) s->pow_of[i] = powf(10.0f, (i - CODE_REF) / 20.0f);
    return true;
}

/* Normalised sync: the sync bit is the LSB of the tone, so tones 1 and 3
   against 0 and 2 must follow the sync vector. */
static float sync_metric(const spec_t *s, int q0, int fh, int drift)
{
    float num = 0, den = 0;
    for (int i = 0; i < 162; ++i) {
        int f = fh + (int)lroundf((float)drift * (float)(i - 81) / 162.0f);
        int q = q0 + 4 * i;
        if (f < 0 || f + 6 >= 2 * NFFT || q < 0 || q >= s->q) continue;
        float p0 = s->pow_of[SPEC(s, q, f)], p1 = s->pow_of[SPEC(s, q, f + 2)];
        float p2 = s->pow_of[SPEC(s, q, f + 4)], p3 = s->pow_of[SPEC(s, q, f + 6)];
        float v = (p1 + p3) - (p0 + p2);
        num += SYNC[i] ? v : -v;
        den += p0 + p1 + p2 + p3;
    }
    return den > 0 ? num / den : 0;
}

typedef struct { int q0, fh, drift; float m; } wcand_t;

/* A call shaped like a real one: a letter in the first two characters, a
   digit, then a letter. Random paths through the code rarely look so. */
static bool plausible_call(const char *c)
{
    size_t n = strlen(c);
    if (n < 3) return false;
    int d = isdigit((unsigned char)c[2]) ? 2 : isdigit((unsigned char)c[1]) ? 1 : -1;
    if (d < 0 || d + 1 >= (int)n || !isupper((unsigned char)c[d + 1])) return false;
    for (int i = 0; i < d; ++i) if (isupper((unsigned char)c[i])) return true;
    return false;
}

static int noise_code(const spec_t *s)
{
    int hist[256] = {0}, cnt = 0;
    for (int q = 0; q < s->q; q += 7)
        for (int f = 2 * (128 - 75); f < 2 * (128 + 75); f += 5) { hist[SPEC(s, q, f)]++; cnt++; }
    int acc = 0;
    for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc * 2 >= cnt) return i; }
    return CODE_REF;
}

int digi_wspr_dec_run(digi_wspr_dec_t *d, digi_wspr_result_t *out, int max)
{
    uint32_t t0 = now_us();
    spec_t s;
    memset(&s, 0, sizeof(s));
    d->last_candidates = 0;
    if (!spectra(d, &s)) return 0;
    /* Tone 0 between 1400 and 1596 Hz of audio, start 0..5 s into the cycle. */
    const int fh_lo = 2 * (128 - 69), fh_hi = 2 * (128 + 66);
    int qmax = (int)(5.0f * DIGI_WSPR_BB_RATE / HOP);
    if (qmax > s.q - 4 * 161 - 1) qmax = s.q - 4 * 161 - 1;
    const int nf = fh_hi - fh_lo + 1, nq = qmax + 1;
    float *grid = digi_alloc(sizeof(float) * (size_t)nf * (size_t)nq);
    if (!grid) { digi_free(s.c); return 0; }
    for (int f = 0; f < nf; ++f)
        for (int q = 0; q < nq; ++q) grid[f * nq + q] = sync_metric(&s, q, fh_lo + f, 0);
    enum { MAXC = 24 };
    wcand_t c[MAXC];
    int nc = 0;
    for (int f = 0; f < nf; ++f)
        for (int q = 0; q < nq; ++q) {
            float m = grid[f * nq + q];
            if (m < 0.06f) continue;
            bool peak = true;
            for (int df = -3; df <= 3 && peak; ++df)
                for (int dq = -2; dq <= 2 && peak; ++dq) {
                    int ff = f + df, qq = q + dq;
                    if ((df || dq) && ff >= 0 && ff < nf && qq >= 0 && qq < nq && grid[ff * nq + qq] > m) peak = false;
                }
            if (!peak) continue;
            if (nc == MAXC && m <= c[nc - 1].m) continue;
            int pos = nc < MAXC ? nc++ : nc - 1;
            while (pos > 0 && c[pos - 1].m < m) { c[pos] = c[pos - 1]; --pos; }
            c[pos] = (wcand_t){q, fh_lo + f, 0, m};
        }
    digi_free(grid);
    d->last_candidates = nc;

    const float noise = s.pow_of[noise_code(&s)] / 0.6931f;
    int found = 0;
    uint32_t yielded = now_us();
    for (int i = 0; i < nc && found < max; ++i) {
        yield_every(&yielded);
        /* Refine time, frequency and drift around the peak. */
        wcand_t b = c[i];
        for (int dr = -8; dr <= 8; ++dr)
            for (int df = -2; df <= 2; ++df)
                for (int dq = -2; dq <= 2; ++dq) {
                    int q = c[i].q0 + dq;
                    if (q < 0 || q + 4 * 161 >= s.q) continue;
                    float m = sync_metric(&s, q, c[i].fh + df, dr);
                    if (m > b.m) b = (wcand_t){q, c[i].fh + df, dr, m};
                }
        /* Soft data bits: tone (2 + sync) against tone (sync), in amplitude. */
        float sv[162], s2 = 0;
        for (int k = 0; k < 162; ++k) {
            int f = b.fh + (int)lroundf((float)b.drift * (float)(k - 81) / 162.0f);
            int q = b.q0 + 4 * k;
            float a0 = sqrtf(s.pow_of[SPEC(&s, q, f + 2 * SYNC[k])]);
            float a1 = sqrtf(s.pow_of[SPEC(&s, q, f + 2 * (SYNC[k] + 2))]);
            sv[k] = a1 - a0;
            s2 += sv[k] * sv[k];
        }
        float sd = sqrtf(s2 / 162.0f) + 1e-12f;
        uint8_t soft[162];
        for (int k = 0; k < 162; ++k) {
            float L = 3.0f * sv[k] / sd;               /* ln P(1)/P(0) */
            float r = 127.5f + 16.0f * L;
            soft[k] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        }
        uint8_t data[7];
        if (!digi_wspr_fano(soft, data, 200000L, NULL)) continue;
        digi_wspr_result_t r;
        memset(&r, 0, sizeof(r));
        r.type1 = digi_wspr_unpack(data, r.call, r.grid, &r.dbm);
        if (!plausible_call(r.call)) continue;
        /* Re-encode: the symbols it implies must stand out of the noise. */
        uint8_t sym[162];
        digi_wspr_encode(data, sym);
        int agree = 0;
        for (int k = 0; k < 162; ++k) agree += (sv[k] > 0) == (sym[k] >> 1);
        r.agree = agree;
        double ps = 0;
        for (int k = 0; k < 162; ++k) {
            int f = b.fh + (int)lroundf((float)b.drift * (float)(k - 81) / 162.0f);
            ps += s.pow_of[SPEC(&s, b.q0 + 4 * k, f + 2 * sym[k])];
        }
        ps = ps / 162.0 - noise;
        if (ps < noise * 1e-4) ps = noise * 1e-4;
        r.snr_db = (float)(10.0 * log10(ps / noise) + 10.0 * log10((375.0 / NFFT) / 2500.0));
        const float df = 375.0f / NFFT;
        r.freq_hz = DIGI_WSPR_CENTER + ((float)b.fh * 0.5f - 128.0f) * df + 1.5f * df;
        r.drift_hz = (float)b.drift * 0.5f * df;
        /* FIR delay is (NTAPS-1)/2 input samples. */
        r.dt_s = (float)b.q0 * HOP / DIGI_WSPR_BB_RATE - 1.0f - (NTAPS - 1) / 2.0f / DIGI_WSPR_RATE;
        if (r.type1) snprintf(r.text, sizeof(r.text), "%s %s %d", r.call, r.grid, r.dbm);
        else snprintf(r.text, sizeof(r.text), "%s (type 2/3)", r.call);
        /* One transmitter per few hertz at a time: a second decode on top of a
           stronger one is the decoder chasing that signal's sidelobes. */
        bool dup = false;
        for (int k = 0; k < found; ++k) {
            bool same = !strcmp(out[k].text, r.text);
            bool over = fabsf(out[k].freq_hz - r.freq_hz) < 4.5f && fabsf(out[k].dt_s - r.dt_s) < 1.5f;
            if (same || over) {
                dup = true;
                if (r.snr_db > out[k].snr_db && (same || r.agree > out[k].agree)) out[k] = r;
            }
        }
        if (!dup) out[found++] = r;
    }
    digi_free(s.c);
    d->last_us = now_us() - t0;
    return found;
}
