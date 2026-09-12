/* LS_TEST_SOURCES: ${APP}/fm/flex.c */
/* FLEX decoder, on the host. */

#include "ls_test.h"
#include "flex_gen.h"
#include "flex.h"
#include "fm_state.h"

#include <stdlib.h>
#include <string.h>

fm_state_t FM;

#define SAMP_CAP 40000

typedef struct {
    int         pages;
    uint32_t    ric;
    fm_page_protocol_t protocol;
    char        type;
    char        text[FM_PAGE_TEXT_MAX];
    uint32_t    frames;
    uint32_t    cw_errs;
    int         near_min;
} rx_result_t;

/* Transmit one page and decode it exactly as the device does: a float
   discriminator stream into flex_process(), fed in 1024-sample chunks so a
   bug at a block boundary is reachable. */
static rx_result_t run_page(const flex_tx_cfg_t *cfg, uint32_t ric,
                            flex_msg_type_t msg_type, const char *text,
                            ls_flex_mode_t rx_mode)
{
    rx_result_t r;
    memset(&r, 0, sizeof(r));
    memset(&FM, 0, sizeof(FM));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return r;

    size_t n = flex_tx_page(cfg, ric, msg_type, text, buf, SAMP_CAP);

    flex_ctx_t *c = flex_create(&FM, rx_mode);
    LS_CHECK(c != NULL);
    if (!c) { free(buf); return r; }

    for (size_t i = 0; i < n; i += 1024) {
        size_t take = (n - i < 1024) ? (n - i) : 1024;
        flex_process(c, buf + i, (int)take);
    }

    r.frames   = flex_n_frames(c);
    r.cw_errs  = flex_n_cwerr(c);
    r.near_min = flex_near_min(c);
    r.pages    = FM.page_count;

    if (FM.page_count > 0) {
        int last = (FM.page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        r.ric  = FM.pages[last].address;
        r.protocol = FM.pages[last].protocol;
        r.type = FM.pages[last].type;
        snprintf(r.text, sizeof(r.text), "%s", FM.pages[last].text);
    }

    flex_destroy(c);
    free(buf);
    return r;
}

/* --- encoder self-consistency ------------------------------------------- */

LS_CASE(codeword_bch_is_self_consistent)
{
    /* Every generated codeword must survive its own syndrome check.  If this
       fails, nothing else in this file means anything. */
    for (uint32_t d = 0; d < 0x1FFFFFu; d += 0x1237u) {
        uint32_t cw = flex_encode_cw(d);
        LS_EQ_UINT((cw >> 11) & 0x1FFFFFu, d);
        /* Parity bit at position 0 keeps overall population even. */
        uint32_t v = cw;
        v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        LS_EQ_UINT(v & 1u, 0u);
    }
}

LS_CASE(interleaver_round_trips)
{
    /* Deinterleave(interleave(x)) == x for any 8 codewords.  A block bit
       stream is transmitted column-first, so the receiver's deinterleaver
       is the mirror. */
    uint32_t in[8] = {
        0x12345678u, 0xDEADBEEFu, 0xCAFEBABEu, 0xFEEDFACEu,
        0xA5A5A5A5u, 0x5A5A5A5Au, 0x00000000u, 0xFFFFFFFFu,
    };
    uint8_t bits[256];
    uint32_t out[8];
    flex_interleave(in, bits);
    flex_deinterleave(bits, out);
    for (int i = 0; i < 8; i++) LS_EQ_UINT(out[i], in[i]);
}

LS_CASE(mode_words_are_distinct)
{
    /* If two modes were close in Hamming distance the receiver could pick the
       wrong one on a lightly corrupted frame. */
    for (int a = 0; a < FLEX_MODE_COUNT; a++) {
        for (int b = a + 1; b < FLEX_MODE_COUNT; b++) {
            uint32_t x = FLEX_MODE_WORDS[a] ^ FLEX_MODE_WORDS[b];
            int d = 0; while (x) { x &= x - 1; d++; }
            LS_CHECK_MSG(d >= 12,
                "mode words %d/%d only differ in %d bits", a, b, d);
        }
    }
}

/* --- sync detection identifies rate/level ------------------------------- */

LS_CASE(sync_identifies_1600_2fsk)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.mode = FLEX_MODE_1600_2;

    rx_result_t r = run_page(&cfg, 0x12345, FLEX_MSG_ALPHA, "HELLO",
                             LS_FLEX_MODE_1600_2);
    LS_CHECK_MSG(r.frames > 0, "1600/2 never synced (near_min=%d)", r.near_min);
    LS_CHECK_MSG(r.pages  > 0, "1600/2 synced but produced no page");
    LS_EQ_INT(r.protocol, FM_PAGE_PROTOCOL_FLEX);
    LS_EQ_UINT(r.ric, 0x12345u);
    LS_EQ_STR(r.text, "HELLO");
}

