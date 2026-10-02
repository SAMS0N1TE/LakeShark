/* LS_TEST_SOURCES: experiments/dfm_decode.c, experiments/exp_dfm17.c,
   ls_experiments.c and ls_geo.c: DFM radiosonde frames built here with the
   sonde's own Hamming code, interleave and Manchester, through the chip
   bytes the LoRa chip would hand over, to serial, position and sensors;
   bit errors the code must correct and ones it must refuse; and RADIOSONDE
   on a faked LR2021 - its session settings, finding the polarity, the
   readout with range from HOME, and the AUTO sweep. */

#include "ls_test.h"

#include "dfm_decode.h"
#include "ls_experiments.h"
#include "ls_gps.h"
#include "ls_lora.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

/* ------------------------------------------------------- the encoder -- */

/* One frame's content: 7 configuration nibbles and two data packets of 13. */
typedef struct { uint8_t conf[7], dat1[13], dat2[13]; } frame_t;

/* The 264 bits after the header, before Manchester. */
typedef struct { uint8_t b[264]; } body_t;

static void nibbles(uint64_t v, int count, uint8_t *out)
{
    for (int i = 0; i < count; i++) out[i] = (uint8_t)((v >> (4 * (count - 1 - i))) & 0xF);
}

/* A data packet: 32 bits, 16 bits, then the packet number. */
static void dat_pkt(uint8_t out[13], uint32_t a, uint16_t b, int id)
{
    nibbles(((uint64_t)a << 20) | ((uint64_t)b << 4) | (uint64_t)id, 13, out);
}

static void conf_pkt(uint8_t out[7], uint32_t v28) { nibbles(v28, 7, out); }

/* Codeword i of a block of L goes out as bit j at L*j + i. */
static void interleave(const uint8_t *nib, int L, uint8_t *bits)
{
    for (int i = 0; i < L; i++) {
        const uint8_t cw = dfm_hamming_encode(nib[i]);
        for (int j = 0; j < 8; j++) bits[L * j + i] = (cw >> (7 - j)) & 1;
    }
}

static void body_of(const frame_t *f, body_t *body)
{
    interleave(f->conf, 7, body->b);
    interleave(f->dat1, 13, body->b + 56);
    interleave(f->dat2, 13, body->b + 160);
}

/* Bit k of the body sits in codeword i, bit j of the block it is in. */
static int body_pos(int block, int i, int j)
{
    static const int OFS[3] = { 0, 56, 160 }, LEN[3] = { 7, 13, 13 };
    return OFS[block] + LEN[block] * j + i;
}

static void chip_set(uint8_t *chips, int c, int v)
{
    if (v) chips[c >> 3] |= (uint8_t)(0x80 >> (c & 7));
    else chips[c >> 3] &= (uint8_t)~(0x80 >> (c & 7));
}

/* Manchester, 0 as 10 and 1 as 01, MSB first; inverted turns every chip. */
static void chips_of(const body_t *body, bool inverted, uint8_t chips[DFM_CHIP_BYTES])
{
    memset(chips, 0, DFM_CHIP_BYTES);
    for (int k = 0; k < 264; k++) {
        const int b = body->b[k];
        chip_set(chips, 2 * k, (!b) ^ inverted);
        chip_set(chips, 2 * k + 1, b ^ inverted);
    }
}

static void chip_flip(uint8_t *chips, int c) { chips[c >> 3] ^= (uint8_t)(0x80 >> (c & 7)); }

/* ------------------------------------------------------ the telemetry -- */

#define SERIAL 26002503u   /* 0x018CC447 */
#define LAT    43.8912345
#define LON    (-70.2561234)

/* Configuration: the measurement channels 0-8 (5 is the battery on an
   STM32 sonde, 6 its MCU temperature), then the serial's two halves on
   channel 0xB. The thermistor reads 104.8 k against 20 k and 332 k
   references: -40 C. */
