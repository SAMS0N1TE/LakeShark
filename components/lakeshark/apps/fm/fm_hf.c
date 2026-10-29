#include "fm_hf.h"

#include <ctype.h>
#include <string.h>

static fm_hf_settings_t s_hf = {
    .demod = FM_HF_AM, .narrow = 0, .agc = 1, .nb = 0,
    .rit_hz = 0, .pitch_hz = 700, .step_hz = 0,
};
static volatile uint32_t s_generation;
static const fm_hf_hooks_t *s_hooks;

void fm_hf_install(const fm_hf_hooks_t *hooks) { s_hooks = hooks; }
const fm_hf_settings_t *fm_hf_get(void) { return &s_hf; }
uint32_t fm_hf_generation(void) { return s_generation; }

static void changed(void)
{
    ++s_generation;
    if (s_hooks && s_hooks->changed) s_hooks->changed();
}

void fm_hf_set_demod(int demod)
{
    if (demod < 0 || demod >= FM_HF_DEMOD_COUNT) return;
    s_hf.demod = (uint8_t)demod;
    changed();
}

void fm_hf_set_narrow(bool narrow) { s_hf.narrow = narrow ? 1 : 0; changed(); }

void fm_hf_set_agc(int agc)
{
    if (agc < 0 || agc > 2) return;
    s_hf.agc = (uint8_t)agc;
    changed();
}

void fm_hf_set_nb(int nb)
{
    if (nb < 0 || nb > 2) return;
    s_hf.nb = (uint8_t)nb;
    changed();
}

void fm_hf_set_rit(int hz)
{
    hz = (hz >= 0 ? hz + FM_HF_RIT_STEP_HZ / 2 : hz - FM_HF_RIT_STEP_HZ / 2) /
         FM_HF_RIT_STEP_HZ * FM_HF_RIT_STEP_HZ;
    if (hz > FM_HF_RIT_MAX_HZ) hz = FM_HF_RIT_MAX_HZ;
    if (hz < -FM_HF_RIT_MAX_HZ) hz = -FM_HF_RIT_MAX_HZ;
    s_hf.rit_hz = (int16_t)hz;
    changed();
}

void fm_hf_rit_step(int direction)
{
    fm_hf_set_rit(s_hf.rit_hz + (direction < 0 ? -FM_HF_RIT_STEP_HZ : FM_HF_RIT_STEP_HZ));
}

void fm_hf_set_pitch(int hz)
{
    if (hz < FM_HF_PITCH_MIN_HZ || hz > FM_HF_PITCH_MAX_HZ) return;
    s_hf.pitch_hz = (uint16_t)(hz / 10 * 10);
    changed();
}

static const uint32_t STEPS[] = { 10, 50, 100, 500, 1000, 5000, 9000, 10000 };
#define N_STEPS ((int)(sizeof(STEPS) / sizeof(STEPS[0])))

static int step_index(uint32_t hz)
{
    for (int i = 0; i < N_STEPS; ++i) if (STEPS[i] == hz) return i;
    return -1;
}

void fm_hf_set_step(uint32_t hz)
{
    if (hz != 0 && step_index(hz) < 0) return;
    s_hf.step_hz = hz;
    changed();
}

uint32_t fm_hf_step_next(uint32_t hz)
{
    const int i = step_index(hz);
    return STEPS[i < 0 || i + 1 >= N_STEPS ? 0 : i + 1];
}

uint32_t fm_hf_filter_hz(const fm_hf_settings_t *s)
{
    switch (s->demod) {
    case FM_HF_USB: case FM_HF_LSB: return s->narrow ? 2400 : 2800;
    case FM_HF_CW:  return s->narrow ? 250 : 500;
    default:        return s->narrow ? 6000 : 8000;
    }
}

static const char *const DEMODS[FM_HF_DEMOD_COUNT] = { "AM", "SAM", "USB", "LSB", "CW" };

const char *fm_hf_demod_label(int demod)
{
    return demod >= 0 && demod < FM_HF_DEMOD_COUNT ? DEMODS[demod] : "?";
}

static bool same_word(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}

bool fm_hf_demod_parse(const char *text, uint32_t hz, int *demod)
{
    if (!text || !demod) return false;
    for (int i = 0; i < FM_HF_DEMOD_COUNT; ++i)
        if (same_word(text, DEMODS[i])) { *demod = i; return true; }
    if (same_word(text, "ssb")) { *demod = fm_hf_auto_sideband(hz); return true; }
    if (same_word(text, "auto")) { *demod = fm_hf_auto_demod(hz); return true; }
    return false;
}

