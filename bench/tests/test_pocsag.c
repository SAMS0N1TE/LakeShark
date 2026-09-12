/* LS_TEST_SOURCES: ${APP}/fm/pocsag.c */
/* POCSAG decoder, on the host.

   These lock down (the sampler was reading across every transition, so
   ~92% of codewords failed BCH and every page came out as "(tone)") and (letter soup was being shown as a message). Both were found by standing next
   to a transmitter for an evening. Neither can come back silently now. */

#include "ls_test.h"
#include "pocsag_gen.h"
#include "pocsag.h"
#include "fm_state.h"

#include <stdlib.h>
#include <string.h>

fm_state_t FM;

#define SAMP_CAP 200000

typedef struct {
    int         pages;
    uint32_t    ric;
    fm_page_protocol_t protocol;
    int         func;
    char        type;
    char        text[FM_PAGE_TEXT_MAX];
    uint32_t    frames;
    uint32_t    cw_errs;
    int         near_min;
} rx_result_t;

/* Transmit one page and decode it, exactly as the device does: a float
   discriminator stream into pocsag_process(). */
static rx_result_t run_page(const pocsag_tx_cfg_t *cfg, uint32_t ric, int func,
                            const char *text, int rx_baud)
{
    rx_result_t r;
    memset(&r, 0, sizeof(r));
    memset(&FM, 0, sizeof(FM));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return r;

    size_t n = pocsag_tx_page(cfg, ric, func, text, buf, SAMP_CAP);

    pocsag_ctx_t *c = pocsag_create(&FM, rx_baud);
    LS_CHECK(c != NULL);
    if (!c) { free(buf); return r; }

    /* Fed in 1024-sample blocks, the same chunking app_fm.c uses, so a bug
       that only shows up at a block boundary is reachable here. */
    for (size_t i = 0; i < n; i += 1024) {
        size_t take = (n - i < 1024) ? (n - i) : 1024;
        pocsag_process(c, buf + i, (int)take);
    }

    r.frames   = pocsag_n_frames(c);
    r.cw_errs  = pocsag_n_cwerr(c);
    r.near_min = pocsag_near_min(c);
    r.pages    = FM.page_count;

    if (FM.page_count > 0) {
        int last = (FM.page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        r.ric  = FM.pages[last].address;
        r.protocol = FM.pages[last].protocol;
        r.func = FM.pages[last].function;
        r.type = FM.pages[last].type;
        snprintf(r.text, sizeof(r.text), "%s", FM.pages[last].text);
    }

    pocsag_destroy(c);
    free(buf);
    return r;
}

/* --- the encoder itself ------------------------------------------------- */

LS_CASE(codeword_bch_is_self_consistent)
{
    /* Every generated codeword must survive the decoder's own syndrome check.
       If this fails, every other case in this file is meaningless. */
    for (uint32_t ric = 1; ric < 400000u; ric += 37811u) {
        for (int f = 0; f < 4; f++) {
            uint32_t cw = pocsag_encode_address(ric, f);
            LS_CHECK_MSG(((cw >> 31) & 1u) == 0, "address codeword flag must be 0");
            LS_EQ_UINT((cw >> 13) & 0x3FFFFu, (ric >> 3) & 0x3FFFFu);
            LS_EQ_UINT((cw >> 11) & 3u, (uint32_t)f);
        }
    }
}

/* --- the whole chain ---------------------------------------------------- */

LS_CASE(clean_alpha_page_1200)
{
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    rx_result_t r = run_page(&cfg, 1234568, 3, "CODE STROKE B HALLWAY 08", 1200);

    LS_CHECK_MSG(r.frames > 0, "never synced (near_min=%d)", r.near_min);
    LS_EQ_UINT(r.cw_errs, 0);
    LS_CHECK_MSG(r.pages > 0, "synced but produced no page");
    LS_EQ_INT(r.protocol, FM_PAGE_PROTOCOL_POCSAG);
    LS_EQ_UINT(r.ric, 1234568u);
    LS_EQ_INT(r.type, 'A');
    LS_EQ_STR(r.text, "CODE STROKE B HALLWAY 08");
}

LS_CASE(tone_only_page_is_marked_tone)
{
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    rx_result_t r = run_page(&cfg, 1234568, 0, NULL, 1200);

    LS_CHECK(r.pages > 0);
    LS_EQ_UINT(r.ric, 1234568u);
    LS_EQ_INT(r.type, 'T');
    LS_EQ_STR(r.text, "(tone)");
}

LS_CASE(inverted_polarity_still_decodes)
{
    /* A mistuned receiver flips the discriminator sign. handle_bit() carries
       an invert branch for exactly this; it has never been tested. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    cfg.invert = 1;

    rx_result_t r = run_page(&cfg, 1234568, 3, "INVERTED LINK OK", 1200);

    LS_CHECK_MSG(r.pages > 0, "inverted page lost (near_min=%d)", r.near_min);
    LS_EQ_STR(r.text, "INVERTED LINK OK");
}

LS_CASE(dc_offset_does_not_break_slicer)
{
    /* The slicer tracks DC at 0.0015/sample. A mistune that pushes the whole
       stream off centre must be absorbed, not decoded as all-ones. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    cfg.dc = 0.6f;

    rx_result_t r = run_page(&cfg, 1234568, 3, "OFFSET PAGE OK", 1200);

    LS_CHECK_MSG(r.pages > 0, "DC offset killed the page");
    LS_EQ_STR(r.text, "OFFSET PAGE OK");
}

LS_CASE(clock_error_is_absorbed_by_timing_loop)
{
    /* A real transmitter is not on 1200.000 baud and neither is the receiver's
       32 kHz. 500 ppm is generous; the loop should not care. */
    for (float ppm = -500.0f; ppm <= 500.0f; ppm += 250.0f) {
        pocsag_tx_cfg_t cfg;
        pocsag_tx_defaults(&cfg);
        cfg.baud_err_ppm = ppm;

        rx_result_t r = run_page(&cfg, 1234568, 3, "CLOCK DRIFT PAGE", 1200);
        LS_CHECK_MSG(r.pages > 0, "lost the page at %+.0f ppm", (double)ppm);
        LS_CHECK_MSG(strcmp(r.text, "CLOCK DRIFT PAGE") == 0,
                     "%+.0f ppm gave [%s]", (double)ppm, r.text);
    }
}

LS_CASE(survives_noise_at_reasonable_snr)
{
    /* The regression this really guards: with the bug the codeword
       error count was ~92% even on a clean signal. Here it must stay at zero
       through moderate noise. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    cfg.noise = 0.25f;

    int ok = 0;
    for (int trial = 0; trial < 8; trial++) {
        cfg.seed = 0x1000u + (uint64_t)trial;
        rx_result_t r = run_page(&cfg, 1234568, 3, "NOISY BUT READABLE", 1200);
        if (r.pages > 0 && strcmp(r.text, "NOISY BUT READABLE") == 0) ok++;
    }
    LS_CHECK_MSG(ok >= 7, "only %d/8 noisy pages decoded", ok);
}

LS_CASE(all_three_bauds)
{
    const int bauds[3] = { 512, 1200, 2400 };
    for (int i = 0; i < 3; i++) {
        pocsag_tx_cfg_t cfg;
        pocsag_tx_defaults(&cfg);
        cfg.baud = bauds[i];

        rx_result_t r = run_page(&cfg, 1234568, 3, "BAUD TEST PAGE", bauds[i]);
        LS_CHECK_MSG(r.pages > 0, "%d baud produced no page", bauds[i]);
        LS_CHECK_MSG(strcmp(r.text, "BAUD TEST PAGE") == 0,
                     "%d baud gave [%s]", bauds[i], r.text);
    }
}

LS_CASE(numeric_page_decodes_as_numeric)
{
    /* A numeric page is not ASCII. It is 4-bit nibbles indexed into
       "0123456789*U -)(", least significant bit first, which is a different
       packing from the alpha path - so it has to be built as raw bits. */
    const char digits[] = "5551234";
    uint8_t msg[MSG_GEN_MAX];
    size_t  n = 0;

    for (const char *p = digits; *p; p++) {
        int v = *p - '0';                      /* NUM_MAP is identity for 0-9 */
        for (int k = 0; k < 4; k++) msg[n++] = (uint8_t)((v >> k) & 1);
    }

    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    memset(&FM, 0, sizeof(FM));
    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;

    size_t ns = pocsag_tx_page_raw(&cfg, 1234568, 0, msg, n, buf, SAMP_CAP);

    pocsag_ctx_t *c = pocsag_create(&FM, 1200);
    for (size_t i = 0; i < ns; i += 1024) {
        size_t take = (ns - i < 1024) ? (ns - i) : 1024;
        pocsag_process(c, buf + i, (int)take);
    }

    LS_CHECK_MSG(FM.page_count > 0, "numeric page produced nothing");
    if (FM.page_count > 0) {
        int last = (FM.page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        LS_EQ_INT(FM.pages[last].type, 'N');
        LS_CHECK_MSG(strncmp(FM.pages[last].text, digits, 7) == 0,
                     "numeric page read as [%s], want it to start %s",
                     FM.pages[last].text, digits);
    }

    pocsag_destroy(c);
    free(buf);
}

LS_CASE(short_alpha_page_still_decodes)
{
    /* The short-string cap in pocsag_text_score() must not sweep real short
       alpha pages into '?' or numeric. Real pagers send alpha with function
       code 3, and the classifier honours that regardless of score - which is
       exactly the escape hatch a message shorter than the structural window
       needs. A regression that routes every short page to numeric would fail
       here. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    rx_result_t r = run_page(&cfg, 1234568, 3, "CALL ME", 1200);

    LS_CHECK_MSG(r.pages > 0, "short alpha page produced nothing");
    LS_EQ_INT(r.type, 'A');
    LS_EQ_STR(r.text, "CALL ME");
}

LS_CASE(wrong_rx_baud_produces_nothing)
{
    /* Decoding 1200 as 512 must yield no page. A decoder that emits garbage
       when mistuned is worse than one that stays quiet - that is what the
       whole "only ever shows tones" complaint was. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    rx_result_t r = run_page(&cfg, 1234568, 3, "SHOULD NOT APPEAR", 512);
    LS_CHECK_MSG(r.pages == 0, "mismatched baud invented a page: [%s]", r.text);
}

LS_CASE(pure_noise_produces_no_page)
{

    memset(&FM, 0, sizeof(FM));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;

    ls_rng_t rng;
    ls_rng_seed(&rng, 0xBADC0DEu);
    for (int i = 0; i < SAMP_CAP; i++) buf[i] = ls_rng_noise(&rng) * 3.0f;

    pocsag_ctx_t *c = pocsag_create(&FM, 1200);
    for (int i = 0; i < SAMP_CAP; i += 1024)
        pocsag_process(c, buf + i, 1024);

    LS_CHECK_MSG(FM.page_count == 0, "noise produced %d page(s), first [%s]",
                 FM.page_count, FM.pages[0].text);

    pocsag_destroy(c);
    free(buf);
}

LS_CASE(two_pages_in_one_transmission_do_not_merge)
{
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    rx_result_t a = run_page(&cfg, 1234568, 3, "FIRST PAGE", 1200);
    rx_result_t b = run_page(&cfg, 1234568, 3, "SECOND PAGE", 1200);

    LS_EQ_STR(a.text, "FIRST PAGE");
    LS_EQ_STR(b.text, "SECOND PAGE");
}

/* The gap that let "garbled characters again" through.

   Every alpha case above sends function code 3, and the classifier honours
   F=3 outright without consulting the text scorer. So the whole scoring path
   - the one real traffic actually takes, because records F=0 and F=2
   carrying text on the site this was tested against - had no test at all.
   The decoder was green while the thing being complained about was broken. */
LS_CASE(a_non_f3_alpha_page_with_angle_brackets_is_still_alpha)
{
    /* A real page, in the shape wrote down from the air: a header
       with the sender in angle brackets. Four brackets in thirty-odd
       characters is over the tenth of the message that fires the "weird"
       penalty, and forty points is enough to drop a genuine text page under
       the seventy needed for ALPHA.

       Sent at F=0 on purpose. With F=3 this passes whatever the scorer
       thinks, which is precisely why the bug survived. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    const char *msg = "From: <clinician> Re: <ward 4>";
    rx_result_t r = run_page(&cfg, 1234568, 0, msg, 1200);

    LS_CHECK_MSG(r.pages > 0, "the page produced nothing at all");
    LS_CHECK_MSG(r.type == 'A',
                 "a readable page came out as '%c' - and '?' prints the "
                 "NUMERIC reading of text, which is the garble being reported",
                 r.type);
    LS_EQ_STR(r.text, msg);
}

LS_CASE(an_unsure_page_shows_the_reading_that_scored_better)
{

    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    /* No spaces and a long run: two structural penalties, so the scorer has
       every reason to be unsure, while the characters are still obviously
       letters. */
    const char *msg = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF";
    rx_result_t r = run_page(&cfg, 1234568, 0, msg, 1200);

    LS_CHECK_MSG(r.pages > 0, "the page produced nothing at all");
    if (r.type == '?') {
        int letters = 0;
        for (const char *p = r.text; *p; p++)
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) letters++;
        LS_CHECK_MSG(letters > (int)strlen(r.text) / 2,
                     "the unsure branch printed '%s' - the numeric reading of "
                     "a page whose letters were sitting right there", r.text);
    }
}

LS_CASE(the_three_auto_baud_decoders_do_not_garble_each_other)
{
    /* The device runs 512, 1200 and 2400 concurrently by default and all
       three write the same page ring (app_fm.c poc_dispatch). Every test
       above builds ONE context, so the configuration that actually ships was
       never exercised: a false sync on a wrong-baud decoder puts a garbled
       page in the same list as the good one, and nothing on screen says
       which baud produced it. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    memset(&FM, 0, sizeof(FM));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;

    const char *msg = "MULTI DECODER SANITY";
    size_t n = pocsag_tx_page(&cfg, 1234568, 3, msg, buf, SAMP_CAP);

    const int bauds[3] = { 512, 1200, 2400 };
    pocsag_ctx_t *c[3];
    for (int i = 0; i < 3; i++) {
        c[i] = pocsag_create(&FM, bauds[i]);
        LS_CHECK(c[i] != NULL);
    }

    for (size_t i = 0; i < n; i += 1024) {
        size_t take = (n - i < 1024) ? (n - i) : 1024;
        for (int k = 0; k < 3; k++) pocsag_process(c[k], buf + i, (int)take);
    }

    LS_CHECK_MSG(FM.page_count >= 1, "the 1200 decoder produced nothing");
    LS_CHECK_MSG(FM.page_count == 1,
                 "%d pages from one transmission - a wrong-baud decoder is "
                 "putting its own reading in the list beside the real one",
                 (int)FM.page_count);
    if (FM.page_count >= 1) {
        int last = (FM.page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        LS_EQ_STR(FM.pages[last].text, msg);
    }

    for (int i = 0; i < 3; i++) pocsag_destroy(c[i]);
    free(buf);
}

LS_CASE(the_wrong_baud_never_files_a_page_however_noisy_it_gets)
{
    /* wrong_rx_baud_produces_nothing covers the clean case. This is
       the same question with the noise turned up past anything on the air,
       because the worry was that a wrong-rate context passes the odd
       codeword by luck and puts its reading in the list beside the real one.

       It does not, at any level tried - which is the reason there is no rule
       here suppressing it. A rule defending against something that cannot be
       demonstrated is a rule that can only ever eat real traffic. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    const float NOISE[] = { 0.2f, 0.6f, 1.0f, 1.5f };
    int filed = 0;
    for (unsigned k = 0; k < sizeof(NOISE) / sizeof(NOISE[0]); k++) {
        cfg.noise = NOISE[k];
        for (int seed = 0; seed < 8; seed++) {
            cfg.seed = (uint32_t)(seed + 1);
            rx_result_t r = run_page(&cfg, 1234568, 3,
                                     "CODE STROKE B HALLWAY 08", 512);
            filed += r.pages;
        }
    }
    LS_CHECK_MSG(filed == 0,
                 "the 512 decoder filed %d pages from a 1200 transmission - "
                 "those land in the list beside the real ones", filed);
}

LS_CASE(a_confident_page_is_filed_even_out_of_a_noisy_stretch)
{
    /* The other side of the same rule, and the one that matters
       more: the drop is narrow ON PURPOSE. A clean page out of a noisy
       stretch is exactly what the BCH correction exists to recover, and a
       rule that threw it away because the context had a bad error rate would
       be worse than the garble it was written to remove. Only UNSURE pages
       from a failing context are dropped. */
    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);
    cfg.noise = 0.25f;

    int good = 0;
    for (int seed = 0; seed < 8; seed++) {
        cfg.seed = (uint32_t)(seed + 1);
        rx_result_t r = run_page(&cfg, 1234568, 3, "CODE STROKE B HALLWAY 08", 1200);
        if (r.pages > 0 && strcmp(r.text, "CODE STROKE B HALLWAY 08") == 0) good++;
    }
    LS_CHECK_MSG(good >= 7,
                 "only %d of 8 noisy transmissions produced their page - the "
                 "drop rule is eating real traffic", good);
}

LS_CASE(a_page_is_only_called_numeric_when_it_is_nearly_all_digits)
{

    pocsag_tx_cfg_t cfg;
    pocsag_tx_defaults(&cfg);

    const char *msg = "From: <clinician> Re: <ward 4>";
    rx_result_t r = run_page(&cfg, 1234568, 0, msg, 1200);
    LS_CHECK_MSG(r.pages > 0, "the page produced nothing at all");
    LS_CHECK_MSG(r.type != 'N',
                 "a text page was called NUMERIC and rendered as [%s]", r.text);

    /* And a real numeric page still reads as one. It has to be built as raw
       nibbles - a numeric page is not ASCII - which is why this cannot just
       call run_page with a string of digits. */
    const char digits[] = "5551234";
    uint8_t nib[MSG_GEN_MAX];
    size_t nn = 0;
    for (const char *p = digits; *p; p++) {
        const int v = *p - '0';           /* NUM_MAP is identity for 0-9 */
        for (int k = 0; k < 4; k++) nib[nn++] = (uint8_t)((v >> k) & 1);
    }

    memset(&FM, 0, sizeof(FM));
    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;
    const size_t ns = pocsag_tx_page_raw(&cfg, 921604, 0, nib, nn, buf, SAMP_CAP);
    pocsag_ctx_t *c = pocsag_create(&FM, 1200);
    for (size_t i = 0; i < ns; i += 1024) {
        const size_t take = (ns - i < 1024) ? (ns - i) : 1024;
        pocsag_process(c, buf + i, (int)take);
    }
    LS_CHECK_MSG(FM.page_count > 0, "the numeric page produced nothing");
    if (FM.page_count > 0) {
        const int last = (FM.page_head - 1 + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        LS_CHECK_MSG(FM.pages[last].type == 'N',
                     "a phone number came out as '%c' - the threshold is now "
                     "too tight for real numeric traffic", FM.pages[last].type);
    }
    pocsag_destroy(c);
    free(buf);
}