static uint32_t conf_word(int k)
{
    switch (k % 11) {
    case 0: return (0u << 24) | 124800u;
    case 1: return (1u << 24) | 100000u;
    case 2: return (2u << 24) | 100000u;
    case 3: return (3u << 24) | 20000u;
    case 4: return (4u << 24) | 332000u;
    case 5: return (5u << 24) | (3120u << 4);
    case 6: return (6u << 24) | (29315u << 4);
    case 7: return (7u << 24) | (1234u << 4);
    case 8: return (8u << 24) | 100000u;
    case 9: return (0xBu << 24) | (0xCu << 20) | ((SERIAL >> 16) << 4) | 0;
    default: return (0xBu << 24) | (0xCu << 20) | ((SERIAL & 0xFFFF) << 4) | 1;
    }
}

/* Data packet `id` of the cycle, in position mode 2 (DFM-06/09) or 3. */
static void dat_mode2(uint8_t out[13], int id, int frnr)
{
    switch (id) {
    case 0: dat_pkt(out, (2u << 8) | (uint32_t)(frnr & 0xFF), 0, 0); break;
    case 1: dat_pkt(out, 0x000001FFu, 22000, 1); break;                  /* 9 SVs, 22.000 s */
    case 2: dat_pkt(out, (uint32_t)(int32_t)(LAT * 1e7), (uint16_t)1234, 2); break;  /* 12.34 m/s */
    case 3: dat_pkt(out, (uint32_t)(int32_t)(LON * 1e7), 24550, 3); break;           /* 245.50 */
    case 4: dat_pkt(out, 1234567u, (uint16_t)520, 4); break;              /* 12345.67 m, +5.2 */
    case 5: dat_pkt(out, (uint32_t)(uint16_t)(int16_t)-2850 << 16, 0, 5); break;     /* -28.5 m */
    case 8: dat_pkt(out, (2026u << 20) | (10u << 16) | (2u << 11) | (12u << 6) | 3u,
                    (uint16_t)(11u << 8), 8); break;
    default: dat_pkt(out, 0, 0, id); break;
    }
}

static void dat_mode3(uint8_t out[13], int id)
{
    switch (id) {
    case 0: dat_pkt(out, (22000u << 16) | (3u << 8), (uint16_t)1234, 0); break;
    case 1: dat_pkt(out, (uint32_t)(int32_t)(LAT * 1e7), 24550, 1); break;
    case 2: dat_pkt(out, (uint32_t)(int32_t)(LON * 1e7), (uint16_t)(int16_t)-1530, 2); break;
    case 3: dat_pkt(out, 1234567u, 0, 3); break;
    case 8: dat_pkt(out, (2026u << 20) | (10u << 16) | (2u << 11) | (12u << 6) | 3u,
                    (uint16_t)(11u << 8), 8); break;
    default: dat_pkt(out, 0, 0, id); break;
    }
}

/* Frame k of a continuous transmission: one configuration channel and the
   next two data packets of the nine-packet cycle. */
static void frame_k(int k, int mode, frame_t *f)
{
    conf_pkt(f->conf, conf_word(k));
    const int a = (2 * k) % 9, b = (2 * k + 1) % 9;
    if (mode == 3) { dat_mode3(f->dat1, a); dat_mode3(f->dat2, b); }
    else { dat_mode2(f->dat1, a, k); dat_mode2(f->dat2, b, k); }
}

static void chips_k(int k, int mode, bool inverted, uint8_t chips[DFM_CHIP_BYTES])
{
    frame_t f;
    body_t body;
    frame_k(k, mode, &f);
    body_of(&f, &body);
    chips_of(&body, inverted, chips);
}

/* ----------------------------------------------------------- framing -- */

LS_CASE(the_sync_word_is_the_header_in_manchester)
{
    uint32_t w = 0, inv = 0;
    for (int i = 15; i >= 0; i--) {
        const int b = (DFM_HEADER >> i) & 1;
        w = (w << 2) | (b ? 1u : 2u);
        inv = (inv << 2) | (b ? 2u : 1u);
    }
    LS_EQ_UINT(w, DFM_SYNC);
    LS_EQ_UINT(inv, DFM_SYNC_INV);
    LS_EQ_UINT(DFM_SYNC ^ DFM_SYNC_INV, 0xFFFFFFFFu);
    /* 528 chips follow it. */
    LS_EQ_INT(DFM_CHIP_BYTES * 8, 2 * (DFM_FRAME_BITS - DFM_HEAD_BITS));
}

