/* LS_TEST_SOURCES: ${APP}/adsb/adsb_source.c ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c */
/* Which receiver ADS-B uses, and the loop that feeds the decoder from the
   LoRa socket's Mode S session. The chip is a fake here: this proves the host
   side (selection, gain mapping, the hand-off into adsb_on_chips and the
   aircraft table), not anything about RF. */

#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_source.h"
#include "adsb_state.h"
#include "ls_lora.h"
#include "mode-s.h"

#include "audio_events.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "perf.h"
#include "plane_audio.h"

#include <math.h>
#include <string.h>

uint32_t mode_s_checksum(unsigned char *msg, int bits);

/* ---------------------------------------------- what adsb_decode.c reaches -- */

static int s_bursts;

void event_bus_publish_contact(evt_kind_t kind, const char *app,
                               const evt_contact_t *c)
{
    (void)kind; (void)app; (void)c;
}

void audio_events_publish(audio_evt_kind_t kind, uint32_t icao,
                          const char *callsign, bool crc_shaky)
{
    (void)kind; (void)icao; (void)callsign; (void)crc_shaky;
}

plane_category_t plane_classify(uint32_t icao, const char *callsign)
{
    (void)icao; (void)callsign;
    return PLANE_UNKNOWN;
}

void perf_count_msg_good(void) { }
void perf_count_msg_bad(void) { }
void perf_count_burst(void) { s_bursts++; }
void perf_mark_good_msg(int64_t now_us) { (void)now_us; }
void perf_mark_position(int64_t now_us) { (void)now_us; }
void perf_set_mag(int avg, int peak) { (void)avg; (void)peak; }
void perf_set_active_count(int n) { (void)n; }
int  perf_get_crc_good(void) { return 0; }
int  perf_get_crc_err(void) { return 0; }

/* ---------------------------------------------------------------- frames -- */

/* Published DF17 samples (the mode-s.org 1090 MHz Riddle). */
static const uint8_t F_IDENT[14] = {   /* 4840D6 "KLM1023 " */
    0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98 };
static const uint8_t F_POS_EVEN[14] = { /* 40621D airborne position, 38000 ft */
    0x8D, 0x40, 0x62, 0x1D, 0x58, 0xC3, 0x82, 0xD6, 0x90, 0xC8, 0xAC, 0x28, 0x63, 0xA7 };
static const uint8_t F_POS_ODD[14] = {
    0x8D, 0x40, 0x62, 0x1D, 0x58, 0xC3, 0x86, 0x43, 0x5C, 0xC4, 0x12, 0x69, 0x2A, 0xD6 };

/* PPM at 2 chips per bit, MSB first: 1 -> "10", 0 -> "01". */
static void chips_of(const uint8_t *msg, int bits, uint8_t out[ADSB_MODES_FRAME_BYTES])
{
    memset(out, 0, ADSB_MODES_FRAME_BYTES);
    for (int b = 0; b < bits; b++) {
        const int one = (msg[b / 8] & (0x80 >> (b % 8))) != 0;
        for (int c = 0; c < 2; c++) {
            const int v = (c == 0) ? one : !one;
            const int pos = 2 * b + c;
            if (v) out[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
        }
    }
}

static void set_pair(uint8_t *chips, int k, int pair)
{
    for (int c = 0; c < 2; c++) {
        const int v = (pair >> (1 - c)) & 1;
        const int pos = 2 * k + c;
        if (v) chips[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
        else   chips[pos / 8] &= (uint8_t)~(0x80 >> (pos % 8));
    }
}

static void flip_pair(uint8_t *chips, int k)
{
    const int pair = (chips[(2 * k) / 8] >> (6 - ((2 * k) & 7))) & 3;
    set_pair(chips, k, pair == 2 ? 1 : 2);
}

static const adsb_aircraft_t *tracked(uint32_t icao)
{
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (a && a->active && a->icao == icao) return a;
    }
    return NULL;
}

/* -------------------------------------------------------- a fake chip ----- */

#define FAKE_MAX 16
typedef struct {
    uint8_t frame[FAKE_MAX][ADSB_MODES_FRAME_BYTES];
    float   rssi[FAKE_MAX];
    int     n, next;
    int     fail_after;           /* poll number (0 based) that fails, -1 never */
    int     polls;
    int     begun, ended;
    uint32_t begin_hz;
    int     begin_step, gain_set;
} fake_t;
static fake_t fake;

static void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.fail_after = -1;
    adsb_decode_init();
    adsb_chip_diag_reset();
    s_bursts = 0;
}

