#include "fsk_packet.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ls_cold.h"
#include "ls_textpack.h"

#define PI_F 3.14159265f

/* ------------------------------------------------------------- I/Q stage */

#define FLOOR_WINDOWS 256
#define HIST_BINS     256
#define MA_MAX        64

typedef struct {
    const int16_t *iq;
    size_t pairs;
    float fs;
    float gate;         /* smoothed power that counts as carrier          */
    unsigned g, m;      /* power and discriminator moving-average lengths */
} iq_job_t;

/* One streaming walk over the samples. With `hist` it fills the
   discriminator histogram over the gated samples; with `edges` it slices at
   `mid` and writes the runs. */
static size_t iq_walk(const iq_job_t *j, uint32_t *hist, float mid,
                      int32_t *edges, size_t cap, float *longest_ms)
{
    float pw_ring[MA_MAX], d_ring[MA_MAX];
    float pw_sum = 0, d_sum = 0;
    unsigned at_p = 0, at_d = 0;
    memset(pw_ring, 0, sizeof(pw_ring));
    memset(d_ring, 0, sizeof(d_ring));
    size_t n_out = 0;
    int level = 0;              /* 1 mark, -1 space or no carrier, 0 none yet */
    uint32_t run = 0, gated_run = 0, gated_best = 0;
    const float us_per = 1e6f / j->fs;
    const float to_hz = j->fs / (2 * PI_F);
    float pi = j->iq[0], pq = j->iq[1];

    for (size_t k = 1; k < j->pairs; k++) {
        const float i = j->iq[2 * k], q = j->iq[2 * k + 1];
        const float pw = i * i + q * q;
        pw_sum += pw - pw_ring[at_p];
        pw_ring[at_p] = pw;
        at_p = (at_p + 1) % j->g;
        const float d = atan2f(q * pi - i * pq, i * pi + q * pq) * to_hz;
        pi = i; pq = q;
        d_sum += d - d_ring[at_d];
        d_ring[at_d] = d;
        at_d = (at_d + 1) % j->m;
        const float dm = d_sum / (float)j->m;
        const bool on = pw_sum / (float)j->g > j->gate && k >= j->g;
        if (on) {
            if (++gated_run > gated_best) gated_best = gated_run;
        } else {
            gated_run = 0;
        }
        if (hist) {
            if (on) {
                int b = (int)((dm / j->fs + 0.5f) * HIST_BINS);
                if (b < 0) b = 0;
                if (b >= HIST_BINS) b = HIST_BINS - 1;
                hist[b]++;
            }
            continue;
        }
        const int lv = on ? (dm > mid ? 1 : -1) : -1;
        if (level == 0) {
            if (!on) continue;          /* nothing before the first carrier */
            level = lv;
            run = 1;
            continue;
        }
        if (lv == level) {
            run++;
            continue;
        }
        if (n_out < cap) {
            const int32_t us = (int32_t)lroundf((float)run * us_per);
            edges[n_out++] = level > 0 ? us : -us;
        }
        level = lv;
        run = 1;
    }
    if (!hist && level != 0 && n_out < cap) {
        const int32_t us = (int32_t)lroundf((float)run * us_per);
        edges[n_out++] = level > 0 ? us : -us;
    }
    if (longest_ms) *longest_ms = (float)gated_best * us_per / 1000.0f;
    return n_out;
}

static float hist_pct(const uint32_t *h, float pct, float fs)
{
    uint64_t total = 0;
    for (int b = 0; b < HIST_BINS; b++) total += h[b];
    if (!total) return 0;
    const uint64_t want = (uint64_t)(pct * (float)total);
    uint64_t acc = 0;
    for (int b = 0; b < HIST_BINS; b++) {
        acc += h[b];
        if (acc > want) return ((b + 0.5f) / HIST_BINS - 0.5f) * fs;
    }
    return fs / 2;
}

