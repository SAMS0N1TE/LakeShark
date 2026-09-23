/* See p25_call_gen.h. Every frame here is written in the order the decoder
 * reads it - p25p1_hdu.c, p25p1_ldu1.c, p25p1_ldu2.c, p25p1_tdu.c,
 * process_IMBE - so a change to one without the other shows up as a test
 * that stops decoding, not as a generator that quietly agrees with itself. */
#include "p25_call_gen.h"

#include <math.h>
#include <string.h>

#include "dsd.h"
#include "p25p1_check_hdu.h"
#include "p25p1_check_ldu.h"
#include "p25p1_const.h"
/* bch_encode is private to the NID module; the test target includes it here
   and does not link it separately (as test_p25_cqpsk_baseline does). */
#include "p25p1_check_nid.c"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SYM_RATE    4800.0
#define SAMPLE_RATE 240000.0
#define STATUS_DIBIT 2          /* any value: the decoder ignores status */

enum { DUID_HDU = 0x0, DUID_TDU = 0x3, DUID_LDU1 = 0x5, DUID_LDU2 = 0xA };

/* ---------------------------------------------------------------- emitter */

typedef struct {
    int *sym;
    int n, max;
    int status_count;   /* the decoder's own counter: status when it hits 35 */
} emit_t;

static int dibit_symbol(int dibit)
{
    static const int LEVELS[4] = { 1, 3, -1, -3 };
    return LEVELS[dibit & 3];
}

static void raw(emit_t *e, int dibit)
{
    if (e->n < e->max) e->sym[e->n] = dibit_symbol(dibit);
    e->n++;
}

/* read_dibit / process_IMBE: a status symbol takes the slot before every
   36th data dibit. */
static void data(emit_t *e, int dibit)
{
    if (e->status_count == 35) {
        raw(e, STATUS_DIBIT);
        e->status_count = 1;
    } else {
        e->status_count++;
    }
    raw(e, dibit);
}

static void bits(emit_t *e, const char *b, int nbits)
{
    for (int i = 0; i < nbits; i += 2) data(e, (b[i] << 1) | b[i + 1]);
}

/* ---------------------------------------------------------- sync and NID */

static const char SYNC[] = "111113113311333313133333";

static void frame_head(emit_t *e, uint16_t nac, int duid, int corrupt)
{
    for (int i = 0; i < 24; i++) raw(e, SYNC[i] == '1' ? 1 : 3);

    int data16[BCH_KK], parity[BCH_RR];
    char cw[BCH_NN];
    for (int i = 0; i < 12; i++) data16[i] = (nac >> (11 - i)) & 1;
    for (int i = 0; i < 4; i++) data16[12 + i] = (duid >> (3 - i)) & 1;
    bch_encode(data16, parity);
    for (int i = 0; i < BCH_KK; i++) cw[i] = (char)data16[i];
    for (int i = 0; i < BCH_RR; i++) cw[BCH_KK + i] = (char)parity[i];
    if (corrupt)            /* beyond BCH's reach: this frame is lost */
        for (int i = 0; i < 40; i += 2) cw[i] ^= 1;

    for (int i = 0; i < 11; i++) raw(e, (cw[i * 2] << 1) | cw[i * 2 + 1]);
    raw(e, STATUS_DIBIT);
    for (int i = 0; i < 20; i++) raw(e, (cw[22 + i * 2] << 1) | cw[23 + i * 2]);
    raw(e, cw[62] << 1);
    e->status_count = 21;   /* where every frame body starts it */
}

/* --------------------------------------------------------------- IMBE FEC */

static const int GOLAY_GEN[12] = {
    0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc, 0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475
};
static const int HAMMING_GEN[4] = { 0x7f08, 0x78e4, 0x66d2, 0x55b1 };

/* mbe_golay2312 reads in[22..11] as data and in[10..0] as parity. */
static void golay2312(char w[23])
{
    int ecc = 0;
    for (int i = 0; i < 12; i++) if (w[22 - i]) ecc ^= GOLAY_GEN[i];
    for (int k = 0; k <= 10; k++) w[k] = (char)((ecc >> k) & 1);
}

static int parity15(int v)
{
    int p = 0;
    for (int i = 0; i < 15; i++) p ^= (v >> i) & 1;
    return p;
}

/* mbe_hamming1511: data in[14..4], parity in[3..0]; the parity is the value
   whose syndrome the decoder computes as zero. */
static void hamming1511(char w[15])
{
    for (int p = 0; p < 16; p++) {
        int block = 0;
        for (int k = 0; k < 4; k++) w[k] = (char)((p >> k) & 1);
        for (int i = 14; i >= 0; i--) block = (block << 1) | w[i];
        int ok = 1;
        for (int k = 0; k < 4; k++) if (parity15(block & HAMMING_GEN[k])) ok = 0;
        if (ok) return;
    }
}

