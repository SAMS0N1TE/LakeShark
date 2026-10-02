/* LS_TEST_SOURCES: experiments/pager_recon.c and apps/fm/pocsag.c, and
   PAGER RECON itself (exp_pagers.c under ls_experiments.c) against a faked
   LoRa chip.

   The batches are real POCSAG: built by the bench's own transmitter
   (fixtures/pocsag_gen.c), which the decoder's tests already hold to the
   standard, then cut where the radio cuts them - after the frame sync. */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_lora.h"
#include "pager_recon.h"
#include "pocsag.h"
#include "pocsag_gen.h"
#include "fm_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

extern const ls_experiment_t exp_pagers;

/* ------------------------------------------------------------ batches -- */

/* The 64 bytes after the first frame sync of a page to `ric`, as the radio
   delivers them. */
static void page_batch(uint32_t ric, int func, const char *text, uint8_t out[PGR_BATCH_BYTES])
{
    static uint8_t bits[BITS_GEN_MAX];
    const size_t n = pocsag_build_bits(ric, func, text, 0, bits, BITS_GEN_MAX);
    LS_CHECK(n >= 32 + 512);
    uint32_t fsc = 0;
    for (int i = 0; i < 32; i++) fsc = (fsc << 1) | bits[i];
    LS_EQ_UINT(fsc, PGR_POCSAG_FSC);
    memset(out, 0, PGR_BATCH_BYTES);
    for (int i = 0; i < 512; i++) out[i / 8] |= (uint8_t)(bits[32 + i] << (7 - i % 8));
}

static void flip(uint8_t *b, int bit) { b[bit / 8] ^= (uint8_t)(0x80u >> (bit % 8)); }

/* ------------------------------------------------------------ plans -- */

static bool has(const uint32_t *hz, int n, uint32_t want)
{
    for (int i = 0; i < n; i++) if (hz[i] == want) return true;
    return false;
}

LS_CASE(the_uhf_plan_is_929_931_and_454_and_nothing_the_board_cannot_hear)
{
    uint32_t hz[PGR_CH_MAX];
    const int n = pgr_plan(PGR_PLAN_UHF, hz, PGR_CH_MAX);
    LS_EQ_INT(n, 106);
    static const uint32_t KNOWN[] = { 929612500u, 929662500u, 929937500u, 931062500u,
                                      931462500u, 931887500u, 929012500u, 931987500u,
                                      454025000u, 454650000u };
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) LS_CHECK_MSG(has(hz, n, KNOWN[i]), "%u", KNOWN[i]);
    for (int i = 0; i < n; i++) {
        LS_CHECK(hz[i] >= 400000000u);                         /* no VHF: the board is deaf there */
        LS_CHECK(!(hz[i] >= 930000000u && hz[i] < 931000000u)); /* narrowband PCS, not POCSAG */
        LS_CHECK(!(hz[i] >= 470000000u && hz[i] < 512000000u)); /* T-band: no paging allocation */
        LS_EQ_UINT(hz[i] % 12500u, 0u);
        for (int k = 0; k < i; k++) LS_CHECK(hz[k] != hz[i]);
    }
    LS_EQ_INT(pgr_plan(PGR_PLAN_UHF, hz, 5), 5);
    LS_EQ_INT(pgr_plan(PGR_PLAN_N, hz, PGR_CH_MAX), 0);
}

LS_CASE(the_onsite_plan_is_the_twelve_business_paging_channels)
{
    uint32_t hz[PGR_CH_MAX];
    const int n = pgr_plan(PGR_PLAN_ONSITE, hz, PGR_CH_MAX);
    LS_EQ_INT(n, 12);
    LS_CHECK(has(hz, n, 457525000u));
    LS_CHECK(has(hz, n, 457600000u));
    LS_CHECK(has(hz, n, 467750000u));
    LS_CHECK(has(hz, n, 467925000u));
    LS_CHECK(!has(hz, n, 467950000u));
}

LS_CASE(every_probe_fits_the_filter_and_lasts_a_preamble_and_a_batch)
{
    for (int i = 0; i < PGR_N_PROBES; i++) {
        const pgr_probe_t *p = pgr_probe(i);
        LS_CHECK(p != NULL);
        /* The driver refuses a session whose rate and deviation overflow the filter. */
        LS_CHECK(p->baud + 2 * p->deviation_hz <= 11700u);
        if (p->kind == PGR_POCSAG) {
            LS_EQ_UINT(p->sync_word, p->inverted ? ~PGR_POCSAG_FSC : PGR_POCSAG_FSC);
            LS_EQ_INT(p->payload_bytes, PGR_BATCH_BYTES);
            LS_CHECK((uint32_t)p->slice_ms * p->baud >= (576u + 544u) * 1000u);
        } else {
            LS_EQ_UINT(p->sync_word, p->inverted ? ~PGR_FLEX_MARKER : PGR_FLEX_MARKER);
            LS_EQ_INT(p->payload_bytes, 2);
            LS_CHECK(p->slice_ms >= 1875);
            LS_CHECK(pgr_probe_is_flex(i));
        }
    }
    LS_CHECK(pgr_probe(PGR_N_PROBES) == NULL);
    LS_EQ_STR(pgr_probe(0)->tag, "1200N");
}

