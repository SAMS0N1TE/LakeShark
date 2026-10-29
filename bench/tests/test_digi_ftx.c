/* FT8 / FT4: message packing, CRC, LDPC, tones against reference vectors
   generated with ft8_lib (MIT), and encode -> GFSK audio -> decode round trips
   in white noise at stated SNRs (2500 Hz reference bandwidth). */
#include "ls_test.h"
#include "digi_ftx.h"
#include "digi_ftx_tables.h"
#include "digi_msg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *text, *payload, *ft8, *ft4; } ref_t;
/* Produced by ft8_lib's ftx_message_encode / ft8_encode / ft4_encode. */
static const ref_t REF[] = {
    {"CQ K1ABC FN42", "000000204def1a8a1988",
     "3140652000000001005476704606021533433140652736011047517007334745455133543140652",
     "001321033112330313110222113111302210231223312331210203121200233032123101212323023000120100233321133032010"},
    {"K1ABC W9XYZ -11", "09bde3506149dc1faa08",
     "3140652032247523504061147017463022603140652054445103423557634070241144523140652",
     "001321002230213332310210120023311110230330110030222311323012102223023101120313000001322133100310132132010"},
    {"W9XYZ K1ABC R-09", "0c293b804def1abfaa88",
     "3140652020355725005476704627463523673140652461375524341536404620765601323140652",
     "001321013121232030210222113111302210233330110330212132301313113303323100033333313300212103332331312132010"},
    {"K1ABC W9XYZ RR73", "09bde3506149dc1fa4c8",
     "3140652032247523504061147017455422543140652656077704107145041657342273103140652",
     "001321002230213332310210120023311110230330133230223100321213021233223102312203232023230330110012101332010"},
    {"CQ DX G4ABC IO91", "000046f04860b30f84c8",
     "3140652000001047505515065407405431173140652420154411575451077765763624233140652",
     "001321033112320221010223023332033110231330233231332132100221010211123100100012332103133103312323111032010"},
    {"PA9XYZ G4ABC 73", "b7bac5404860b31fa508",
     "3140652667262063005515065417456025063140652127014576453750621130766072743140652",
     "001322221231010202210223023332033110230330132030003020112200013001023100320112200122211103330212020032010"},
    {"CQ PJ4/K1ABC", "000001a3a311caa00460",
     "3140652000000016073153143630005206073140652040337166016431570726475464323140652",
     "001321033112330300112010231323202210232113233102033121230302032101223101203032302120113000032103112132010"},
    {"W9XYZ K1ABC RRR", "0c293b804def1a9fa488",
     "3140652020355725005476704617455530313140652564305535161117524523127753273140652",
     "001321013121232030210222113111302210230330133331300331313301113330223103232220003100312222021001110132010"},
    {"K1ABC/R W9XYZ EN37", "09bde3586149dc085648",
     "3140652032247523404061147005134332153140652623707512241501513760247527103140652",
     "001321002230213332320210120023311110231212330131232000103031023212323103033322303230132133121223001332010"},
};
#define NREF ((int)(sizeof(REF) / sizeof(REF[0])))

static void hex10(const uint8_t *p, char *out)
{
    for (int i = 0; i < 10; ++i) sprintf(out + 2 * i, "%02x", p[i]);
}

LS_CASE(payloads_match_reference)
{
    for (int i = 0; i < NREF; ++i) {
        uint8_t p[10];
        char hex[24];
        LS_CHECK(digi_msg_pack(REF[i].text, p, NULL));
        hex10(p, hex);
        if (strcmp(hex, REF[i].payload)) printf("  %s: %s != %s\n", REF[i].text, hex, REF[i].payload);
        LS_EQ_STR(hex, REF[i].payload);
    }
}