LS_CASE(the_hamming_code_is_systematic_with_distance_four)
{
    for (int a = 0; a < 16; a++) {
        const uint8_t ca = dfm_hamming_encode((uint8_t)a);
        LS_EQ_INT(ca >> 4, a);
        for (int b = a + 1; b < 16; b++) {
            const int d = __builtin_popcount(ca ^ dfm_hamming_encode((uint8_t)b));
            LS_CHECK_MSG(d >= 4, "%d and %d differ in %d bits", a, b, d);
        }
    }
}

/* ------------------------------------------------------------ decoding -- */

static void run_frames(dfm_t *d, int from, int count, int mode, bool inverted, int *positions)
{
    uint8_t chips[DFM_CHIP_BYTES];
    for (int k = from; k < from + count; k++) {
        chips_k(k, mode, inverted, chips);
        dfm_frame_info_t fi;
        LS_CHECK(dfm_frame(d, chips, inverted, &fi));
        LS_EQ_INT(fi.blocks_ok, 3);
        LS_EQ_INT(fi.corrected, 0);
        LS_EQ_INT(fi.violations, 0);
        if (fi.position && positions) (*positions)++;
    }
}

static void check_mode2_report(const dfm_report_t *r)
{
    LS_EQ_UINT(r->serial, SERIAL);
    LS_CHECK(!r->serial_hex);
    LS_EQ_STR(r->type, "DFM17");
    LS_EQ_INT(r->subtype, 0xB);
    LS_CHECK(r->have_fix);
    LS_NEAR(r->lat, LAT, 1e-6);
    LS_NEAR(r->lon, LON, 1e-6);
    /* Ellipsoid 12345.67 m, geoid 28.5 m below it. */
    LS_NEAR(r->alt_m, 12345.67 - 28.5, 0.01);
    LS_NEAR(r->vel_h, 12.34, 1e-3);
    LS_NEAR(r->heading, 245.5, 1e-3);
    LS_NEAR(r->vel_v, 5.2, 1e-3);
    LS_EQ_INT(r->year, 2026);
    LS_EQ_INT(r->month, 10);
    LS_EQ_INT(r->day, 2);
    LS_EQ_INT(r->hour, 12);
    LS_EQ_INT(r->minute, 3);
    LS_NEAR(r->sec, 22.0, 1e-3);
    LS_EQ_INT(r->sats, 11);
    LS_CHECK(r->have_batt);
    LS_NEAR(r->batt_v, 3.12, 1e-3);
    LS_CHECK(r->have_temp);
    LS_NEAR(r->temp_c, -40.0, 0.5);
}

LS_CASE(clean_frames_give_serial_position_and_sensors)
{
    dfm_t d;
    dfm_init(&d);
    LS_EQ_STR(d.out.type, "DFM");
    int positions = 0;
    /* The battery is believed once every channel has come since the serial:
       three rounds of configuration, and seven cycles of data. */
    run_frames(&d, 0, 32, 2, false, &positions);
    LS_CHECK(positions >= 2);
    check_mode2_report(&d.out);
    LS_EQ_UINT(d.out.fixes, (uint32_t)positions);
    LS_CHECK(d.out.frnr >= 0);
}

LS_CASE(an_inverted_receiver_decodes_the_same_frames)
{
    dfm_t d;
    dfm_init(&d);
    run_frames(&d, 0, 32, 2, true, NULL);
    check_mode2_report(&d.out);
}

LS_CASE(position_mode_three_is_read_one_packet_earlier_and_above_sea_level)
{
    dfm_t d;
    dfm_init(&d);
    int positions = 0;
    run_frames(&d, 0, 32, 3, false, &positions);
    LS_CHECK(positions >= 2);
    const dfm_report_t *r = &d.out;
    LS_EQ_UINT(r->serial, SERIAL);
    LS_NEAR(r->lat, LAT, 1e-6);
    LS_NEAR(r->lon, LON, 1e-6);
    LS_NEAR(r->alt_m, 12345.67, 0.01);
    LS_NEAR(r->vel_h, 12.34, 1e-3);
    LS_NEAR(r->heading, 245.5, 1e-3);
    LS_NEAR(r->vel_v, -15.3, 1e-3);
    LS_NEAR(r->sec, 22.0, 1e-3);
    LS_EQ_INT(r->frnr, -1);
}