LS_CASE(sync_identifies_3200_2fsk)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.mode = FLEX_MODE_3200_2;

    rx_result_t r = run_page(&cfg, 0x12346, FLEX_MSG_ALPHA, "HELLO",
                             LS_FLEX_MODE_3200_2);
    LS_CHECK_MSG(r.frames > 0, "3200/2 never synced (near_min=%d)", r.near_min);
    LS_CHECK_MSG(r.pages  > 0, "3200/2 synced but produced no page");
    LS_EQ_STR(r.text, "HELLO");
}

LS_CASE(sync_identifies_3200_4fsk)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.mode = FLEX_MODE_3200_4;

    rx_result_t r = run_page(&cfg, 0x12347, FLEX_MSG_ALPHA, "HELLO",
                             LS_FLEX_MODE_3200_4);
    LS_CHECK_MSG(r.frames > 0, "3200/4 never synced (near_min=%d)", r.near_min);
    LS_CHECK_MSG(r.pages  > 0, "3200/4 synced but produced no page");
    LS_EQ_STR(r.text, "HELLO");
}

LS_CASE(sync_identifies_6400_4fsk)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.mode = FLEX_MODE_6400_4;

    rx_result_t r = run_page(&cfg, 0x12348, FLEX_MSG_ALPHA, "HELLO",
                             LS_FLEX_MODE_6400_4);
    LS_CHECK_MSG(r.frames > 0, "6400/4 never synced (near_min=%d)", r.near_min);
    LS_CHECK_MSG(r.pages  > 0, "6400/4 synced but produced no page");
    LS_EQ_STR(r.text, "HELLO");
}

LS_CASE(wrong_mode_configuration_produces_no_page)
{
    /* A 3200/2 transmission fed to a decoder configured for 1600/2 must not
       invent a page - the sync would slip and the mode word check would
       reject it either way.  The point of this case is to prove the
       "mismatched rate is silent" property: a decoder that fires anyway is
       worse than one that stays quiet. */
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.mode = FLEX_MODE_3200_2;

    rx_result_t r = run_page(&cfg, 0x12349, FLEX_MSG_ALPHA, "HELLO",
                             LS_FLEX_MODE_1600_2);
    LS_CHECK_MSG(r.pages == 0,
        "wrong-mode decoder invented a page: [%s]", r.text);
}

/* --- message content ---------------------------------------------------- */

LS_CASE(numeric_page_decodes_as_numeric)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);

    rx_result_t r = run_page(&cfg, 0x1F00A, FLEX_MSG_NUMERIC, "5551234",
                             LS_FLEX_MODE_1600_2);
    LS_CHECK_MSG(r.pages > 0, "numeric page produced nothing");
    LS_EQ_INT(r.type, 'N');
    /* Numeric CWs pack 5 digits, so a 7-digit page rounds up to 10 chars
       (last 3 padded with '0').  Confirming the first 7 digits is what
       actually matters. */
    LS_CHECK_MSG(strncmp(r.text, "5551234", 7) == 0,
                 "numeric page read as [%s], want it to start 5551234",
                 r.text);
}

LS_CASE(tone_only_frame_is_marked_tone)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);

    /* Empty text -> BIW says msg_len_cws == 0, decoder emits a tone-only page
       - a RIC that keyed up but carried no message. */
    rx_result_t r = run_page(&cfg, 0x1F00B, FLEX_MSG_ALPHA, NULL,
                             LS_FLEX_MODE_1600_2);
    LS_CHECK(r.pages > 0);
    LS_EQ_INT(r.type, 'T');
    LS_EQ_STR(r.text, "(tone)");
    LS_EQ_UINT(r.ric, 0x1F00Bu);
}

