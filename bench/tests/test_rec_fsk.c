/* LS_TEST_SOURCES: rec_fsk.c, ls_sub_fsk.c
 *
 * LS-1241: REC saved a 2-FSK trap transmitter as OOK. These drive the FSK
 * slicer with synthetic IQ shaped like that transmitter - 2400 baud, 19 kHz
 * deviation, 64-bit preamble, sync D391D391, six payload bytes - and check
 * the bits, the deviation and the bit rate come back, and that the .sub
 * preset built from them is the operator's known-working one.
 */

#include "ls_test.h"
#include "rec_fsk.h"
#include "ls_sub_fsk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FS      256000.0f
#define BAUD    2400.0f
#define DEV     19000.0f
#define MAXBITS 256
#define MAXEDGE 512

static int frame_bits(uint8_t *bits)
{
    static const uint8_t payload[] = { 0x5A, 0x13, 0xC7, 0x00, 0xFF, 0x81 };
    int n = 0;
    for (int i = 0; i < 64; i++) bits[n++] = (uint8_t)((i & 1) == 0);
    const uint32_t sync = 0xD391D391u;
    for (int i = 31; i >= 0; i--) bits[n++] = (uint8_t)((sync >> i) & 1u);
    for (size_t b = 0; b < sizeof(payload); b++)
        for (int i = 7; i >= 0; i--) bits[n++] = (uint8_t)((payload[b] >> i) & 1u);
    return n;
}

typedef struct {
    int32_t edge[MAXEDGE];
    int     n;
    uint32_t dev;
} capture_t;

/* Run a burst through the slicer the way the recorder does: silence, the
   frame, silence; run-length the output into signed microseconds. */
/* A short carrier before the frame, as noise over the gate threshold looks
   to the slicer: `blip_len` samples at `blip_hz`, well before the frame. */
static int   s_blip_len;
static float s_blip_hz;

static void run(const uint8_t *bits, int nbits, float offset_hz, float noise,
                unsigned seed, capture_t *out)
{
    rec_fsk_t s;
    rec_fsk_init(&s, FS);
    ls_rng_t rng;
    ls_rng_seed(&rng, seed);

    const int lead = 2000, tail = 2000;
    const float spb = FS / BAUD;
    const int body = (int)(nbits * spb);
    float phase = 0.0f;

    memset(out, 0, sizeof(*out));
    bool level = false;
    uint32_t run_len = 0;
    bool started = false;

    for (int k = 0; k < lead + body + tail; k++) {
        const bool blip = k >= 500 && k < 500 + s_blip_len;
        const bool carrier = blip || (k >= lead && k < lead + body);
        float ii = 0.0f, qq = 0.0f;
        if (blip) {
            phase += 6.28318530718f * s_blip_hz / FS;
            ii = 60.0f * cosf(phase);
            qq = 60.0f * sinf(phase);
        } else if (carrier) {
            const int b = (int)((k - lead) / spb);
            const float f = offset_hz + (bits[b] ? DEV : -DEV);
            phase += 6.28318530718f * f / FS;
            ii = 60.0f * cosf(phase);
            qq = 60.0f * sinf(phase);
        }
        ii += noise * 127.0f * ls_rng_noise(&rng);
        qq += noise * 127.0f * ls_rng_noise(&rng);
        if (k == lead) rec_fsk_clear_stats(&s);
        const bool hi = rec_fsk_step(&s, (int)lrintf(ii), (int)lrintf(qq), carrier);

        if (!started) {
            if (k < lead || !hi) continue;   /* the frame is what is scored */
            started = true;
            level = true;
            run_len = 1;
            continue;
        }
        if (hi == level) { run_len++; continue; }
        const int32_t us = (int32_t)lrintf(run_len * 1e6f / FS);
        if (out->n < MAXEDGE) out->edge[out->n++] = level ? us : -us;
        level = hi;
        run_len = 1;
    }
    out->dev = rec_fsk_deviation(&s);
}

/* Bits back out of edges at the known rate. */
static int to_bits(const capture_t *c, uint8_t *bits, int max)
{
    const float bit_us = 1e6f / BAUD;
    int n = 0;
    for (int i = 0; i < c->n; i++) {
        const int32_t v = c->edge[i];
        const int k = (int)lrintf((float)abs(v) / bit_us);
        for (int j = 0; j < k && n < max; j++) bits[n++] = (uint8_t)(v > 0);
    }
    return n;
}

/* The slicer may spend the first preamble bit finding the second tone -
   that is what a preamble is for - so align within a few bits and forgive
   the first SKIP bits. Everything after, sync and payload included, must
   match exactly. */
#define SKIP 4

static int bit_errors(const uint8_t *want, int nwant, const uint8_t *got, int ngot)
{
    int best = nwant;
    for (int shift = -2; shift <= 2; shift++) {
        int errs = 0;
        for (int i = SKIP; i < nwant; i++) {
            const int j = i + shift;
            if (j < 0 || j >= ngot || want[i] != got[j]) errs++;
        }
        if (errs < best) best = errs;
    }
    return best;
}