static void fake_add(const uint8_t chips[ADSB_MODES_FRAME_BYTES], float rssi_dbm)
{
    memcpy(fake.frame[fake.n], chips, ADSB_MODES_FRAME_BYTES);
    fake.rssi[fake.n] = rssi_dbm;
    fake.n++;
}

static esp_err_t fake_begin(uint32_t hz, int step)
{ fake.begun++; fake.begin_hz = hz; fake.begin_step = step; return ESP_OK; }
static int fake_poll(uint8_t *buf, size_t size, float *rssi)
{
    const int k = fake.polls++;
    if (size < ADSB_MODES_FRAME_BYTES) return -1;
    if (fake.fail_after == k) return -2;
    if (fake.next >= fake.n) return 0;
    memcpy(buf, fake.frame[fake.next], ADSB_MODES_FRAME_BYTES);
    if (rssi) *rssi = fake.rssi[fake.next];
    fake.next++;
    return ADSB_MODES_FRAME_BYTES;
}
static esp_err_t fake_set_gain(int step) { fake.gain_set = step; return ESP_OK; }
static esp_err_t fake_end(void) { fake.ended++; return ESP_OK; }

static const adsb_modes_ops_t FAKE_OPS = {
    .begin = fake_begin, .poll = fake_poll, .set_gain = fake_set_gain, .end = fake_end,
};

/* ------------------------------------------------------------- selection -- */

LS_CASE(unless_the_chip_is_chosen_an_iq_receiver_wins_whatever_the_chip_can_do)
{
    LS_EQ_INT(ADSB_SRC_IQ, adsb_source_choose(false, true, 0));
    LS_EQ_INT(ADSB_SRC_IQ, adsb_source_choose(false, true, LS_LORA_CAP_MODES_RX));
    LS_EQ_INT(ADSB_SRC_IQ, adsb_source_choose(false, true, 0xFFFFFFFFu));
}

LS_CASE(the_chosen_lora_chip_is_taken_over_an_iq_receiver_when_it_has_mode_s)
{
    LS_EQ_INT(ADSB_SRC_LORA, adsb_source_choose(true, true, LS_LORA_CAP_MODES_RX));
    LS_EQ_INT(ADSB_SRC_LORA, adsb_source_choose(true, false, LS_LORA_CAP_MODES_RX));
}

LS_CASE(a_chosen_chip_without_mode_s_falls_back_to_the_iq_receiver)
{
    /* An SX126x chosen for ADS-B: the picker refuses it, and an old choice
       left over from an LR2021 lands here. */
    LS_EQ_INT(ADSB_SRC_IQ, adsb_source_choose(true, true, LS_LORA_CAP_LORA | LS_LORA_CAP_FSK));
    LS_EQ_INT(ADSB_SRC_NONE, adsb_source_choose(true, false, LS_LORA_CAP_LORA | LS_LORA_CAP_FSK));
}

LS_CASE(without_an_iq_receiver_the_lora_chip_is_chosen_when_it_has_the_mode_s_session)
{
    LS_EQ_INT(ADSB_SRC_LORA, adsb_source_choose(false, false, LS_LORA_CAP_MODES_RX));
    LS_EQ_INT(ADSB_SRC_LORA, adsb_source_choose(false, false, LS_LORA_CAP_MODES_RX | LS_LORA_CAP_RSSI_INST));
}

LS_CASE(with_neither_there_is_no_source)
{
    LS_EQ_INT(ADSB_SRC_NONE, adsb_source_choose(false, false, 0));
    LS_EQ_INT(ADSB_SRC_NONE, adsb_source_choose(true, false, 0));
    /* An SX126x, or an LR20xx that lacks the session: other capabilities do not count. */
    LS_EQ_INT(ADSB_SRC_NONE, adsb_source_choose(false, false, LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST));
    LS_EQ_INT(ADSB_SRC_NONE, adsb_source_choose(false, false, LS_LORA_CAP_OOK_RX | LS_LORA_CAP_WIDE_RX_BW));
}