LS_CASE(one_wrong_bit_in_every_codeword_is_put_right)
{
    dfm_t d;
    dfm_init(&d);
    uint8_t chips[DFM_CHIP_BYTES];
    int positions = 0;
    for (int k = 0; k < 32; k++) {
        chips_k(k, 2, false, chips);
        /* Turn both chips of one bit in each of the 33 codewords, a
           different bit each time: a clean Manchester pair, the wrong bit. */
        static const int LEN[3] = { 7, 13, 13 };
        for (int blk = 0, n = 0; blk < 3; blk++)
            for (int i = 0; i < LEN[blk]; i++, n++) {
                const int p = body_pos(blk, i, (n + k) % 8);
                chip_flip(chips, 2 * p);
                chip_flip(chips, 2 * p + 1);
            }
        dfm_frame_info_t fi;
        LS_CHECK(dfm_frame(&d, chips, false, &fi));
        LS_EQ_INT(fi.blocks_ok, 3);
        LS_EQ_INT(fi.corrected, 33);
        LS_EQ_INT(fi.violations, 0);
        if (fi.position) positions++;
    }
    /* 13 corrected codewords is more than a data block is believed with,
       and a configuration channel that needed correcting gives its value
       but is not trusted to say which channel carries the serial. */
    LS_EQ_INT(positions, 0);
    LS_EQ_UINT(d.out.serial, 0);

    /* ...and with at most four codewords of each data block hit, positions
       come through as well. */
    dfm_init(&d);
    positions = 0;
    for (int k = 0; k < 32; k++) {
        chips_k(k, 2, false, chips);
        for (int blk = 1; blk < 3; blk++)
            for (int i = 0; i < 4; i++) {
                const int p = body_pos(blk, 3 * i, (i + k) % 8);
                chip_flip(chips, 2 * p);
                chip_flip(chips, 2 * p + 1);
            }
        dfm_frame_info_t fi;
        LS_CHECK(dfm_frame(&d, chips, false, &fi));
        LS_EQ_INT(fi.corrected, 8);
        if (fi.position) positions++;
    }
    LS_CHECK(positions >= 2);
    check_mode2_report(&d.out);
}

LS_CASE(a_broken_chip_pair_helps_correct_a_second_error)
{
    dfm_t d;
    dfm_init(&d);
    uint8_t chips[DFM_CHIP_BYTES];
    chips_k(1, 2, false, chips);   /* packets 2 and 3: latitude, longitude */
    /* Codeword 0 of the first data block: bit 1 wrong outright, and bit 5's
       pair broken (second chip turned) so it is wrong and marked. */
    int p = body_pos(1, 0, 1);
    chip_flip(chips, 2 * p);
    chip_flip(chips, 2 * p + 1);
    p = body_pos(1, 0, 5);
    chip_flip(chips, 2 * p + 1);
    dfm_frame_info_t fi;
    LS_CHECK(dfm_frame(&d, chips, false, &fi));
    LS_EQ_INT(fi.violations, 1);
    LS_EQ_INT(fi.blocks_ok, 3);
    LS_EQ_INT(fi.blocks_bad, 0);
    LS_EQ_INT(fi.corrected, 2);
    LS_NEAR(d.lat, LAT, 1e-6);
}

LS_CASE(two_hidden_errors_in_a_codeword_lose_the_block_not_the_position)
{
    dfm_t d;
    dfm_init(&d);
    run_frames(&d, 0, 32, 2, false, NULL);
    LS_CHECK(d.out.have_fix);
    const uint32_t fixes = d.out.fixes;

    /* Every frame from here has two clean-looking wrong bits in the first
       codeword of each data block: neither block is believed, so no new
       position is made from them and the last one stands. */
    uint8_t chips[DFM_CHIP_BYTES];
    for (int k = 32; k < 48; k++) {
        chips_k(k, 2, false, chips);
        for (int blk = 1; blk < 3; blk++)
            for (int j = 0; j < 2; j++) {
                const int p = body_pos(blk, 0, j);
                chip_flip(chips, 2 * p);
                chip_flip(chips, 2 * p + 1);
            }
        dfm_frame_info_t fi;
        LS_CHECK(dfm_frame(&d, chips, false, &fi));
        LS_EQ_INT(fi.blocks_ok, 1);
        LS_EQ_INT(fi.blocks_bad, 2);
        LS_CHECK(!fi.position);
    }
    LS_EQ_UINT(d.out.fixes, fixes);
    check_mode2_report(&d.out);
}