/* ----------------------------------------------------------- batches -- */

LS_CASE(the_decoders_bch_check_says_clean_fixed_or_lost)
{
    uint32_t cw = pocsag_encode_address(1234567, 0);
    const uint32_t good = cw;
    LS_EQ_INT(pocsag_check_codeword(&cw), 0);
    cw = good ^ (1u << 17);
    LS_EQ_INT(pocsag_check_codeword(&cw), 1);
    LS_EQ_UINT(cw, good);
    cw = good ^ (1u << 17) ^ (1u << 3);
    LS_EQ_INT(pocsag_check_codeword(&cw), -1);
}

LS_CASE(a_clean_page_batch_reads_as_one_capcode_and_its_message)
{
    uint8_t b[PGR_BATCH_BYTES];
    /* A capcode whose low bits are 0 sits in frame 0, so the whole message
       fits behind it in the batch. */
    page_batch(1234560, 3, "HELLO WORLD", b);
    pgr_batch_t r;
    LS_EQ_INT(pgr_scan_batch(b, false, &r), 16);
    LS_CHECK(pgr_batch_real(&r));
    LS_EQ_INT(r.clean, 16);
    LS_EQ_INT(r.fixed, 0);
    LS_EQ_INT(r.bad, 0);
    LS_EQ_INT(r.address, 1);
    LS_EQ_INT(r.message, 4);       /* 11 characters, 77 bits: four codewords */
    LS_EQ_INT(r.idle, 11);
    LS_EQ_INT(r.n_caps, 1);
    LS_EQ_UINT(r.caps[0], 1234560u);
}

LS_CASE(the_capcode_takes_its_low_bits_from_the_frame)
{
    for (uint32_t ric = 800; ric < 808; ric++) {
        uint8_t b[PGR_BATCH_BYTES];
        page_batch(ric, 0, NULL, b);
        pgr_batch_t r;
        pgr_scan_batch(b, false, &r);
        LS_EQ_INT(r.n_caps, 1);
        LS_EQ_UINT(r.caps[0], ric);
    }
}

LS_CASE(bit_errors_count_as_fixed_and_lost_and_set_the_ber_floor)
{
    uint8_t b[PGR_BATCH_BYTES];
    page_batch(1234567, 3, "HELLO WORLD", b);
    flip(b, 0 * 32 + 5);           /* one each in three codewords      */
    flip(b, 4 * 32 + 30);
    flip(b, 15 * 32 + 1);
    flip(b, 9 * 32 + 2);           /* two in one: past fixing          */
    flip(b, 9 * 32 + 20);
    pgr_batch_t r;
    LS_EQ_INT(pgr_scan_batch(b, false, &r), 15);
    LS_EQ_INT(r.fixed, 3);
    LS_EQ_INT(r.bad, 1);
    LS_CHECK(pgr_batch_real(&r));
    /* (3 + 2*1) wrong bits of 512. */
    LS_NEAR(pgr_ber_pct(r.fixed, r.bad, 16), 100.0 * 5 / 512, 1e-4);
    LS_NEAR(pgr_ber_pct(0, 0, 0), -1.0, 1e-9);
}

LS_CASE(an_inverted_transmitter_reads_the_same_when_told)
{
    uint8_t b[PGR_BATCH_BYTES];
    page_batch(2000000, 1, "123", b);
    for (int i = 0; i < PGR_BATCH_BYTES; i++) b[i] = (uint8_t)~b[i];
    pgr_batch_t r;
    LS_EQ_INT(pgr_scan_batch(b, true, &r), 16);
    LS_EQ_UINT(r.caps[0], 2000000u);
    /* The complement of a codeword is a codeword too (all ones is one), so
       BCH alone cannot tell polarity: the sync word the radio matched does. */
    pgr_scan_batch(b, false, &r);
    LS_EQ_INT(r.clean, 16);
    LS_CHECK(r.n_caps == 0 || r.caps[0] != 2000000u);
}

LS_CASE(noise_that_matched_the_sync_is_not_a_frame)
{
    uint32_t x = 12345;
    int real = 0;
    for (int t = 0; t < 2000; t++) {
        uint8_t b[PGR_BATCH_BYTES];
        for (int i = 0; i < PGR_BATCH_BYTES; i++) { x = x * 1103515245u + 12345u; b[i] = (uint8_t)(x >> 16); }
        pgr_batch_t r;
        pgr_scan_batch(b, false, &r);
        if (pgr_batch_real(&r)) real++;
    }
    LS_EQ_INT(real, 0);
}