const char *fm_hf_agc_label(int agc)
{
    static const char *const L[] = { "FAST", "SLOW", "OFF" };
    return agc >= 0 && agc < 3 ? L[agc] : "?";
}

const char *fm_hf_nb_label(int nb)
{
    static const char *const L[] = { "OFF", "LOW", "HIGH" };
    return nb >= 0 && nb < 3 ? L[nb] : "?";
}

/* v1: [31:28] 1, [27:25] demod, [24] narrow, [23:22] agc, [21:20] nb,
   [19:12] pitch/10, [11:8] step index + 1 (0 follows the band). */
#define HF_PACK_V1 1u

uint32_t fm_hf_pack(const fm_hf_settings_t *s)
{
    const int si = s->step_hz ? step_index(s->step_hz) + 1 : 0;
    return (HF_PACK_V1 << 28) | ((uint32_t)s->demod << 25) |
           ((uint32_t)(s->narrow & 1) << 24) | ((uint32_t)s->agc << 22) |
           ((uint32_t)s->nb << 20) | ((uint32_t)(s->pitch_hz / 10) << 12) |
           ((uint32_t)si << 8);
}

bool fm_hf_unpack(uint32_t w, fm_hf_settings_t *s)
{
    if ((w >> 28) != HF_PACK_V1 || (w & 0xffu)) return false;
    const uint32_t demod = (w >> 25) & 7u, agc = (w >> 22) & 3u, nb = (w >> 20) & 3u;
    const uint32_t pitch = ((w >> 12) & 0xffu) * 10u, si = (w >> 8) & 0xfu;
    if (demod >= FM_HF_DEMOD_COUNT || agc > 2 || nb > 2 || si > (uint32_t)N_STEPS ||
        pitch < FM_HF_PITCH_MIN_HZ || pitch > FM_HF_PITCH_MAX_HZ) return false;
    s->demod = (uint8_t)demod;
    s->narrow = (uint8_t)((w >> 24) & 1u);
    s->agc = (uint8_t)agc;
    s->nb = (uint8_t)nb;
    s->pitch_hz = (uint16_t)pitch;
    s->step_hz = si ? STEPS[si - 1] : 0;
    s->rit_hz = 0;
    return true;
}

void fm_hf_load(const fm_hf_settings_t *s)
{
    s_hf = *s;
    ++s_generation;
}

/* ------------------------------------------------------------ band plan -- */