LS_CASE(tones_match_reference)
{
    for (int i = 0; i < NREF; ++i) {
        uint8_t p[10], t[105];
        char s[106];
        for (int k = 0; k < 10; ++k) { unsigned v; sscanf(REF[i].payload + 2 * k, "%2x", &v); p[k] = (uint8_t)v; }
        LS_EQ_INT(digi_ftx_tones(DIGI_FT8, p, t), 79);
        for (int k = 0; k < 79; ++k) s[k] = (char)('0' + t[k]);
        s[79] = 0;
        LS_EQ_STR(s, REF[i].ft8);
        LS_EQ_INT(digi_ftx_tones(DIGI_FT4, p, t), 105);
        for (int k = 0; k < 105; ++k) s[k] = (char)('0' + t[k]);
        s[105] = 0;
        LS_EQ_STR(s, REF[i].ft4);
    }
}

LS_CASE(unpack_prints_what_was_packed)
{
    static const char *const M[] = {
        "CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC R-09", "K1ABC W9XYZ RR73",
        "CQ DX G4ABC IO91", "PA9XYZ G4ABC 73", "W9XYZ K1ABC RRR", "K1ABC/R W9XYZ EN37",
        "CQ PJ4/K1ABC", "CQ 291 W9XYZ EM48", "G4ABC/P PA9XYZ JO22", "K1ABC W9XYZ R EN37",
        "HELLO WORLD", "TNX 73 GL", "QRZ K1ABC FN42", "3DA0XYZ K1ABC +05", "W9XYZ K1ABC +99",
        "K1ABC W9XYZ -30",
    };
    digi_hash_t h;
    digi_hash_init(&h);
    for (unsigned i = 0; i < sizeof(M) / sizeof(M[0]); ++i) {
        uint8_t p[10];
        char out[DIGI_TEXT_MAX];
        digi_msg_fields_t f;
        LS_CHECK(digi_msg_pack(M[i], p, &h));
        LS_CHECK(digi_msg_unpack(p, out, sizeof(out), &h, &f));
        if (strcmp(out, M[i])) printf("  '%s' -> '%s'\n", M[i], out);
        LS_EQ_STR(out, M[i]);
    }
}

LS_CASE(fields_carry_grid_report_and_cq)
{
    uint8_t p[10];
    digi_msg_fields_t f;
    char out[DIGI_TEXT_MAX];
    LS_CHECK(digi_msg_pack("CQ K1ABC FN42", p, NULL));
    LS_CHECK(digi_msg_unpack(p, out, sizeof(out), NULL, &f));
    LS_CHECK(f.cq);
    LS_EQ_STR(f.call_de, "K1ABC");
    LS_EQ_STR(f.grid, "FN42");
    LS_CHECK(digi_msg_pack("K1ABC W9XYZ R-09", p, NULL));
    LS_CHECK(digi_msg_unpack(p, out, sizeof(out), NULL, &f));
    LS_CHECK(!f.cq);
    LS_CHECK(f.has_report);
    LS_EQ_INT(f.report, -9);
    LS_EQ_STR(f.call_to, "K1ABC");
    LS_EQ_STR(f.grid, "");
    /* Free text is free text, never two hashed "calls". */
    LS_CHECK(digi_msg_pack("HELLO WORLD", p, NULL));
    LS_CHECK(digi_msg_unpack(p, out, sizeof(out), NULL, &f));
    LS_EQ_INT(f.type, DIGI_MSG_FREE);
}

LS_CASE(hashed_call_resolves_once_heard)
{
    digi_hash_t h;
    digi_hash_init(&h);
    uint8_t p[10];
    char out[DIGI_TEXT_MAX];
    LS_CHECK(digi_msg_pack("<PJ4/K1ABC> W9XYZ RR73", p, NULL));
    LS_CHECK(digi_msg_unpack(p, out, sizeof(out), &h, NULL));
    LS_EQ_STR(out, "<...> W9XYZ RR73");
    digi_hash_save(&h, "PJ4/K1ABC");
    LS_CHECK(digi_msg_unpack(p, out, sizeof(out), &h, NULL));
    LS_EQ_STR(out, "<PJ4/K1ABC> W9XYZ RR73");
}

