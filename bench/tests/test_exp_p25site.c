/* LS_TEST_SOURCES: experiments/p25_sitefind.c, and P25 SITE FINDER itself
   (exp_p25site.c under ls_experiments.c) against a faked LoRa chip: its
   console setup (frequency, pinned polarity, filter, deviation, preamble
   detector), what lasts one run, and what a short read does.

   The NIDs are built with the BCH(63,16) generator and were checked against
   OP25's own decoder (gr-op25_repeater/lib/bch.cc bchDec, unpacked as
   p25_framer.cc does): each decodes with no errors to its NAC and DUID, and
   each comes back from five flipped bits. The symbol stream is built the way
   a C4FM transmitter sends it - frame sync, NID, status symbol after the
   eleventh NID dibit - and sliced by sign, as the radio does. */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_lora.h"
#include "p25_sitefind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

extern const ls_experiment_t exp_p25site;

static uint32_t s_rng = 2463534242u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static int bits32(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

/* ---------------------------------------------------------- the code -- */

LS_CASE(the_frame_sync_slices_to_04CF5F_and_its_complement)
{
    LS_EQ_UINT(p25sf_fs_signs(P25SF_FS), P25SF_SYNC_NORMAL);
    LS_EQ_UINT(P25SF_SYNC_INVERTED, (~P25SF_SYNC_NORMAL) & 0xFFFFFFu);
}

LS_CASE(the_encoder_matches_nids_op25_decodes_clean)
{
    static const struct { uint16_t nac; uint8_t duid; uint64_t nid; } V[] = {
        { 0x293, 0x7, 0x2937F88514ABAECCull },
        { 0x8A1, 0x7, 0x8A17BC1ACE5EEAEEull },
        { 0x5C2, 0x5, 0x5C258D5B695819BFull },   /* LDU1: parity bit set */
        { 0xF7E, 0x0, 0xF7E0CA9A425D42F0ull },
    };
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        const uint64_t nid = p25sf_nid_encode(V[i].nac, V[i].duid);
        LS_CHECK_MSG(nid == V[i].nid, "%03X/%X: %016llX", V[i].nac, V[i].duid, (unsigned long long)nid);
    }
}

/* The 63-bit codeword (NID without its parity bit) is a multiple of the
   generator: an independent check on the systematic encoder. */
LS_CASE(every_encoded_nid_is_a_bch_codeword)
{
    const uint64_t g = 0xCD930BDD3B2Bull;
    for (int t = 0; t < 4000; t++) {
        const uint16_t m = (uint16_t)rnd();
        uint64_t r = p25sf_nid_encode((uint16_t)(m >> 4), (uint8_t)(m & 15)) >> 1;
        for (int b = 62; b >= 47; b--) if (r & (1ull << b)) r ^= g << (b - 47);
        LS_EQ_UINT((uint32_t)r, 0u);
        LS_EQ_UINT((uint32_t)(r >> 32), 0u);
    }
}

/* What makes the sign bits enough: all 65536 projections differ, and since
   the code is linear the smallest weight of a nonzero one is the distance. */
LS_CASE(the_sign_bits_alone_name_every_nid_six_bits_apart)
{
    int min_w = 64;
    for (uint32_t m = 1; m < 65536u; m++) {
        const uint32_t s = p25sf_nid_signs(p25sf_nid_encode((uint16_t)(m >> 4), (uint8_t)(m & 15)) & ~1ull);
        const int w = bits32(s);
        if (w < min_w) min_w = w;
    }
    LS_EQ_INT(min_w, 6);
}

LS_CASE(the_decoder_returns_every_valid_nid_and_takes_two_errors)
{
    for (int t = 0; t < 600; t++) {
        static const uint8_t D[7] = { 0, 3, 5, 7, 10, 12, 15 };
        const uint16_t nac = (uint16_t)(rnd() & 0xFFF);
        const uint8_t duid = D[rnd() % 7];
        const uint32_t s = p25sf_nid_signs(p25sf_nid_encode(nac, duid));
        const int e = t % 3;
        uint32_t r = s;
        int flipped = 0;
        while (flipped < e) { const uint32_t b = 1u << (rnd() % 32); if (!((r ^ s) & b)) { r ^= b; flipped++; } }
        p25sf_nid_t out;
        LS_CHECK(p25sf_nid_decode(r, &out));
        LS_EQ_UINT(out.nac, nac);
        LS_EQ_UINT(out.duid, duid);
        LS_EQ_INT(out.dist, e);
    }
}

LS_CASE(three_errors_are_never_taken_for_a_nid)
{
    for (int t = 0; t < 300; t++) {
        const uint16_t nac = (uint16_t)(rnd() & 0xFFF);
        const uint32_t s = p25sf_nid_signs(p25sf_nid_encode(nac, 7));
        uint32_t r = s;
        int flipped = 0;
        while (flipped < 3) { const uint32_t b = 1u << (rnd() % 32); if (!((r ^ s) & b)) { r ^= b; flipped++; } }
        p25sf_nid_t out;
        LS_CHECK(!p25sf_nid_decode(r, &out));
        LS_CHECK(out.dist >= 3);
    }
}