#define K(x) ((uint32_t)(x) * 1000u)
#define B(n, lo, hi, dial, d, kind, step) \
    { n, K(lo), K(hi), (uint32_t)(dial), FM_HF_##d, FM_HF_KIND_##kind, step }

/* Broadcast bands per the ITU HF broadcast allocations, the amateur bands of
   ITU Region 2 (US band plan voice segments for the dial), CB channels 1-40,
   and the time stations: WWV/WWVH on 2.5 to 20 MHz, CHU on its three. */
static const fm_hf_band_t BANDS[] = {
    B("MW bcast",   530,   1700,  1000000, AM,  BROADCAST, 10000),
    B("160m ham",  1800,   2000,  1900000, LSB, HAM,         100),
    B("120m SW",   2300,   2495,  2400000, AM,  BROADCAST,  5000),
    B("WWV 2.5",   2500,   2500,  2500000, AM,  TIME,       1000),
    B("90m SW",    3200,   3400,  3300000, AM,  BROADCAST,  5000),
    B("CHU 3.33",  3330,   3330,  3330000, AM,  TIME,       1000),
    B("80m ham",   3500,   4000,  3850000, LSB, HAM,         100),
    B("75m SW",    3900,   4000,  3950000, AM,  BROADCAST,  5000),
    B("60m SW",    4750,   5060,  4900000, AM,  BROADCAST,  5000),
    B("WWV 5",     5000,   5000,  5000000, AM,  TIME,       1000),
    B("60m ham",   5330,   5407,  5357000, USB, HAM,         100),
    B("49m SW",    5900,   6200,  6050000, AM,  BROADCAST,  5000),
    B("40m ham",   7000,   7300,  7185000, LSB, HAM,         100),
    B("41m SW",    7200,   7450,  7350000, AM,  BROADCAST,  5000),
    B("CHU 7.85",  7850,   7850,  7850000, AM,  TIME,       1000),
    B("31m SW",    9400,   9900,  9650000, AM,  BROADCAST,  5000),
    B("WWV 10",   10000,  10000, 10000000, AM,  TIME,       1000),
    B("30m ham",  10100,  10150, 10120000, CW,  HAM,          50),
    B("25m SW",   11600,  12100, 11850000, AM,  BROADCAST,  5000),
    B("22m SW",   13570,  13870, 13720000, AM,  BROADCAST,  5000),
    B("20m ham",  14000,  14350, 14230000, USB, HAM,         100),
    B("CHU 14.67",14670,  14670, 14670000, AM,  TIME,       1000),
    B("WWV 15",   15000,  15000, 15000000, AM,  TIME,       1000),
    B("19m SW",   15100,  15800, 15400000, AM,  BROADCAST,  5000),
    B("16m SW",   17480,  17900, 17700000, AM,  BROADCAST,  5000),
    B("17m ham",  18068,  18168, 18130000, USB, HAM,         100),
    B("15m SW",   18900,  19020, 18960000, AM,  BROADCAST,  5000),
    B("WWV 20",   20000,  20000, 20000000, AM,  TIME,       1000),
    B("15m ham",  21000,  21450, 21300000, USB, HAM,         100),
    B("13m SW",   21450,  21850, 21650000, AM,  BROADCAST,  5000),
    B("12m ham",  24890,  24990, 24950000, USB, HAM,         100),
    B("11m SW",   25670,  26100, 25850000, AM,  BROADCAST,  5000),
    B("CB 11m",   26965,  27405, 27185000, AM,  CB,        10000),
    B("10m ham",  28000,  29700, 28400000, USB, HAM,         100),
};
#define N_BANDS ((int)(sizeof(BANDS) / sizeof(BANDS[0])))

int fm_hf_band_count(void) { return N_BANDS; }
const fm_hf_band_t *fm_hf_band(int i) { return i >= 0 && i < N_BANDS ? &BANDS[i] : NULL; }

int fm_hf_band_find(uint32_t hz)
{
    /* A station first (it sits inside broadcast and ham bands), then the
       narrowest band holding hz: where 41m broadcasting overlaps the top of
       40m (7.2-7.3 MHz) that is broadcasting, which is what is heard there. */
    int best = -1;
    uint32_t best_w = UINT32_MAX;
    for (int i = 0; i < N_BANDS; ++i) {
        const fm_hf_band_t *b = &BANDS[i];
        if (b->lo_hz == b->hi_hz) {
            if (hz + 5000u >= b->lo_hz && hz <= b->lo_hz + 5000u) return i;
            continue;
        }
        if (hz >= b->lo_hz && hz <= b->hi_hz && b->hi_hz - b->lo_hz < best_w) {
            best = i;
            best_w = b->hi_hz - b->lo_hz;
        }
    }
    return best;
}

int fm_hf_auto_demod(uint32_t hz)
{
    const int i = fm_hf_band_find(hz);
    return i >= 0 ? BANDS[i].demod : FM_HF_AM;
}

int fm_hf_auto_sideband(uint32_t hz)
{
    if (hz >= 5330000u && hz <= 5407000u) return FM_HF_USB;   /* 60 m: USB by rule */
    return hz < 10000000u ? FM_HF_LSB : FM_HF_USB;
}

uint32_t fm_hf_step_at(uint32_t hz)
{
    if (s_hf.step_hz) return s_hf.step_hz;
    const int i = fm_hf_band_find(hz);
    if (i >= 0) return BANDS[i].step_hz;
    return s_hf.demod == FM_HF_AM || s_hf.demod == FM_HF_SAM ? 5000 : 100;
}

void fm_hf_tune(uint32_t hz, int demod)
{
    if (demod < 0 || demod >= FM_HF_DEMOD_COUNT) demod = fm_hf_auto_demod(hz);
    s_hf.demod = (uint8_t)demod;
    s_hf.rit_hz = 0;
    changed();
    if (s_hooks && s_hooks->tune) s_hooks->tune(hz);
}

bool fm_hf_band_apply(int i)
{
    const fm_hf_band_t *b = fm_hf_band(i);
    if (!b) return false;
    /* The band's own step, until one is chosen again. */
    s_hf.step_hz = 0;
    fm_hf_tune(b->dial_hz, b->demod);
    return true;
}
