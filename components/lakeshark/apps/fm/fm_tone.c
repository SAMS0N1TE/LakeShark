#include "fm_tone.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

const uint16_t fm_ctcss_tenths[FM_CTCSS_COUNT] = {
    670,693,719,744,770,797,825,854,885,915,948,974,1000,1035,1072,
    1109,1148,1188,1230,1273,1318,1365,1413,1462,1514,1567,1598,
    1622,1655,1679,1713,1738,1773,1799,1835,1862,1899,1928,1966,
    1995,2035,2065,2107,2181,2257,2291,2336,2418,2503,2541
};
const uint16_t fm_dcs_codes[FM_DCS_COUNT] = {
    0023,0025,0026,0031,0032,0036,0043,0047,0051,0053,0054,0065,
    0071,0072,0073,0074,0114,0115,0116,0122,0125,0131,0132,0134,
    0143,0145,0152,0155,0156,0162,0165,0172,0174,0205,0212,0223,
    0225,0226,0243,0244,0245,0246,0251,0252,0255,0261,0263,0265,
    0266,0271,0274,0306,0311,0315,0325,0331,0332,0343,0346,0351,
    0356,0364,0365,0371,0411,0412,0413,0423,0431,0432,0445,0446,
    0452,0454,0455,0462,0464,0465,0466,0503,0506,0516,0523,0526,
    0532,0546,0565,0606,0612,0624,0627,0631,0632,0654,0662,0664,
    0703,0712,0723,0731,0732,0734,0743,0754,
};

uint32_t fm_dcs_word(uint16_t code)
{
    /* Systematic Golay (23,12), fixed DCS marker 100 and nine code bits.
       Divide by x^11+x^10+x^6+x^5+x^4+x^2+1 for the check bits. */
    uint32_t data = 0x800u | (code & 0x1ffu), rem = data << 11;
    for (int bit = 22; bit >= 11; --bit)
        if (rem & (1u << bit)) rem ^= 0xc75u << (bit - 11);
    return data | (rem << 12);
}

void fm_tone_init(fm_tone_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < FM_CTCSS_COUNT; ++i)
        s->coeff[i] = 2.0f * cosf(6.283185307f * fm_ctcss_tenths[i] / 10000.0f);
    for (int i = 0; i < 8; ++i) s->dcs[i].phase = i * 1250;
}

static void report(fm_tone_t *s, uint16_t selection, float confidence)
{
    s->result.selection = selection;
    s->result.confidence = confidence;
    s->seen = s->ticks;
}

static void ctcss(fm_tone_t *s)
{
    float energy = 0, best = 0, second = 0;
    int index = 0;
    for (int j = 0; j < 300; ++j) energy += s->ring[j] * s->ring[j];
    if (energy < 1e-7f) return;
    for (int i = 0; i < FM_CTCSS_COUNT; ++i) {
        float a = 0, b = 0;
        for (int j = 0; j < 300; ++j) {
            float v = s->ring[(s->pos + j) % 300] + s->coeff[i] * a - b;
            b = a; a = v;
        }
        float power = a*a + b*b - s->coeff[i]*a*b;
        if (power > best) { second = best; best = power; index = i; }
        else if (power > second) second = power;
    }
    float confidence = 2.0f * best / (300.0f * energy);
    /* A coherent 300 ms window qualifies a tone; neighbouring bins must
       lose decisively so drift between two standard tones cannot open audio. */
    if (confidence > 0.85f && best > second * 1.8f)
        report(s, index + 1, fminf(confidence, 1.0f));
}

static uint16_t decode(uint32_t word)
{
    for (int inv = 0; inv < 2; ++inv) {
        uint32_t w = word ^ (inv ? 0x7fffffu : 0);
        if ((w & 0xe00u) != 0x800u || fm_dcs_word(w & 0x1ffu) != w) continue;
        for (int i = 0; i < FM_DCS_COUNT; ++i)
            if (fm_dcs_codes[i] == (w & 0x1ffu)) return 51 + i + inv * FM_DCS_COUNT;
    }
    return 0;
}

static uint32_t selection_word(uint16_t selection)
{
    unsigned index = selection - 51;
    return fm_dcs_word(fm_dcs_codes[index % FM_DCS_COUNT]) ^
           (index >= FM_DCS_COUNT ? 0x7fffffu : 0);
}