LS_CASE(crc_forms_agree_and_ldpc_codewords_check)
{
    srand(7);
    for (int trial = 0; trial < 50; ++trial) {
        uint8_t p[10], a91[12], cw[22];
        for (int k = 0; k < 10; ++k) p[k] = (uint8_t)rand();
        p[9] &= 0xF8;
        digi_ftx_add_crc(p, a91);
        LS_CHECK(digi_ftx_crc_ok(a91));
        uint8_t t[12];
        memcpy(t, p, 10); t[9] &= 0xF8; t[10] = t[11] = 0;
        uint16_t c = (uint16_t)(((a91[9] & 7) << 11) | (a91[10] << 3) | (a91[11] >> 5));
        LS_EQ_INT(digi_crc14(t, 82), c);
        a91[3] ^= 0x10;
        LS_CHECK(!digi_ftx_crc_ok(a91));
        a91[3] ^= 0x10;
        digi_ldpc_encode(a91, cw);
        /* The generator and the parity-check graph describe the same code. */
        for (int m = 0; m < DIGI_LDPC_M; ++m) {
            int par = 0;
            for (int k = 0; k < digi_ldpc_nrw[m]; ++k) {
                int b = digi_ldpc_nm[m][k] - 1;
                par ^= (cw[b / 8] >> (7 - b % 8)) & 1;
            }
            LS_EQ_INT(par, 0);
        }
        /* BP recovers it from noisy soft bits. */
        float llr[174];
        uint8_t bits[174];
        for (int b = 0; b < 174; ++b) {
            int v = (cw[b / 8] >> (7 - b % 8)) & 1;
            float n = ((float)rand() / RAND_MAX - 0.5f) * 3.0f;
            llr[b] = (v ? 2.0f : -2.0f) + n;
        }
        LS_EQ_INT(digi_ldpc_decode(llr, 30, bits), 0);
        for (int b = 0; b < 174; ++b) LS_EQ_INT(bits[b], (cw[b / 8] >> (7 - b % 8)) & 1);
    }
}

/* ---- audio round trips ------------------------------------------------- */