LS_CASE(a_nid_with_a_duid_p25_does_not_use_is_refused)
{
    p25sf_nid_t out;
    LS_CHECK(!p25sf_nid_decode(p25sf_nid_signs(p25sf_nid_encode(0x123, 0x9)), &out));
    LS_EQ_INT(out.dist, 0);
    LS_EQ_UINT(out.duid, 9u);
    LS_EQ_STR(p25sf_duid_name(7), "TSBK");
    LS_EQ_STR(p25sf_duid_name(15), "TDULC");
    LS_EQ_STR(p25sf_duid_name(9), "?");
    LS_EQ_INT(p25sf_duid_slot(10), 4);
}

/* ------------------------------------------------------- on the air -- */

/* The symbols of a frame start: FS, then the NID's 32 dibits with a status
   symbol after the eleventh, then a few of the body. */
static int frame_dibits(uint64_t nid, uint8_t status, uint8_t *out)
{
    int n = 0;
    for (int t = 0; t < 24; t++) out[n++] = (uint8_t)((P25SF_FS >> (46 - 2 * t)) & 3u);
    for (int t = 0; t < 32; t++) {
        if (t == P25SF_STATUS_INDEX) out[n++] = status;
        out[n++] = (uint8_t)((nid >> (62 - 2 * t)) & 3u);
    }
    for (int t = 0; t < 8; t++) out[n++] = (uint8_t)(rnd() & 3u);
    return n;
}

/* C4FM: 01 +1800, 00 +600, 10 -600, 11 -1800 Hz. A receiver that calls a
   positive deviation 0 delivers each dibit's first bit; `flip` is the other
   kind of receiver. */
static uint8_t slice(uint8_t dibit, bool flip)
{
    static const int HZ[4] = { 600, 1800, -600, -1800 };
    const uint8_t bit = HZ[dibit] < 0;
    return flip ? (uint8_t)!bit : bit;
}

/* What the radio hands over: hunts the 24-bit sync it was given, then the
   next five bytes. */
static bool radio_receive(const uint8_t *bits, int n, uint32_t sync, uint8_t *payload)
{
    uint32_t sr = 0;
    for (int i = 0; i < n; i++) {
        sr = ((sr << 1) | bits[i]) & 0xFFFFFFu;
        if (i >= 23 && sr == sync && i + 40 < n) {
            memset(payload, 0, P25SF_PAYLOAD_BYTES);
            for (int k = 0; k < 40; k++) payload[k / 8] |= (uint8_t)(bits[i + 1 + k] << (7 - k % 8));
            return true;
        }
    }
    return false;
}

LS_CASE(a_sliced_frame_gives_its_nac_either_polarity_any_status)
{
    static const uint8_t D[7] = { 0, 3, 5, 7, 10, 12, 15 };
    for (int t = 0; t < 56; t++) {
        const bool flip = t & 1;
        const uint8_t status = (uint8_t)((t >> 1) & 3);
        const uint16_t nac = (uint16_t)(rnd() & 0xFFF);
        const uint8_t duid = D[t % 7];
        uint8_t dibits[80], bits[80];
        const int n = frame_dibits(p25sf_nid_encode(nac, duid), status, dibits);
        for (int i = 0; i < n; i++) bits[i] = slice(dibits[i], flip);
        uint8_t payload[P25SF_PAYLOAD_BYTES];
        /* Only the sync for this receiver's polarity finds the frame. */
        const uint32_t right = flip ? P25SF_SYNC_INVERTED : P25SF_SYNC_NORMAL;
        const uint32_t wrong = flip ? P25SF_SYNC_NORMAL : P25SF_SYNC_INVERTED;
        LS_CHECK(!radio_receive(bits, n, wrong, payload));
        LS_CHECK(radio_receive(bits, n, right, payload));
        p25sf_nid_t out;
        LS_CHECK(p25sf_nid_decode(p25sf_payload_signs(payload, flip), &out));
        LS_EQ_UINT(out.nac, nac);
        LS_EQ_UINT(out.duid, duid);
        LS_EQ_INT(out.dist, 0);
    }
}