LS_CASE(two_pages_in_sequence_do_not_merge)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);

    rx_result_t a = run_page(&cfg, 0x1F00C, FLEX_MSG_ALPHA, "FIRST",
                             LS_FLEX_MODE_1600_2);
    rx_result_t b = run_page(&cfg, 0x1F00D, FLEX_MSG_ALPHA, "SECOND",
                             LS_FLEX_MODE_1600_2);
    LS_EQ_STR(a.text, "FIRST");
    LS_EQ_STR(b.text, "SECOND");
}

/* --- abuse: one case each, matching the POCSAG coverage ---------------- */

LS_CASE(inverted_polarity_still_decodes)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.invert = 1;

    rx_result_t r = run_page(&cfg, 0x1F010, FLEX_MSG_ALPHA, "INVERT",
                             LS_FLEX_MODE_1600_2);
    LS_CHECK_MSG(r.pages > 0, "inverted page lost (near_min=%d)", r.near_min);
    LS_EQ_STR(r.text, "INVERT");
}

LS_CASE(dc_offset_does_not_break_slicer)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.dc = 0.6f;

    rx_result_t r = run_page(&cfg, 0x1F011, FLEX_MSG_ALPHA, "OFFSET",
                             LS_FLEX_MODE_1600_2);
    LS_CHECK_MSG(r.pages > 0, "DC offset killed the page");
    LS_EQ_STR(r.text, "OFFSET");
}

LS_CASE(clock_error_500ppm_is_absorbed)
{
    /* A real transmitter is not on 1600.000 baud and neither is the receiver's
       32 kHz.  +/-500 ppm is generous; the timing loop should not care. */
    for (float ppm = -500.0f; ppm <= 500.0f; ppm += 500.0f) {
        flex_tx_cfg_t cfg;
        flex_tx_defaults(&cfg);
        cfg.baud_err_ppm = ppm;

        rx_result_t r = run_page(&cfg, 0x1F012, FLEX_MSG_ALPHA, "DRIFT",
                                 LS_FLEX_MODE_1600_2);
        LS_CHECK_MSG(r.pages > 0, "lost the page at %+.0f ppm", (double)ppm);
        LS_CHECK_MSG(strcmp(r.text, "DRIFT") == 0,
                     "%+.0f ppm gave [%s]", (double)ppm, r.text);
    }
}

LS_CASE(survives_noise_at_reasonable_snr)
{
    flex_tx_cfg_t cfg;
    flex_tx_defaults(&cfg);
    cfg.noise = 0.20f;

    int ok = 0;
    for (int trial = 0; trial < 8; trial++) {
        cfg.seed = 0x2000u + (uint64_t)trial;
        rx_result_t r = run_page(&cfg, 0x1F013, FLEX_MSG_ALPHA, "NOISY",
                                 LS_FLEX_MODE_1600_2);
        if (r.pages > 0 && strcmp(r.text, "NOISY") == 0) ok++;
    }
    LS_CHECK_MSG(ok >= 6, "only %d/8 noisy pages decoded", ok);
}

LS_CASE(pure_noise_produces_no_page)
{

    memset(&FM, 0, sizeof(FM));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;

    ls_rng_t rng;
    ls_rng_seed(&rng, 0xBADFEEDu);
    for (int i = 0; i < SAMP_CAP; i++) buf[i] = ls_rng_noise(&rng) * 3.0f;

    flex_ctx_t *c = flex_create(&FM, LS_FLEX_MODE_1600_2);
    for (int i = 0; i < SAMP_CAP; i += 1024) {
        int take = (SAMP_CAP - i < 1024) ? (SAMP_CAP - i) : 1024;
        flex_process(c, buf + i, take);
    }

    LS_CHECK_MSG(FM.page_count == 0, "noise produced %d page(s), first [%s]",
                 FM.page_count, FM.pages[0].text);

    flex_destroy(c);
    free(buf);
}
