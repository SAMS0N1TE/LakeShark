/* PAGER RECON's pure half: see pager_recon.h. The BCH check is the POCSAG
   decoder's own (pocsag_check_codeword), so a codeword counts here exactly
   when the decoder would take it. The FLEX sync marker and mode codes are
   the ones multimon-ng's demod_flex.c matches. */
#include "pager_recon.h"

#include <stdio.h>
#include <string.h>

#include "pocsag.h"

/* ------------------------------------------------------------- plans -- */

typedef struct { uint32_t first_hz, step_hz; uint16_t count; } band_t;

static const band_t UHF[] = {
    { 929012500u, 25000u, 40 },     /* Part 90 private carrier paging     */
    { 931012500u, 25000u, 40 },     /* Part 22 common carrier paging      */
    { 454025000u, 25000u, 26 },     /* Part 22 UHF, GA-GZ                 */
};

static const band_t ONSITE[] = {
    { 457525000u, 25000u, 4 },      /* 457.525-457.600 on-site            */
    { 467750000u, 25000u, 8 },      /* 467.750-467.925 on-site paging     */
};

static int fill(const band_t *b, int nb, uint32_t *hz, int max)
{
    int n = 0;
    for (int i = 0; i < nb; i++)
        for (int k = 0; k < b[i].count && n < max; k++) hz[n++] = b[i].first_hz + (uint32_t)k * b[i].step_hz;
    return n;
}

int pgr_plan(pgr_plan_t which, uint32_t *hz, int max)
{
    if (!hz || max <= 0) return 0;
    switch (which) {
    case PGR_PLAN_UHF:    return fill(UHF, (int)(sizeof(UHF) / sizeof(UHF[0])), hz, max);
    case PGR_PLAN_ONSITE: return fill(ONSITE, (int)(sizeof(ONSITE) / sizeof(ONSITE[0])), hz, max);
    default:              return 0;
    }
}

const char *pgr_plan_name(pgr_plan_t which)
{
    switch (which) {
    case PGR_PLAN_UHF:    return "UHF PAGING";
    case PGR_PLAN_ONSITE: return "ON-SITE";
    default:              return "?";
    }
}

/* ------------------------------------------------------------ probes -- */

/* +-4.5 kHz is POCSAG's deviation and +-4.8 kHz FLEX's. Each slice is long
   enough for a preamble and one whole batch at that rate (576 + 544 bits),
   or for one FLEX frame of 1.875 s. */
static const pgr_probe_t PROBES[PGR_N_PROBES] = {
    { PGR_POCSAG, false, 1200, 4500, PGR_POCSAG_FSC,   PGR_BATCH_BYTES, 1000, "1200N" },
    { PGR_POCSAG, true,  1200, 4500, ~PGR_POCSAG_FSC,  PGR_BATCH_BYTES, 1000, "1200I" },
    { PGR_POCSAG, false,  512, 4500, PGR_POCSAG_FSC,   PGR_BATCH_BYTES, 2400, "512N"  },
    { PGR_POCSAG, true,   512, 4500, ~PGR_POCSAG_FSC,  PGR_BATCH_BYTES, 2400, "512I"  },
    { PGR_POCSAG, false, 2400, 4500, PGR_POCSAG_FSC,   PGR_BATCH_BYTES,  600, "2400N" },
    { PGR_POCSAG, true,  2400, 4500, ~PGR_POCSAG_FSC,  PGR_BATCH_BYTES,  600, "2400I" },
    { PGR_FLEX,   false, 1600, 4800, PGR_FLEX_MARKER,  2,               2000, "FLEXN" },
    { PGR_FLEX,   true,  1600, 4800, ~PGR_FLEX_MARKER, 2,               2000, "FLEXI" },
};

const pgr_probe_t *pgr_probe(int i) { return i >= 0 && i < PGR_N_PROBES ? &PROBES[i] : NULL; }
bool pgr_probe_is_flex(int i) { return i >= 0 && i < PGR_N_PROBES && PROBES[i].kind == PGR_FLEX; }