LS_CASE(an_inner_symbol_sliced_wrong_is_a_correctable_error)
{
    /* +600 and -600 are the ones noise pushes over zero. */
    uint8_t dibits[80], bits[80];
    const int n = frame_dibits(p25sf_nid_encode(0x8A1, 7), 0, dibits);
    for (int i = 0; i < n; i++) bits[i] = slice(dibits[i], false);
    int turned = 0;
    for (int i = 24; i < 24 + 33 && turned < 2; i++)
        if (i != 24 + P25SF_STATUS_INDEX && (dibits[i] == 0 || dibits[i] == 2)) { bits[i] ^= 1; turned++; }
    LS_EQ_INT(turned, 2);
    uint8_t payload[P25SF_PAYLOAD_BYTES];
    LS_CHECK(radio_receive(bits, n, P25SF_SYNC_NORMAL, payload));
    p25sf_nid_t out;
    LS_CHECK(p25sf_nid_decode(p25sf_payload_signs(payload, false), &out));
    LS_EQ_UINT(out.nac, 0x8A1u);
    LS_EQ_INT(out.dist, 2);
}

/* ---------------------------------------------------------- profiles -- */

static const char PROFILE[] =
    "# LakeShark P25 profile - test\r\n"
    "version=2\r\n"
    "system=Route\r\n"
    "# ---------------------------------------------------------------------------\r\n"
    "# --- Manchester NH -- P25 Phase 1 -- WACN BEE00, SysID 8A6, NAC 8A1 --------\r\n"
    "# WARNING: Manchester POLICE is 100% encrypted.\r\n"
    "control=859487500|42.9956|-71.4548|25000\r\n"
    "control=858487500|42.9956|-71.4548|25000\r\n"
    "# --- Nashua NH -- P25 -- WACN BEE00, SysID 5C6, NAC 5C2 --------------------\r\n"
    "control=853450000|42.7654|-71.4676|20000\r\n"
    "# 852.750 is listed as a control channel by one source only.\r\n"
    "control=852750000|42.7654|-71.4676|20000\r\n"
    "control=859487500\r\n"
    "# --- NH State Police -- conventional VHF --------------------------------\r\n"
    "control=154650000\r\n"
    "control=460125000\r\n"
    "# 3A2 Somewhere Else\r\n"
    "  control=771106250\n"
    "control=7711x\n"
    "control=\n"
    "control=860000000";

/* The experiment's own range: 150 to 1100 MHz, so the VHF channel is in. */
LS_CASE(a_profile_gives_its_control_channels_with_the_nac_each_section_names)
{
    p25sf_chan_t ch[16];
    const int n = p25sf_profile_parse(PROFILE, strlen(PROFILE), 150000000u, 1100000000u, ch, 16);
    LS_EQ_INT(n, 8);
    LS_EQ_UINT(ch[0].hz, 859487500u); LS_EQ_INT(ch[0].nac, 0x8A1); LS_EQ_STR(ch[0].label, "Manchester NH");
    LS_EQ_UINT(ch[1].hz, 858487500u); LS_EQ_INT(ch[1].nac, 0x8A1);
    LS_EQ_UINT(ch[2].hz, 853450000u); LS_EQ_INT(ch[2].nac, 0x5C2); LS_EQ_STR(ch[2].label, "Nashua NH");
    /* "852.750" in a comment is not a NAC. */
    LS_EQ_UINT(ch[3].hz, 852750000u); LS_EQ_INT(ch[3].nac, 0x5C2);
    /* The duplicate 859.4875 is skipped; a section without a NAC clears it,
       and its VHF 154.650 is taken in file order. */
    LS_EQ_UINT(ch[4].hz, 154650000u); LS_EQ_INT(ch[4].nac, -1); LS_EQ_STR(ch[4].label, "NH State Police");
    LS_EQ_UINT(ch[5].hz, 460125000u); LS_EQ_INT(ch[5].nac, -1); LS_EQ_STR(ch[5].label, "NH State Police");
    LS_EQ_UINT(ch[6].hz, 771106250u); LS_EQ_INT(ch[6].nac, 0x3A2); LS_EQ_STR(ch[6].label, "Somewhere Else");
    LS_EQ_UINT(ch[7].hz, 860000000u);
    LS_EQ_INT(p25sf_profile_parse(PROFILE, strlen(PROFILE), 150000000u, 1100000000u, ch, 2), 2);
    LS_EQ_INT(p25sf_profile_parse("", 0, 0, 1, ch, 16), 0);
}

/* The parser's limits are the caller's: a 200 MHz floor leaves the VHF one out. */
LS_CASE(a_profile_parser_keeps_to_the_range_it_is_given)
{
    p25sf_chan_t ch[16];
    LS_EQ_INT(p25sf_profile_parse(PROFILE, strlen(PROFILE), 200000000u, 1100000000u, ch, 16), 7);
    LS_EQ_UINT(ch[4].hz, 460125000u);
    LS_EQ_INT(p25sf_profile_parse(PROFILE, strlen(PROFILE), 154650000u, 154650000u, ch, 16), 1);
    LS_EQ_UINT(ch[0].hz, 154650000u);
    LS_EQ_INT(p25sf_profile_parse(PROFILE, strlen(PROFILE), 154650001u, 460124999u, ch, 16), 0);
}

/* ===================================================== the experiment == */

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

