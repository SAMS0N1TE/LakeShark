/* LS_TEST_SOURCES: ${APP}/acars/acars.c ${APP}/acars/acars_msk.c */

#include "ls_test.h"
#include "acars_gen.h"
#include "acars.h"
#include "acars_msk.h"
#include "esp_heap_caps.h"

#include <stdlib.h>
#include <string.h>

#define SAMP_CAP 40000

typedef struct {
    int      msgs;
    char     reg[8];
    char     label[3];
    char     mode;
    char     block_id;
    char     text[ACARS_TEXT_MAX + 1];
    int      text_len;
    int      parity_errors;
    uint32_t n_synced;
    uint32_t n_bad_crc;
} rx_result_t;

/* Transmit one ACARS message and decode it exactly as the device does: a
   float audio stream at ACARS_SAMP_RATE into acars_process(), fed in
   1024-sample chunks so a bug at a block boundary is reachable. */
static rx_result_t run_msg(const acars_tx_cfg_t *cfg, const acars_msg_t *m)
{
    rx_result_t r;
    memset(&r, 0, sizeof(r));

    acars_state_t state;
    memset(&state, 0, sizeof(state));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return r;

    size_t n = acars_tx_msg(cfg, m, buf, SAMP_CAP);

    acars_ctx_t *c = acars_create(&state);
    LS_CHECK(c != NULL);
    if (!c) { free(buf); return r; }

    for (size_t i = 0; i < n; i += 1024) {
        size_t take = (n - i < 1024) ? (n - i) : 1024;
        acars_process(c, buf + i, (int)take);
    }

    r.n_synced  = acars_n_synced(c);
    r.n_bad_crc = acars_n_bad_crc(c);
    r.msgs      = state.msg_count;

    if (state.msg_count > 0) {
        int last = (state.msg_head - 1 + ACARS_MSG_LOG_MAX) % ACARS_MSG_LOG_MAX;
        snprintf(r.reg, sizeof(r.reg), "%s", state.msgs[last].reg);
        snprintf(r.label, sizeof(r.label), "%s", state.msgs[last].label);
        r.mode         = state.msgs[last].mode;
        r.block_id     = state.msgs[last].block_id;
        r.text_len     = state.msgs[last].text_len;
        r.parity_errors = state.msgs[last].parity_errors;
        int tn = r.text_len; if (tn > ACARS_TEXT_MAX) tn = ACARS_TEXT_MAX;
        memcpy(r.text, state.msgs[last].text, (size_t)tn);
        r.text[tn] = 0;
    }

    acars_destroy(c);
    free(buf);
    return r;
}

static void make_msg(acars_msg_t *m, const char *reg, const char *label,
                     const char *text)
{
    memset(m, 0, sizeof(*m));
    m->mode     = '2';
    m->tak      = 0x15;                /* NAK */
    m->block_id = '1';
    snprintf(m->reg,   sizeof(m->reg),   "%-7s", reg ? reg : ".N12345");
    snprintf(m->label, sizeof(m->label), "%s",   label ? label : "H1");
    m->text = text;
}

LS_CASE(decoder_allocations_prefer_psram_and_cleanup_partial_failure)
{
    acars_state_t state;
    memset(&state, 0, sizeof(state));

    ls_shim_heap_reset();
    acars_ctx_t *ctx = acars_create(&state);
    LS_CHECK(ctx != NULL);
    LS_EQ_UINT(ls_shim_heap_call_count(), 2);
    LS_EQ_UINT(ls_shim_heap_call_caps(1),
               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_heap_call_caps(2),
               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    LS_CHECK(ls_shim_heap_call_size(1) > 0);
    LS_CHECK(ls_shim_heap_call_size(2) > 0);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 2);
    acars_destroy(ctx);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);

    ls_shim_heap_reset();
    ls_shim_heap_fail_from(2);
    LS_CHECK(acars_create(&state) == NULL);
    LS_EQ_UINT(ls_shim_heap_call_count(), 3);
    LS_EQ_UINT(ls_shim_heap_call_caps(2),
               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_heap_call_caps(3),
               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);

    /* Failure of both context attempts allocates nothing and remains safe to
       retry after pressure subsides. */
    ls_shim_heap_reset();
    ls_shim_heap_fail_from(1);
    LS_CHECK(acars_create(&state) == NULL);
    LS_EQ_UINT(ls_shim_heap_call_count(), 2);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);

    ls_shim_heap_reset();
    ctx = acars_create(&state);
    LS_CHECK(ctx != NULL);
    acars_destroy(ctx);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
}