/* The inverse of mbe_eccImbe7200x4400C0, mbe_demodulateImbe7200x4400Data and
   mbe_eccImbe7200x4400Data, in that order. */
static void imbe_encode(const uint8_t in[P25_CALL_IMBE_BYTES], char fr[8][23])
{
    char d[88];
    for (int b = 0; b < 88; b++) d[b] = (char)((in[b >> 3] >> (7 - (b & 7))) & 1);
    memset(fr, 0, 8 * 23);
    int k = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 22; j > 10; j--) fr[i][j] = d[k++];
        golay2312(fr[i]);
    }
    for (int i = 4; i < 7; i++) {
        for (int j = 14; j >= 4; j--) fr[i][j] = d[k++];
        hamming1511(fr[i]);
    }
    for (int j = 6; j >= 0; j--) fr[7][j] = d[k++];

    unsigned pr[115];
    unsigned seed = 0;
    for (int i = 22; i >= 11; i--) seed = (seed << 1) | (unsigned)fr[0][i];
    pr[0] = 16 * seed;
    for (int i = 1; i < 115; i++) pr[i] = (173 * pr[i - 1] + 13849) % 65536;
    for (int i = 1; i < 115; i++) pr[i] /= 32768;
    int n = 1;
    for (int i = 1; i < 4; i++) for (int j = 22; j >= 0; j--) fr[i][j] ^= (char)pr[n++];
    for (int i = 4; i < 7; i++) for (int j = 14; j >= 0; j--) fr[i][j] ^= (char)pr[n++];
}

static void imbe(emit_t *e, const uint8_t in[P25_CALL_IMBE_BYTES])
{
    char fr[8][23];
    imbe_encode(in, fr);
    for (int j = 0; j < 72; j++)
        data(e, (fr[iW[j]][iX[j]] << 1) | fr[iY[j]][iZ[j]]);
}

/* ------------------------------------------------------------ hex words */

static void put_bits(char (*words)[6], int first_word, int nwords, int bit, uint32_t v, int n)
{
    (void)nwords;
    for (int i = 0; i < n; i++, bit++)
        words[first_word - bit / 6][bit % 6] = (char)((v >> (n - 1 - i)) & 1);
}

static void hamming_word(emit_t *e, const char w[6])
{
    char p[4];
    encode_hamming_10_6_3((char *)w, p);
    bits(e, w, 6);
    bits(e, p, 4);
}

static void golay_word(emit_t *e, const char w[6])
{
    char p[12];
    encode_golay_24_6((char *)w, p);
    bits(e, w, 6);
    bits(e, p, 12);
}

static void lsd(emit_t *e)
{
    for (int i = 0; i < 16; i++) data(e, 0);
}

/* ---------------------------------------------------------------- frames */

static void hdu(emit_t *e, const p25_call_t *c)
{
    char d[20][6], p[16][6];
    memset(d, 0, sizeof(d));
    /* 120 bits from word 19 down: MI 72, MFID 8, ALGID 8, KID 16, TGID 16 */
    put_bits(d, 19, 20, 0, 0, 24); put_bits(d, 19, 20, 24, 0, 24); put_bits(d, 19, 20, 48, 0, 24);
    put_bits(d, 19, 20, 72, 0, 8);
    put_bits(d, 19, 20, 80, c->algid, 8);
    put_bits(d, 19, 20, 88, c->kid, 16);
    put_bits(d, 19, 20, 104, c->talkgroup, 16);
    encode_reedsolomon_36_20_17((char *)d, (char *)p);
    frame_head(e, c->nac, DUID_HDU, 0);
    for (int i = 19; i >= 0; i--) golay_word(e, d[i]);
    for (int i = 15; i >= 0; i--) golay_word(e, p[i]);
    for (int i = 0; i < 5; i++) raw(e, 0);      /* skipDibit(5) */
    raw(e, STATUS_DIBIT);
}