LS_CASE(the_source_names_fit_the_status_line)
{
    LS_EQ_STR("none", adsb_source_name(ADSB_SRC_NONE));
    LS_EQ_STR("iq", adsb_source_name(ADSB_SRC_IQ));
    LS_EQ_STR("lr-chip", adsb_source_name(ADSB_SRC_LORA));
    LS_CHECK(strlen(adsb_source_name(ADSB_SRC_LORA)) < 16);   /* LS_RECEIVER_DIAG_LABEL_MAX */
}

/* ----------------------------------------------------------------- gain --- */

LS_CASE(the_gain_setting_maps_onto_the_chips_thirteen_steps)
{
    LS_EQ_INT(13, adsb_lr_gain_step(0));       /* automatic: the chip AGC loses */
    LS_EQ_INT(13, adsb_lr_gain_step(-5));
    LS_EQ_INT(1, adsb_lr_gain_step(1));        /* the smallest manual setting   */
    LS_EQ_INT(1, adsb_lr_gain_step(38));
    LS_EQ_INT(2, adsb_lr_gain_step(39));
    LS_EQ_INT(7, adsb_lr_gain_step(248));      /* the middle                    */
    LS_EQ_INT(13, adsb_lr_gain_step(495));
    LS_EQ_INT(13, adsb_lr_gain_step(496));     /* the app's default: the most   */
    LS_EQ_INT(13, adsb_lr_gain_step(10000));
    int prev = 1;
    for (int g = 1; g <= 496; g++) {
        const int s = adsb_lr_gain_step(g);
        LS_CHECK_MSG(s >= prev && s >= 0 && s <= 13, "gain %d -> step %d after %d", g, s, prev);
        prev = s;
    }
}

/* ------------------------------------------------------- the receive loop -- */

LS_CASE(a_known_df17_frame_from_the_chip_ends_up_in_the_aircraft_table)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    chips_of(F_IDENT, 112, chips);
    fake_add(chips, -71.2f);

    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(1, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(1, st.frames);
    LS_EQ_UINT(1, st.decoded);
    LS_EQ_UINT(0, st.bad_chips);
    LS_EQ_UINT(0, st.bad_crc);
    LS_EQ_INT(1, s_bursts);                         /* the DIAG burst count still moves */
    LS_EQ_INT(-712, adsb_last_rssi_tenths_dbm());
    LS_EQ_INT(1, adsb_state_active_count());

    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_STR("KLM1023", a->callsign);
        LS_EQ_INT(1, a->msg_count);
        LS_EQ_INT(1, a->good_msg_count);
    }
}

LS_CASE(an_even_and_odd_position_pair_gives_a_position_as_it_does_over_iq)
{
    fake_reset();
    ls_shim_time_set(1000000000LL);
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    chips_of(F_POS_EVEN, 112, chips);
    fake_add(chips, -60.0f);
    chips_of(F_POS_ODD, 112, chips);
    fake_add(chips, NAN);                           /* one batch: only the last has a level */

    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(2, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(2, st.decoded);
    LS_EQ_INT(-600, adsb_last_rssi_tenths_dbm());   /* NAN kept the last real one */

    const adsb_aircraft_t *a = tracked(0x40621D);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_INT(38000, a->altitude);
        LS_CHECK(a->pos_valid);
        const int near_even = fabs(a->lat - 52.25720) < 0.01 && fabs(a->lon - 3.91937) < 0.01;
        const int near_odd  = fabs(a->lat - 52.26578) < 0.01 && fabs(a->lon - 3.93891) < 0.01;
        LS_CHECK_MSG(near_even || near_odd, "position %.5f %.5f", a->lat, a->lon);
    }
}