/* --- encoder self-consistency ------------------------------------------- */

LS_CASE(parity_helpers_round_trip)
{
    /* Every 7-bit character stuffed with odd parity must survive its own
       parity check.  If this fails, nothing else in the file means
       anything - the byte-alignment invariant is broken. */
    for (int c = 0; c < 128; c++) {
        uint8_t b = acars_apply_parity((uint8_t)c);
        LS_CHECK_MSG(acars_byte_parity_ok(b),
            "0x%02x apply_parity -> 0x%02x, parity check failed", c, b);
        LS_EQ_UINT(b & 0x7Fu, (unsigned)c);
    }
    /* Flipping any single bit of a parity-valid byte must break the
       parity check.  This is the invariant the decoder relies on to
       classify a single-bit hit as "parity error, not framing loss". */
    for (int c = 0; c < 128; c++) {
        uint8_t b = acars_apply_parity((uint8_t)c);
        for (int k = 0; k < 8; k++) {
            uint8_t bad = (uint8_t)(b ^ (1u << k));
            LS_CHECK_MSG(!acars_byte_parity_ok(bad),
                "flipping bit %d of 0x%02x should fail parity", k, b);
        }
    }
}

LS_CASE(crc_is_deterministic_and_covers_content)
{
    /* Same input, same output.  And any single-bit change to the input
       must change the CRC - otherwise we would let a bit hit through. */
    const uint8_t sample[] = { 0x01, 0x32, 0x2E, 0x4E, 0x31, 0x32, 0x33 };
    uint16_t c0 = acars_crc16(sample, sizeof(sample));
    uint16_t c1 = acars_crc16(sample, sizeof(sample));
    LS_EQ_UINT(c0, c1);

    for (size_t i = 0; i < sizeof(sample); i++) {
        uint8_t tmp[sizeof(sample)];
        memcpy(tmp, sample, sizeof(tmp));
        tmp[i] ^= 0x01;
        uint16_t c2 = acars_crc16(tmp, sizeof(tmp));
        LS_CHECK_MSG(c2 != c0,
            "single-bit change at %zu did not move the CRC", i);
    }
}

/* --- MSK slicer, driven directly (no framing) --------------------------- */

LS_CASE(msk_slicer_reproduces_transmitted_bits)
{
    /* Feed audio for a known bit pattern through the MSK slicer and read bits back out. */

    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    cfg.preamble_bits = 32;

    /* Two SYN bytes (0x16) with odd parity, sent LSB bit first.  Parity
       bit for 0x16 is 0 (already odd popcount). */
    static const uint8_t sync_bits[16] = {
        0,1,1,0,1,0,0,0,   0,1,1,0,1,0,0,0
    };

    static uint8_t tx_bits[256];
    memcpy(tx_bits, sync_bits, sizeof(sync_bits));
    ls_rng_t rng; ls_rng_seed(&rng, 0xACADEC0u);
    for (size_t i = sizeof(sync_bits); i < sizeof(tx_bits); i++)
        tx_bits[i] = (uint8_t)(ls_rng_u32(&rng) & 1u);

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;

    size_t n = acars_tx_bits(&cfg, tx_bits, sizeof(tx_bits), buf, SAMP_CAP);

    acars_msk_t *msk = acars_msk_create();
    LS_CHECK(msk != NULL);
    if (!msk) { free(buf); return; }

    (void)acars_msk_process(msk, buf, (int)n);

    uint8_t out[512];
    int got = acars_msk_read_bits(msk, out, (int)sizeof(out));

    /* First 16 bits emitted should be the sync bits (from the phase's
       hist buffer, replayed to the caller so a framer above can lock).
       Everything after that comes from the newly-locked phase and
       should reproduce the transmitted bits, starting at bit 16. */
    LS_CHECK_MSG(got >= 128,
        "slicer emitted only %d bits from a %d-bit transmission",
        got, (int)sizeof(tx_bits));

    int compare = got - 16;
    if (compare > (int)sizeof(tx_bits) - 16) compare = (int)sizeof(tx_bits) - 16;
    if (compare > 200) compare = 200;

    int matches = 0;
    for (int i = 0; i < compare; i++)
        if (out[16 + i] == tx_bits[16 + i]) matches++;

    LS_CHECK_MSG(matches >= compare - 3,
        "MSK slicer matched %d/%d bits after sync (want %d)",
        matches, compare, compare - 3);

    acars_msk_destroy(msk);
    free(buf);
}

