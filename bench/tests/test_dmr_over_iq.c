/* LS_TEST_SOURCES: ${DMR_WATCH_SRCS}
 *                  ${APP}/p25/dsp_pipeline.c ${APP}/p25/dsd_filters.c
 *                  ${APP}/p25/p25_cqpsk_controls.c
 *                  ${APP}/p25/p25_demod_control.c
 *                  fixtures/dmr_iq_gen.c
 *
 * The seam between the receiver and the DMR decoder: 4FSK IQ at the rate the
 * radio delivers, through the demodulator the board actually runs, then the
 * DMR watcher's own timing recovery, framer and FEC, out as a talkgroup and a
 * radio ID.
 *
 * The bursts are shaped with the root raised cosine ETSI specifies, so the
 * receiver's RRC makes a raised cosine of it and there is one right sampling
 * instant per symbol for the timing loop to find - and a wrong one costing
 * real errors when it does not. Carrier and symbol clock offsets, noise and
 * a spectrum the other way round are all applied at IQ, where a radio would
 * apply them.
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
static const uint8_t BS_DATA [6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };
static const uint8_t MS_DATA [6] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };
/* Systematic TACT, AT=1, TC=0/1, LCSS=00 - slot 1 and slot 2. */
static const uint8_t CACH[2][3] = { {0x80, 0x02, 0x02}, {0x88, 0x00, 0x20} };

#define SYM_MAX    8192
#define PAIRS_MAX  (SYM_MAX * 52)
#define IQ_MAX     (1 << 21)
#define AUDIO_MAX  (1 << 19)

static float   s_dev[SYM_MAX];
static float   s_fiq[PAIRS_MAX * 2];
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

/* A repeater's outbound stream: CACH then burst, slot after slot, every
   burst a voice LC header for the same call in slot 1 and idle-pattern
   payload in slot 2. Preceded by `lead` symbols of the alternating idle
   pattern, or of nothing at all if `lead_off`. */
static int build_bs(const dmr_lc_t *lc, int bursts, int lead, bool lead_off)
{
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_DATA, 7, 1, lc);
    int n = lead_off ? dmr_iq_dev_off(s_dev, 0, SYM_MAX, lead)
                     : dmr_iq_dev_idle(s_dev, 0, SYM_MAX, lead);
    for (int i = 0; i < bursts && n > 0; i++) {
        n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, CACH[i & 1], 24);
        if (n < 0) break;
        if (i & 1) n = dmr_iq_dev_idle(s_dev, n, SYM_MAX, 132);
        else       n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, burst, DMR_BURST_BITS);
    }
    if (n > 0) n = dmr_iq_dev_idle(s_dev, n, SYM_MAX, 60);
    return n;
}

/* The float path is the LR2021's. Returns audio samples. */
static int demodulate_float(int pairs)
{
    dsp_state_t dsp;
    dsp_init(&dsp);
    /* The gain the P25 app uses for C4FM. At unity the discriminator output
       rounds to zero and the pipeline hands back silence. */
    dsp_select_mode(&dsp, DEMOD_C4FM, p25_demod_output_gain(DEMOD_C4FM, false));
    int n_audio = 0;
    for (int off = 0; off < pairs && n_audio < AUDIO_MAX; off += 2048) {
        const int take = (pairs - off) < 2048 ? (pairs - off) : 2048;
        n_audio += dsp_process_float_iq(&dsp, s_fiq + 2 * off, take,
                                        s_audio + n_audio, AUDIO_MAX - n_audio);
    }
    return n_audio;
}

static int run(const dmr_iq_channel_t *ch, int n_sym, unsigned seed, dmr_watch_t *w)
{
    ls_rng_t rng;
    ls_rng_seed(&rng, seed);
    const int pairs = dmr_iq_render_stream(s_dev, n_sym, ch, &rng, s_fiq, PAIRS_MAX);
    if (pairs <= 0) return 0;
    const int n_audio = demodulate_float(pairs);
    dmr_watch_reset();
    dmr_watch_samples(s_audio, n_audio);
    memset(w, 0, sizeof(*w));
    dmr_watch_get(w);
    return n_audio;
}

