/* WSPR: packing and channel symbols against a published reference (the
   DB0ABC JN59 12 vector in ftl/digimodes, MIT, which follows G4JNT's
   description), Fano decoding of noisy soft bits, and a full two-minute
   synthetic receive at several SNRs in 2500 Hz. */
#include "ls_test.h"
#include "digi_wspr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char REF_DB0ABC[] =
    "132200001022113022320323131222222230012120020230112033210001301022033030323030012010112223101230223020001201023332112213232023310220032100130020020112301122013002";

LS_CASE(symbols_match_reference)
{
    uint8_t data[7], sym[162];
    char s[163];
    LS_EQ_INT((int)strlen(REF_DB0ABC), 162);
    LS_CHECK(digi_wspr_pack("DB0ABC", "JN59", 12, data));
    digi_wspr_encode(data, sym);
    for (int i = 0; i < 162; ++i) s[i] = (char)('0' + sym[i]);
    s[162] = 0;
    LS_EQ_STR(s, REF_DB0ABC);
}

LS_CASE(pack_unpack_round_trip)
{
    static const struct { const char *call, *grid; int dbm; } M[] = {
        {"K1ABC", "FN42", 37}, {"DB0ABC", "JN59", 13}, {"G4JNT", "IO90", 30},
        {"VK2XYZ", "QF56", 23}, {"W1A", "FN31", 0}, {"9A1AA", "JN75", 60},
    };
    for (unsigned i = 0; i < sizeof(M) / sizeof(M[0]); ++i) {
        uint8_t data[7];
        char call[12], grid[7];
        int dbm;
        LS_CHECK(digi_wspr_pack(M[i].call, M[i].grid, M[i].dbm, data));
        bool t1 = digi_wspr_unpack(data, call, grid, &dbm);
        if (!t1) printf("  %s %s %d -> %s %s %d\n", M[i].call, M[i].grid, M[i].dbm, call, grid, dbm);
        LS_CHECK(t1);
        LS_EQ_STR(call, M[i].call);
        LS_EQ_STR(grid, M[i].grid);
        LS_EQ_INT(dbm, M[i].dbm);
    }
    uint8_t data[7];
    LS_CHECK(!digi_wspr_pack("K1ABCDE", "FN42", 37, data));
    LS_CHECK(!digi_wspr_pack("K1ABC", "ZZ42", 37, data));
}