LS_CASE(noise_is_not_a_frame)
{
    dfm_t d;
    dfm_init(&d);
    uint8_t chips[DFM_CHIP_BYTES];
    uint32_t x = 0x12345678u;
    int believed = 0;
    for (int k = 0; k < 200; k++) {
        for (int i = 0; i < DFM_CHIP_BYTES; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            chips[i] = (uint8_t)x;
        }
        dfm_frame_info_t fi;
        dfm_frame(&d, chips, false, &fi);
        LS_CHECK(fi.violations > 60);
        believed += fi.position;
    }
    LS_EQ_INT(believed, 0);
    LS_CHECK(!d.out.have_fix);
}

/* --------------------------------------------------- the experiment -- */

static int s_takes, s_gives;
const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { s_takes++; return true; }
void ls_exp_hw_radio_give(void) { s_gives++; }
bool ls_exp_hw_wake(void) { return false; }

/* The air: one sonde sending frames back to back, every 224 ms, heard only
   by a session tuned within 6 kHz with the sync word of its polarity. */
static bool s_fsk, s_scan;
static ls_fsk_cfg_t s_cfg;
static int s_begins, s_ends, s_scan_begins, s_rows;
static uint32_t s_air_hz;
static bool s_air_inverted, s_air_on;
static int s_air_k;
static int64_t s_air_next;
static float s_home_lat = 43.6591f, s_home_lon = -70.2568f;
static bool s_have_home = true;

uint32_t ls_lora_caps(void) { return LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz < 24038 && hz > 12019 ? 24038 : hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (s_fsk || s_scan) return ESP_ERR_INVALID_STATE;
    s_begins++;
    s_cfg = *cfg;
    s_fsk = true;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_ends++; s_fsk = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return ESP_OK; }

static bool hears_sonde(void)
{
    const uint32_t d = s_cfg.freq_hz > s_air_hz ? s_cfg.freq_hz - s_air_hz : s_air_hz - s_cfg.freq_hz;
    return s_air_on && s_fsk && d <= 6000 &&
           s_cfg.sync_word == (s_air_inverted ? DFM_SYNC_INV : DFM_SYNC);
}

int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi)
{
    const int64_t now = esp_timer_get_time();
    if (!s_air_on || now < s_air_next) return 0;
    const int k = s_air_k++;
    s_air_next += 224000;
    if (!hears_sonde() || size < DFM_CHIP_BYTES) return 0;
    chips_k(k, 2, s_air_inverted, buf);
    *rssi = -91.5f;
    return DFM_CHIP_BYTES;
}

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    *dbm = hears_sonde() ? -90.0f : -121.0f;
    return ESP_OK;
}

esp_err_t ls_lora_scan_begin(uint32_t min_hz, uint32_t max_hz)
{
    if (s_fsk) return ESP_ERR_INVALID_STATE;
    LS_EQ_UINT(min_hz, 400000000u);
    LS_EQ_UINT(max_hz, 406000000u);
    s_scan_begins++;
    s_scan = true;
    return ESP_OK;
}
int ls_lora_scan_pass(float *dbm, int n, bool *row_done)
{
    if (!s_scan) return 0;
    LS_EQ_INT(n, 601);
    for (int i = 0; i < n; i++) dbm[i] = -120.0f + (float)(i % 4);
    /* A carrier on the sonde's channel and, louder, one that is not a sonde. */
    if (s_air_on) dbm[(s_air_hz - 400000000u) / 10000u] = -88.0f;
    dbm[150] = -70.0f;   /* 401.500 MHz */
    /* A row takes two passes, as a real one takes many. */
    *row_done = ++s_rows % 2 == 0;
    return n;
}
esp_err_t ls_lora_scan_end(void) { s_scan = false; return ESP_OK; }