/* A control channel on one frequency sending a TSBK every 75 ms, heard by a
   receiver that slices the other way round. */
static struct { uint32_t hz; uint16_t nac; bool inverted; int noisy; int short_len; } SITE;
static bool s_open;
static ls_fsk_cfg_t s_cfg;
static int64_t s_sent;
static int s_begins;

int64_t esp_timer_get_time(void);

uint32_t ls_lora_caps(void) { return LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST; }
static uint32_t s_bw_cap;           /* the widest rung, when a test wants one */
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return s_bw_cap && hz > s_bw_cap ? s_bw_cap : hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (s_open) return ESP_ERR_INVALID_STATE;
    if (cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz) return ESP_ERR_INVALID_ARG;
    if (cfg->sync_bits != 24 || (cfg->sync_word & 0xFFu)) return ESP_ERR_INVALID_ARG;
    if (cfg->freq_hz < 150000000u || cfg->freq_hz > 1100000000u) return ESP_ERR_INVALID_ARG;
    if (cfg->deviation_hz < 600 || cfg->deviation_hz > 200000) return ESP_ERR_INVALID_ARG;
    if (cfg->preamble_detect_bits > 32 || cfg->preamble_detect_bits % 8) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_open = true;
    s_begins++;
    s_sent = esp_timer_get_time();
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_open = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return s_open ? ESP_OK : ESP_ERR_INVALID_STATE; }

int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi)
{
    if (!s_open || size < s_cfg.payload_bytes) return -1;
    const int64_t now = esp_timer_get_time();
    if (s_cfg.freq_hz != SITE.hz || now - s_sent < 75000) return 0;
    const uint32_t want = (SITE.inverted ? P25SF_SYNC_INVERTED : P25SF_SYNC_NORMAL) << 8;
    if (s_cfg.sync_word != want) return 0;
    s_sent = now;
    uint8_t dibits[80], bits[80];
    const int n = frame_dibits(p25sf_nid_encode(SITE.nac, 7), 1, dibits);
    for (int i = 0; i < n; i++) bits[i] = slice(dibits[i], SITE.inverted);
    /* One frame in `noisy` has three sign bits wrong. */
    if (SITE.noisy && rnd() % SITE.noisy == 0) { bits[30] ^= 1; bits[40] ^= 1; bits[50] ^= 1; }
    uint8_t full[P25SF_PAYLOAD_BYTES];
    LS_CHECK(radio_receive(bits, n, s_cfg.sync_word >> 8, full));
    if (rssi) *rssi = -81.0f;
    if (SITE.short_len) {
        /* A read that is not the configured length: the bytes it has are the
           start of a real payload, and what is past them is not touched. */
        const int len = SITE.short_len < (int)size ? SITE.short_len : (int)size;
        for (int i = 0; i < len && i < P25SF_PAYLOAD_BYTES; i++) buf[i] = full[i];
        return len;
    }
    memcpy(buf, full, sizeof(full));
    return P25SF_PAYLOAD_BYTES;
}

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    *dbm = s_cfg.freq_hz == SITE.hz ? -80.0f : -121.0f;
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
    const int n = ls_exp_read_lines(&exp_p25site, out, 24);
    for (int i = 0; i < n; i++) if (strstr(out[i], needle)) return true;
    return false;
}

static void dump(void)
{
    static char out[24][LS_EXP_LINE];
    const int n = ls_exp_read_lines(&exp_p25site, out, 24);
    for (int i = 0; i < n; i++) ls_note("  |%s|", out[i]);
}

static int console(const char *line)
{
    static char buf[160];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    return ls_exp_console(argc, argv);
}

static void fresh(void)
{
    ls_exp_stop();
    ls_exp_settle(100);
    ls_exp_forget();
    ls_exp_register(&exp_p25site);
    ls_shim_time_set(1000000);
}

LS_CASE(one_frequency_finds_the_polarity_and_reads_the_nac)
{
    fresh();
    SITE.hz = 859487500u; SITE.nac = 0x8A1; SITE.inverted = true; SITE.noisy = 5;
    LS_EQ_INT(console("exp p25site start 859.4875"), 0);
    run_for(10LL * 1000000);
    dump();
    LS_CHECK(ls_exp_running() == &exp_p25site);
    LS_CHECK(lines_have("ONE FREQ  859.4875 MHz"));
    LS_CHECK(lines_have("859.4875 8A1    -"));
    LS_CHECK(lines_have("  I "));
    LS_CHECK(lines_have("TSBK"));
    LS_CHECK(!lines_have("NACS"));      /* the three-error frames never named another NAC */
    LS_EQ_INT(s_cfg.bitrate, 4800);
    LS_EQ_INT(s_cfg.payload_bytes, P25SF_PAYLOAD_BYTES);
    ls_exp_stop();
    ls_exp_settle(100);
    LS_CHECK(!s_open);
    LS_CHECK(lines_have("stopped"));
}