static void check_call(const dmr_watch_t *w, const dmr_lc_t *lc, int want_lc,
                       const char *what)
{
    LS_CHECK_MSG(w->bursts >= (uint32_t)want_lc, "%s: %u bursts framed", what,
                 (unsigned)w->bursts);
    LS_CHECK_MSG(w->lc_ok >= (uint32_t)want_lc, "%s: %u of %d link controls exact "
                 "(%u RS-fixed)", what, (unsigned)w->lc_ok, want_lc,
                 (unsigned)w->lc_fixed);
    LS_CHECK_MSG(w->have_lc, "%s: no link control", what);
    if (!w->have_lc) return;
    LS_EQ_UINT(w->lc.destination, lc->destination);
    LS_EQ_UINT(w->lc.source, lc->source);
    LS_EQ_UINT(w->colour_code, 7);
    LS_CHECK_MSG(w->call[0].have_lc && w->call[0].active,
                 "%s: slot 1 has no live call", what);
    LS_EQ_INT((int)dmr_watch_verdict(w), (int)DMR_VERDICT_DMR);
}

LS_CASE(a_repeater_stream_frames_and_decodes_from_iq)
{
    const dmr_lc_t lc = reference_lc();
    const int n = build_bs(&lc, 12, 240, false);
    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC };
    dmr_watch_t w;
    LS_CHECK(run(&ch, n, 1, &w) > 0);
    ls_note("clean: bursts=%u lc=%u fixed=%u resliced=%u polarity=%d",
            (unsigned)w.bursts, (unsigned)w.lc_ok, (unsigned)w.lc_fixed,
            (unsigned)w.refined, (int)w.polarity);
    /* Six headers in slot 1; slot 2's idle payload carries a data sync too
       but no checked content. */
    check_call(&w, &lc, 6, "clean");
    LS_EQ_INT(w.polarity, 0);
    LS_CHECK(w.from_samples);
}

LS_CASE(the_other_polarity_decodes_and_says_so)
{
    const dmr_lc_t lc = reference_lc();
    const int n = build_bs(&lc, 12, 240, false);
    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .invert = 1 };
    dmr_watch_t w;
    run(&ch, n, 2, &w);
    ls_note("inverted: bursts=%u lc=%u polarity=%d", (unsigned)w.bursts,
            (unsigned)w.lc_ok, (int)w.polarity);
    check_call(&w, &lc, 6, "inverted");
    LS_EQ_INT(w.polarity, 1);
}

LS_CASE(a_kilohertz_of_carrier_error_either_way_is_followed)
{
    const dmr_lc_t lc = reference_lc();
    const int n = build_bs(&lc, 12, 240, false);
    for (int s = -1; s <= 1; s += 2) {
        dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .freq_off_hz = 1000.0f * s,
                                .cn_db = 20.0f };
        dmr_watch_t w;
        run(&ch, n, 30u + (unsigned)s, &w);
        ls_note("%+d Hz at 20 dB: bursts=%u lc=%u fixed=%u", 1000 * s,
                (unsigned)w.bursts, (unsigned)w.lc_ok, (unsigned)w.lc_fixed);
        check_call(&w, &lc, 6, s < 0 ? "-1 kHz" : "+1 kHz");
    }
}