static uint16_t canonical(uint32_t word)
{
    uint16_t best = 0;
    /* Some DCS code/polarity pairs are cyclic aliases on the air. Display
       the lowest numbered code consistently; gate matching accepts aliases. */
    for (int i = 0; i < 23; ++i) {
        uint16_t code = decode(word);
        if (code && (!best || (code - 51) % FM_DCS_COUNT < (best - 51) % FM_DCS_COUNT))
            best = code;
        word = (word >> 1) | ((word & 1u) << 22);
    }
    return best;
}

static void sample(fm_tone_t *s, float x)
{
    ++s->ticks;
    s->ring[s->pos] = x;
    s->pos = (s->pos + 1) % 300;
    if (s->filled < 300) ++s->filled;
    if (++s->hop == 50) {
        s->hop = 0;
        if (s->filled == 300) ctcss(s);
    }
    /* Eight symbol phases avoid assuming that a block starts on a bit edge.
       Windows shift LSB first, checking the fixed marker and all parity bits.
       Two identical words 171 ms apart reject accidental code-like speech. */
    for (int i = 0; i < 8; ++i) {
        s->dcs[i].phase += 1344;
        if (s->dcs[i].phase < 10000) continue;
        s->dcs[i].phase -= 10000;
        s->dcs[i].word = (s->dcs[i].word >> 1) | (x > 0 ? 0x400000u : 0);
        if (s->dcs[i].bits < 23) { ++s->dcs[i].bits; continue; }
        uint16_t code = decode(s->dcs[i].word);
        if (!code) continue;
        unsigned pol = code >= 51 + FM_DCS_COUNT;
        unsigned elapsed = s->ticks - s->dcs[i].last_tick[pol];
        if (code == s->dcs[i].candidate[pol] && elapsed >= 165 && elapsed <= 178)
            report(s, canonical(s->dcs[i].word), 1.0f);
        s->dcs[i].candidate[pol] = code;
        s->dcs[i].last_tick[pol] = s->ticks;
    }
    /* DCS refreshes once per word, so hang must exceed a word plus dropout. */
    if (s->result.selection && s->ticks - s->seen > 350)
        memset(&s->result, 0, sizeof(s->result));
}

void fm_tone_process(fm_tone_t *s, const float *audio, int n)
{
    for (int i = 0; i < n; ++i) {
        float x = audio[i];
        /* Four poles at 350 Hz suppress voice and aliasing before 32:1
           decimation. DC removal retains the 134.4 bit/s DCS waveform. */
        for (int p = 0; p < 4; ++p) {
            s->low[p] += 0.0664f * (x - s->low[p]);
            x = s->low[p];
        }
        s->sum += x;
        if (++s->decim < 32) continue;
        x = s->sum / 32.0f;
        s->sum = 0; s->decim = 0;
        s->dc += 0.02f * (x - s->dc);
        sample(s, x - s->dc);
    }
}

bool fm_tone_matches(uint16_t required, fm_tone_result_t result)
{
    if (!required) return true;
    if (required >= FM_TONE_CHOICES || !result.selection || result.selection >= FM_TONE_CHOICES)
        return false;
    if (required == result.selection) return true;
    if (required <= FM_CTCSS_COUNT || result.selection <= FM_CTCSS_COUNT) return false;
    uint32_t word = selection_word(required), received = selection_word(result.selection);
    for (int i = 0; i < 23; ++i) {
        if (word == received) return true;
        word = (word >> 1) | ((word & 1u) << 22);
    }
    return false;
}

bool fm_tone_receive(fm_tone_t *s, const float *audio, int n, bool carrier, uint16_t required)
{
    if (carrier) fm_tone_process(s, audio, n);
    else if (s->carrier) fm_tone_init(s);
    s->carrier = carrier;
    return carrier && fm_tone_matches(required, s->result);
}

void fm_tone_label(uint16_t selection, char *out, size_t n)
{
    if (!selection || selection >= FM_TONE_CHOICES) snprintf(out, n, "OFF");
    else if (selection <= FM_CTCSS_COUNT) {
        unsigned tone = fm_ctcss_tenths[selection - 1];
        snprintf(out, n, "%u.%u Hz", tone / 10, tone % 10);
    } else {
        unsigned i = selection - 51;
        snprintf(out, n, "D%03o%c", fm_dcs_codes[i % FM_DCS_COUNT],
                 i >= FM_DCS_COUNT ? 'I' : 'N');
    }
}