static void ldu1(emit_t *e, const p25_call_t *c, const uint8_t (*v)[P25_CALL_IMBE_BYTES], int corrupt)
{
    char d[12][6], p[12][6];
    memset(d, 0, sizeof(d));
    /* 72 bits from word 11 down: LCF 0 (group voice), MFID 0, service
       options 0, reserved 0, TGID 16, source 24. */
    put_bits(d, 11, 12, 0, 0, 32);
    put_bits(d, 11, 12, 32, c->talkgroup, 16);
    put_bits(d, 11, 12, 48, c->source, 24);
    encode_reedsolomon_24_12_13((char *)d, (char *)p);
    frame_head(e, c->nac, DUID_LDU1, corrupt);
    imbe(e, v[0]); imbe(e, v[1]);
    for (int i = 11; i >= 8; i--) hamming_word(e, d[i]);
    imbe(e, v[2]);
    for (int i = 7; i >= 4; i--) hamming_word(e, d[i]);
    imbe(e, v[3]);
    for (int i = 3; i >= 0; i--) hamming_word(e, d[i]);
    imbe(e, v[4]);
    for (int i = 11; i >= 8; i--) hamming_word(e, p[i]);
    imbe(e, v[5]);
    for (int i = 7; i >= 4; i--) hamming_word(e, p[i]);
    imbe(e, v[6]);
    for (int i = 3; i >= 0; i--) hamming_word(e, p[i]);
    imbe(e, v[7]);
    lsd(e);
    imbe(e, v[8]);
    raw(e, STATUS_DIBIT);
}

static void ldu2(emit_t *e, const p25_call_t *c, const uint8_t (*v)[P25_CALL_IMBE_BYTES])
{
    char d[16][6], p[8][6];
    memset(d, 0, sizeof(d));
    /* 96 bits from word 15 down: MI 72, ALGID 8, KID 16 */
    put_bits(d, 15, 16, 0, 0x123456, 24); put_bits(d, 15, 16, 24, 0x789abc, 24);
    put_bits(d, 15, 16, 48, 0xdef012, 24);
    put_bits(d, 15, 16, 72, c->algid, 8);
    put_bits(d, 15, 16, 80, c->kid, 16);
    encode_reedsolomon_24_16_9((char *)d, (char *)p);
    frame_head(e, c->nac, DUID_LDU2, 0);
    imbe(e, v[0]); imbe(e, v[1]);
    for (int i = 15; i >= 12; i--) hamming_word(e, d[i]);
    imbe(e, v[2]);
    for (int i = 11; i >= 8; i--) hamming_word(e, d[i]);
    imbe(e, v[3]);
    for (int i = 7; i >= 4; i--) hamming_word(e, d[i]);
    imbe(e, v[4]);
    for (int i = 3; i >= 0; i--) hamming_word(e, d[i]);
    imbe(e, v[5]);
    for (int i = 7; i >= 4; i--) hamming_word(e, p[i]);
    imbe(e, v[6]);
    for (int i = 3; i >= 0; i--) hamming_word(e, p[i]);
    imbe(e, v[7]);
    lsd(e);
    imbe(e, v[8]);
    raw(e, STATUS_DIBIT);
}

static void tdu(emit_t *e, const p25_call_t *c)
{
    frame_head(e, c->nac, DUID_TDU, 0);
    for (int i = 0; i < 14; i++) data(e, 0);
    raw(e, STATUS_DIBIT);
}

int p25_call_symbols(const p25_call_t *call, const uint8_t (*v)[P25_CALL_IMBE_BYTES],
                     int n_imbe, int *sym, int sym_max)
{
    if (!call || !v || n_imbe <= 0 || n_imbe % 18) return -1;
    emit_t e = { sym, 0, sym_max, 21 };
    if (call->hdu) hdu(&e, call);
    for (int f = 0, pair = 1; f < n_imbe; f += 18, pair++) {
        ldu1(&e, call, v + f, call->corrupt_ldu1 == pair);
        ldu2(&e, call, v + f + 9);
    }
    if (call->tdu) tdu(&e, call);
    return e.n <= sym_max ? e.n : -1;
}

int p25_call_frame_start(const p25_call_t *call, int frame)
{
    int at = 0;
    if (call->hdu) {
        if (frame == 0) return 0;
        at += P25_CALL_HDU_SYMBOLS;
        frame--;
    }
    return at + frame * P25_CALL_LDU_SYMBOLS;
}

/* ---------------------------------------------------------------- render */

int p25_call_render_bytes(int n_sym, const p25_call_channel_t *ch)
{
    double s = n_sym / SYM_RATE + ch->lead_s + ch->tail_s;
    return 2 * (int)(s * SAMPLE_RATE);
}

/* The C4FM frequency pulse of TIA-102.BAAA: a raised-cosine Nyquist filter
   (alpha 0.2) followed by the shaping filter pi f T / sin(pi f T) that makes
   C4FM receivable by a CQPSK-style receiver. Built once, by inverse DFT of
   that response, at 50 samples a symbol over +-PULSE_SPAN symbols, and
   scaled so a symbol's own pulse peaks at 1 (the deviation of that symbol).
   The receiver's filter is not this filter's matched pair by design, so
   what a clean call decodes with here is what the receiver really does. */