bool settings_get_home(float *lat, float *lon)
{
    if (!s_have_home) return false;
    *lat = s_home_lat;
    *lon = s_home_lon;
    return true;
}
void ls_gps_get(ls_gps_state_t *out) { memset(out, 0, sizeof(*out)); }

extern const ls_experiment_t exp_dfm17;

static const ls_experiment_t *fresh(void)
{
    ls_exp_forget();
    ls_exp_register(&exp_dfm17);
    s_fsk = s_scan = false;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_begins = s_ends = s_scan_begins = s_rows = 0;
    s_air_hz = 403410000u;
    s_air_inverted = false;
    s_air_on = true;
    s_air_k = 0;
    ls_shim_time_set(1000000);
    s_air_next = 1000000;
    s_have_home = true;
    return &exp_dfm17;
}

static int run_console(const char *line)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    return ls_exp_console(argc, argv);
}

/* The worker for `ms` milliseconds, a poll every 2 ms. */
static void run_ms(int ms)
{
    for (int i = 0; i < ms / 2; i++) {
        ls_exp_service();
        ls_shim_time_advance(2000);
    }
}

static char s_out[24][LS_EXP_LINE];

static int read_lines(const ls_experiment_t *e)
{
    return ls_exp_read_lines(e, s_out, 24);
}

static bool has_line(int n, const char *prefix)
{
    for (int i = 0; i < n; i++)
        if (!strncmp(s_out[i], prefix, strlen(prefix))) return true;
    return false;
}

static const char *line_of(int n, const char *prefix)
{
    for (int i = 0; i < n; i++)
        if (!strncmp(s_out[i], prefix, strlen(prefix))) return s_out[i];
    return "(none)";
}

LS_CASE(radiosonde_takes_a_frequency_in_the_band_or_auto)
{
    fresh();
    LS_EQ_INT(run_console("exp dfm17 start 399.9"), 1);
    LS_EQ_INT(run_console("exp dfm17 start 406.1"), 1);
    LS_EQ_INT(run_console("exp dfm17 start four"), 1);
    LS_CHECK(ls_exp_running() == NULL);
    LS_EQ_INT(run_console("exp dfm17 start 403.41"), 0);
    LS_CHECK(ls_exp_running() == &exp_dfm17);
    LS_EQ_INT(run_console("exp dfm17 stop"), 0);
    LS_CHECK(!s_fsk);
}