LS_CASE(an_empty_source_ends_the_step_and_is_counted)
{
    fake_reset();
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(0, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(1, st.empty_polls);
    LS_EQ_UINT(0, st.frames);
    LS_EQ_INT(0, adsb_state_active_count());
}

LS_CASE(one_step_takes_at_most_the_frames_it_was_allowed)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    for (int i = 0; i < 5; i++) { chips_of(F_IDENT, 112, chips); fake_add(chips, -70.0f); }
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(2, adsb_modes_pump(&FAKE_OPS, &st, 2));
    LS_EQ_INT(2, fake.next);
    LS_EQ_INT(3, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(5, st.frames);
    LS_EQ_UINT(1, st.empty_polls);                  /* the poll that ended the second step */
}

LS_CASE(a_source_failure_is_returned_and_counted_after_the_frames_before_it)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    chips_of(F_IDENT, 112, chips);
    fake_add(chips, -70.0f);
    fake.fail_after = 1;
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(-2, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(1, st.frames);
    LS_EQ_UINT(1, st.decoded);
    LS_EQ_UINT(1, st.errors);
    LS_CHECK(tracked(0x4840D6) != NULL);

    LS_EQ_INT(-1, adsb_modes_pump(NULL, &st, 9));
    adsb_modes_ops_t no_poll = FAKE_OPS;
    no_poll.poll = NULL;
    LS_EQ_INT(-1, adsb_modes_pump(&no_poll, &st, 9));
}

LS_CASE(a_frame_with_invalid_chips_or_a_bad_crc_is_counted_and_not_tracked)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];

    chips_of(F_IDENT, 112, chips);
    set_pair(chips, 20, 3);                          /* "11" in the first 56 bits */
    fake_add(chips, -70.0f);

    chips_of(F_IDENT, 112, chips);
    flip_pair(chips, 40); flip_pair(chips, 41);      /* two message bits wrong: past what repair fixes */
    fake_add(chips, -70.0f);

    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(2, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(2, st.frames);
    LS_EQ_UINT(0, st.decoded);
    LS_EQ_UINT(1, st.bad_chips);
    LS_EQ_UINT(1, st.bad_crc);
    LS_EQ_INT(0, adsb_state_active_count());
    LS_CHECK(tracked(0x4840D6) == NULL);
}

LS_CASE(a_single_flipped_bit_is_repaired_by_the_same_decoder_that_repairs_iq_frames)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];

    /* The decoder repairs a corrected frame only for an aircraft it has
       already heard clean (its phantom gate), exactly as on the IQ path. */
    chips_of(F_IDENT, 112, chips);
    fake_add(chips, -70.0f);
    flip_pair(chips, 50);                            /* one bit, in the identification */
    fake_add(chips, -70.0f);
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(2, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_UINT(2, st.decoded);
    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) LS_EQ_INT(2, a->msg_count);
}

LS_CASE(the_frame_source_has_no_transmit_entry_point)
{
    /* begin, poll, set_gain, end: nothing else. A receiver cannot be handed a
       transmit call it has no slot for. */
    LS_EQ_UINT(4 * sizeof(void *), sizeof(adsb_modes_ops_t));
}

/* ------------------------------------------------------ the chip-frame log -- */

static uint32_t hist_total(const adsb_chip_diag_t *d)
{
    uint32_t t = 0;
    for (int i = 0; i < ADSB_FIRST_BAD_BUCKETS; i++) t += d->first_bad[i];
    return t;
}

