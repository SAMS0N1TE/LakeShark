/* LS_TEST_SOURCES: ${DMR_IQ_SRCS}
 *
 * Where DMR receive stops working, measured rather than guessed: a stream of
 * voice LC headers rendered as RRC-shaped 4FSK IQ with Gaussian noise at a
 * stated carrier-to-noise ratio, through the board's C4FM demodulator and
 * the DMR watcher, counting how many link controls come out.
 *
 * C/N is carrier power over the noise power in 12.5 kHz - the DMR channel -
 * with the noise white across the whole 240 kHz the pipeline is fed. Raw
 * BER is the timing loop's own slicing, before a burst is re-sliced at its
 * sync; the LC rate is after re-slicing, BPTC and RS.
 *
 * The printed table is the result. The assertions only hold the threshold
 * where it was measured, with a margin, so a change that costs sensitivity
 * shows up as a failure rather than a slightly different table nobody
 * reads.
 */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_watch.h"
#include "dmr_gen.h"
#include "dmr_iq_gen.h"
#include "dsp_pipeline.h"
#include "p25_demod_control.h"

#include <stdio.h>
#include <string.h>

int p25_qual_protocol_score(const void *a, const void *b);
int p25_qual_protocol_score(const void *a, const void *b)
{
    (void)a; (void)b; return 0;
}

void sys_log(unsigned char color, const char *fmt, ...)
{
    (void)color;
    (void)fmt;
}

static const uint8_t BS_DATA[6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };
static const uint8_t MS_DATA[6] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };
static const uint8_t CACH[2][3] = { {0x80, 0x02, 0x02}, {0x88, 0x00, 0x20} };

#define BURSTS     40
#define LEAD       240
#define SYM_MAX    (LEAD + BURSTS * 288 + 400)
#define PAIRS_MAX  (SYM_MAX * 51)
#define AUDIO_MAX  (SYM_MAX * 11)

static float   s_dev[SYM_MAX];
static float   s_fiq[PAIRS_MAX * 2];
static int16_t s_audio[AUDIO_MAX];

typedef struct {
    float  cn;
    double ber;            /* timing loop's slicing, before re-slicing     */
    int    framed, lc;     /* of BURSTS                                    */
} point_t;

static int build(bool mobile)
{
    dmr_lc_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.destination = 3172;
    lc.source = 2621441;
    uint8_t burst[33];
    dmr_gen_burst(burst, mobile ? MS_DATA : BS_DATA, 7, 1, &lc);
    int n = dmr_iq_dev_idle(s_dev, 0, SYM_MAX, LEAD);
    for (int i = 0; i < BURSTS && n > 0; i++) {
        if (mobile) {
            /* One burst a frame, the other slot unkeyed. */
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, burst, DMR_BURST_BITS);
            n = dmr_iq_dev_off(s_dev, n, SYM_MAX, 156);
        } else {
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, CACH[i & 1], 24);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, burst, DMR_BURST_BITS);
        }
    }
    if (n > 0) n = dmr_iq_dev_idle(s_dev, n, SYM_MAX, 60);
    return n;
}

static int demodulate(int pairs)
{
    dsp_state_t dsp;
    dsp_init(&dsp);
    dsp_select_mode(&dsp, DEMOD_C4FM, p25_demod_output_gain(DEMOD_C4FM, false));
    int n_audio = 0;
    for (int off = 0; off < pairs && n_audio < AUDIO_MAX; off += 2048) {
        const int take = (pairs - off) < 2048 ? (pairs - off) : 2048;
        n_audio += dsp_process_float_iq(&dsp, s_fiq + 2 * off, take,
                                        s_audio + n_audio, AUDIO_MAX - n_audio);
    }
    return n_audio;
}

/* Bits apart in the ETSI mapping: +3 01, +1 00, -1 10, -3 11. */
static int dibit_errors(int a, int b)
{
    const int da = a == 3 ? 1 : a == 1 ? 0 : a == -1 ? 2 : 3;
    const int db = b == 3 ? 1 : b == 1 ? 0 : b == -1 ? 2 : 3;
    return ((da ^ db) & 1) + ((da ^ db) >> 1);
}

/* The loop's decisions against what was sent, at the alignment that agrees
   best - the pipeline's filters delay the stream by a few symbols. Counted
   over keyed symbols only. */
