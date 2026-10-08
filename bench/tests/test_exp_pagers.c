/* LS_TEST_SOURCES: experiments/pager_recon.c and apps/fm/pocsag.c, and
   PAGER RECON itself (exp_pagers.c under ls_experiments.c) against a faked
   LoRa chip.

   The batches are real POCSAG: built by the bench's own transmitter
   (fixtures/pocsag_gen.c), which the decoder's tests already hold to the
   standard, then cut where the radio cuts them - after the frame sync. */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_lora.h"
#include "ls_lora_lr20xx.h"
#include "p25_state.h"
#include "pager_recon.h"
#include "pocsag.h"
#include "pocsag_gen.h"
#include "fm_state.h"
#include "settings.h"

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
        LS_CHECK(hz[i] >= 400000000u);                         /* the plans are UHF only; a one-frequency run can still tune VHF */
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
    uint8_t payload[200];       /* after the first sync, normal polarity   */
    size_t len;                 /* what the transmitter sends in one go    */
    int limit, sent;            /* sends allowed (0: for ever), made       */
    uint32_t sync;              /* the sync word it sends (0: POCSAG's)    */
    int short_at;               /* the send number cut short (0: none)     */
    size_t short_len;           /* ... to this many bytes, still positive  */
} TX;
static bool s_open;
static ls_fsk_cfg_t s_cfg;
static int64_t s_opened, s_sent;
static int s_begins;
static uint8_t s_stream_fifo[32768];
static size_t s_stream_n, s_stream_at, s_stream_gap_at;
static int s_stream_faults, s_retunes;
static esp_err_t s_retune_result;

int64_t esp_timer_get_time(void);

static uint32_t s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
static p25_lr_cfg_t s_gain;
static lr20xx_gfsk_rx_stats_t s_chip_stats;
void p25_lr_get_cfg(p25_lr_cfg_t *out) { *out = s_gain; }
esp_err_t lr20xx_get_gfsk_rx_stats(lr20xx_gfsk_rx_stats_t *out)
{ *out = s_chip_stats; return ESP_OK; }
uint32_t ls_lora_caps(void) { return s_caps; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (s_open) return ESP_ERR_INVALID_STATE;
    if (!cfg->stream && cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_open = true;
    s_opened = s_sent = esp_timer_get_time();
    s_begins++;
    return ESP_OK;
}
int ls_lora_fsk_stream_read(uint8_t *buf, size_t size, bool *restarted)
{
    *restarted = s_stream_at == 0;
    if (s_stream_faults) { s_stream_faults--; return -1; }
    if (!s_open || !s_cfg.stream || size < 256) return -1;
    size_t n = s_stream_n - s_stream_at;
    const size_t prefix = n && s_cfg.stream_sync_prefix && !s_stream_at ? 4 : 0;
    if (s_stream_gap_at && s_stream_at < s_stream_gap_at && n > s_stream_gap_at-s_stream_at)
        n = s_stream_gap_at-s_stream_at;
    if (s_stream_gap_at && s_stream_at == s_stream_gap_at) { *restarted = true; s_stream_gap_at = 0; }
    if (n > size-prefix) n = size-prefix;
    if (prefix) for (int i = 0; i < 4; i++) buf[i] = s_cfg.sync_word >> (24-8*i);
    memcpy(buf+prefix, s_stream_fifo + s_stream_at, n);
    s_stream_at += n;
    return (int)(n+prefix);
}
esp_err_t ls_lora_fsk_retune(uint32_t hz)
{
    s_retunes++;
    if (s_retune_result != ESP_OK) return s_retune_result;
    s_cfg.freq_hz = hz;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_open = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return s_open ? ESP_OK : ESP_ERR_INVALID_STATE; }

static bool hears_tx(void)
{
    const uint32_t want = TX.sync ? TX.sync : TX.inverted ? ~PGR_POCSAG_FSC : PGR_POCSAG_FSC;
    return s_open && s_cfg.freq_hz == TX.hz && s_cfg.bitrate == TX.baud && s_cfg.sync_word == want &&
           s_cfg.payload_bytes == TX.len;
}

int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi)
{
    if (!s_open || size < s_cfg.payload_bytes) return -1;
    const int64_t now = esp_timer_get_time();
    const int64_t period = (int64_t)(TX.len * 8) * 1000000 / (TX.baud ? TX.baud : 1200);
    if (!hears_tx() || now - s_sent < period) return 0;
    if (TX.limit && TX.sent >= TX.limit) return 0;
    TX.sent++;
    s_sent = now;
    /* A short read still returns a positive count, and leaves the rest of the
       caller's buffer as the packet before it left it. */
    const size_t n = TX.short_at && TX.sent == TX.short_at ? TX.short_len : TX.len;
    for (size_t i = 0; i < n; i++) buf[i] = TX.inverted ? (uint8_t)~TX.payload[i] : TX.payload[i];
    if (rssi) *rssi = -71.0f;
    return (int)n;
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
    s_stream_n = s_stream_at = s_stream_gap_at = 0;
    s_stream_faults = s_retunes = 0;
    memset(&s_gain, 0, sizeof(s_gain));
    memset(&s_chip_stats, 0, sizeof(s_chip_stats));
    s_retune_result = ESP_OK;
    memset(&TX, 0, sizeof(TX));
    TX.hz = hz;
    TX.baud = baud;
    TX.inverted = inverted;
    page_batch(1234560, 3, "HELLO WORLD", TX.payload);
    TX.len = PGR_BATCH_BYTES;
    /* OPTIONS: the UHF plan, the default dwell, message text as asked. */
    exp_pagers.opts[0].set(&exp_pagers.opts[0], PGR_PLAN_UHF);
    exp_pagers.opts[4].set(&exp_pagers.opts[4], 0);
    exp_pagers.opts[5].set(&exp_pagers.opts[5], 1);
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
    /* Console starts persist the one-frequency plan in OPTIONS. */
    LS_EQ_INT(exp_pagers.opts[0].get(&exp_pagers.opts[0]), PGR_PLAN_N);
}