/* ----------------------------------------------------------- batches -- */

int pgr_scan_batch(const uint8_t *data, bool inverted, pgr_batch_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!data) return 0;
    for (int i = 0; i < 16; i++) {
        uint32_t cw = 0;
        for (int j = 0; j < 4; j++) cw = (cw << 8) | data[i * 4 + j];
        if (inverted) cw = ~cw;
        const int r = pocsag_check_codeword(&cw);
        if (r < 0) { out->bad++; continue; }
        if (r == 0) out->clean++;
        else out->fixed++;
        if (cw == PGR_POCSAG_IDLE) { out->idle++; continue; }
        if (cw & 0x80000000u) { out->message++; continue; }
        out->address++;
        /* Codeword i sits in frame i/2, and the frame is the capcode's low
           three bits. */
        const uint32_t cap = (((cw >> 13) & 0x3FFFFu) << 3) | (uint32_t)(i / 2);
        bool dup = false;
        for (int k = 0; k < out->n_caps; k++) if (out->caps[k] == cap) dup = true;
        if (!dup) out->caps[out->n_caps++] = cap;
    }
    return out->clean + out->fixed;
}

bool pgr_batch_real(const pgr_batch_t *b) { return b && b->clean + b->fixed >= 8; }

/* ------------------------------------------------------------- FLEX -- */

static const struct { uint16_t code; const char *name; } FLEX_MODES[] = {
    { 0x870C, "1600/2" }, { 0xB068, "1600/4" }, { 0x7B18, "3200/2" },
    { 0xDEA0, "3200/4" }, { 0x4C7C, "6400/4" },
};
#define N_FLEX_MODES ((int)(sizeof(FLEX_MODES) / sizeof(FLEX_MODES[0])))

static int bits16(uint16_t v) { int n = 0; while (v) { v &= (uint16_t)(v - 1); n++; } return n; }

/* After the marker comes the mode code complemented; a session synced on
   the complemented marker has every bit flipped, which undoes it. Two bits
   wrong are forgiven: the codes are at least six apart. */
int pgr_flex_mode(const uint8_t *two, bool inverted)
{
    if (!two) return -1;
    uint16_t v = (uint16_t)((two[0] << 8) | two[1]);
    if (!inverted) v = (uint16_t)~v;
    for (int i = 0; i < N_FLEX_MODES; i++)
        if (bits16((uint16_t)(v ^ FLEX_MODES[i].code)) <= 2) return i;
    return -1;
}

const char *pgr_flex_mode_name(int mode) { return mode >= 0 && mode < N_FLEX_MODES ? FLEX_MODES[mode].name : "?"; }

/* ------------------------------------------------------------ counts -- */

bool pgr_capset_add(pgr_capset_t *s, uint32_t cap)
{
    for (int i = 0; i < s->n; i++) if (s->v[i] == cap) return false;
    if (s->n < PGR_CAPS_MAX) { s->v[s->n++] = cap; return true; }
    /* Full: the count stops, and says it stopped. */
    s->overflow = true;
    return false;
}

float pgr_ber_pct(uint32_t fixed, uint32_t bad, uint32_t codewords)
{
    if (!codewords) return -1.0f;
    return 100.0f * (float)(fixed + 2u * bad) / (32.0f * (float)codewords);
}