static double raw_ber(int n_audio, int n_sym)
{
    static int8_t got[SYM_MAX + 64];
    dmr_symsync_t ss;
    dmr_symsync_reset(&ss, 10.0f);
    dmr_symbol_t sym;
    int k = 0;
    for (int i = 0; i < n_audio && k < SYM_MAX + 64; i++) {
        if (!dmr_symsync_push(&ss, s_audio[i], &sym)) continue;
        got[k++] = (int8_t)(sym.value > sym.umid ? 3 : sym.value > sym.center ? 1
                            : sym.value > sym.lmid ? -1 : -3);
    }
    long best_err = -1, best_n = 1;
    for (int lag = 0; lag < 40; lag++) {
        long err = 0, n = 0;
        for (int s = LEAD; s < n_sym && s + lag < k; s++) {
            if (s_dev[s] == DMR_IQ_OFF) continue;
            const int want = s_dev[s] > 1000.0f ? 3 : s_dev[s] > 0.0f ? 1
                           : s_dev[s] > -1000.0f ? -1 : -3;
            err += dibit_errors(want, got[s + lag]);
            n += 2;
        }
        if (best_err < 0 || err * best_n < best_err * n) { best_err = err; best_n = n; }
    }
    return best_n ? (double)best_err / (double)best_n : 1.0;
}

static point_t measure(bool mobile, float cn, float offset_hz, unsigned seed)
{
    point_t p = { cn, 1.0, 0, 0 };
    const int n_sym = build(mobile);
    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .cn_db = cn, .freq_off_hz = offset_hz,
                            .start_sym = 0.31f };
    ls_rng_t rng;
    ls_rng_seed(&rng, seed);
    const int pairs = dmr_iq_render_stream(s_dev, n_sym, &ch, &rng, s_fiq, PAIRS_MAX);
    if (pairs <= 0) return p;
    const int n_audio = demodulate(pairs);
    /* Across a mobile's unkeyed gaps the loop free-runs on noise and gains
       or loses symbols, so one alignment does not hold for the stream. */
    p.ber = mobile ? -1.0 : raw_ber(n_audio, n_sym);
    dmr_watch_reset();
    dmr_watch_samples(s_audio, n_audio);
    dmr_watch_t w;
    memset(&w, 0, sizeof(w));
    dmr_watch_get(&w);
    p.framed = (int)w.bursts;
    p.lc = (int)(w.lc_ok + w.lc_fixed);
    return p;
}

/* The lowest C/N from which every point up decodes at least 90% of the
   link controls. */
static float sweep(const char *name, bool mobile, float offset_hz)
{
    point_t pts[24];
    int n = 0;
    for (float cn = 4.0f; cn <= 14.0f; cn += 1.0f)
        pts[n++] = measure(mobile, cn, offset_hz, 1000u + (unsigned)cn);
    pts[n++] = measure(mobile, 20.0f, offset_hz, 1020u);
    printf("\n  %s, carrier offset %+.0f Hz, %d bursts a point\n", name, offset_hz, BURSTS);
    printf("    C/N dB   raw BER   framed   LC ok\n");
    float threshold = 99.0f;
    for (int i = n - 1; i >= 0; i--) {
        if (pts[i].lc * 10 >= BURSTS * 9) threshold = pts[i].cn;
        else break;
    }
    for (int i = 0; i < n; i++) {
        char ber[16];
        if (pts[i].ber < 0.0) snprintf(ber, sizeof(ber), "%7s", "-");
        else snprintf(ber, sizeof(ber), "%7.4f", pts[i].ber);
        printf("    %5.1f   %s   %3d/%d   %3d/%d%s\n", pts[i].cn, ber,
               pts[i].framed, BURSTS, pts[i].lc, BURSTS,
               pts[i].cn == threshold ? "   <- 90% from here up" : "");
    }
    return threshold;
}

LS_CASE(link_control_threshold_on_a_repeater_stream)
{
    const float t0 = sweep("repeater (BS sync, CACH, continuous)", false, 0.0f);
    const float tp = sweep("repeater", false, 1000.0f);
    const float tm = sweep("repeater", false, -1000.0f);
    printf("\n  LC decode threshold (>= 90%% of link controls): %.0f dB at 0 Hz, "
           "%.0f dB at +1 kHz, %.0f dB at -1 kHz C/N in 12.5 kHz\n", t0, tp, tm);
    ls_note("threshold %.0f / %.0f / %.0f dB", t0, tp, tm);
    /* Measured at 8 dB with no offset and 8 dB either side of 1 kHz. */
    LS_CHECK_MSG(t0 <= 9.0f, "threshold rose to %.0f dB", t0);
    LS_CHECK_MSG(tp <= 10.0f && tm <= 10.0f, "+-1 kHz threshold rose to %.0f/%.0f dB", tp, tm);
}