LS_CASE(the_console_refuses_a_frequency_out_of_range)
{
    fresh(929612500u, 1200, false, false);
    char why[96] = "";
    char a0[] = "149.999";
    char *argv[] = { a0 };
    LS_CHECK(!exp_pagers.configure(1, argv, why, sizeof(why)));
    LS_CHECK(strstr(why, "150-1100") != NULL);
}

LS_CASE(vhf_fixed_probe_stays_listening_and_overrides_are_only_for_one_run)
{
    fresh(152600000u, 2400, true, false);
    char why[96] = "";
    char *args[] = { "152.6", "2400I", "19500", "4500" };
    LS_CHECK(exp_pagers.configure(4, args, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_INT(s_cfg.freq_hz, 152600000u);
    LS_EQ_INT(s_cfg.bandwidth_hz, 19500u);
    const int begins = s_begins;
    TX.hz = 0; /* With no signal, a fixed probe must not rotate. */
    run_for(15LL * 1000000);
    LS_EQ_INT(s_begins, begins);
    TX.hz = 152600000u;
    run_for(2LL * 1000000);
    LS_CHECK(lines_have("152.6000 POC  2400I"));
    ls_exp_stop();
    ls_exp_settle(100);
    char *normal[] = { "152.6" };
    LS_CHECK(exp_pagers.configure(1, normal, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_INT(s_cfg.bandwidth_hz, 11700u);
    LS_EQ_INT(s_cfg.bitrate, 1200u);
    ls_exp_stop();
    ls_exp_settle(100);
}

/* OPTIONS' PROBE pins a ONE FREQ run to one probe, which then never steps;
   AUTO steps as before, and a scan plan ignores it. */
LS_CASE(one_freq_listens_on_the_probe_options_name)
{
    fresh(152600000u, 1200, true, false);
    for (int i = 0; i < PGR_N_PROBES; i++)
        LS_EQ_STR(exp_pagers.opts[4].names[i + 1], pgr_probe(i)->tag);
    exp_pagers.opts[1].set_num(&exp_pagers.opts[1], 152.6);
    exp_pagers.opts[4].set(&exp_pagers.opts[4], 2);           /* 1200I */
    ls_exp_start(&exp_pagers);
    run_for(30LL * 1000000);
    LS_EQ_UINT(s_cfg.freq_hz, 152600000u);
    LS_EQ_UINT(s_cfg.bitrate, 1200u);
    LS_EQ_UINT(s_cfg.sync_word, ~PGR_POCSAG_FSC);
    LS_CHECK(lines_have("152.6000 POC  1200I"));
    ls_exp_stop();
    ls_exp_settle(100);

    TX.hz = 0;                                               /* nothing on the air */
    ls_exp_start(&exp_pagers);
    run_for(30LL * 1000000);                                 /* AUTO would be on 512 by now */
    LS_EQ_UINT(s_cfg.bitrate, 1200u);
    LS_EQ_UINT(s_cfg.sync_word, ~PGR_POCSAG_FSC);
    ls_exp_stop();
    ls_exp_settle(100);
    exp_pagers.opts[4].set(&exp_pagers.opts[4], 0);           /* AUTO */
}

/* On a part with the detector, POCSAG listens behind it unless the console
   says otherwise; FLEX does not. */
LS_CASE(pocsag_probes_take_the_preamble_detector_where_the_part_has_one)
{
    fresh(152600000u, 1200, true, false);
    s_caps |= LS_LORA_CAP_FSK_DETECT;
    char why[96] = "";
    char *probe[] = { "152.6", "1200I" };
    LS_CHECK(exp_pagers.configure(2, probe, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 16u);
    ls_exp_stop();
    ls_exp_settle(100);

    char *flex[] = { "929.6125", "FLEXN" };
    LS_CHECK(exp_pagers.configure(2, flex, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    ls_exp_stop();
    ls_exp_settle(100);

    char *off[] = { "152.6", "1200I", "11700", "4500", "0" };
    LS_CHECK(exp_pagers.configure(5, off, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    ls_exp_stop();
    ls_exp_settle(100);

    s_caps &= ~LS_LORA_CAP_FSK_DETECT;
}

LS_CASE(preamble_detector_argument_is_validated_and_lasts_one_run)
{
    fresh(152600000u, 2400, true, false);
    char why[96] = "";

    /* Not given: off, as it always was. */
    char *plain[] = { "152.6", "2400I", "19500", "4500" };
    LS_CHECK(exp_pagers.configure(4, plain, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    ls_exp_stop();
    ls_exp_settle(100);

    static const char *const good[] = { "0", "8", "16", "24", "32" };
    for (unsigned i = 0; i < 5; i++) {
        char *args[] = { "152.6", "2400I", "19500", "4500", (char *)good[i] };
        LS_CHECK(exp_pagers.configure(5, args, why, sizeof(why)));
        ls_exp_start(&exp_pagers);
        ls_exp_service();
        LS_EQ_UINT(s_cfg.preamble_detect_bits, (unsigned)atoi(good[i]));
        LS_EQ_INT(s_cfg.bandwidth_hz, 19500u);
        ls_exp_stop();
        ls_exp_settle(100);
    }

    /* Anything else is refused with the choices named, and sets nothing. */
    static const char *const bad[] = { "1", "12", "33", "40", "64", "-8", "16x", "" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *args[] = { "152.6", "2400I", "19500", "4500", (char *)bad[i] };
        why[0] = 0;
        LS_CHECK(!exp_pagers.configure(5, args, why, sizeof(why)));
        LS_CHECK(strstr(why, "0 (off), 8, 16, 24 or 32") != NULL);
    }
    char *seven[] = { "152.6", "2400I", "19500", "4500", "16", "1", "1" };
    LS_CHECK(!exp_pagers.configure(7, seven, why, sizeof(why)));
    LS_CHECK(strstr(why, "detect 0|8|16|24|32") != NULL);

    /* A refused line leaves nothing from an earlier one: it is one run's. */
    char *on[] = { "152.6", "2400I", "19500", "4500", "32" };
    LS_CHECK(exp_pagers.configure(5, on, why, sizeof(why)));
    char *refused[] = { "152.6", "2400I", "19500", "4500", "12" };
    LS_CHECK(!exp_pagers.configure(5, refused, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* Used once, then gone: the next start, with or without a frequency. */
    LS_CHECK(exp_pagers.configure(5, on, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 32u);
    ls_exp_stop();
    ls_exp_settle(100);
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    ls_exp_stop();
    ls_exp_settle(100);
    char *only[] = { "152.6" };
    LS_CHECK(exp_pagers.configure(1, only, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    LS_EQ_INT(s_cfg.bandwidth_hz, 11700u);
    ls_exp_stop();
    ls_exp_settle(100);
}

/* ============================================ fixed-length burst capture == */

static const uint8_t FSC_BYTES[4] = { 0x7C, 0xD2, 0x15, 0xD8 };

/* `n` batches as a transmitter sends them after its first sync: each is a page
   to its own capcode, and the frame sync stands in front of every one after
   the first. Normal polarity; the fake complements all of it when inverted. */
static size_t burst_payload(int n)
{
    static const uint32_t RIC[3] = { 1234560, 2000000, 1500000 };
    size_t at = 0;
    for (int k = 0; k < n; k++) {
        if (k) { memcpy(TX.payload + at, FSC_BYTES, 4); at += 4; }
        page_batch(RIC[k], 3, "HELLO WORLD", TX.payload + at);
        at += PGR_BATCH_BYTES;
    }
    return at;
}

static bool configure_burst(const char *probe, const char *batches, char *why, size_t n)
{
    char *args[] = { "152.6", (char *)probe, "11700", "4500", "16", (char *)batches };
    return exp_pagers.configure(6, args, why, n);
}

/* Starts a one-shot burst run and lets `sends` bursts arrive. */
static void run_burst(const char *probe, const char *batches, int sends)
{
    char why[96] = "";
    LS_CHECK(configure_burst(probe, batches, why, sizeof(why)));
    TX.limit = sends;
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    run_for(10LL * 1000000);
    dump();
}

LS_CASE(burst_lengths_are_one_batch_then_each_later_batch_and_its_sync)
{
    fresh(152600000u, 1200, false, false);
    LS_EQ_INT(burst_payload(2), 132);
    LS_EQ_INT(burst_payload(3), 200);
}

LS_CASE(two_and_three_batch_bursts_decode_every_batch_in_both_polarities)
{
    for (int inv = 0; inv < 2; inv++) {
        for (int n = 2; n <= 3; n++) {
            fresh(152600000u, 1200, inv, false);
            TX.len = burst_payload(n);
            char b[2] = { (char)('0' + n), 0 };
            run_burst(inv ? "1200I" : "1200N", b, 1);
            LS_EQ_UINT(s_cfg.payload_bytes, n == 2 ? 132u : 200u);
            LS_EQ_UINT(s_cfg.preamble_detect_bits, 16u);
            LS_EQ_INT(TX.sent, 1);
            LS_CHECK(lines_have(inv ? "152.6000 POC  1200I" : "152.6000 POC  1200N"));
            char want[40];
            /* One frame and 16 clean codewords per batch. */
            snprintf(want, sizeof(want), "BCH ok %d fixed 0 lost 0", 16 * n);
            LS_CHECK_MSG(lines_have(want), "%s", want);
            /* The table row: one hardware sync plus a checked one per later batch, n frames. */
            snprintf(want, sizeof(want), "%5d %5d", n, n);
            LS_CHECK_MSG(lines_have(want), "%s", want);
            ls_exp_stop();
            ls_exp_settle(100);
        }
    }
}

LS_CASE(each_batch_of_a_burst_reaches_the_message_decoder)
{
    fresh(152600000u, 1200, true, true);
    TX.len = burst_payload(3);
    run_burst("1200I", "3", 1);
    LS_CHECK(lines_have("152.6000 1500000 A HELLO WORLD"));
    LS_CHECK(lines_have("152.6000 2000000 A HELLO WORLD"));
    LS_CHECK(lines_have("152.6000 1234560 A HELLO WORLD"));
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(a_bad_intervening_sync_ends_the_walk)
{
    /* The second sync wrong by one bit, then the idle word, then the right
       word moved a bit along: none of the batches after it is believed. */
    static const uint8_t IDLE[4] = { 0x7A, 0x89, 0xC1, 0x97 };
    for (int mode = 0; mode < 3; mode++) {
        for (int inv = 0; inv < 2; inv++) {
            fresh(152600000u, 1200, inv, false);
            TX.len = burst_payload(3);
            uint8_t *sync2 = TX.payload + 64 + 4 + 64;
            if (mode == 0) {
                sync2[1] ^= 0x10;
            } else if (mode == 1) {
                memcpy(sync2, IDLE, 4);
            } else {
                uint32_t w = (uint32_t)FSC_BYTES[0] << 24 | (uint32_t)FSC_BYTES[1] << 16 |
                             (uint32_t)FSC_BYTES[2] << 8 | FSC_BYTES[3];
                w >>= 1;
                for (int i = 0; i < 4; i++) sync2[i] = (uint8_t)(w >> (24 - 8 * i));
            }
            run_burst(inv ? "1200I" : "1200N", "3", 1);
            LS_CHECK_MSG(lines_have("BCH ok 32 fixed 0 lost 0"), "mode %d inv %d", mode, inv);
            LS_CHECK(!lines_have("BCH ok 48"));
            ls_exp_stop();
            ls_exp_settle(100);
        }
    }
    /* A bad FIRST intervening sync costs the second and third batches. */
    fresh(152600000u, 1200, false, false);
    TX.len = burst_payload(3);
    TX.payload[64] ^= 0x01;
    run_burst("1200N", "3", 1);
    LS_CHECK(lines_have("BCH ok 16 fixed 0 lost 0"));
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(a_burst_in_the_other_polarity_is_not_read_as_this_one)
{
    fresh(152600000u, 1200, true, false);
    TX.len = burst_payload(2);
    run_burst("1200N", "2", 1);
    LS_EQ_INT(TX.sent, 0);
    LS_CHECK(!lines_have("152.6000 POC"));
    ls_exp_stop();
    ls_exp_settle(100);
}

/* Only the fake's contract: a radio that delivers nothing gives nothing. What a
   real chip does with a transmission shorter than the fixed length is not
   known, and this does not say. */
LS_CASE(a_fake_that_withholds_the_short_transmission_delivers_no_batch)
{
    fresh(152600000u, 1200, false, false);
    TX.len = PGR_BATCH_BYTES;             /* the transmitter stops after one batch */
    run_burst("1200N", "2", 3);
    LS_EQ_UINT(s_cfg.payload_bytes, 132u);
    LS_EQ_INT(TX.sent, 0);
    LS_CHECK(!lines_have("152.6000 POC"));
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(batches_is_refused_unless_one_to_three_on_a_fixed_pocsag_probe)
{
    fresh(152600000u, 1200, false, false);
    char why[96];
    static const char *const bad[] = { "0", "4", "-1", "2x", "", "255", "256", "99999999999" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = 0;
        LS_CHECK_MSG(!configure_burst("1200I", bad[i], why, sizeof(why)), "'%s'", bad[i]);
        LS_CHECK(strstr(why, "batches: 1 (default), 2 or 3") != NULL);
    }
    static const char *const flex[] = { "FLEXN", "FLEXI" };
    for (int i = 0; i < 2; i++) {
        for (int n = 2; n <= 3; n++) {
            char b[2] = { (char)('0' + n), 0 };
            why[0] = 0;
            LS_CHECK(!configure_burst(flex[i], b, why, sizeof(why)));
            LS_CHECK(strstr(why, "not FLEX") != NULL);
        }
    }
    why[0] = 0;
    LS_CHECK(!configure_burst("900X", "2", why, sizeof(why)));
    LS_CHECK(strstr(why, "probe:") != NULL);
    /* A filter too narrow for the probe is still refused when batches is given. */
    char *narrow[] = { "152.6", "2400I", "9000", "4500", "16", "2" };
    LS_CHECK(!exp_pagers.configure(6, narrow, why, sizeof(why)));
    LS_CHECK(strstr(why, "filter must contain") != NULL);
    char *seven[] = { "152.6", "1200I", "11700", "4500", "16", "2", "3" };
    LS_CHECK(!exp_pagers.configure(7, seven, why, sizeof(why)));
}

LS_CASE(the_burst_length_lasts_one_run_and_every_other_start_stays_one_batch)
{
    fresh(152600000u, 1200, false, false);
    char why[96] = "";

    /* The proven command, byte for byte, is untouched. */
    char *proven[] = { "152.6", "1200I", "11700", "4500", "16" };
    LS_CHECK(exp_pagers.configure(5, proven, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 64u);
    LS_EQ_UINT(s_cfg.sync_word, ~PGR_POCSAG_FSC);
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 16u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* "1" is the default spelled out. */
    LS_CHECK(configure_burst("1200I", "1", why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 64u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* Used once, then gone: the next plain start. */
    LS_CHECK(configure_burst("1200I", "3", why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 200u);
    ls_exp_stop();
    ls_exp_settle(100);
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 64u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* A refused line leaves nothing from an earlier accepted one. */
    LS_CHECK(configure_burst("1200I", "2", why, sizeof(why)));
    LS_CHECK(!configure_burst("1200I", "4", why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 64u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* So does a later console line without the argument, or a plan name. */
    LS_CHECK(configure_burst("1200I", "2", why, sizeof(why)));
    LS_CHECK(exp_pagers.configure(5, proven, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 64u);
    ls_exp_stop();
    ls_exp_settle(100);
    char *plan[] = { "uhf" };
    LS_CHECK(configure_burst("1200I", "3", why, sizeof(why)));
    LS_CHECK(exp_pagers.configure(1, plan, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 64u);
    ls_exp_stop();
    ls_exp_settle(100);
}

/* A read that comes back positive but short is not a packet: the bytes past it
   are whatever the packet before left, which here is a perfectly good batch. */
LS_CASE(a_short_positive_read_is_dropped_not_parsed_with_the_previous_packets_tail)
{
    /* Default 64 bytes: the second read is 40 of them. */
    fresh(152600000u, 1200, false, false);
    TX.limit = 2; TX.short_at = 2; TX.short_len = 40;
    char why[96] = "";
    char *plain[] = { "152.6", "1200N", "11700", "4500", "16" };
    LS_CHECK(exp_pagers.configure(5, plain, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    run_for(10LL * 1000000);
    dump();
    LS_EQ_INT(TX.sent, 2);
    LS_CHECK(lines_have("BCH ok 16 fixed 0 lost 0"));
    LS_CHECK(lines_have("    1     1"));
    ls_exp_stop();
    ls_exp_settle(100);

    /* 2 and 3 batches, in both polarities: the second read stops inside
       the second batch, so the first would still look whole. */
    for (int inv = 0; inv < 2; inv++) {
        for (int n = 2; n <= 3; n++) {
            fresh(152600000u, 1200, inv, false);
            TX.len = burst_payload(n);
            TX.short_at = 2; TX.short_len = TX.len - 7;
            char b[2] = { (char)('0' + n), 0 };
            run_burst(inv ? "1200I" : "1200N", b, 2);
            LS_EQ_INT(TX.sent, 2);
            char want[40];
            snprintf(want, sizeof(want), "BCH ok %d fixed 0 lost 0", 16 * n);
            LS_CHECK_MSG(lines_have(want), "n %d inv %d: %s", n, inv, want);
            snprintf(want, sizeof(want), "%5d %5d", n, n);
            LS_CHECK_MSG(lines_have(want), "n %d inv %d: %s", n, inv, want);
            ls_exp_stop();
            ls_exp_settle(100);
        }
    }

    /* FLEX, 2 bytes: the second read is 1 byte, the first byte new and the second stale. */
    fresh(152600000u, 1600, false, false);
    TX.sync = PGR_FLEX_MARKER;
    TX.len = 2;
    TX.payload[0] = 0x78; TX.payload[1] = 0xF3;       /* ~0x870C: mode 0, 1600/2 */
    TX.limit = 2; TX.short_at = 2; TX.short_len = 1;
    char *flex[] = { "152.6", "FLEXN" };
    LS_CHECK(exp_pagers.configure(2, flex, why, sizeof(why)));
    ls_exp_start(&exp_pagers);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.payload_bytes, 2u);
    run_for(10LL * 1000000);
    dump();
    LS_EQ_INT(TX.sent, 2);
    LS_CHECK(lines_have("152.6000 FLEX 1600/2    1"));
    ls_exp_stop();
    ls_exp_settle(100);
}

/* A message that runs from one batch into the next is joined only when the
   batch before it was accepted. */
LS_CASE(a_message_is_not_spliced_across_a_batch_that_was_not_accepted)
{
    static const char TEXT[] = "SPLICE-ACROSS-A-LOST-BATCH-0123";
    /* The generator never runs a message past its first batch, so the two
       batches are made here: capcode 1234567 sits in frame 7, the last, with
       the start of the message after it; the rest of the message opens the
       next batch, and idle ends it. */
    uint8_t msg[7 * sizeof(TEXT)];
    size_t n_msg = 0;
    for (const char *t = TEXT; *t; t++)
        for (int k = 0; k < 7; k++) msg[n_msg++] = (uint8_t)((*t >> k) & 1);
    uint32_t wa[16], wc[16];
    for (int i = 0; i < 16; i++) wa[i] = wc[i] = PGR_POCSAG_IDLE;
    wa[14] = pocsag_encode_address(1234567, 3);
    size_t used = 0;
    for (int i = 15, in_a = 1, ci = 0; used < n_msg; ) {
        uint32_t d = 0;
        for (int b = 0; b < 20; b++, used++) d = (d << 1) | (used < n_msg ? msg[used] : 0u);
        if (in_a) wa[i] = pocsag_encode_message(d); else wc[ci++] = pocsag_encode_message(d);
        if (in_a) in_a = 0;
    }
    uint8_t a[PGR_BATCH_BYTES], c[PGR_BATCH_BYTES], noise[PGR_BATCH_BYTES];
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) {
            a[i * 4 + k] = (uint8_t)(wa[i] >> (24 - 8 * k));
            c[i * 4 + k] = (uint8_t)(wc[i] >> (24 - 8 * k));
        }
    uint32_t x = 99;
    pgr_batch_t r;
    do {
        for (int i = 0; i < PGR_BATCH_BYTES; i++) { x = x * 1103515245u + 12345u; noise[i] = (uint8_t)(x >> 16); }
        pgr_scan_batch(noise, false, &r);
    } while (pgr_batch_real(&r));
    pgr_scan_batch(a, false, &r);
    LS_CHECK(pgr_batch_real(&r));
    pgr_scan_batch(c, false, &r);
    LS_CHECK(pgr_batch_real(&r));

    /* Two accepted batches in a row: the whole message arrives. */
    fresh(152600000u, 1200, false, true);
    memcpy(TX.payload, a, 64);
    memcpy(TX.payload + 64, FSC_BYTES, 4);
    memcpy(TX.payload + 68, c, 64);
    TX.len = 132;
    run_burst("1200N", "2", 1);
    LS_CHECK(lines_have("1234567 A SPLICE-ACROSS-A-LOST-BATCH-0123"));
    LS_CHECK(lines_have("BCH ok 32 fixed 0 lost 0"));
    ls_exp_stop();
    ls_exp_settle(100);

    /* The batch between is noise: the two good ones still count, and the
       second does not finish the first's message. */
    fresh(152600000u, 1200, false, true);
    memcpy(TX.payload, a, 64);
    memcpy(TX.payload + 64, FSC_BYTES, 4);
    memcpy(TX.payload + 68, noise, 64);
    memcpy(TX.payload + 132, FSC_BYTES, 4);
    memcpy(TX.payload + 136, c, 64);
    TX.len = 200;
    run_burst("1200N", "3", 1);
    LS_CHECK(lines_have("BCH ok 32 fixed 0 lost 0"));
    LS_CHECK(lines_have("    3     2"));
    LS_CHECK(!lines_have("1234567"));
    LS_CHECK(!lines_have("SPLICE"));
    LS_CHECK(!lines_have("LOST-BATCH"));
    ls_exp_stop();
    ls_exp_settle(100);
}


static void start_stream_console(char *method)
{
    char *argv[] = { "exp", "pagers", "start", "152.600", method };
    LS_EQ_INT(ls_exp_console(method ? 5 : 4, argv), 0);
}

LS_CASE(stream_console_uses_software_sync_and_keeps_text_off)
{
    fresh(152600000u, 1200, true, false);
    const uint32_t caps = s_caps;
    s_caps |= LS_LORA_CAP_FSK_STREAM;
    uint8_t bits[BITS_GEN_MAX];
    const size_t nb = pocsag_build_bits(1234568, 3, "STREAM PRIVATE TEXT", 576, bits, sizeof(bits));
    memset(s_stream_fifo, 0, sizeof(s_stream_fifo));
    const size_t samples = PGR_STREAM_RATE / 1200;
    for (size_t i = 0; i < nb * samples + 128; i++)
        s_stream_fifo[i / 8] |= (uint8_t)((bits[i / samples < nb ? i / samples : nb - 1] ^ 1) << (7 - i % 8));
    s_stream_n = nb * samples / 8 + 16;
    s_gain.rx_gain_step = 13;
    s_gain.rx_boost_step = 8;
    s_gain.pulse_shape = 1;
    s_chip_stats.sync_ok = 65534;
    s_chip_stats.sync_fail = 100;
    start_stream_console("sign");
    ls_exp_service();  /* baseline counters before the new run's events */
    s_chip_stats.sync_ok = 1;
    s_chip_stats.sync_fail = 107;
    run_for(1000000);
    LS_CHECK(s_cfg.stream);
    LS_EQ_UINT(s_cfg.bitrate, PGR_STREAM_RATE);
    LS_EQ_UINT(s_cfg.deviation_hz, 4500);
    LS_EQ_UINT(s_cfg.bandwidth_hz, PGR_STREAM_BW);
    LS_EQ_UINT(s_cfg.sync_bits, 8);
    LS_EQ_UINT(s_cfg.sync_word, 0xCC000000u);
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0);
    LS_EQ_UINT(s_cfg.rx_gain_step, 13);
    LS_EQ_UINT(s_cfg.rx_boost_step, 8);
    LS_EQ_UINT(s_cfg.pulse_shape, 1);
    dump();
    LS_CHECK(lines_have("152.6000 POC  1200I"));
    LS_CHECK(lines_have("STREAM frames 2 pages 1 err 0"));
    char flow[96];
    snprintf(flow, sizeof(flow), "bytes %llu starts 1 restarts 0", (unsigned long long)s_stream_n);
    LS_CHECK(lines_have(flow));
    LS_CHECK(lines_have("chip sync-ok +3 sync-fail +7"));
    LS_CHECK(!lines_have("STREAM PRIVATE TEXT"));
    LS_EQ_INT(s_retunes, 0);
    ls_exp_stop(); ls_exp_settle(100);
    start_stream_console("packet");
    run_for(100000);
    LS_CHECK(!s_cfg.stream);
    LS_CHECK(lines_have("PACKET frames"));
    s_caps = caps;
}

LS_CASE(sign_stream_trim_uses_retune_and_reports_read_errors)
{
    fresh(152600000u, 1200, false, false);
    const uint32_t caps = s_caps;
    s_caps |= LS_LORA_CAP_FSK_STREAM;
    exp_pagers.opts[5].set(&exp_pagers.opts[5], 0);
    start_stream_console("sign");
    run_for(10000);
    LS_CHECK(s_cfg.stream);
    s_stream_faults = 1;
    run_for(300000);
    LS_CHECK(lines_have("err 1"));
    for (int i = 0; i < 32; i++) {
        s_stream_at = 0; s_stream_n = 4800;
        memset(s_stream_fifo, 0xEE, s_stream_n);
        run_for(30000);
    }
    LS_EQ_UINT(s_cfg.freq_hz, 152606000u);
    LS_CHECK(s_retunes >= 24);
    LS_CHECK(lines_have("trim +6000 Hz"));
    ls_exp_stop(); ls_exp_settle(100);
    exp_pagers.opts[5].set(&exp_pagers.opts[5], 1);
    start_stream_console(NULL);
    run_for(10000);
    LS_CHECK(!s_cfg.stream);
    exp_pagers.opts[5].set(&exp_pagers.opts[5], 0);
    s_caps = caps;
}

LS_CASE(stream_discontinuities_and_persistent_faults_keep_data_flow_visible)
{
    fresh(152600000u, 1200, false, false);
    const uint32_t caps = s_caps;
    s_caps |= LS_LORA_CAP_FSK_STREAM;
    start_stream_console("sign");
    run_for(10000);
    LS_EQ_UINT(s_cfg.rx_gain_step, 0);
    LS_EQ_UINT(s_cfg.rx_boost_step, 0);
    memset(s_stream_fifo, 0xAA, 64);
    s_stream_n = 64;
    run_for(300000);
    LS_CHECK(lines_have("bytes 64 starts 1 restarts 0"));
    s_stream_at = 0;  /* fake driver's first data after another sync */
    run_for(300000);
    LS_CHECK(lines_have("bytes 128 starts 1 restarts 1"));
    const int begins = s_begins;
    s_stream_faults = 50;
    run_for(300000);
    LS_EQ_INT(s_begins, begins + 1);
    LS_CHECK(s_open);
    s_stream_at = 0;
    run_for(300000);
    LS_CHECK(lines_have("bytes 192 starts 2 restarts 1"));
    LS_CHECK(lines_have("err 50"));
    ls_exp_stop(); ls_exp_settle(100);
    LS_CHECK(lines_have("bytes 192 starts 2 restarts 1"));
    start_stream_console("sign");
    run_for(300000);
    LS_CHECK(lines_have("bytes 0 starts 0 restarts 0"));
    LS_CHECK(lines_have("chip sync-ok +0 sync-fail +0"));
    ls_exp_stop(); ls_exp_settle(100);
    s_caps = caps;
}

LS_CASE(native_stream_ten_batches_gap_bch_and_saved_start)
{
    fresh(152600000u, 1200, true, false);
    const uint32_t caps = s_caps;
    s_caps |= LS_LORA_CAP_FSK_STREAM | LS_LORA_CAP_FSK_DETECT;
    exp_pagers.opts[5].set(&exp_pagers.opts[5], 0);
    exp_pagers.opts[2].set_num(&exp_pagers.opts[2], 12);
    /* Same measured command, native by default on LR2021. */
    char *argv[] = { "152.6", "1200I", "11700", "4500", "16" };
    char why[96];
    LS_CHECK(exp_pagers.configure(5, argv, why, sizeof(why)));
    ls_exp_start(&exp_pagers); ls_exp_service();
    LS_CHECK(s_cfg.stream); LS_CHECK(s_cfg.stream_sync_prefix);
    LS_EQ_UINT(s_cfg.bitrate, 1200); LS_EQ_UINT(s_cfg.bandwidth_hz, 11700);
    LS_EQ_UINT(s_cfg.deviation_hz, 4500); LS_EQ_UINT(s_cfg.preamble_detect_bits, 16);
    LS_EQ_UINT(s_cfg.sync_bits, 32); LS_EQ_UINT(s_cfg.sync_word, ~PGR_POCSAG_FSC);
    uint8_t batch[64]; page_batch(1234560, 0, NULL, batch);
    flip(batch, 2*32+9); /* fixed */
    flip(batch, 3*32+9); flip(batch, 3*32+17); /* lost */
    size_t at = 0;
    for (int k = 0; k < 10; k++) {
        if (k) for (int b = 0; b < 4; b++) s_stream_fifo[at++] = s_cfg.sync_word >> (24-8*b);
        for (int b = 0; b < 64; b++) s_stream_fifo[at++] = (uint8_t)~batch[b];
        if (k == 4) s_stream_gap_at = at; /* loss seam, next bytes include genuine sync */
    }
    s_stream_n = at;
    run_for(1000000);
    LS_CHECK(lines_have("STREAM frames 10 pages 10 err 10"));
    LS_CHECK(lines_have("BCH ok 140 fixed 10 lost 10"));
    LS_CHECK(lines_have("bytes 680 starts 1 restarts 1"));
    LS_EQ_INT(s_retunes, 0);
    settings_pagers_t saved;
    settings_get_pagers(&saved);
    LS_EQ_UINT(saved.hz, 152600000); LS_EQ_INT(saved.plan, PGR_PLAN_N);
    LS_EQ_INT(saved.probe, 1); LS_EQ_INT(saved.dwell, 12); LS_EQ_INT(saved.method, 0);
    ls_exp_stop(); ls_exp_settle(100);
    /* A start restores the settings cache, even if populated by boot NVS. */
    saved.dwell = 15; LS_CHECK(settings_set_pagers(&saved));
    s_stream_n = s_stream_at = 0;
    ls_exp_start(&exp_pagers); ls_exp_service();
    LS_EQ_UINT(s_cfg.freq_hz, 152600000); LS_EQ_UINT(s_cfg.sync_word, ~PGR_POCSAG_FSC);
    LS_CHECK(s_cfg.stream); LS_EQ_INT(exp_pagers.opts[2].num(&exp_pagers.opts[2]), 15);
    ls_exp_stop(); ls_exp_settle(100);
    s_caps = caps;
}
