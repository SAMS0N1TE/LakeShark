/* LS_TEST_SOURCES: ${APP}/dmr/dmr_sync.c ${APP}/dmr/dmr_slot.c
 *                  ${APP}/dmr/dmr_bptc.c ${APP}/dmr/dmr_lc.c
 *                  ${APP}/dmr/dmr_burst.c ${APP}/dmr/dmr_framer.c
 *                  ${APP}/dmr/dmr_watch.c
 *                  ${APP}/p25/dsp_pipeline.c ${APP}/p25/dsd_filters.c
 *                  ${APP}/p25/p25_cqpsk_controls.c
 *                  ${APP}/p25/p25_demod_control.c
 *                  fixtures/dmr_gen.c fixtures/dmr_iq_gen.c
 *
 * The seam between the receiver and the DMR decoder: 4FSK IQ at the rate the
 * RTL delivers, through the demodulator the board actually runs, out as
 * symbols.
 *
 * WHAT THIS DOES NOT COVER. Symbol timing recovery lives in getSymbol(), and
 * driving that needs the dsd_opts/dsd_state the frame-sync loop owns. These
 * cases subsample at a fixed phase instead, which leaves about 7% of bits
 * wrong even on a noiseless signal - enough that a 48-bit sync match is
 * marginal - so framing is NOT asserted here. test_dmr_watch covers framing,
 * from symbols. What is asserted is that the burst is still recoverable from
 * the demodulated stream, at the right polarity, which is what catches the
 * IQ-to-symbol path or the dibit mapping breaking.
 */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_watch.h"
#include "dmr_gen.h"
#include "dmr_iq_gen.h"
#include "dsp_pipeline.h"
#include "p25_demod_control.h"

#include <string.h>

/* p25_demod_control's hunt logic scores protocols; no hunt runs here. */
int p25_qual_protocol_score(const void *a, const void *b);
int p25_qual_protocol_score(const void *a, const void *b)
{
    (void)a; (void)b; return 0;
}

/* The demodulator logs through this; the bench has no console. */
void sys_log(unsigned char color, const char *fmt, ...)
{
    (void)color;
    (void)fmt;
}

static const uint8_t BS_VOICE[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };

#define IQ_MAX     (1 << 21)
#define AUDIO_MAX  (1 << 18)

static uint8_t s_iq[IQ_MAX];
static int16_t s_audio[AUDIO_MAX];

static dmr_lc_t reference_lc(void)
{
    dmr_lc_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.flco        = 0;
    lc.destination = 3172;
    lc.source      = 2621441;
    return lc;
}

static int build_iq(const dmr_lc_t *lc, int repeats,
                    const dmr_iq_channel_t *ch, ls_rng_t *rng)
{
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_VOICE, 7, 1, lc);

    int n = dmr_iq_render_idle(240, ch, rng, s_iq, IQ_MAX);
    for (int i = 0; i < repeats; i++) {
        const int got = dmr_iq_render_bits(burst, DMR_BURST_BITS, ch, rng,
                                           s_iq + n, IQ_MAX - n);
        if (got <= 0) break;
        n += got;
        n += dmr_iq_render_idle(24, ch, rng, s_iq + n, IQ_MAX - n);
    }
    return n;
}

static int demodulate(int iq_bytes, int *centre_out, int *umid_out,
                      int *lmid_out)
{
    dsp_state_t dsp;
    dsp_init(&dsp);
    /* The gain the P25 app uses for C4FM. At unity the discriminator output
       rounds to zero and the pipeline hands back silence. */
    dsp_select_mode(&dsp, DEMOD_C4FM,
                    p25_demod_output_gain(DEMOD_C4FM, false));

    int n_audio = 0;
    for (int off = 0; off < iq_bytes && n_audio < AUDIO_MAX; off += 4096) {
        const int take = (iq_bytes - off) < 4096 ? (iq_bytes - off) : 4096;
        n_audio += dsp_process_iq(&dsp, s_iq + off, take,
                                  s_audio + n_audio, AUDIO_MAX - n_audio);
    }
    if (n_audio <= 0) return 0;

    int lo = 32767, hi = -32768;
    for (int i = 0; i < n_audio; i++) {
        if (s_audio[i] < lo) lo = s_audio[i];
        if (s_audio[i] > hi) hi = s_audio[i];
    }
    const int centre = (hi + lo) / 2;
    if (centre_out) *centre_out = centre;
    if (umid_out)   *umid_out = centre + (hi - centre) / 2;
    if (lmid_out)   *lmid_out = centre + (lo - centre) / 2;
    return n_audio;
}

/* How many of the known burst's 264 bits survive, at the best sampling phase
   and alignment. `inverted_out` says which way round the discriminator
   presented them. */