LS_CASE(an_inverted_signal_with_offset_noise_and_clock_error_still_decodes)
{
    const dmr_lc_t lc = reference_lc();
    const int n = build_bs(&lc, 12, 240, false);
    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .invert = 1, .freq_off_hz = -1000.0f,
                            .cn_db = 18.0f, .clock_ppm = 100.0f, .start_sym = 0.37f };
    dmr_watch_t w;
    run(&ch, n, 77, &w);
    ls_note("everything at once: bursts=%u lc=%u fixed=%u polarity=%d",
            (unsigned)w.bursts, (unsigned)w.lc_ok, (unsigned)w.lc_fixed, (int)w.polarity);
    check_call(&w, &lc, 5, "combined");
    LS_EQ_INT(w.polarity, 1);
}

LS_CASE(a_mobile_burst_out_of_silence_is_resliced_at_its_sync)
{
    /* No carrier ahead of it: the discriminator has been reading noise, the
       level tracker has an eye full of it and the timing loop has nothing.
       A mobile's first burst is what that looks like. */
    const dmr_lc_t lc = reference_lc();
    uint8_t burst[33];
    dmr_gen_burst(burst, MS_DATA, 7, 1, &lc);
    int n = dmr_iq_dev_off(s_dev, 0, SYM_MAX, 400);
    n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, burst, DMR_BURST_BITS);
    n = dmr_iq_dev_off(s_dev, n, SYM_MAX, 156);
    n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, burst, DMR_BURST_BITS);
    n = dmr_iq_dev_off(s_dev, n, SYM_MAX, 200);
    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .cn_db = 25.0f, .start_sym = 0.5f };
    dmr_watch_t w;
    run(&ch, n, 5, &w);
    ls_note("mobile: bursts=%u lc=%u fixed=%u resliced=%u", (unsigned)w.bursts,
            (unsigned)w.lc_ok, (unsigned)w.lc_fixed, (unsigned)w.refined);
    LS_CHECK_MSG(w.lc_ok + w.lc_fixed >= 1, "neither mobile burst decoded");
    LS_CHECK_MSG(w.refined >= 1, "no burst was re-sliced at its sync");
    LS_CHECK(w.have_lc && w.lc.source == lc.source);
}

LS_CASE(a_call_joined_mid_superframe_is_identified_from_iq)
{
    /* Voice only, no header: bursts B-F have no sync and are read by
       counting 576 bits on from A's, through the timing loop's own slicing.
       If the soft sync's idea of where A ends and the framers' bit count
       disagree by so much as a symbol, no embedded LC assembles. */
    dmr_lc_t lc = reference_lc();
    uint8_t lc9[9], idle_block[12] = {0}, idle[33], b[33];
    dmr_lc_pack(&lc, lc9);
    uint32_t frag[4];
    dmr_emb_lc_encode(lc9, frag);
    dmr_gen_data_burst(idle, BS_DATA, 7, 9, idle_block);
    static const uint8_t lcss[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };

    int n = dmr_iq_dev_idle(s_dev, 0, SYM_MAX, 240);
    for (int sf = 0; sf < 3; sf++)
        for (int i = 0; i < 6; i++) {
            if (i == 0) dmr_gen_voice_burst(b, BS_VOICE, 0, 0, (unsigned)(sf * 6));
            else dmr_gen_voice_burst(b, NULL,
                                     dmr_emb_encode(7, false, i < 5 ? lcss[i - 1] : DMR_LCSS_SINGLE),
                                     i < 5 ? frag[i - 1] : 0, (unsigned)(sf * 6 + i));
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, CACH[0], 24);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, b, DMR_BURST_BITS);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, CACH[1], 24);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, idle, DMR_BURST_BITS);
        }
    n = dmr_iq_dev_idle(s_dev, n, SYM_MAX, 60);
    LS_CHECK(n > 0);

    for (int inv = 0; inv < 2; inv++) {
        dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .cn_db = 20.0f, .invert = inv,
                                .freq_off_hz = inv ? -700.0f : 700.0f };
        dmr_watch_t w;
        run(&ch, n, 900u + (unsigned)inv, &w);
        ls_note("late entry %s: bursts=%u embedded=%u fail=%u voice B-F=%u polarity=%d",
                inv ? "inverted" : "normal", (unsigned)w.bursts, (unsigned)w.emb_lc_ok,
                (unsigned)w.emb_lc_fail, (unsigned)w.voice_bursts, (int)w.polarity);
        LS_CHECK_MSG(w.emb_lc_ok == 3, "%u embedded LCs of 3", (unsigned)w.emb_lc_ok);
        LS_EQ_INT(w.polarity, inv);
        LS_CHECK(w.call[0].have_lc && w.call[0].late_entry);
        LS_EQ_UINT(w.call[0].lc.source, lc.source);
        LS_EQ_UINT(w.call[0].lc.destination, lc.destination);
        LS_EQ_UINT(w.lc_ok, 0);
        LS_EQ_INT((int)dmr_watch_verdict(&w), (int)DMR_VERDICT_DMR);
    }
}