static void sort_floats(float *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        const float x = v[i];
        size_t j = i;
        while (j && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
}

static uint32_t preamble_symbol_us(const int32_t *e, size_t n);

size_t fsk_pkt_iq16_edges(const int16_t *iq, size_t pairs, uint32_t fs,
                          int32_t *edges, size_t cap, fsk_pkt_rf_t *rf)
{
    if (rf) memset(rf, 0, sizeof(*rf));
    if (!iq || pairs < 256 || fs < 8000 || !edges || !cap) return 0;

    /* Floor: the 20th percentile of short window means. */
    size_t w = fs / 1000;
    if (w < 16) w = 16;
    while (pairs / w > FLOOR_WINDOWS) w *= 2;
    const size_t nw = pairs / w;
    if (nw < 2) return 0;
    float means[FLOOR_WINDOWS];
    float peak = 0;
    for (size_t k = 0; k < nw; k++) {
        double acc = 0;
        for (size_t s = 0; s < w; s++) {
            const float i = iq[2 * (k * w + s)], q = iq[2 * (k * w + s) + 1];
            acc += (double)i * i + (double)q * q;
        }
        means[k] = (float)(acc / (double)w);
        if (means[k] > peak) peak = means[k];
    }
    sort_floats(means, nw);
    float flr = means[nw / 5];
    if (flr < 1) flr = 1;
    if (peak < 10 * flr) return 0;

    iq_job_t j = { .iq = iq, .pairs = pairs, .fs = (float)fs, .gate = 10 * flr };
    j.g = fs / 5000;
    if (j.g < 4) j.g = 4;
    if (j.g > MA_MAX) j.g = MA_MAX;
    /* First pass smooths for symbols down to 50 us; the second for the
       symbol time the first one measured. */
    unsigned m = fs / 20000;
    size_t n = 0;
    float lo = 0, hi = 0, mid = 0, longest = 0;
    for (int pass = 0; pass < 2; pass++) {
        j.m = m < 1 ? 1 : m > MA_MAX ? MA_MAX : m;
        uint32_t hist[HIST_BINS];
        memset(hist, 0, sizeof(hist));
        iq_walk(&j, hist, 0, NULL, 0, &longest);
        lo = hist_pct(hist, 0.10f, (float)fs);
        hi = hist_pct(hist, 0.90f, (float)fs);
        mid = (lo + hi) / 2;
        n = iq_walk(&j, NULL, mid, edges, cap, NULL);
        const uint32_t sym_us = preamble_symbol_us(edges, n);
        if (!sym_us) break;
        const unsigned next = (unsigned)((float)sym_us * (float)fs / 1e6f / 4);
        if (next == j.m) break;
        m = next;
    }
    if (rf) {
        rf->offset_hz = mid;
        rf->deviation_hz = (hi - lo) / 2;
        rf->snr_db = 10 * log10f(peak / flr);
        rf->burst_ms = longest;
    }
    return n;
}

/* ---------------------------------------------------------- packet stage */

#define MIN_PREAMBLE_RUNS 12
#define MAX_BITS 1536

uint16_t fsk_pkt_crc16(const uint8_t *m, size_t n, uint16_t poly, uint16_t init)
{
    uint16_t c = init;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(m[i] << 8);
        for (int b = 0; b < 8; b++) c = c & 0x8000 ? (uint16_t)((c << 1) ^ poly) : (uint16_t)(c << 1);
    }
    return c;
}

void fsk_pkt_pn9(uint8_t *buf, size_t n)
{
    uint16_t s = 0x1FF;
    for (size_t i = 0; i < n; i++) {
        buf[i] ^= (uint8_t)s;
        for (int b = 0; b < 8; b++) s = (uint16_t)((s >> 1) | (((s ^ (s >> 5)) & 1) << 8));
    }
}

/* Raw edges with glitches folded in: a run shorter than `min_us` joins the
   run before it, and runs of one sign then join up. */
typedef struct {
    const int32_t *e;
    size_t n, at;
    int32_t min_us;
    int32_t pending;
    size_t pending_at;  /* raw index the pending run started at */
    size_t out_at;      /* raw index the run just returned started at */
} runs_t;

static void runs_init(runs_t *r, const int32_t *e, size_t n, size_t at, int32_t min_us)
{
    r->e = e; r->n = n; r->at = at; r->min_us = min_us; r->pending = 0;
    r->pending_at = r->out_at = at;
}

/* Next folded run, 0 at the end; r->out_at says where it started. */
static int32_t runs_next(runs_t *r)
{
    while (r->at < r->n) {
        const size_t here = r->at;
        const int32_t v = r->e[r->at++];
        if (!v) continue;
        if (!r->pending) { r->pending = v; r->pending_at = here; continue; }
        const int32_t av = v < 0 ? -v : v;
        const bool same = (v > 0) == (r->pending > 0);
        if (same || av < r->min_us) {
            r->pending += r->pending > 0 ? av : -av;
            continue;
        }
        const int32_t out = r->pending;
        r->out_at = r->pending_at;
        r->pending = v;
        r->pending_at = here;
        return out;
    }
    const int32_t out = r->pending;
    r->out_at = r->pending_at;
    r->pending = 0;
    return out;
}