LS_CASE(the_chip_log_records_each_frame_with_its_level_and_verdict)
{
    fake_reset();
    uint8_t ok[ADSB_MODES_FRAME_BYTES], bad_pair[ADSB_MODES_FRAME_BYTES];
    uint8_t bad_crc[ADSB_MODES_FRAME_BYTES], tail[ADSB_MODES_FRAME_BYTES];
    chips_of(F_IDENT, 112, ok);
    chips_of(F_IDENT, 112, bad_pair); set_pair(bad_pair, 20, 3);          /* "11" in the first 56 bits */
    chips_of(F_IDENT, 112, bad_crc);  flip_pair(bad_crc, 40); flip_pair(bad_crc, 41);
    chips_of(F_IDENT, 112, tail);                                         /* two "00"/"11" pairs beyond bit 56: */
    for (int k = 70; k < 72; k++)                                         /* the CRC judges them, and fails */
        set_pair(tail, k, (F_IDENT[k / 8] >> (7 - k % 8)) & 1 ? 0 : 3);
    fake_add(ok, -71.2f);
    fake_add(bad_pair, -80.0f);
    fake_add(bad_crc, NAN);
    fake_add(tail, -55.5f);

    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(4, adsb_modes_pump(&FAKE_OPS, &st, 9));

    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(4, d.n_recs);
    LS_EQ_UINT(4, d.frames);
    LS_EQ_UINT(1, d.decoded);
    LS_EQ_UINT(1, d.bad_chips);
    LS_EQ_UINT(2, d.bad_crc);
    LS_EQ_UINT(0, d.errors);
    /* The log and the pump's own counters say the same thing. */
    LS_EQ_UINT(st.frames, d.frames);
    LS_EQ_UINT(st.decoded, d.decoded);
    LS_EQ_UINT(st.bad_chips, d.bad_chips);
    LS_EQ_UINT(st.bad_crc, d.bad_crc);

    LS_EQ_UINT(1, d.recs[0].seq);
    LS_EQ_UINT(ADSB_FRAME_OK, d.recs[0].verdict);
    LS_EQ_INT(-1, d.recs[0].first_bad_bit);
    LS_EQ_INT(-712, d.recs[0].rssi_tenths);
    LS_CHECK(memcmp(d.recs[0].chips, ok, ADSB_MODES_FRAME_BYTES) == 0);
    LS_EQ_UINT(ADSB_MODES_FRAME_BYTES, d.recs[0].len);

    LS_EQ_UINT(ADSB_FRAME_BAD_CHIPS, d.recs[1].verdict);
    LS_EQ_INT(20, d.recs[1].first_bad_bit);
    LS_CHECK(memcmp(d.recs[1].chips, bad_pair, ADSB_MODES_FRAME_BYTES) == 0);

    LS_EQ_UINT(ADSB_FRAME_BAD_CRC, d.recs[2].verdict);
    LS_EQ_INT(-1, d.recs[2].first_bad_bit);
    LS_EQ_INT(ADSB_RSSI_NONE, d.recs[2].rssi_tenths);      /* NAN: not measured */

    LS_EQ_UINT(ADSB_FRAME_BAD_CRC, d.recs[3].verdict);     /* tail pairs are not bad_chips */
    LS_EQ_INT(70, d.recs[3].first_bad_bit);
    LS_EQ_INT(-555, d.recs[3].rssi_tenths);

    /* Two frames had no invalid pair; one fell in bits 16-23 and one in 64-71. */
    LS_EQ_UINT(2, d.clean);
    LS_EQ_UINT(1, d.first_bad[2]);
    LS_EQ_UINT(1, d.first_bad[8]);
    LS_EQ_UINT(4, hist_total(&d) + d.clean);
}

LS_CASE(the_chip_log_keeps_the_last_eight_frames_oldest_first)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    uint8_t seen[11][ADSB_MODES_FRAME_BYTES];
    for (int i = 0; i < 11; i++) {
        chips_of(F_IDENT, 112, chips);
        chips[27] = (uint8_t)(0x10 + i);               /* tell the frames apart; the tail is noise */
        memcpy(seen[i], chips, sizeof(chips));
        fake_add(chips, -70.0f - (float)i);
    }
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(9, adsb_modes_pump(&FAKE_OPS, &st, 9));
    LS_EQ_INT(2, adsb_modes_pump(&FAKE_OPS, &st, 9));

    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(11, d.frames);
    LS_EQ_UINT(ADSB_CHIP_LOG_N, d.n_recs);
    for (int i = 0; i < ADSB_CHIP_LOG_N; i++) {
        const int frame = 3 + i;                      /* frames 0..2 fell off the front */
        LS_EQ_UINT((uint32_t)frame + 1, d.recs[i].seq);
        LS_CHECK_MSG(memcmp(d.recs[i].chips, seen[frame], ADSB_MODES_FRAME_BYTES) == 0,
                     "entry %d is not frame %d", i, frame);
        LS_EQ_INT(-(700 + 10 * frame), d.recs[i].rssi_tenths);
    }

    /* Fewer than eight: only what there is. */
    adsb_chip_diag_reset();
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(0, d.n_recs);
    LS_EQ_UINT(0, d.frames);
    fake.next = 0;
    LS_EQ_INT(3, adsb_modes_pump(&FAKE_OPS, &st, 3));
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(3, d.n_recs);
    LS_EQ_UINT(1, d.recs[0].seq);
    LS_CHECK(memcmp(d.recs[0].chips, seen[0], ADSB_MODES_FRAME_BYTES) == 0);
    LS_CHECK(memcmp(d.recs[2].chips, seen[2], ADSB_MODES_FRAME_BYTES) == 0);
}