LS_CASE(the_profile_is_needed_for_a_scan_and_its_absence_is_said)
{
    fresh();
    exp_p25site.opts[0].set(&exp_p25site.opts[0], 0);
    LS_CHECK(console("exp p25site start") != 0);
    char st[96];
    ls_exp_state_line(&exp_p25site, st, sizeof(st));
    LS_CHECK_MSG(strstr(st, "No profile at /sdcard/p25_profile.txt") != NULL, "%s", st);
}

/* ------------------------------------------------ console setup, one run -- */

static void site(uint32_t hz, uint16_t nac, bool inverted)
{
    SITE.hz = hz; SITE.nac = nac; SITE.inverted = inverted; SITE.noisy = 0; SITE.short_len = 0;
}

/* OPTIONS' one frequency, so a bare `start` has something to listen to. */
static void options_one_freq(double mhz)
{
    exp_p25site.opts[1].set_num(&exp_p25site.opts[1], mhz);
    ls_exp_stop();
    ls_exp_settle(100);
}

static int console_args(const char *const *args, int n)
{
    char *argv[12] = { (char *)"exp", (char *)"p25site", (char *)"start" };
    int argc = 3;
    for (int i = 0; i < n && argc < 12; i++) argv[argc++] = (char *)args[i];
    return ls_exp_console(argc, argv);
}

static unsigned long syncs_seen(void)
{
    static char out[24][LS_EXP_LINE];
    const int n = ls_exp_read_lines(&exp_p25site, out, 24);
    for (int i = 0; i < n; i++) {
        unsigned long v;
        double f;
        if (sscanf(out[i], "%lf syncs %lu", &f, &v) == 2) return v;
    }
    return 0;
}

static void check_defaults(uint32_t hz)
{
    LS_EQ_UINT(s_cfg.freq_hz, hz);
    LS_EQ_UINT(s_cfg.bitrate, 4800u);
    LS_EQ_UINT(s_cfg.deviation_hz, 1800u);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 12500u);
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 0u);
    LS_EQ_UINT(s_cfg.sync_bits, 24u);
    LS_EQ_UINT(s_cfg.payload_bytes, (unsigned)P25SF_PAYLOAD_BYTES);
}