/* Median run length, as a first guess at the symbol time. */
static int32_t median_run_us(const int32_t *e, size_t n)
{
    /* 96 log bins from 8 us to 80 ms */
    uint32_t h[96] = {0};
    uint32_t total = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t a = e[i] < 0 ? -e[i] : e[i];
        if (a < 8 || a > 80000) continue;
        int b = (int)(logf((float)a / 8.0f) / logf(10000.0f) * 96);
        if (b > 95) b = 95;
        h[b]++; total++;
    }
    if (!total) return 0;
    uint32_t acc = 0;
    for (int b = 0; b < 96; b++) {
        acc += h[b];
        if (acc * 2 >= total) return (int32_t)(8.0f * powf(10000.0f, (b + 0.5f) / 96));
    }
    return 0;
}

typedef struct {
    size_t raw_at;      /* where the preamble's first run starts in the raw edges */
    float sym_us;
} preamble_t;

/* The next run of at least MIN_PREAMBLE_RUNS folded runs of nearly one
   length, from raw index `from`. The symbol time comes from pairs of runs,
   which cancels any slicer bias between the two tones. */
static bool find_preamble(const int32_t *e, size_t n, size_t from, int32_t min_us, preamble_t *p)
{
    runs_t r;
    runs_init(&r, e, n, from, min_us);
    size_t start_raw = from, count = 0;
    double sum = 0;
    for (;;) {
        const int32_t v = runs_next(&r);
        if (!v) break;
        const float a = (float)(v < 0 ? -v : v);
        const float mean = count ? (float)(sum / count) : a;
        if (count && fabsf(a - mean) <= 0.35f * mean) {
            sum += a; count++;
            continue;
        }
        if (count >= MIN_PREAMBLE_RUNS) break;
        start_raw = r.out_at;   /* this run starts a new candidate */
        sum = a; count = 1;
    }
    if (count < MIN_PREAMBLE_RUNS) return false;
    p->raw_at = start_raw;
    p->sym_us = (float)(sum / (double)count);
    return p->sym_us > 0;
}

static uint32_t preamble_symbol_us(const int32_t *e, size_t n)
{
    const int32_t med = median_run_us(e, n);
    if (!med) return 0;
    preamble_t p;
    return find_preamble(e, n, 0, med * 45 / 100, &p) ? (uint32_t)lroundf(p.sym_us) : 0;
}