LS_CASE(the_ring_wraps_cleanly_many_times_over)
{
    adsb_chip_diag_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    for (int i = 0; i < 1000; i++) {
        memset(chips, (uint8_t)i, sizeof(chips));
        adsb_chip_diag_record(chips, sizeof(chips), -100 - i % 7, ADSB_FRAME_BAD_CHIPS, i % 112);
    }
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(1000, d.frames);
    LS_EQ_UINT(ADSB_CHIP_LOG_N, d.n_recs);
    for (int k = 0; k < ADSB_CHIP_LOG_N; k++) {
        const int i = 1000 - ADSB_CHIP_LOG_N + k;
        LS_EQ_UINT((uint32_t)i + 1, d.recs[k].seq);
        LS_EQ_UINT((uint8_t)i, d.recs[k].chips[0]);
        LS_EQ_UINT((uint8_t)i, d.recs[k].chips[27]);
        LS_EQ_INT(i % 112, d.recs[k].first_bad_bit);
    }
}

LS_CASE(the_first_bad_bit_histogram_buckets_by_eight_bits_up_to_a_hundred_and_twelve)
{
    adsb_chip_diag_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES] = { 0 };
    static const struct { int bit, bucket; } want[] = {
        { 0, 0 }, { 7, 0 }, { 8, 1 }, { 15, 1 }, { 16, 2 }, { 55, 6 }, { 56, 7 },
        { 63, 7 }, { 64, 8 }, { 103, 12 }, { 104, 13 }, { 111, 13 },
        { 112, 13 }, { 500, 13 },                         /* nothing beyond the last bucket is lost */
    };
    uint32_t expect[ADSB_FIRST_BAD_BUCKETS] = { 0 };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        adsb_chip_diag_record(chips, sizeof(chips), -900, ADSB_FRAME_BAD_CHIPS, want[i].bit);
        expect[want[i].bucket]++;
    }
    adsb_chip_diag_record(chips, sizeof(chips), -900, ADSB_FRAME_OK, -1);     /* clean: not in the histogram */
    adsb_chip_diag_record(chips, sizeof(chips), -900, ADSB_FRAME_BAD_CRC, -1);

    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    for (int b = 0; b < ADSB_FIRST_BAD_BUCKETS; b++)
        LS_CHECK_MSG(d.first_bad[b] == expect[b], "bucket %d: got %lu want %lu", b,
                     (unsigned long)d.first_bad[b], (unsigned long)expect[b]);
    LS_EQ_UINT(2, d.clean);
    LS_EQ_UINT(sizeof(want) / sizeof(want[0]) + 2, d.frames);
    LS_EQ_UINT(d.frames, hist_total(&d) + d.clean);
    LS_EQ_INT(14, ADSB_FIRST_BAD_BUCKETS);                 /* 0-7 ... 104-111 */
}

LS_CASE(a_source_failure_is_counted_in_the_log_and_reset_clears_everything)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    chips_of(F_IDENT, 112, chips);
    fake_add(chips, -70.0f);
    fake.fail_after = 1;
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(-2, adsb_modes_pump(&FAKE_OPS, &st, 9));
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(1, d.errors);
    LS_EQ_UINT(1, d.frames);

    adsb_chip_diag_reset();
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(0, d.errors);
    LS_EQ_UINT(0, d.frames);
    LS_EQ_UINT(0, d.n_recs);
    LS_EQ_UINT(0, hist_total(&d) + d.clean);
}