LS_CASE(the_pocsag_decoder_reads_the_same_batch_for_message_text)
{
    uint8_t b[PGR_BATCH_BYTES];
    page_batch(1234560, 3, "HELLO WORLD", b);
    fm_state_t *st = calloc(1, sizeof(*st));
    pocsag_ctx_t *c = pocsag_create(st, 1200);
    LS_CHECK(pocsag_process_batch(c, b, PGR_BATCH_BYTES, false, false));
    LS_EQ_UINT(pocsag_n_pages(c), 1u);
    LS_EQ_UINT(st->pages[0].address, 1234560u);
    LS_EQ_STR(st->pages[0].text, "HELLO WORLD");
    pocsag_destroy(c);
    free(st);
}

/* -------------------------------------------------------------- FLEX -- */

static int bits16(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

LS_CASE(flex_mode_codes_are_read_either_way_round_and_through_two_errors)
{
    static const uint16_t CODES[] = { 0x870C, 0xB068, 0x7B18, 0xDEA0, 0x4C7C };
    /* Two errors stay unambiguous only while the codes are five or more apart. */
    for (int i = 0; i < 5; i++)
        for (int k = i + 1; k < 5; k++) LS_CHECK(bits16((uint32_t)(CODES[i] ^ CODES[k])) >= 5);
    for (int i = 0; i < 5; i++) {
        const uint16_t sent = (uint16_t)~CODES[i];      /* the complement follows the marker */
        uint8_t two[2] = { (uint8_t)(sent >> 8), (uint8_t)sent };
        LS_EQ_INT(pgr_flex_mode(two, false), i);
        uint8_t inv[2] = { (uint8_t)~two[0], (uint8_t)~two[1] };
        LS_EQ_INT(pgr_flex_mode(inv, true), i);
        two[0] ^= 0x41;
        LS_EQ_INT(pgr_flex_mode(two, false), i);
    }
    LS_EQ_STR(pgr_flex_mode_name(0), "1600/2");
    LS_EQ_STR(pgr_flex_mode_name(9), "?");
    const uint8_t junk[2] = { 0x00, 0x00 };
    LS_EQ_INT(pgr_flex_mode(junk, true), -1);
}

/* ------------------------------------------------------------- counts -- */

LS_CASE(the_capcode_set_counts_distinct_and_says_when_it_is_full)
{
    static pgr_capset_t s;
    memset(&s, 0, sizeof(s));
    LS_CHECK(pgr_capset_add(&s, 7));
    LS_CHECK(!pgr_capset_add(&s, 7));
    for (uint32_t i = 100; i < 100 + PGR_CAPS_MAX; i++) pgr_capset_add(&s, i);
    LS_EQ_INT(s.n, PGR_CAPS_MAX);
    LS_CHECK(s.overflow);
}

LS_CASE(the_floor_is_the_median_and_ages_read_short)
{
    float v[5] = { -110, -120, -60, -118, -119 };
    LS_NEAR(pgr_median(v, 5), -118.0, 1e-6);
    float w[4] = { -1, -3, -2, -4 };
    LS_NEAR(pgr_median(w, 4), -2.5, 1e-6);
    char a[8];
    pgr_age(a, sizeof(a), 12 * 1000000LL); LS_EQ_STR(a, "12s");
    pgr_age(a, sizeof(a), 300 * 1000000LL); LS_EQ_STR(a, "5m");
    pgr_age(a, sizeof(a), -1); LS_EQ_STR(a, "-");
}

/* ===================================================== the experiment == */

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

/* One transmitter: a pager channel sending a batch every batch-time, at one
   rate and polarity. Everywhere else is noise near -119 dBm. */
static struct {
    uint32_t hz;
    uint16_t baud;
    bool inverted;
    uint8_t batch[PGR_BATCH_BYTES];
} TX;
static bool s_open;
static ls_fsk_cfg_t s_cfg;
static int64_t s_opened, s_sent;
static int s_begins;

int64_t esp_timer_get_time(void);

uint32_t ls_lora_caps(void) { return LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (s_open) return ESP_ERR_INVALID_STATE;
    if (cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_open = true;
    s_opened = s_sent = esp_timer_get_time();
    s_begins++;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_open = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return s_open ? ESP_OK : ESP_ERR_INVALID_STATE; }

static bool hears_tx(void)
{
    const uint32_t want = TX.inverted ? ~PGR_POCSAG_FSC : PGR_POCSAG_FSC;
    return s_open && s_cfg.freq_hz == TX.hz && s_cfg.bitrate == TX.baud && s_cfg.sync_word == want &&
           s_cfg.payload_bytes == PGR_BATCH_BYTES;
}

int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi)
{
    if (!s_open || size < s_cfg.payload_bytes) return -1;
    const int64_t now = esp_timer_get_time();
    const int64_t period = 544LL * 1000000 / (TX.baud ? TX.baud : 1200);
    if (!hears_tx() || now - s_sent < period) return 0;
    s_sent = now;
    for (int i = 0; i < PGR_BATCH_BYTES; i++) buf[i] = TX.inverted ? (uint8_t)~TX.batch[i] : TX.batch[i];
    if (rssi) *rssi = -71.0f;
    return PGR_BATCH_BYTES;
}

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    static unsigned k;
    *dbm = s_cfg.freq_hz == TX.hz ? -72.0f : -119.0f + (float)(k++ % 3);
    return ESP_OK;
}