float pgr_median(float *v, int n)
{
    if (!v || n <= 0) return 0.0f;
    for (int i = 1; i < n; i++) {
        const float x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

void pgr_age(char *out, size_t n, int64_t age_us)
{
    if (age_us < 0) { snprintf(out, n, "-"); return; }
    const int64_t s = age_us / 1000000;
    if (s < 60)         snprintf(out, n, "%ds", (int)s);
    else if (s < 3600)  snprintf(out, n, "%dm", (int)(s / 60));
    else if (s < 86400) snprintf(out, n, "%dh", (int)(s / 3600));
    else                snprintf(out, n, "%dd", (int)(s / 86400));
}

/* ------------------------------------------------------------- stream -- */

static void stream_cw(void *arg, uint32_t cw, int idx, int result)
{
    pgr_stream_count_t *b = arg;
    if (!idx) b->batch_good = 0;
    if (result < 0) b->bad++;
    else b->batch_good++;
    if (idx == 15 && b->batch_good >= 8) b->frames++;
    if (result < 0) return;
    if (result) b->fixed++; else b->clean++;
    if (cw == PGR_POCSAG_IDLE) return;
    if (cw & 0x80000000u) { b->message++; return; }
    b->address++;
    const uint32_t cap = (((cw >> 13) & 0x3FFFFu) << 3) | (uint32_t)(idx / 2);
    if (b->n_caps < 16) b->caps[b->n_caps++] = cap;
}

bool pgr_stream_init(pgr_stream_t *s, fm_state_t *text)
{
    static const int BAUD[] = { 512, 1200, 2400 };
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 3; i++) {
        s->decoder[i] = pocsag_create(text, BAUD[i]);
        if (!s->decoder[i]) { pgr_stream_free(s); return false; }
        pocsag_sample_rate(s->decoder[i], PGR_STREAM_RATE);
        pocsag_observe(s->decoder[i], stream_cw, &s->counts[i]);
    }
    return true;
}

void pgr_stream_free(pgr_stream_t *s)
{
    for (int i = 0; i < 3; i++) {
        pocsag_destroy(s->decoder[i]);
        s->decoder[i] = NULL;
    }
}

void pgr_stream_seam(pgr_stream_t *s)
{
    for (int i = 0; i < 3; i++) pocsag_seam(s->decoder[i]);
    memset(s->lowpass, 0, sizeof(s->lowpass));
    s->density_n = s->density_ones = 0;
}

int pgr_stream_best(const pgr_stream_t *s)
{
    int best = 0;
    for (int i = 1; i < 3; i++)
        if (s->counts[i].frames > s->counts[best].frames ||
            (s->counts[i].frames == s->counts[best].frames && s->counts[i].clean + s->counts[i].fixed >
            s->counts[best].clean + s->counts[best].fixed)) best = i;
    return best;
}

void pgr_stream_feed(pgr_stream_t *s, const uint8_t *data, size_t n)
{
    for (size_t j = 0; j < n; j++) {
        float samples[8];
        for (int k = 0; k < 8; k++) {
            const int bit = (data[j] >> (7 - k)) & 1;
            samples[k] = bit ? 1.0f : -1.0f;
            s->density_ones += bit;
            if (++s->density_n == PGR_STREAM_RATE) {
                s->bias = 2.0f * s->density_ones / s->density_n - 1.0f;
                /* Density is also payload dependent. Use small steps, and
                   ignore a stuck slicer rather than chasing it to a rail. */
                int step = s->bias > 0.12f ? 250 : s->bias < -0.12f ? -250 : 0;
                if (s->bias > 0.9f || s->bias < -0.9f) step = 0;
                s->want_trim_hz = s->trim_hz + step;
                if (s->want_trim_hz > PGR_STREAM_TRIM_MAX) s->want_trim_hz = PGR_STREAM_TRIM_MAX;
                if (s->want_trim_hz < -PGR_STREAM_TRIM_MAX) s->want_trim_hz = -PGR_STREAM_TRIM_MAX;
                s->density_n = s->density_ones = 0;
            }
        }
        for (int i = 0; i < 3; i++) {
            /* A quarter-symbol low-pass keeps sign chatter out of the PLL. */
            float filtered[8];
            const float alpha = 4.0f * pocsag_baud_of(s->decoder[i]) / PGR_STREAM_RATE;
            for (int k = 0; k < 8; k++) {
                s->lowpass[i] += alpha * (samples[k] - s->lowpass[i]);
                filtered[k] = s->lowpass[i];
            }
            pocsag_process(s->decoder[i], filtered, 8);
            pgr_stream_count_t *b = &s->counts[i];
            for (int k = 0; k < b->n_caps; k++) pgr_capset_add(&s->caps[i], b->caps[k]);
            b->n_caps = 0;
        }
    }
}