LS_CASE(link_control_threshold_on_isolated_mobile_bursts)
{
    const float t = sweep("mobile (MS sync, one slot keyed, no CACH)", true, 0.0f);
    printf("\n  LC decode threshold, mobile bursts: %.0f dB C/N in 12.5 kHz\n", t);
    ls_note("mobile threshold %.0f dB", t);
    /* Measured at 7 dB. */
    LS_CHECK_MSG(t <= 10.0f, "threshold rose to %.0f dB", t);
}

/* Six voice superframes in slot 1 with no header - a call joined late -
   and idle bursts in slot 2. The embedded LC is the only identity there is,
   assembled from four bursts that carry no sync of their own. */
#define SUPERFRAMES 6

static int embedded_lcs(float cn, unsigned seed)
{
    dmr_lc_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.destination = 3172;
    lc.source = 2621441;
    uint8_t lc9[9], idle_block[12] = {0}, idle[33], b[33];
    dmr_lc_pack(&lc, lc9);
    uint32_t frag[4];
    dmr_emb_lc_encode(lc9, frag);
    dmr_gen_data_burst(idle, BS_DATA, 7, 9, idle_block);
    static const uint8_t lcss[4] = { DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST };
    static const uint8_t BS_VOICE[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };

    int n = dmr_iq_dev_idle(s_dev, 0, SYM_MAX, LEAD);
    for (int sf = 0; sf < SUPERFRAMES && n > 0; sf++)
        for (int i = 0; i < 6 && n > 0; i++) {
            if (i == 0) dmr_gen_voice_burst(b, BS_VOICE, 0, 0, (unsigned)(sf * 6));
            else dmr_gen_voice_burst(b, NULL,
                                     dmr_emb_encode(7, false, i < 5 ? lcss[i - 1] : DMR_LCSS_SINGLE),
                                     i < 5 ? frag[i - 1] : 0, (unsigned)(sf * 6 + i));
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, CACH[0], 24);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, b, DMR_BURST_BITS);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, CACH[1], 24);
            n = dmr_iq_dev_bits(s_dev, n, SYM_MAX, idle, DMR_BURST_BITS);
        }
    if (n > 0) n = dmr_iq_dev_idle(s_dev, n, SYM_MAX, 60);
    if (n <= 0) return -1;

    dmr_iq_channel_t ch = { .shape = DMR_IQ_RRC, .cn_db = cn, .start_sym = 0.31f };
    ls_rng_t rng;
    ls_rng_seed(&rng, seed);
    const int pairs = dmr_iq_render_stream(s_dev, n, &ch, &rng, s_fiq, PAIRS_MAX);
    if (pairs <= 0) return -1;
    const int n_audio = demodulate(pairs);
    dmr_watch_reset();
    dmr_watch_samples(s_audio, n_audio);
    dmr_watch_t w;
    memset(&w, 0, sizeof(w));
    dmr_watch_get(&w);
    return (int)w.emb_lc_ok;
}

LS_CASE(embedded_lc_threshold_on_a_call_joined_late)
{
    printf("\n  late entry: embedded LC from voice bursts B-E, %d superframes a point\n",
           SUPERFRAMES);
    printf("    C/N dB   embedded LC ok\n");
    int got[24];
    float cns[24];
    int n = 0;
    for (float cn = 6.0f; cn <= 16.0f; cn += 1.0f) {
        cns[n] = cn;
        got[n++] = embedded_lcs(cn, 2000u + (unsigned)cn);
    }
    float threshold = 99.0f;
    for (int i = n - 1; i >= 0 && got[i] >= SUPERFRAMES - 1; i--) threshold = cns[i];
    for (int i = 0; i < n; i++)
        printf("    %5.1f   %d/%d%s\n", cns[i], got[i], SUPERFRAMES,
               cns[i] == threshold ? "   <- 5 of 6 or better from here up" : "");
    printf("\n  embedded LC threshold: %.0f dB C/N in 12.5 kHz\n", threshold);
    ls_note("embedded LC threshold %.0f dB", threshold);
    /* Measured at 11 dB. Four bursts' fragments must all come through
       Hamming(16,11) a row at a time, with no BPTC behind them, so it sits
       above the full LC's threshold. */
    LS_CHECK_MSG(threshold <= 13.0f, "threshold rose to %.0f dB", threshold);
}