LS_CASE(the_log_is_a_fixed_ring_with_no_allocation_and_a_bounded_text_report)
{
    /* The record sits in static storage; what the pump hands it is copied, and
       what it hands back is a copy. Sizes are the contract: a frame is the
       chip's 28 bytes and nothing grows with the rate. */
    LS_EQ_INT(8, ADSB_CHIP_LOG_N);
    LS_CHECK(sizeof(adsb_chip_rec_t) <= 48);
    LS_CHECK(sizeof(adsb_chip_diag_t) <= 640);

    adsb_chip_diag_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    chips_of(F_IDENT, 112, chips);
    adsb_chip_diag_record(chips, sizeof(chips), -712, ADSB_FRAME_OK, -1);
    adsb_chip_diag_record(chips, sizeof(chips), ADSB_RSSI_NONE, ADSB_FRAME_BAD_CHIPS, 12);
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);

    char line[200];
    adsb_chip_diag_format_rec(&d.recs[0], line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "#1 ") == line, "%s", line);
    LS_CHECK_MSG(strstr(line, "-71.2 dBm ok ") != NULL, "%s", line);
    char hex[64] = "";
    for (int i = 0; i < ADSB_MODES_FRAME_BYTES; i++) snprintf(hex + 2 * i, 3, "%02X", chips[i]);
    LS_CHECK_MSG(strstr(line, hex) != NULL, "%s", line);
    LS_EQ_INT(56, (int)strlen(strstr(line, "ok ") + 3));     /* all 28 bytes, hex */

    adsb_chip_diag_format_rec(&d.recs[1], line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "n/a dBm bad_chips@12 ") != NULL, "%s", line);

    adsb_chip_diag_format_counts(&d, line, sizeof(line));
    LS_CHECK_MSG(strcmp(line, "frames=2 decoded=1 bad_chips=1 bad_crc=0 errors=0 clean_pairs=1") == 0, "%s", line);

    adsb_chip_diag_format_hist(&d, line, sizeof(line));
    LS_CHECK_MSG(strncmp(line, "0-7:0 8-15:1 16-23:0 ", 21) == 0, "%s", line);
    LS_CHECK_MSG(strstr(line, " 104-111:0") != NULL, "%s", line);

    /* A buffer too small is cut, never overrun. */
    char tiny[12];
    memset(tiny, 'x', sizeof(tiny));
    adsb_chip_diag_format_rec(&d.recs[0], tiny, 8);
    LS_EQ_INT('x', tiny[8]);
    LS_CHECK(strlen(tiny) < 8);
}

LS_CASE(counts_stay_consistent_whatever_the_mix_of_frames)
{
    fake_reset();
    uint8_t chips[ADSB_MODES_FRAME_BYTES];
    for (int i = 0; i < 9; i++) {
        chips_of(F_IDENT, 112, chips);
        if (i % 3 == 1) set_pair(chips, 5 + i, 3);
        if (i % 3 == 2) { flip_pair(chips, 40); flip_pair(chips, 41); }
        fake_add(chips, -60.0f);
    }
    adsb_modes_stats_t st = { 0 };
    LS_EQ_INT(9, adsb_modes_pump(&FAKE_OPS, &st, 9));
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(9, d.frames);
    LS_EQ_UINT(d.frames, d.decoded + d.bad_chips + d.bad_crc);
    LS_EQ_UINT(d.frames, hist_total(&d) + d.clean);
    LS_EQ_UINT(3, d.decoded);
    LS_EQ_UINT(3, d.bad_chips);
    LS_EQ_UINT(3, d.bad_crc);
}

/* ---------------------------------------------------------------- rates --- */

static void record_n(int n, int verdict, int64_t at_us)
{
    uint8_t chips[ADSB_MODES_FRAME_BYTES] = { 0 };
    ls_shim_time_set(at_us);
    for (int i = 0; i < n; i++)
        adsb_chip_diag_record(chips, sizeof(chips), -900, verdict, verdict == ADSB_FRAME_BAD_CHIPS ? 3 : -1);
}