LS_CASE(the_default_run_is_4800_1800_12500_with_the_detector_off_and_both_polarities)
{
    fresh();
    options_one_freq(154.785);
    site(154785000u, 0x293, false);
    LS_EQ_INT(console("exp p25site start"), 0);
    check_defaults(154785000u);
    /* Normal first, and the opposite way once a slice passes without a NID. */
    LS_EQ_UINT(s_cfg.sync_word, P25SF_SYNC_NORMAL << 8);
    site(0, 0, false);
    bool saw_inverted = false;
    for (int t = 0; t < 12; t++) { run_for(500000); saw_inverted |= s_cfg.sync_word == (P25SF_SYNC_INVERTED << 8); }
    LS_CHECK(saw_inverted);
    LS_CHECK(exp_p25site.opts[1].lo == 150.0);
    LS_CHECK(exp_p25site.opts[1].hi == 1100.0);
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(the_frequency_floor_is_150_mhz_and_154_785_is_listened_to)
{
    static const struct { const char *arg; bool ok; uint32_t hz; } T[] = {
        { "150", true, 150000000u }, { "150.0", true, 150000000u }, { "150.0000004", true, 150000000u },
        { "154.785", true, 154785000u },
        { "460.125", true, 460125000u }, { "1100", true, 1100000000u }, { "1100.", true, 1100000000u },
        { "149.999999", false, 0 }, { "149.9999", false, 0 }, { "149", false, 0 }, { "0", false, 0 },
        { "1100.000001", false, 0 }, { "1100.1", false, 0 }, { "1101", false, 0 }, { "99999999999", false, 0 },
        { "abc", false, 0 }, { "154.785x", false, 0 }, { "1.54785e2", false, 0 }, { "0x9A", false, 0 },
        { "-154.785", false, 0 }, { "+154.785", false, 0 }, { "154.785.1", false, 0 }, { ".", false, 0 },
        { "", false, 0 }, { "nan", false, 0 }, { "inf", false, 0 }, { " 154.785", false, 0 },
        { "154,785", false, 0 }, { "1000000000000000000000000000", false, 0 },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        fresh();
        site(T[i].hz ? T[i].hz : 154785000u, 0x293, false);
        const char *args[] = { T[i].arg };
        const int rc = console_args(args, 1);
        LS_CHECK_MSG((rc == 0) == T[i].ok, "'%s' -> %d", T[i].arg, rc);
        if (T[i].ok) {
            LS_CHECK_MSG(ls_exp_running() == &exp_p25site && s_open && s_cfg.freq_hz == T[i].hz,
                         "'%s' -> %u Hz", T[i].arg, (unsigned)s_cfg.freq_hz);
        } else {
            LS_CHECK_MSG(!s_open && ls_exp_running() != &exp_p25site, "'%s' started something", T[i].arg);
        }
        ls_exp_stop();
        ls_exp_settle(100);
    }
}

LS_CASE(a_vhf_site_at_154_785_is_read_through_the_console_run)
{
    fresh();
    site(154785000u, 0x293, false);
    LS_EQ_INT(console("exp p25site start 154.785"), 0);
    run_for(5LL * 1000000);
    dump();
    LS_CHECK(lines_have("ONE FREQ  154.7850 MHz"));
    LS_CHECK(lines_have("154.7850 293"));
    LS_CHECK(lines_have("TSBK"));
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(the_overrides_reach_the_radio_and_last_one_run)
{
    fresh();
    options_one_freq(154.785);
    site(154785000u, 0x293, true);
    LS_EQ_INT(console("exp p25site start 154.785 I 15000 2400 16"), 0);
    LS_EQ_UINT(s_cfg.freq_hz, 154785000u);
    LS_EQ_UINT(s_cfg.bitrate, 4800u);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 15000u);
    LS_EQ_UINT(s_cfg.deviation_hz, 2400u);
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 16u);
    LS_EQ_UINT(s_cfg.sync_word, P25SF_SYNC_INVERTED << 8);
    run_for(2LL * 1000000);
    LS_CHECK(lines_have("TSBK"));
    ls_exp_stop();
    ls_exp_settle(100);

    /* The next start without any: the defaults, and both polarities. */
    LS_EQ_INT(console("exp p25site start"), 0);
    check_defaults(154785000u);
    LS_EQ_UINT(s_cfg.sync_word, P25SF_SYNC_NORMAL << 8);
    ls_exp_stop();
    ls_exp_settle(100);

    /* And one with a frequency only. */
    LS_EQ_INT(console("exp p25site start 154.785 N 9000 1200 8"), 0);
    LS_EQ_UINT(s_cfg.preamble_detect_bits, 8u);
    ls_exp_stop();
    ls_exp_settle(100);
    LS_EQ_INT(console("exp p25site start 154.785"), 0);
    check_defaults(154785000u);
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(an_override_never_started_is_dropped_by_the_next_start_and_by_profile)
{
    fresh();
    options_one_freq(154.785);
    char why[96];
    char a0[] = "154.785", a1[] = "I", a2[] = "20000", a3[] = "3000", a4[] = "24";
    char *good[] = { a0, a1, a2, a3, a4 };
    LS_CHECK(exp_p25site.configure(5, good, why, sizeof(why)));
    /* Asked for again with a bad detector: nothing of the first is left. */
    char b4[] = "7";
    char *bad[] = { a0, a1, a2, a3, b4 };
    LS_CHECK(!exp_p25site.configure(5, bad, why, sizeof(why)));
    LS_EQ_INT(console("exp p25site start"), 0);
    check_defaults(154785000u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* `profile` is as it was: it names the profile, and takes nothing else. */
    LS_CHECK(exp_p25site.configure(5, good, why, sizeof(why)));
    char p[] = "profile";
    char *prof[] = { p };
    LS_CHECK(exp_p25site.configure(1, prof, why, sizeof(why)));
    LS_CHECK(console("exp p25site start") != 0);
    char st[96];
    ls_exp_state_line(&exp_p25site, st, sizeof(st));
    LS_CHECK_MSG(strstr(st, "No profile at /sdcard/p25_profile.txt") != NULL, "%s", st);
    LS_CHECK(!s_open);
    LS_CHECK(console("exp p25site start profile") != 0);
    LS_CHECK(console("exp p25site start profile N") != 0);
    LS_CHECK(!s_open);
    options_one_freq(154.785);
    LS_EQ_INT(console("exp p25site start"), 0);
    check_defaults(154785000u);
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(polarity_and_detector_arguments_are_strict)
{
    static const struct { const char *a[5]; int n; bool ok; } T[] = {
        { { "154.785", "auto" }, 2, true }, { { "154.785", "AUTO" }, 2, true },
        { { "154.785", "n" }, 2, true }, { { "154.785", "i" }, 2, true },
        { { "154.785", "I", "12500", "1800", "0" }, 5, true },
        { { "154.785", "I", "8400" }, 3, true },                 /* 4800 + 2*1800 is the least */
        { { "154.785", "I", "12500", "600" }, 4, true },
        { { "154.785", "I", "12500", "200000" }, 4, false },     /* the filter cannot hold it */
        { { "154.785", "I", "404800", "200000" }, 4, true },
        { { "154.785", "I", "1000000", "200000", "32" }, 5, true },
        { { "154.785", "X" }, 2, false }, { { "154.785", "NI" }, 2, false }, { { "154.785", "" }, 2, false },
        { { "154.785", "0" }, 2, false }, { { "154.785", "1" }, 2, false }, { { "154.785", "autox" }, 2, false },
        { { "154.785", "I", "8399" }, 3, false }, { { "154.785", "I", "0" }, 3, false },
        { { "154.785", "I", "" }, 3, false }, { { "154.785", "I", "-12500" }, 3, false },
        { { "154.785", "I", "+12500" }, 3, false }, { { "154.785", "I", "12500.5" }, 3, false },
        { { "154.785", "I", "0x30D4" }, 3, false }, { { "154.785", "I", "1e4" }, 3, false },
        { { "154.785", "I", "1000001" }, 3, false },
        { { "154.785", "I", "4294967296" }, 3, false }, { { "154.785", "I", "4294967297" }, 3, false },
        { { "154.785", "I", "18446744073709551617" }, 3, false },
        { { "154.785", "I", "99999999999999999999999" }, 3, false },
        { { "154.785", "I", "12500", "599" }, 4, false }, { { "154.785", "I", "12500", "0" }, 4, false },
        { { "154.785", "I", "12500", "200001" }, 4, false }, { { "154.785", "I", "12500", "-1800" }, 4, false },
        { { "154.785", "I", "12500", "4294967296" }, 4, false },
        { { "154.785", "I", "12500", "1800", "7" }, 5, false }, { { "154.785", "I", "12500", "1800", "4" }, 5, false },
        { { "154.785", "I", "12500", "1800", "40" }, 5, false }, { { "154.785", "I", "12500", "1800", "33" }, 5, false },
        { { "154.785", "I", "12500", "1800", "-8" }, 5, false }, { { "154.785", "I", "12500", "1800", "8x" }, 5, false },
        { { "154.785", "I", "12500", "1800", "" }, 5, false },
        { { "154.785", "I", "12500", "1800", "4294967304" }, 5, false },   /* 2^32 + 8 */
        { { "154.785", "I", "12500", "1800", "18446744073709551624" }, 5, false },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        fresh();
        options_one_freq(154.785);
        site(154785000u, 0x293, false);
        const int rc = console_args(T[i].a, T[i].n);
        LS_CHECK_MSG((rc == 0) == T[i].ok, "case %d (%s ...) -> %d", (int)i, T[i].a[1], rc);
        if (!T[i].ok) {
            LS_CHECK_MSG(!s_open, "case %d started a session", (int)i);
            /* A good frequency in front of a bad argument leaves nothing for the next bare start. */
            LS_EQ_INT(console("exp p25site start"), 0);
            check_defaults(154785000u);
        }
        ls_exp_stop();
        ls_exp_settle(100);
    }

    /* Six arguments is one too many, and a frequency alone is still fine. */
    fresh();
    options_one_freq(154.785);
    const char *six[] = { "154.785", "I", "12500", "1800", "0", "1" };
    LS_CHECK(console_args(six, 6) != 0);
    LS_CHECK(!s_open);
}

LS_CASE(the_filter_rung_the_part_would_use_must_hold_the_signal)
{
    fresh();
    options_one_freq(154.785);
    site(154785000u, 0x293, false);
    s_bw_cap = 20000;
    /* 100 kHz asked for, 20 kHz is the widest the part has: 4800 + 2*8000 does not fit. */
    const char *wide[] = { "154.785", "N", "100000", "8000" };
    LS_CHECK(console_args(wide, 4) != 0);
    char why[96], a0[] = "154.785", a1[] = "N", a2[] = "100000", a3[] = "8000", a3b[] = "6000";
    char *wide_v[] = { a0, a1, a2, a3 }, *fits_v[] = { a0, a1, a2, a3b };
    LS_CHECK(!exp_p25site.configure(4, wide_v, why, sizeof(why)));    /* refused at the console, not at begin */
    LS_CHECK_MSG(strstr(why, "rung 20000") != NULL, "%s", why);
    LS_CHECK(exp_p25site.configure(4, fits_v, why, sizeof(why)));
    LS_CHECK(!s_open);
    const char *fits[] = { "154.785", "N", "100000", "6000" };       /* 16800 <= 20000 */
    LS_EQ_INT(console_args(fits, 4), 0);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 20000u);
    ls_exp_stop();
    ls_exp_settle(100);
    s_bw_cap = 0;
    LS_EQ_INT(console_args(wide, 4), 0);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 100000u);
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(every_detector_length_the_generic_api_takes_reaches_the_radio)
{
    static const char *const D[] = { "0", "8", "16", "24", "32" };
    for (int i = 0; i < 5; i++) {
        fresh();
        site(154785000u, 0x293, false);
        const char *args[] = { "154.785", "auto", "12500", "1800", D[i] };
        LS_EQ_INT(console_args(args, 5), 0);
        LS_EQ_UINT(s_cfg.preamble_detect_bits, (unsigned)atoi(D[i]));
        LS_EQ_UINT(s_cfg.bandwidth_hz, 12500u);
        LS_EQ_UINT(s_cfg.deviation_hz, 1800u);
        ls_exp_stop();
        ls_exp_settle(100);
    }
}

LS_CASE(a_pinned_polarity_is_never_switched_not_even_on_a_channel_gone_quiet)
{
    /* Pinned the wrong way for the site: silence for a minute and more, and
       the other sync word is never tried. */
    fresh();
    site(154785000u, 0x293, true);
    LS_EQ_INT(console("exp p25site start 154.785 N"), 0);
    LS_EQ_UINT(s_cfg.sync_word, P25SF_SYNC_NORMAL << 8);
    const int begins = s_begins;
    for (int t = 0; t < 80; t++) {
        run_for(500000);
        LS_CHECK_MSG(s_cfg.sync_word == (P25SF_SYNC_NORMAL << 8), "switched at %d", t);
    }
    LS_EQ_INT(s_begins, begins);
    LS_CHECK(!lines_have("TSBK"));
    LS_EQ_UINT(syncs_seen(), 0u);
    ls_exp_stop();
    ls_exp_settle(100);

    /* Pinned the right way: heard, then a long silence, then heard again on
       the same session. */
    fresh();
    site(154785000u, 0x8A1, true);
    LS_EQ_INT(console("exp p25site start 154.785 I"), 0);
    run_for(4LL * 1000000);
    LS_CHECK(lines_have("154.7850 8A1"));
    LS_CHECK(lines_have("  I "));
    const unsigned long before = syncs_seen();
    LS_CHECK(before > 0);
    const int begins_i = s_begins;
    site(0, 0, true);
    for (int t = 0; t < 240; t++) {          /* two minutes: past the 30 s the auto mode gives up on */
        run_for(500000);
        LS_CHECK_MSG(s_cfg.sync_word == (P25SF_SYNC_INVERTED << 8), "switched at %d", t);
    }
    LS_EQ_INT(s_begins, begins_i);
    LS_CHECK(lines_have("  I "));          /* still locked to the pin, not "-" */
    site(154785000u, 0x8A1, true);
    run_for(4LL * 1000000);
    LS_CHECK(syncs_seen() > before);
    LS_EQ_INT(s_begins, begins_i);
    LS_CHECK(s_cfg.sync_word == (P25SF_SYNC_INVERTED << 8));
    ls_exp_stop();
    ls_exp_settle(100);

    /* The same silence in auto does give the lock up and try the other way. */
    fresh();
    site(154785000u, 0x8A1, false);
    LS_EQ_INT(console("exp p25site start 154.785 auto"), 0);
    run_for(4LL * 1000000);
    LS_CHECK(lines_have("  N "));
    site(0, 0, false);
    bool inverted_tried = false;
    for (int t = 0; t < 240; t++) { run_for(500000); inverted_tried |= s_cfg.sync_word == (P25SF_SYNC_INVERTED << 8); }
    LS_CHECK(inverted_tried);
    ls_exp_stop();
    ls_exp_settle(100);

    /* A pin does not outlive its run. */
    fresh();
    options_one_freq(154.785);
    site(154785000u, 0x8A1, true);
    LS_EQ_INT(console("exp p25site start"), 0);
    run_for(8LL * 1000000);
    LS_CHECK(lines_have("  I "));
    LS_CHECK(lines_have("154.7850 8A1"));
    ls_exp_stop();
    ls_exp_settle(100);
}

LS_CASE(a_read_of_the_wrong_length_is_never_a_sync)
{
    fresh();
    site(154785000u, 0x293, false);
    LS_EQ_INT(console("exp p25site start 154.785 N"), 0);
    run_for(3LL * 1000000);
    LS_CHECK(lines_have("TSBK"));
    /* Quiet for a second so the readout has caught up, then the baseline. */
    site(0, 0, false);
    run_for(1LL * 1000000);
    const unsigned long base = syncs_seen();
    LS_CHECK(base > 0);
    /* Short ones carry the start of a real payload with the last packet's
       bytes after it; longer ones are not what was asked for either. */
    static const int LENS[] = { 1, 2, 3, 4, 6, 7, 8 };
    for (size_t i = 0; i < sizeof(LENS) / sizeof(LENS[0]); i++) {
        site(154785000u, 0x293, false);
        SITE.short_len = LENS[i];
        run_for(2LL * 1000000);
        site(0, 0, false);
        run_for(500000);
        LS_CHECK_MSG(syncs_seen() == base, "length %d counted: %lu vs %lu", LENS[i], syncs_seen(), base);
    }
    LS_CHECK(ls_exp_running() == &exp_p25site);
    site(154785000u, 0x293, false);
    run_for(3LL * 1000000);
    site(0, 0, false);
    run_for(500000);
    LS_CHECK(syncs_seen() > base);
    ls_exp_stop();
    ls_exp_settle(100);
}