static void run_for(int64_t us)
{
    for (int64_t t = 0; t < us; t += 2000) {
        ls_exp_service();
        ls_shim_time_advance(2000);
    }
}

static bool lines_have(const char *needle)
{
    static char out[24][LS_EXP_LINE];
    const int n = ls_exp_read_lines(&exp_pagers, out, 24);
    for (int i = 0; i < n; i++) if (strstr(out[i], needle)) return true;
    return false;
}

static void dump(void)
{
    static char out[24][LS_EXP_LINE];
    const int n = ls_exp_read_lines(&exp_pagers, out, 24);
    for (int i = 0; i < n; i++) ls_note("  |%s|", out[i]);
}

static void fresh(uint32_t hz, uint16_t baud, bool inverted, bool text)
{
    ls_exp_stop();
    ls_exp_settle(100);
    ls_exp_forget();
    ls_exp_register(&exp_pagers);
    ls_shim_time_set(1000000);
    memset(&TX, 0, sizeof(TX));
    TX.hz = hz;
    TX.baud = baud;
    TX.inverted = inverted;
    page_batch(1234560, 3, "HELLO WORLD", TX.batch);
    /* OPTIONS: the UHF plan, the default dwell, message text as asked. */
    exp_pagers.opts[0].set(&exp_pagers.opts[0], PGR_PLAN_UHF);
    exp_pagers.opts[2].set_num(&exp_pagers.opts[2], 6);
    exp_pagers.opts[3].set(&exp_pagers.opts[3], text ? 1 : 0);
}

LS_CASE(the_scan_surveys_then_finds_an_inverted_512_channel_and_locks_on_it)
{
    fresh(929612500u, 512, true, false);
    ls_exp_start(&exp_pagers);
    run_for(60LL * 1000000);
    dump();
    LS_CHECK(ls_exp_running() == &exp_pagers);
    LS_CHECK(lines_have("SCAN UHF PAGING 106 ch"));
    LS_CHECK(lines_have("floor -11"));
    LS_CHECK(lines_have("929.6125 POC  512I"));
    LS_CHECK(lines_have("POCSAG 1  FLEX 0  CARRIER 0"));
    LS_CHECK(lines_have("BCH ok"));
    /* The message stays unread with MESSAGE TEXT off. */
    LS_CHECK(!lines_have("HELLO"));
    LS_CHECK(lines_have("message text off"));
    ls_exp_stop();
    ls_exp_settle(100);
    LS_CHECK(!s_open);
    /* What was found stays up after the stop. */
    LS_CHECK(lines_have("929.6125 POC  512I"));
    LS_CHECK(lines_have("stopped"));
}

LS_CASE(message_text_is_shown_only_when_asked)
{
    fresh(931062500u, 1200, false, true);
    ls_exp_start(&exp_pagers);
    run_for(40LL * 1000000);
    dump();
    LS_CHECK(lines_have("931.0625 POC  1200N"));
    LS_CHECK(lines_have("931.0625 1234560 A HELLO WORLD"));
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(one_frequency_from_the_console_steps_through_the_probes_until_one_decodes)
{
    fresh(454125000u, 2400, true, false);
    char cmd[] = "exp pagers start 454.125";
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(cmd, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    LS_EQ_INT(ls_exp_console(argc, argv), 0);
    run_for(15LL * 1000000);
    dump();
    LS_CHECK(lines_have("ONE FREQ  454.1250 MHz"));
    LS_CHECK(lines_have("454.1250 POC  2400I"));
    ls_exp_stop();
    ls_exp_settle(100);
    /* The console's frequency was for that run: OPTIONS still scan. */
    LS_EQ_INT(exp_pagers.opts[0].get(&exp_pagers.opts[0]), PGR_PLAN_UHF);
}

LS_CASE(the_console_refuses_a_frequency_out_of_range)
{
    fresh(929612500u, 1200, false, false);
    char why[96] = "";
    char a0[] = "150";
    char *argv[] = { a0 };
    LS_CHECK(!exp_pagers.configure(1, argv, why, sizeof(why)));
    LS_CHECK(strstr(why, "200-1100") != NULL);
}