static void check_case(float offset, float noise, unsigned seed)
{
    uint8_t bits[MAXBITS], back[MAXBITS * 2];
    const int n = frame_bits(bits);
    capture_t c;
    run(bits, n, offset, noise, seed, &c);
    const int got = to_bits(&c, back, (int)sizeof(back));
    /* The frame ends on a one, then silence: trailing zeros are the gap. */
    LS_CHECK_MSG(bit_errors(bits, n, back, got) == 0,
                 "offset %.0f Hz noise %.2f: %d bit errors",
                 offset, noise, bit_errors(bits, n, back, got));
    LS_NEAR(c.dev, DEV, DEV * 0.08);
    LS_NEAR(rec_fsk_bitrate(c.edge, c.n, 40), BAUD, BAUD * 0.03);
}

LS_CASE(a_trap_frame_on_frequency_comes_back_bit_for_bit)
{
    check_case(0.0f, 0.02f, 1);
}

LS_CASE(a_tuner_eight_kilohertz_off_still_slices_on_the_centre)
{
    check_case(8000.0f, 0.02f, 2);
    check_case(-8000.0f, 0.02f, 3);
}

LS_CASE(both_tones_on_one_side_of_zero_are_still_told_apart)
{
    /* 25 kHz off with 19 kHz deviation puts both tones above zero. */
    check_case(25000.0f, 0.02f, 4);
    check_case(-25000.0f, 0.02f, 5);
}

LS_CASE(noise_at_a_usable_level_costs_no_bits)
{
    check_case(3000.0f, 0.10f, 6);
}

LS_CASE(a_noise_blip_while_armed_does_not_poison_the_next_burst)
{
    /* On the bench a 40-sample blip at -59 kHz left a tone there, the
       centre at -33 kHz, and a 2.3 s trap burst sliced as one long mark. */
    s_blip_len = 40;
    s_blip_hz  = -59000.0f;
    check_case(-7000.0f, 0.02f, 7);
    s_blip_hz  = 70000.0f;
    check_case(3000.0f, 0.02f, 8);
    s_blip_len = 0;
}

LS_CASE(silence_reads_as_a_space)
{
    rec_fsk_t s;
    rec_fsk_init(&s, FS);
    LS_CHECK(!rec_fsk_step(&s, 50, 0, false));
    LS_EQ_INT(0, (int)rec_fsk_deviation(&s));
}

LS_CASE(bit_rate_ignores_the_bounding_runs_and_chatter)
{
    /* First and last runs are burst-bounded; a 20 us sliver is chatter. */
    const int32_t e[] = { 90, -417, 420, -414, 833, -20, 417, -1250, 419, -30000 };
    LS_NEAR(rec_fsk_bitrate(e, 10, 40), 2400, 20);
    LS_EQ_INT(0, (int)rec_fsk_bitrate(e, 1, 40));
}

LS_CASE(the_measured_trap_writes_the_known_working_preset)
{
    /* The operator's working trap file, byte for byte. */
    const ls_sub_mod_t mod = { .bitrate = 2400, .deviation_hz = 19000 };
    char text[320];
    LS_CHECK(ls_sub_preset_text(&mod, text, sizeof(text)) > 0);
    LS_CHECK(strstr(text, "Preset: FuriHalSubGhzPresetCustom\n") != NULL);
    LS_CHECK(strstr(text,
        "Custom_preset_data: 02 0D 0B 06 08 32 07 04 14 00 13 02 12 04 11 83 10 67 15 34 "
        "18 18 19 16 1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00\n") != NULL);
}

LS_CASE(a_loaded_file_reports_what_it_will_replay_as)
{
    uint32_t dev = 1;
    LS_CHECK(!rec_fsk_preset_is_fsk("FuriHalSubGhzPresetOok650Async", NULL, &dev));
    LS_EQ_INT(0, (int)dev);
    LS_CHECK(rec_fsk_preset_is_fsk("FuriHalSubGhzPresetCustom",
        "02 0D 0B 06 08 32 07 04 14 00 13 02 12 04 11 83 10 67 15 34 18 18 19 16 "
        "1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00 C0 00 00 00 00 00 00 00", &dev));
    LS_EQ_INT(19043, (int)dev);
    /* The same registers with MDMCFG2 set for ASK/OOK. */
    LS_CHECK(!rec_fsk_preset_is_fsk("FuriHalSubGhzPresetCustom",
        "02 0D 12 30 15 34 00 00 C0 00 00 00 00 00 00 00", &dev));
    LS_CHECK(rec_fsk_preset_is_fsk("FuriHalSubGhzPreset2FSKDev476Async", NULL, &dev));
    LS_EQ_INT(47607, (int)dev);
}