LS_CASE(msk_slicer_stays_silent_on_dc)
{
    /* Flat DC in should not produce bits.  The slicer only starts
       emitting once it has found the sync pattern on one of the eight
       candidate bit-clock phases - so a silent input keeps it silent,
       which is exactly what stops noise from being framed as a bogus
       message downstream. */
    acars_msk_t *msk = acars_msk_create();
    LS_CHECK(msk != NULL);
    if (!msk) return;

    float dc_buf[2048];
    for (int i = 0; i < 2048; i++) dc_buf[i] = 0.0f;

    (void)acars_msk_process(msk, dc_buf, 2048);
    int got = acars_msk_available(msk);
    LS_CHECK_MSG(got == 0,
        "silent input produced %d bits - slicer should not fire without sync",
        got);

    acars_msk_destroy(msk);
}

/* --- whole-chain decode ------------------------------------------------- */

LS_CASE(clean_message_decodes_verbatim)
{
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);

    acars_msg_t m;
    make_msg(&m, ".N12345", "H1", "TEST/MESSAGE FROM BENCH");

    rx_result_t r = run_msg(&cfg, &m);

    LS_CHECK_MSG(r.n_synced >= 1, "never found sync");
    LS_CHECK_MSG(r.msgs > 0, "synced but did not deliver a message");
    LS_EQ_STR(r.reg,   ".N12345");
    LS_EQ_STR(r.label, "H1");
    LS_EQ_INT(r.mode, '2');
    LS_EQ_STR(r.text, "TEST/MESSAGE FROM BENCH");
    LS_EQ_UINT(r.n_bad_crc, 0);
    LS_EQ_INT(r.parity_errors, 0);
}

LS_CASE(header_only_message_delivers_empty_text)
{
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);

    acars_msg_t m;
    make_msg(&m, ".N54321", "Q0", NULL);   /* no text field */

    rx_result_t r = run_msg(&cfg, &m);
    LS_CHECK_MSG(r.msgs > 0, "header-only message produced nothing");
    LS_EQ_STR(r.reg, ".N54321");
    LS_EQ_STR(r.label, "Q0");
    LS_EQ_INT(r.text_len, 0);
}

/* --- parity ------------------------------------------------------------- */

LS_CASE(bad_parity_in_text_is_counted_not_silently_accepted)
{

    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    /* Corrupt the parity of char index 15 (after SOH, mode, 7 reg, tak,
       2 label, blk, STX = 14 chars, so the 15th is the first text char). */
    cfg.corrupt_parity_at = 15;

    acars_msg_t m;
    make_msg(&m, ".N12345", "H1", "PARITY-TEST-STRING");

    rx_result_t r = run_msg(&cfg, &m);
    LS_CHECK_MSG(r.msgs > 0, "message with parity error was dropped entirely - "
                             "single-bit hits should still deliver, counted");
    LS_CHECK_MSG(r.parity_errors > 0,
        "message delivered with zero parity errors despite corruption");
}

LS_CASE(bad_parity_on_soh_prevents_sync_lock)
{

    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    cfg.corrupt_parity_at = 0;   /* SOH */

    acars_msg_t m;
    make_msg(&m, ".N12345", "H1", "SHOULD-NOT-APPEAR");

    rx_result_t r = run_msg(&cfg, &m);
    LS_CHECK_MSG(r.msgs == 0,
        "sync locked onto bad SOH and delivered [%s]", r.text);
}

/* --- CRC ---------------------------------------------------------------- */

LS_CASE(corrupted_payload_fails_crc_and_no_message_is_delivered)
{
    /* A false accept puts fiction on screen.  A bit flip inside the text
       (after CRC computation) must not deliver. */
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    cfg.corrupt_text_bit = 1;

    acars_msg_t m;
    make_msg(&m, ".N12345", "H1", "REAL PAYLOAD");

    rx_result_t r = run_msg(&cfg, &m);
    LS_CHECK_MSG(r.msgs == 0,
        "CRC did not reject fiction: got [%s]", r.text);
    LS_CHECK_MSG(r.n_bad_crc > 0, "n_bad_crc never incremented");
}

/* --- abuse -------------------------------------------------------------- */

LS_CASE(inverted_polarity_still_decodes)
{
    /* A mistuned receiver flips the audio polarity - mark and space
       swap.  The framer carries an invert branch it picks up during the
       sync search; this exercises it. */
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    cfg.invert = 1;

    acars_msg_t m;
    make_msg(&m, ".N99999", "H1", "INVERTED MESSAGE OK");

    rx_result_t r = run_msg(&cfg, &m);
    LS_CHECK_MSG(r.msgs > 0, "inverted message lost");
    LS_EQ_STR(r.text, "INVERTED MESSAGE OK");
}