LS_CASE(radiosonde_listens_at_2500_chips_and_finds_the_polarity)
{
    const ls_experiment_t *e = fresh();
    s_air_inverted = true;
    LS_EQ_INT(run_console("exp dfm17 start 403.41"), 0);
    LS_EQ_INT(s_begins, 1);
    LS_EQ_UINT(s_cfg.freq_hz, 403410000u);
    LS_EQ_UINT(s_cfg.bitrate, 2500);
    LS_EQ_UINT(s_cfg.deviation_hz, 3000);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 24038);
    LS_EQ_UINT(s_cfg.sync_word, DFM_SYNC);
    LS_EQ_INT(s_cfg.sync_bits, 32);
    LS_EQ_INT(s_cfg.payload_bytes, 66);
    LS_CHECK(s_cfg.bitrate + 2 * s_cfg.deviation_hz <= s_cfg.bandwidth_hz);

    int n = read_lines(e);
    LS_EQ_STR(s_out[0], "listening 403.410 MHz");
    LS_EQ_STR(s_out[1], "FREQ    403.410 MHz  FIXED  pol + trying");

    /* Nothing with the header the right way up: the other way is tried. */
    run_ms(2100);
    LS_EQ_UINT(s_cfg.sync_word, DFM_SYNC_INV);
    LS_EQ_UINT(s_cfg.freq_hz, 403410000u);
    /* The serial, every measurement channel and a GPS cycle after them. */
    run_ms(7000);
    n = read_lines(e);
    for (int i = 0; i < n; i++) ls_note("%s", s_out[i]);
    LS_EQ_STR(s_out[0], "DFM17 FOUND  ID D26002503");
    LS_EQ_STR(s_out[1], "FREQ    403.410 MHz  FIXED  pol - held");
    LS_EQ_STR(s_out[2], "RSSI    -91.5 dBm frame  -90.0 carrier");
    LS_EQ_STR(line_of(n, "ALT"), "ALT     12317 m   ASC +5.2 m/s");
    LS_EQ_STR(line_of(n, "POS"), "POS     43.89123 N  70.25612 W");
    LS_EQ_STR(line_of(n, "SPEED"), "SPEED   12.3 m/s  HDG 246");
    /* HOME is about 25 km south of it. */
    LS_EQ_STR(line_of(n, "RANGE"), "RANGE   25.8 km  BRG 000 N  home");
    LS_EQ_STR(line_of(n, "TIME"), "TIME    2026-10-02 12:03:22Z  sats 11");
    LS_EQ_STR(line_of(n, "SENSOR"), "SENSOR  BATT 3.12 V  TEMP -40.0 C");
    LS_CHECK(has_line(n, "FRAMES "));
    LS_CHECK(has_line(n, "SEEN    "));
    for (int i = 0; i < n; i++) LS_CHECK_MSG(strlen(s_out[i]) <= 51, "line %d is %d long", i, (int)strlen(s_out[i]));

    /* Held: the polarity stays while frames come. */
    const int begins = s_begins;
    run_ms(5000);
    LS_EQ_INT(s_begins, begins);

    /* The sonde goes quiet: ten seconds on, both ways are tried again. */
    s_air_on = false;
    run_ms(12500);
    n = read_lines(e);
    LS_CHECK(strstr(s_out[1], "trying") != NULL);
    LS_CHECK(s_begins > begins);

    LS_EQ_INT(run_console("exp dfm17 stop"), 0);
    LS_CHECK(!s_fsk);
}

LS_CASE(radiosonde_without_home_or_gps_says_so)
{
    const ls_experiment_t *e = fresh();
    s_have_home = false;
    LS_EQ_INT(run_console("exp dfm17 start 403.41"), 0);
    run_ms(4000);
    const int n = read_lines(e);
    LS_EQ_STR(line_of(n, "RANGE"), "RANGE   no HOME and no GPS fix");
    LS_EQ_INT(run_console("exp dfm17 stop"), 0);
}

LS_CASE(auto_sweeps_tunes_to_a_carrier_and_sets_aside_one_that_says_nothing)
{
    const ls_experiment_t *e = fresh();
    s_air_hz = 404520000u;
    LS_EQ_INT(run_console("exp dfm17 start auto"), 0);
    LS_EQ_INT(s_scan_begins, 1);
    LS_CHECK(!s_fsk);
    int n = read_lines(e);
    LS_EQ_STR(s_out[0], "scanning 400.000-406.000 MHz");

    /* The first row: the loudest carrier is 401.500, which is no sonde. */
    run_ms(4);
    LS_CHECK(s_fsk);
    LS_EQ_UINT(s_cfg.freq_hz, 401500000u);
    /* Eight seconds of nothing either way up, then back to the sweep, and
       401.500 is left alone for a while: the sonde's carrier is next. */
    run_ms(8100);
    LS_CHECK(s_scan_begins >= 2);
    run_ms(10);
    LS_CHECK(s_fsk);
    LS_EQ_UINT(s_cfg.freq_hz, 404520000u);
    run_ms(7000);
    n = read_lines(e);
    for (int i = 0; i < n; i++) ls_note("%s", s_out[i]);
    LS_EQ_STR(s_out[0], "DFM17 FOUND  ID D26002503");
    LS_EQ_STR(s_out[1], "FREQ    404.520 MHz  AUTO  pol + held");
    LS_EQ_STR(line_of(n, "QUIET"), "QUIET   401.500");

    /* It lands: thirty seconds of silence, and the sweep looks again, the
       last of it still on the screen. */
    s_air_on = false;
    run_ms(30100);
    n = read_lines(e);
    LS_EQ_STR(s_out[0], "DFM17 LOST  ID D26002503");
    LS_CHECK(has_line(n, "SCAN    row"));
    LS_EQ_INT(run_console("exp dfm17 stop"), 0);
    LS_CHECK(!s_fsk && !s_scan);
}