LS_CASE(the_trigger_and_decode_rates_come_from_the_completed_seconds)
{
    ls_shim_time_set(100000000LL);                       /* the session starts at t = 100 s */
    adsb_chip_diag_reset();
    record_n(1, ADSB_FRAME_OK, 101200000LL);             /* second 101: four frames, one decoded */
    record_n(3, ADSB_FRAME_BAD_CHIPS, 101700000LL);
    record_n(2, ADSB_FRAME_BAD_CHIPS, 102500000LL);      /* second 102: two frames */
    ls_shim_time_set(104000000LL);                       /* seconds 101, 102, 103 are complete */

    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_INT(3, d.rate_window_s);
    LS_NEAR(d.trigger_per_s, 2.0, 0.001);                /* 6 frames in 3 s */
    LS_NEAR(d.decoded_per_s, 1.0 / 3.0, 0.001);
    LS_EQ_UINT(4, d.elapsed_s);
    LS_NEAR(d.avg_trigger_per_s, 1.5, 0.001);            /* 6 frames in 4 s since the start */
    LS_NEAR(d.avg_decoded_per_s, 0.25, 0.001);

    char line[200];
    adsb_chip_diag_format_rates(&d, line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "triggers 2.0/s, decoded 0.3/s (last 3 s)") != NULL, "%s", line);
    LS_CHECK_MSG(strstr(line, "since start 1.5/s and 0.2/s over 4 s") != NULL ||
                 strstr(line, "since start 1.5/s and 0.3/s over 4 s") != NULL, "%s", line);
}

LS_CASE(there_is_no_rate_before_a_whole_second_has_been_counted)
{
    ls_shim_time_set(100000000LL);
    adsb_chip_diag_reset();
    record_n(5, ADSB_FRAME_BAD_CHIPS, 100400000LL);
    adsb_chip_diag_t d;
    ls_shim_time_set(100900000LL);
    adsb_chip_diag_snapshot(&d);
    LS_EQ_INT(0, d.rate_window_s);                       /* the session's own second is not complete */
    LS_NEAR(d.trigger_per_s, 0.0, 0.0001);
    ls_shim_time_set(101500000LL);
    adsb_chip_diag_snapshot(&d);
    LS_EQ_INT(0, d.rate_window_s);                       /* second 101 is still running */
    char line[100];
    adsb_chip_diag_format_rates(&d, line, sizeof(line));
    LS_CHECK_MSG(strstr(line, "n/a") != NULL, "%s", line);

    ls_shim_time_set(102000000LL);
    adsb_chip_diag_snapshot(&d);
    LS_EQ_INT(1, d.rate_window_s);                       /* second 101 has ended: it had nothing */
    LS_NEAR(d.trigger_per_s, 0.0, 0.0001);
    LS_EQ_UINT(5, d.frames);                             /* the counters are not windowed */
}

LS_CASE(the_rate_window_is_ten_seconds_and_old_seconds_are_not_read_as_new_ones)
{
    ls_shim_time_set(0);
    adsb_chip_diag_reset();
    record_n(3, ADSB_FRAME_BAD_CHIPS, 5500000LL);        /* second 5 */
    record_n(1, ADSB_FRAME_OK, 21500000LL);              /* second 21 shares second 5's bin */
    ls_shim_time_set(23000000LL);
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_INT(ADSB_RATE_WINDOW_S, d.rate_window_s);      /* seconds 13..22 */
    LS_NEAR(d.trigger_per_s, 0.1, 0.0001);               /* only the one frame of second 21 */
    LS_NEAR(d.decoded_per_s, 0.1, 0.0001);
    LS_EQ_UINT(4, d.frames);
    LS_NEAR(d.avg_trigger_per_s, 4.0 / 23.0, 0.001);

    ls_shim_time_set(60000000LL);                        /* a quiet minute later: nothing in the window */
    adsb_chip_diag_snapshot(&d);
    LS_EQ_INT(ADSB_RATE_WINDOW_S, d.rate_window_s);
    LS_NEAR(d.trigger_per_s, 0.0, 0.0001);
}

LS_CASE(marking_the_session_start_restarts_the_clock_and_keeps_the_counts)
{
    ls_shim_time_set(10000000LL);
    adsb_chip_diag_reset();
    record_n(2, ADSB_FRAME_BAD_CHIPS, 10500000LL);
    ls_shim_time_set(50000000LL);
    adsb_chip_diag_mark_start();                         /* the chip session began a while after */
    record_n(4, ADSB_FRAME_BAD_CHIPS, 51500000LL);
    ls_shim_time_set(54000000LL);
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    LS_EQ_UINT(6, d.frames);
    LS_EQ_INT(3, d.rate_window_s);                       /* 51, 52, 53 */
    LS_NEAR(d.trigger_per_s, 4.0 / 3.0, 0.001);
    LS_EQ_UINT(4, d.elapsed_s);
}