static float gauss(void)
{
    float u = ((float)rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    float v = ((float)rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

LS_CASE(fano_corrects_noisy_soft_bits)
{
    srand(3);
    int ok = 0;
    for (int t = 0; t < 20; ++t) {
        uint8_t data[7], sym[162], soft[162], got[7];
        digi_wspr_pack("K1ABC", "FN42", (t % 6) * 10, data);
        digi_wspr_encode(data, sym);
        for (int i = 0; i < 162; ++i) {
            float v = ((sym[i] >> 1) ? 1.0f : -1.0f) + 0.9f * gauss();   /* about 11% hard errors */
            float r = 127.5f + 48.0f * v;
            soft[i] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        }
        long cyc = 0;
        if (digi_wspr_fano(soft, got, 500000L, &cyc) && !memcmp(got, data, 7)) ok++;
    }
    printf("  fano: %d/20\n", ok);
    LS_CHECK(ok >= 18);
}

typedef struct { const char *call, *grid; int dbm; float f, dt, snr, drift; } wsig_t;

static int receive(const wsig_t *s, int ns, unsigned seed, digi_wspr_result_t *r, int max, bool verbose)
{
    const long n = 120L * 12000;
    float *x = malloc(sizeof(float) * (size_t)n);
    const float sigma = 0.02f;
    srand(seed);
    for (long i = 0; i < n; ++i) x[i] = sigma * gauss();
    for (int k = 0; k < ns; ++k) {
        uint8_t data[7], sym[162];
        digi_wspr_pack(s[k].call, s[k].grid, s[k].dbm, data);
        digi_wspr_encode(data, sym);
        float a = sqrtf(2.0f * sigma * sigma * (2500.0f / 6000.0f) * powf(10.0f, s[k].snr / 10.0f));
        if (s[k].drift == 0.0f) {
            digi_wspr_synth(sym, s[k].f, a, x, n, (long)((1.0f + s[k].dt) * 12000));
        } else {
            /* Linear drift across the transmission. */
            double ph = 0;
            long start = (long)((1.0f + s[k].dt) * 12000), len = 162L * DIGI_WSPR_SPS;
            for (long t = 0; t < len; ++t) {
                double f = s[k].f + s[k].drift * ((double)t / len - 0.5) + DIGI_WSPR_TONE_HZ * sym[t / DIGI_WSPR_SPS];
                ph += 6.283185307179586 * f / 12000.0;
                if (start + t < n) x[start + t] += a * (float)sin(ph);
            }
        }
    }
    digi_wspr_dec_t *d = calloc(1, sizeof(*d));
    LS_CHECK(digi_wspr_dec_init(d));
    digi_wspr_dec_reset(d);
    for (long i = 0; i < n; i += 1000) digi_wspr_dec_feed(d, x + i, (int)(n - i < 1000 ? n - i : 1000));
    int got = digi_wspr_dec_run(d, r, max);
    if (verbose) printf("  wspr: %d candidates, %d decodes, %.0f ms\n", d->last_candidates, got, d->last_us / 1000.0);
    digi_wspr_dec_free(d);
    free(d);
    free(x);
    return got;
}

LS_CASE(three_beacons_decode_with_freq_dt_snr)
{
    const wsig_t S[] = {
        {"K1ABC", "FN42", 37, 1450.0f, 0.0f, -20.0f, 0.0f},
        {"G4JNT", "IO90", 30, 1520.3f, 0.8f, -24.0f, 0.0f},
        {"VK2XYZ", "QF56", 23, 1580.0f, -0.5f, -22.0f, 1.5f},
    };
    digi_wspr_result_t r[16];
    int n = receive(S, 3, 5u, r, 16, true);
    for (int i = 0; i < n; ++i) printf("  got %s %+.1f dB %.1f Hz agree %d\n", r[i].text, r[i].snr_db, r[i].freq_hz, r[i].agree);
    LS_EQ_INT(n, 3);
    for (int k = 0; k < 3; ++k) {
        char want[32];
        snprintf(want, sizeof(want), "%s %s %d", S[k].call, S[k].grid, S[k].dbm);
        const digi_wspr_result_t *x = NULL;
        for (int i = 0; i < n; ++i) if (!strcmp(r[i].text, want)) x = &r[i];
        LS_CHECK(x != NULL);
        if (!x) continue;
        float fc = S[k].f + 1.5f * DIGI_WSPR_TONE_HZ;
        printf("  %-18s f %7.1f (%7.1f) dt %+5.2f (%+5.2f) snr %+5.1f (%+5.1f) drift %+4.1f\n",
               x->text, x->freq_hz, fc, x->dt_s, S[k].dt, x->snr_db, S[k].snr, x->drift_hz);
        LS_CHECK(fabsf(x->freq_hz - fc) < 1.0f);
        LS_CHECK(fabsf(x->dt_s - S[k].dt) < 0.25f);
        LS_CHECK(fabsf(x->snr_db - S[k].snr) < 3.0f);
    }
}

LS_CASE(wspr_decodes_at_minus_26)
{
    int ok = 0;
    for (int t = 0; t < 4; ++t) {
        wsig_t s = {"G4JNT", "IO90", 30, 1430.0f + 31.0f * t, 0.2f * t - 0.2f, -26.0f, 0.0f};
        digi_wspr_result_t r[8];
        int n = receive(&s, 1, 40u + (unsigned)t, r, 8, false);
        for (int i = 0; i < n; ++i) if (!strcmp(r[i].text, "G4JNT IO90 30")) ok++;
    }
    printf("  WSPR at -26 dB: %d/4\n", ok);
    LS_CHECK(ok >= 3);
    int weak = 0;   /* reported, not asserted: WSJT-X quotes -28 */
    for (int t = 0; t < 4; ++t) {
        wsig_t s = {"G4JNT", "IO90", 30, 1440.0f + 29.0f * t, 0.1f * t, -28.0f, 0.0f};
        digi_wspr_result_t r[8];
        int n = receive(&s, 1, 80u + (unsigned)t, r, 8, false);
        for (int i = 0; i < n; ++i) if (!strcmp(r[i].text, "G4JNT IO90 30")) weak++;
    }
    printf("  WSPR at -28 dB: %d/4\n", weak);
}

LS_CASE(noise_alone_decodes_nothing)
{
    digi_wspr_result_t r[8];
    LS_EQ_INT(receive(NULL, 0, 9u, r, 8, true), 0);
}