static float gauss(void)
{
    float u = ((float)rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    float v = ((float)rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

typedef struct { const char *text; float f0, dt, snr; } sig_t;

/* Make one slot of audio: signals at their SNR in white noise of sigma 0.02. */
static float *slot_audio(digi_ftx_mode_t m, const sig_t *s, int ns, int *n_out, unsigned seed)
{
    const digi_ftx_spec_t *sp = digi_ftx_spec(m);
    int n = sp->slot_ms * 12;
    float *x = calloc((size_t)n, sizeof(float));
    const float sigma = 0.02f;
    srand(seed);
    for (int i = 0; i < n; ++i) x[i] = sigma * gauss();
    for (int k = 0; k < ns; ++k) {
        uint8_t p[10], tones[105];
        digi_msg_pack(s[k].text, p, NULL);
        digi_ftx_tones(m, p, tones);
        /* SNR in 2500 Hz: P_sig = A^2/2, noise in 2500 Hz = sigma^2 * 2500/6000. */
        float a = sqrtf(2.0f * sigma * sigma * (2500.0f / 6000.0f) * powf(10.0f, s[k].snr / 10.0f));
        digi_ftx_synth(m, tones, s[k].f0, a, x, n, (int)((0.5f + s[k].dt) * 12000.0f));
    }
    *n_out = n;
    return x;
}

static int run_slot(digi_ftx_mode_t m, const sig_t *s, int ns, unsigned seed,
                    digi_ftx_result_t *res, int max, bool verbose)
{
    int n;
    float *x = slot_audio(m, s, ns, &n, seed);
    digi_ftx_dec_t d;
    LS_CHECK(digi_ftx_dec_init(&d, m, 200.0f, 3000.0f));
    digi_ftx_dec_reset(&d);
    /* Feed in uneven blocks, as a receiver would. */
    for (int i = 0; i < n;) {
        int k = 437 + (i / 437) % 300;
        if (k > n - i) k = n - i;
        digi_ftx_dec_feed(&d, x + i, k);
        i += k;
    }
    int got = digi_ftx_dec_run(&d, res, max, NULL);
    if (verbose)
        printf("  %s: %d cands, %d ldpc, %d decodes, %.0f ms\n", m == DIGI_FT8 ? "FT8" : "FT4",
               d.last_candidates, d.last_ldpc, got, d.last_us / 1000.0);
    digi_ftx_dec_free(&d);
    free(x);
    return got;
}

static const digi_ftx_result_t *find(const digi_ftx_result_t *r, int n, const char *text)
{
    for (int i = 0; i < n; ++i) if (!strcmp(r[i].text, text)) return &r[i];
    return NULL;
}

static void several_signals(digi_ftx_mode_t m)
{
    const sig_t S[] = {
        {"CQ K1ABC FN42", 600.0f, 0.0f, -10.0f},
        {"K1ABC W9XYZ -11", 1210.0f, 0.3f, -14.0f},
        {"W9XYZ K1ABC R-09", 1800.0f, -0.4f, -8.0f},
        {"CQ DX G4ABC IO91", 2400.0f, 1.0f, m == DIGI_FT8 ? -16.0f : -12.0f},
    };
    digi_ftx_result_t r[32];
    int n = run_slot(m, S, 4, 11u, r, 32, true);
    LS_EQ_INT(n, 4);
    for (int i = 0; i < 4; ++i) {
        const digi_ftx_result_t *x = find(r, n, S[i].text);
        LS_CHECK(x != NULL);
        if (!x) continue;
        printf("  %-18s f %7.1f (%6.1f)  dt %+5.2f (%+5.2f)  snr %+5.1f (%+5.1f)\n", x->text,
               x->freq_hz, S[i].f0, x->dt_s, S[i].dt, x->snr_db, S[i].snr);
        LS_CHECK(fabsf(x->freq_hz - S[i].f0) <= digi_ftx_spec(m)->tone_hz * 0.6f);
        LS_CHECK(fabsf(x->dt_s - S[i].dt) <= 0.1f);
        LS_CHECK(fabsf(x->snr_db - S[i].snr) <= 3.0f);
    }
}

LS_CASE(ft8_four_signals_decode_with_freq_dt_snr) { several_signals(DIGI_FT8); }
LS_CASE(ft4_four_signals_decode_with_freq_dt_snr) { several_signals(DIGI_FT4); }

/* Decode rate at a given SNR over several noise seeds. */
static int rate(digi_ftx_mode_t m, float snr, int trials)
{
    int ok = 0;
    for (int t = 0; t < trials; ++t) {
        sig_t s = {"K1ABC W9XYZ RR73", 1000.0f + 37.0f * t, 0.1f * (t % 5) - 0.2f, snr};
        digi_ftx_result_t r[8];
        int n = run_slot(m, &s, 1, 100u + (unsigned)t, r, 8, false);
        if (find(r, n, s.text)) ok++;
    }
    printf("  %s at %+.0f dB: %d/%d\n", m == DIGI_FT8 ? "FT8" : "FT4", snr, ok, trials);
    return ok;
}

/* Host-measured 50% points (30 trials each): FT8 about -18.7 dB, FT4 about
   -14.6 dB. WSJT-X quotes -21 and -17.5 with its OSD and multi-symbol
   demodulators, which this decoder does not have yet. */
LS_CASE(ft8_decodes_at_minus_10_16_and_18)
{
    LS_EQ_INT(rate(DIGI_FT8, -10.0f, 6), 6);
    LS_EQ_INT(rate(DIGI_FT8, -16.0f, 10), 10);
    LS_CHECK(rate(DIGI_FT8, -18.0f, 10) >= 5);
    rate(DIGI_FT8, -20.0f, 10);   /* reported, not asserted */
}

LS_CASE(ft4_decodes_at_minus_10_and_minus_13)
{
    LS_EQ_INT(rate(DIGI_FT4, -10.0f, 6), 6);
    LS_CHECK(rate(DIGI_FT4, -13.0f, 10) >= 9);
    rate(DIGI_FT4, -15.0f, 10);
}

LS_CASE(noise_alone_decodes_nothing)
{
    for (unsigned seed = 1; seed <= 3; ++seed) {
        digi_ftx_result_t r[8];
        LS_EQ_INT(run_slot(DIGI_FT8, NULL, 0, seed, r, 8, seed == 1), 0);
        LS_EQ_INT(run_slot(DIGI_FT4, NULL, 0, seed, r, 8, false), 0);
    }
}