#define PULSE_SPS  50
#define PULSE_SPAN 8
#define PULSE_LEN  (2 * PULSE_SPAN * PULSE_SPS + 1)
static double s_pulse[PULSE_LEN];
static int s_pulse_ready;

static void build_pulse(void)
{
    if (s_pulse_ready) return;
    const int n = 4096;                    /* DFT size, samples */
    const double fs = SYM_RATE * PULSE_SPS, T = 1.0 / SYM_RATE, a = 0.2;
    static double H[4096];
    for (int k = 0; k < n; k++) {
        double f = (k <= n / 2 ? k : k - n) * fs / n, af = fabs(f), h;
        const double f1 = (1 - a) / (2 * T), f2 = (1 + a) / (2 * T);
        if (af <= f1) h = 1.0;
        else if (af <= f2) h = 0.5 * (1 + cos(M_PI * T / a * (af - f1)));
        else h = 0.0;
        double x = M_PI * f * T;
        if (h > 0 && fabs(x) > 1e-9) h *= x / sin(x);
        H[k] = h;
    }
    for (int i = 0; i < PULSE_LEN; i++) {
        double t = (i - PULSE_SPAN * PULSE_SPS);
        double acc = 0;
        for (int k = 0; k < n; k++) acc += H[k] * cos(2 * M_PI * k * t / n);
        s_pulse[i] = acc;
    }
    const double peak = s_pulse[PULSE_SPAN * PULSE_SPS];
    for (int i = 0; i < PULSE_LEN; i++) s_pulse[i] /= peak;
    s_pulse_ready = 1;
}

/* The frequency deviation, in symbol units, at position u symbols. */
static double deviation(const int *sym, int n_sym, double u)
{
    const int centre = (int)floor(u);
    double acc = 0;
    for (int s = centre - PULSE_SPAN; s <= centre + PULSE_SPAN; s++) {
        if (s < 0 || s >= n_sym) continue;
        const double d = (u - s) * PULSE_SPS;            /* samples from s's peak */
        const int i = (int)lround(d) + PULSE_SPAN * PULSE_SPS;
        if (i >= 0 && i < PULSE_LEN) acc += sym[s] * s_pulse[i];
    }
    return acc;
}

static double fade_gain(const p25_call_channel_t *ch, double t)
{
    double g = 1.0;
    for (int i = 0; i < ch->n_fades && i < 4; i++) {
        const p25_call_fade_t *f = &ch->fades[i];
        if (t >= f->start_s && t < f->start_s + f->len_s) g *= pow(10.0, f->gain_db / 20.0);
    }
    return g;
}

int p25_call_render(const int *sym, int n_sym, const p25_call_channel_t *ch,
                    ls_rng_t *rng, uint8_t *iq, int iq_max)
{
    const int need = p25_call_render_bytes(n_sym, ch);
    if (need > iq_max) return 0;
    build_pulse();
    const double rate = SYM_RATE * (1.0 + ch->clock_ppm * 1e-6);
    const double noise = 110.0 * ch->noise_sd / 0.289;
    double phase = 0.0;
    int out = 0;
    for (int k = 0; out + 1 < need; k++) {
        const double t = k / SAMPLE_RATE;
        const double u = (t - ch->lead_s) * rate;       /* position in symbols */
        double ci = 0.0, cq = 0.0;
        if (u >= -PULSE_SPAN && u < n_sym + PULSE_SPAN) {
            /* The symbol's centre is at u = s + 0.5, where the old
               half-cosine renderer had it settled. */
            const double level = deviation(sym, n_sym, u - 0.5);
            double off = ch->freq_off_hz;
            if (ch->keyup_tau_s > 0) off += ch->keyup_hz * exp(-(t - ch->lead_s) / ch->keyup_tau_s);
            phase += 2.0 * M_PI * (level * 600.0 + off) / SAMPLE_RATE;
            ci = 110.0 * cos(phase);
            cq = 110.0 * sin(phase);
        }
        double ni = 0.0, nq = 0.0;
        if (noise > 0.0 && rng) {
            ni = noise * ls_rng_noise(rng);
            nq = noise * ls_rng_noise(rng);
        }
        /* An overloaded tuner loses the wanted signal against the noise of
           everything after it, so a fade takes the carrier down and leaves
           the noise where it was. */
        const double g = fade_gain(ch, t);
        int i = (int)lround(127.5 + g * ci + ni);
        int q = (int)lround(127.5 + g * cq + nq);
        if (i < 0) i = 0; else if (i > 255) i = 255;
        if (q < 0) q = 0; else if (q > 255) q = 255;
        iq[out++] = (uint8_t)i;
        iq[out++] = (uint8_t)q;
    }
    return out;
}
