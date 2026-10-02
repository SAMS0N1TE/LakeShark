/* LS_TEST_SOURCES: experiments/p25_sitefind.c, and P25 SITE FINDER itself
   (exp_p25site.c under ls_experiments.c) against a faked LoRa chip.

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

LS_CASE(a_profile_gives_its_control_channels_with_the_nac_each_section_names)
{
    p25sf_chan_t ch[16];
    const int n = p25sf_profile_parse(PROFILE, strlen(PROFILE), 200000000u, 1100000000u, ch, 16);
    LS_EQ_INT(n, 7);
    LS_EQ_UINT(ch[0].hz, 859487500u); LS_EQ_INT(ch[0].nac, 0x8A1); LS_EQ_STR(ch[0].label, "Manchester NH");
    LS_EQ_UINT(ch[1].hz, 858487500u); LS_EQ_INT(ch[1].nac, 0x8A1);
    LS_EQ_UINT(ch[2].hz, 853450000u); LS_EQ_INT(ch[2].nac, 0x5C2); LS_EQ_STR(ch[2].label, "Nashua NH");
    /* "852.750" in a comment is not a NAC. */
    LS_EQ_UINT(ch[3].hz, 852750000u); LS_EQ_INT(ch[3].nac, 0x5C2);
    /* The duplicate 859.4875 and the VHF one are skipped; a section without a
       NAC clears it. */
    LS_EQ_UINT(ch[4].hz, 460125000u); LS_EQ_INT(ch[4].nac, -1); LS_EQ_STR(ch[4].label, "NH State Police");
    LS_EQ_UINT(ch[5].hz, 771106250u); LS_EQ_INT(ch[5].nac, 0x3A2); LS_EQ_STR(ch[5].label, "Somewhere Else");
    LS_EQ_UINT(ch[6].hz, 860000000u);
    LS_EQ_INT(p25sf_profile_parse(PROFILE, strlen(PROFILE), 200000000u, 1100000000u, ch, 2), 2);
    LS_EQ_INT(p25sf_profile_parse("", 0, 0, 1, ch, 16), 0);
}

/* ===================================================== the experiment == */

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

/* A control channel on one frequency sending a TSBK every 75 ms, heard by a
   receiver that slices the other way round. */
static struct { uint32_t hz; uint16_t nac; bool inverted; int noisy; } SITE;
static bool s_open;
static ls_fsk_cfg_t s_cfg;
static int64_t s_sent;

int64_t esp_timer_get_time(void);

uint32_t ls_lora_caps(void) { return LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (s_open) return ESP_ERR_INVALID_STATE;
    if (cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz) return ESP_ERR_INVALID_ARG;
    if (cfg->sync_bits != 24 || (cfg->sync_word & 0xFFu)) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_open = true;
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
    LS_CHECK(radio_receive(bits, n, s_cfg.sync_word >> 8, buf));
    if (rssi) *rssi = -81.0f;
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
    static char buf[96];
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