LS_CASE(the_older_ramp_shaped_u8_stream_frames_too)
{
    /* The fixture this test used to sample at one fixed phase, through the
       unsigned 8-bit path an RTL takes. With timing recovered it is not 7%
       of bits wrong any more; it frames and the LC checks. */
    const dmr_lc_t lc = reference_lc();
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_VOICE, 7, 1, &lc);
    dmr_gen_burst(burst, BS_DATA, 7, 1, &lc);
    dmr_iq_channel_t ch = { .noise_sd = 2.0f, .freq_off_hz = 300.0f };
    ls_rng_t rng;
    ls_rng_seed(&rng, 776655);
    int bytes = dmr_iq_render_idle(240, &ch, &rng, s_iq, IQ_MAX);
    for (int i = 0; i < 4; i++) {
        bytes += dmr_iq_render_bits(burst, DMR_BURST_BITS, &ch, &rng, s_iq + bytes,
                                    IQ_MAX - bytes);
        bytes += dmr_iq_render_idle(24, &ch, &rng, s_iq + bytes, IQ_MAX - bytes);
    }
    dsp_state_t dsp;
    dsp_init(&dsp);
    dsp_select_mode(&dsp, DEMOD_C4FM, p25_demod_output_gain(DEMOD_C4FM, false));
    int n_audio = 0;
    for (int off = 0; off < bytes; off += 4096) {
        const int take = (bytes - off) < 4096 ? (bytes - off) : 4096;
        n_audio += dsp_process_iq(&dsp, s_iq + off, take, s_audio + n_audio,
                                  AUDIO_MAX - n_audio);
    }
    dmr_watch_reset();
    dmr_watch_samples(s_audio, n_audio);
    dmr_watch_t w;
    LS_CHECK(dmr_watch_get(&w));
    ls_note("ramp u8: bursts=%u lc=%u fixed=%u", (unsigned)w.bursts,
            (unsigned)w.lc_ok, (unsigned)w.lc_fixed);
    LS_CHECK_MSG(w.lc_ok >= 3, "%u of 4 headers decoded", (unsigned)w.lc_ok);
    LS_CHECK(w.have_lc && w.lc.destination == lc.destination);
}

LS_CASE(an_idle_carrier_produces_no_link_control)
{
    /* lc_ok is what says "this is a DMR channel", so a carrier with only the
       idle pattern on it, or no carrier at all, must not move it. */
    int n = dmr_iq_dev_idle(s_dev, 0, SYM_MAX, 3000);
    n = dmr_iq_dev_off(s_dev, n, SYM_MAX, 3000);
    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .cn_db = 15.0f };
    dmr_watch_t w;
    run(&ch, n, 12345, &w);
    LS_EQ_UINT(w.lc_ok, 0);
    LS_EQ_UINT(w.lc_fixed, 0);
    LS_EQ_UINT(w.emb_lc_ok, 0);
    LS_CHECK_MSG(!w.have_lc, "an idle carrier produced a link control");
    LS_CHECK(dmr_watch_verdict(&w) != DMR_VERDICT_DMR);
}