LS_CASE(dc_offset_does_not_break_slicer)
{
    /* The DC tracker in the MSK slicer absorbs slow tuning offsets.  A
       real receiver rides above/below zero by a modest fraction of the
       signal amplitude; that must not derail the tone correlators. */
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);
    cfg.dc = 0.4f;

    acars_msg_t m;
    make_msg(&m, ".N12345", "H1", "DC OFFSET MESSAGE");

    rx_result_t r = run_msg(&cfg, &m);
    LS_CHECK_MSG(r.msgs > 0, "DC offset killed the message");
    LS_EQ_STR(r.text, "DC OFFSET MESSAGE");
}

LS_CASE(clock_error_500ppm_is_absorbed)
{
    /* Same shape as POCSAG's clock case - one message at each extreme,
       both must decode cleanly. */
    for (float ppm = -500.0f; ppm <= 500.0f; ppm += 500.0f) {
        acars_tx_cfg_t cfg;
        acars_tx_defaults(&cfg);
        cfg.baud_err_ppm = ppm;

        acars_msg_t m;
        make_msg(&m, ".N12345", "H1", "DRIFT");

        rx_result_t r = run_msg(&cfg, &m);
        LS_CHECK_MSG(r.msgs > 0,
            "lost the message at %+.0f ppm", (double)ppm);
        LS_CHECK_MSG(strcmp(r.text, "DRIFT") == 0,
            "%+.0f ppm gave [%s]", (double)ppm, r.text);
    }
}

LS_CASE(survives_moderate_noise)
{

    acars_msg_t m;
    make_msg(&m, ".N12345", "H1", "NOISY");

    int ok = 0;
    for (int trial = 0; trial < 8; trial++) {
        acars_tx_cfg_t cfg;
        acars_tx_defaults(&cfg);
        cfg.noise = 0.08f;
        cfg.seed  = 0x3000u + (uint64_t)trial;

        rx_result_t r = run_msg(&cfg, &m);
        if (r.msgs > 0 && strcmp(r.text, "NOISY") == 0) ok++;
    }
    LS_CHECK_MSG(ok >= 6, "only %d/8 noisy messages decoded", ok);
}

LS_CASE(two_messages_back_to_back_do_not_merge)
{
    /* Between messages there is no state persisted in acars_state_t
       across run_msg calls (each call zeroes it), so this is really
       "does the same decoder produce independent results on two
       separately-transmitted messages."  A regression that leaves the
       CRC accumulator dirty would show up here. */
    acars_tx_cfg_t cfg;
    acars_tx_defaults(&cfg);

    acars_msg_t m1; make_msg(&m1, ".N00001", "H1", "FIRST");
    acars_msg_t m2; make_msg(&m2, ".N00002", "H1", "SECOND");

    rx_result_t a = run_msg(&cfg, &m1);
    rx_result_t b = run_msg(&cfg, &m2);

    LS_EQ_STR(a.text, "FIRST");
    LS_EQ_STR(a.reg,  ".N00001");
    LS_EQ_STR(b.text, "SECOND");
    LS_EQ_STR(b.reg,  ".N00002");
}

LS_CASE(pure_noise_delivers_no_message)
{

    acars_state_t state;
    memset(&state, 0, sizeof(state));

    float *buf = (float *)malloc(SAMP_CAP * sizeof(float));
    LS_CHECK(buf != NULL);
    if (!buf) return;

    ls_rng_t rng; ls_rng_seed(&rng, 0xBAADACA5u);
    for (int i = 0; i < SAMP_CAP; i++) buf[i] = ls_rng_noise(&rng) * 2.0f;

    acars_ctx_t *c = acars_create(&state);
    for (int i = 0; i < SAMP_CAP; i += 1024) {
        int take = (SAMP_CAP - i < 1024) ? (SAMP_CAP - i) : 1024;
        acars_process(c, buf + i, take);
    }

    LS_CHECK_MSG(state.msg_count == 0,
        "noise produced %d message(s), first reg=[%s] text=[%s]",
        state.msg_count,
        state.msg_count > 0 ? state.msgs[0].reg  : "",
        state.msg_count > 0 ? state.msgs[0].text : "");

    acars_destroy(c);
    free(buf);
}