static inline int bit_at(const uint8_t *b, int i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }
static inline void bit_put(uint8_t *b, int i, int v)
{
    if (v) b[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    else   b[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
}

static uint32_t take(const uint8_t *b, int at, int nbits, int inv)
{
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++) v = v << 1 | (uint32_t)(bit_at(b, at + i) ^ inv);
    return v;
}

static const uint32_t STD_BAUD[] = { 600, 1200, 2000, 2400, 4000, 4800, 9600, 10000, 19200, 20000,
                                     38400, 50000, 76800, 100000 };

static uint32_t standard_baud(uint32_t baud)
{
    for (size_t i = 0; i < sizeof(STD_BAUD) / sizeof(STD_BAUD[0]); i++) {
        const float r = (float)baud / (float)STD_BAUD[i];
        if (r > 0.97f && r < 1.03f) return STD_BAUD[i];
    }
    return 0;
}

static const struct { uint16_t word; } KNOWN_SYNC[] = {
    { 0xD391 },   /* CC1101, CC1120 default  */
    { 0x2DD4 },   /* Si443x, SX1231 examples */
};

/* Try every length for a CRC that checks; the CRC must end at or after
   byte `min_end`. Fills the crc fields and len/data. */
static bool crc_search(const uint8_t *body, int nbytes, int min_end, fsk_pkt_t *p)
{
    static const struct { fsk_crc_t kind; uint16_t poly, init, xorout; } K[] = {
        { FSK_CRC_CC1101, 0x8005, 0xFFFF, 0 },
        { FSK_CRC_CCITT,  0x1021, 0xFFFF, 0 },
        { FSK_CRC_SX1231, 0x1021, 0x1D0F, 0xFFFF },
    };
    uint8_t buf[FSK_PKT_MAX_BYTES + 2];
    if (nbytes > (int)sizeof(buf)) nbytes = (int)sizeof(buf);
    for (int white = 0; white < 2; white++) {
        memcpy(buf, body, (size_t)nbytes);
        if (white) fsk_pkt_pn9(buf, (size_t)nbytes);
        for (int L = 1; L + 2 <= nbytes; L++) {
            if (L + 2 < min_end) continue;
            const uint16_t got = (uint16_t)(buf[L] << 8 | buf[L + 1]);
            for (size_t k = 0; k < sizeof(K) / sizeof(K[0]); k++) {
                const uint16_t c = fsk_pkt_crc16(buf, (size_t)L, K[k].poly, K[k].init) ^ K[k].xorout;
                if (c != got) continue;
                p->crc_kind = K[k].kind;
                p->crc = got;
                p->whitened = white;
                p->length_byte = buf[0] == L - 1;
                p->len = (uint8_t)L;
                memcpy(p->data, buf, (size_t)L);
                return true;
            }
        }
    }
    return false;
}

static void body_bytes(const uint8_t *bits, int at, int nb, int inv, uint8_t *out, int *nbytes)
{
    int k = (nb - at) / 8;
    if (k > FSK_PKT_MAX_BYTES + 2) k = FSK_PKT_MAX_BYTES + 2;
    if (k < 0) k = 0;
    for (int i = 0; i < k; i++) out[i] = (uint8_t)take(bits, at + 8 * i, 8, inv);
    *nbytes = k;
}

/* One packet from a bit row that starts on the preamble. */
static bool parse_row(const uint8_t *bits, int nb, int pre_bits, fsk_pkt_t *p)
{
    uint8_t body[FSK_PKT_MAX_BYTES + 2];
    int nbytes = 0;
    /* A chip's default sync word, in either polarity, near the preamble end */
    for (int inv = 0; inv < 2; inv++) {
        for (int at = pre_bits - 6; at <= pre_bits + 6; at++) {
            if (at < 0 || at + 16 > nb) continue;
            const uint32_t w = take(bits, at, 16, inv);
            for (size_t s = 0; s < sizeof(KNOWN_SYNC) / sizeof(KNOWN_SYNC[0]); s++) {
                if (w != KNOWN_SYNC[s].word) continue;
                p->sync = w; p->sync_bits = 16; p->sync_known = true; p->inverted = inv;
                p->preamble_bits = at;
                int body_at = at + 16;
                p->sync_repeats = 1;
                if (body_at + 16 <= nb && take(bits, body_at, 16, inv) == w) { p->sync_repeats = 2; body_at += 16; }
                body_bytes(bits, body_at, nb, inv, body, &nbytes);
                if (!crc_search(body, nbytes, 0, p)) {
                    p->crc_kind = FSK_CRC_NONE;
                    p->len = (uint8_t)(nbytes < FSK_PKT_MAX_BYTES ? nbytes : FSK_PKT_MAX_BYTES);
                    memcpy(p->data, body, p->len);
                }
                return p->len > 0;
            }
        }
    }
    /* Unknown sync word. The alternation breaks somewhere in its first
       bits (a word can start 1010 itself), so every start up to a byte back
       is tried, and only a CRC that ends where the burst ends counts: that
       keeps chance matches over all those tries rare. With no CRC anywhere
       the word is taken from just before the break. */
    int brk = -1;
    for (int i = 1; i < nb; i++) if (bit_at(bits, i) == bit_at(bits, i - 1)) { brk = i; break; }
    if (brk < 8) return false;
    for (int inv = 0; inv < 2; inv++) {
        for (int at = brk - 8; at <= brk; at++) {
            if (at < 0 || at + 16 > nb) continue;
            body_bytes(bits, at + 16, nb, inv, body, &nbytes);
            if (!crc_search(body, nbytes, nbytes - 4, p)) continue;
            p->sync = take(bits, at, 16, inv); p->sync_bits = 16; p->sync_known = false;
            p->sync_repeats = 1; p->inverted = inv; p->preamble_bits = at;
            return true;
        }
    }
    const int at = brk - 1;
    if (at + 16 > nb) return false;
    body_bytes(bits, at + 16, nb, 0, body, &nbytes);
    p->sync = take(bits, at, 16, 0); p->sync_bits = 16; p->sync_known = false;
    p->sync_repeats = 1; p->inverted = false; p->preamble_bits = at;
    p->crc_kind = FSK_CRC_NONE;
    p->len = (uint8_t)(nbytes < FSK_PKT_MAX_BYTES ? nbytes : FSK_PKT_MAX_BYTES);
    memcpy(p->data, body, p->len);
    return p->len > 0;
}

static bool same_packet(const fsk_pkt_t *a, const fsk_pkt_t *b)
{
    return a->len == b->len && a->crc_kind == b->crc_kind && a->sync == b->sync &&
           !memcmp(a->data, b->data, a->len);
}

int fsk_pkt_from_edges(const int32_t *edges, size_t n, fsk_pkt_t *out, int max)
{
    if (!edges || !n || !out || max <= 0) return 0;
    const int32_t med = median_run_us(edges, n);
    if (!med) return 0;
    const int32_t min_us = med * 45 / 100;
    int found = 0;
    size_t from = 0;
    uint8_t bits[MAX_BITS / 8];
    while (found < max && from < n) {
        preamble_t pre;
        if (!find_preamble(edges, n, from, min_us, &pre)) break;
        const float T = pre.sym_us;
        runs_t r;
        runs_init(&r, edges, n, pre.raw_at, (int32_t)(T * 0.5f));
        int nb = 0, pre_bits = -1;
        size_t runs_seen = 0;
        for (;;) {
            const int32_t v = runs_next(&r);
            if (!v) break;
            const float a = (float)(v < 0 ? -v : v);
            int k = (int)lroundf(a / T);
            if (k < 1) k = 1;
            const bool end = k > 40;
            if (end) k = v < 0 ? 24 : 0;        /* the gap: room for trailing zeros */
            const int bit = v > 0;
            for (int i = 0; i < k && nb < MAX_BITS; i++) bit_put(bits, nb++, bit);
            /* the preamble ends at the first run that is not one symbol */
            if (pre_bits < 0 && runs_seen >= MIN_PREAMBLE_RUNS && k != 1) pre_bits = nb - k;
            runs_seen++;
            if (end || nb >= MAX_BITS) break;
        }
        from = r.at > pre.raw_at ? r.at : pre.raw_at + 1;
        if (pre_bits < 0) continue;
        fsk_pkt_t p;
        memset(&p, 0, sizeof(p));
        p.baud = (uint32_t)lroundf(1e6f / T);
        p.baud_std = standard_baud(p.baud);
        if (!parse_row(bits, nb, pre_bits, &p)) continue;
        bool dup = false;
        for (int i = 0; i < found; i++) dup |= same_packet(&out[i], &p);
        if (!dup) out[found++] = p;
    }
    return found;
}

LS_COLD int fsk_pkt_describe(const fsk_pkt_t *p, char *out, size_t n)
{
    if (!p || !out || !n) return 0;
    static const char *const CRC[] = { "", "CRC", "CRC CCITT", "CRC SX1231" };
    char hex[3 * 16 + 4];
    size_t h = 0;
    const int show = p->len > 16 ? 16 : p->len;
    for (int i = 0; i < show; i++) h += (size_t)snprintf(hex + h, sizeof(hex) - h, i ? " %02X" : "%02X", p->data[i]);
    if (p->len > show) snprintf(hex + h, sizeof(hex) - h, " ..");
    char sync[16];
    snprintf(sync, sizeof(sync), "%04lX%s", (unsigned long)p->sync, p->sync_repeats > 1 ? "x2" : "");
    if (p->crc_kind == FSK_CRC_NONE)
        return LS_SNPRINTF(t_fsk_packet_2_fsk_lu_bd, out, n, "2-FSK %lu bd sync %s%s %u B %s, no CRC", (unsigned long)(p->baud_std ? p->baud_std : p->baud),
                        sync, p->inverted ? " inv" : "", p->len, hex);
    return LS_SNPRINTF(t_fsk_packet_2_fsk_lu_bd_2, out, n, "2-FSK %lu bd sync %s%s%s %u B %s %s %04X ok", (unsigned long)(p->baud_std ? p->baud_std : p->baud),
                    sync, p->inverted ? " inv" : "", p->whitened ? " PN9" : "", p->len, hex, CRC[p->crc_kind], p->crc);
}

LS_COLD int fsk_pkt_describe_short(const fsk_pkt_t *p, char *out, size_t n)
{
    if (!p || !out || !n) return 0;
    char hex[3 * 6 + 4];
    size_t h = 0;
    const int show = p->len > 6 ? 6 : p->len;
    for (int i = 0; i < show; i++) h += (size_t)snprintf(hex + h, sizeof(hex) - h, i ? " %02X" : "%02X", p->data[i]);
    if (p->len > show) snprintf(hex + h, sizeof(hex) - h, " ..");
    return LS_SNPRINTF(t_fsk_packet_lu_bd_04lx_s, out, n, "%lu bd %04lX%s | %s | %s", (unsigned long)(p->baud_std ? p->baud_std : p->baud),
                    (unsigned long)p->sync, p->sync_repeats > 1 ? "x2" : "", hex,
                    p->crc_kind == FSK_CRC_NONE ? "no CRC" : "CRC ok");
}