static int burst_bit_errors(int n_audio, int centre, int umid, int lmid,
                            const dmr_lc_t *lc, int *inverted_out)
{
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_VOICE, 7, 1, lc);

    int best_err = 9999, best_inv = 0;
    static uint8_t got[4096];
    for (int phase = 0; phase < DSP_SPS; phase++) {
        int n_bits = 0;
        for (int i = phase; i < n_audio && n_bits + 1 < 4096; i += DSP_SPS) {
            const int v = s_audio[i];
            got[n_bits++] = (uint8_t)(v > centre ? 0 : 1);
            got[n_bits++] = (uint8_t)(v > umid ? 1 : v > centre ? 0
                                               : v > lmid ? 0 : 1);
        }
        for (int inv = 0; inv < 2; inv++)
            for (int at = 0; at + (int)DMR_BURST_BITS < n_bits; at++) {
                int err = 0;
                for (unsigned b = 0; b < DMR_BURST_BITS; b++) {
                    const int want = (burst[b >> 3] >> (7u - (b & 7u))) & 1u;
                    int have = got[at + b];
                    if (inv) have = !have;
                    if (have != want && ++err >= best_err) break;
                }
                if (err < best_err) { best_err = err; best_inv = inv; }
            }
    }
    if (inverted_out) *inverted_out = best_inv;
    return best_err;
}

LS_CASE(a_burst_rendered_as_iq_survives_the_demodulator)
{
    const dmr_lc_t lc = reference_lc();
    dmr_iq_channel_t ch = { .noise_sd = 0.0f, .freq_off_hz = 0.0f };
    ls_rng_t rng;
    ls_rng_seed(&rng, 20260920);

    const int bytes = build_iq(&lc, 4, &ch, &rng);
    LS_CHECK_MSG(bytes > 0, "no IQ was rendered");

    int centre = 0, umid = 0, lmid = 0;
    const int n_audio = demodulate(bytes, &centre, &umid, &lmid);
    LS_CHECK_MSG(n_audio > 0, "the demodulator produced no output");
    if (n_audio <= 0) return;
    LS_CHECK_MSG(umid - lmid > 500,
                 "the demodulated eye is only %d wide - no four levels",
                 umid - lmid);

    int inv = -1;
    const int err = burst_bit_errors(n_audio, centre, umid, lmid, &lc, &inv);
    ls_note("clean: %d bit errors of %u, inverted=%d", err, DMR_BURST_BITS, inv);

    /* Measured at 19 on a noiseless signal with this fixture and a fixed
       sampling phase. The bar is where a mapping error or a broken IQ path
       goes straight through it, not at the current number. */
    LS_CHECK_MSG(err < 40, "%d of %u bits wrong - the IQ path or the dibit "
                 "mapping has moved", err, DMR_BURST_BITS);
    LS_EQ_INT(inv, 0);
}

LS_CASE(noise_and_a_carrier_error_degrade_it_rather_than_break_it)
{
    const dmr_lc_t lc = reference_lc();
    dmr_iq_channel_t ch = { .noise_sd = 2.0f, .freq_off_hz = 300.0f };
    ls_rng_t rng;
    ls_rng_seed(&rng, 776655);

    const int bytes = build_iq(&lc, 4, &ch, &rng);
    int centre = 0, umid = 0, lmid = 0;
    const int n_audio = demodulate(bytes, &centre, &umid, &lmid);
    if (n_audio <= 0) { LS_CHECK_MSG(false, "no demodulator output"); return; }

    int inv = -1;
    const int err = burst_bit_errors(n_audio, centre, umid, lmid, &lc, &inv);
    ls_note("noisy: %d bit errors of %u", err, DMR_BURST_BITS);
    LS_CHECK_MSG(err < 60, "%d of %u bits wrong with 300 Hz of offset",
                 err, DMR_BURST_BITS);
}

LS_CASE(an_idle_carrier_produces_no_link_control)
{
    /* lc_ok is what says "this is a DMR channel", so an unmodulated carrier
       must not move it. */
    dmr_iq_channel_t ch = { .noise_sd = 1.0f, .freq_off_hz = 0.0f };
    ls_rng_t rng;
    ls_rng_seed(&rng, 12345);

    const int bytes = dmr_iq_render_idle(3000, &ch, &rng, s_iq, IQ_MAX);
    LS_CHECK(bytes > 0);

    int centre = 0, umid = 0, lmid = 0;
    const int n_audio = demodulate(bytes, &centre, &umid, &lmid);
    if (n_audio <= 0) { LS_CHECK_MSG(false, "no demodulator output"); return; }

    dmr_watch_reset();
    for (int i = 0; i < n_audio; i += DSP_SPS)
        dmr_watch_symbol(s_audio[i], centre, umid, lmid);

    dmr_watch_t w;
    LS_CHECK(dmr_watch_get(&w));
    LS_EQ_UINT(w.lc_ok, 0);
    LS_CHECK_MSG(!w.have_lc, "an idle carrier produced a link control");
}
