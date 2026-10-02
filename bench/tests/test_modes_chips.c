/* LS_TEST_SOURCES: ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c */
/* Mode S frames that arrive as OOK chips (the LR2021 after its 0x0285
   preamble) reach the same decoder, CRC repair and aircraft table as the RTL
   magnitude path. Host-only: the MSB-first chip packing is an assumption the
   silicon has not confirmed. */

#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_state.h"
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

/* Published DF17 samples (ADS-B decoding references). */
static const uint8_t F_IDENT[14] = {   /* 4840D6 "KLM1023 " */
    0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98 };
static const uint8_t F_POS_EVEN[14] = { /* 40621D airborne position, 38000 ft */
    0x8D, 0x40, 0x62, 0x1D, 0x58, 0xC3, 0x82, 0xD6, 0x90, 0xC8, 0xAC, 0x28, 0x63, 0xA7 };
static const uint8_t F_POS_ODD[14] = {
    0x8D, 0x40, 0x62, 0x1D, 0x58, 0xC3, 0x86, 0x43, 0x5C, 0xC4, 0x12, 0x69, 0x2A, 0xD6 };
static const uint8_t F_VEL[14] = {     /* 485020 airborne velocity */
    0x8D, 0x48, 0x50, 0x20, 0x99, 0x44, 0x09, 0x94, 0x08, 0x38, 0x17, 0x5B, 0x28, 0x4F };

static void seal(uint8_t *m, int bits, uint32_t overlay)
{
    const int n = bits / 8;
    m[n - 3] = m[n - 2] = m[n - 1] = 0;
    const uint32_t crc = mode_s_checksum(m, bits) ^ overlay;
    m[n - 3] = (uint8_t)(crc >> 16);
    m[n - 2] = (uint8_t)(crc >> 8);
    m[n - 1] = (uint8_t)crc;
}

/* DF11 all-call reply: parity is the bare CRC. */
static void df11(uint8_t m[14], uint32_t aa)
{
    memset(m, 0, 14);
    m[0] = (11 << 3) | 5;
    m[1] = (uint8_t)(aa >> 16);
    m[2] = (uint8_t)(aa >> 8);
    m[3] = (uint8_t)aa;
    seal(m, 56, 0);
}

/* DF4 altitude reply: 7 bytes whose parity is CRC xor the address. */
static void df4(uint8_t m[14], uint32_t aa)
{
    memset(m, 0, 14);
    m[0] = (4 << 3);
    m[2] = 0x19; m[3] = 0x38;           /* some altitude code */
    seal(m, 56, aa);
}

/* PPM at 2 chips per bit, MSB first: 1 -> "10", 0 -> "01". Bits [first, bits)
   of msg become the chip stream; the rest of `n` bytes is filled with `fill`
   (what the chip keeps clocking after a short frame). Returns bytes used. */
static void chips_of(const uint8_t *msg, int first, int bits,
                     uint8_t *out, int n, uint8_t fill)
{
    memset(out, fill, n);
    for (int b = first; b < bits; b++) {
        const int k = b - first;            /* pair index */
        const int one = (msg[b / 8] & (0x80 >> (b % 8))) != 0;
        const int chip0 = 2 * k;
        for (int c = 0; c < 2; c++) {
            const int v = (c == 0) ? one : !one;
            const int pos = chip0 + c;
            if (pos / 8 >= n) return;
            if (v) out[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
            else   out[pos / 8] &= (uint8_t)~(0x80 >> (pos % 8));
        }
    }
}

/* Overwrite pair k of a chip buffer with raw chips (2 bits, e.g. 3 = "11"). */
static void set_pair(uint8_t *chips, int k, int pair)
{
    const int pos = 2 * k;
    for (int c = 0; c < 2; c++) {
        const int v = (pair >> (1 - c)) & 1;
        if (v) chips[(pos + c) / 8] |= (uint8_t)(0x80 >> ((pos + c) % 8));
        else   chips[(pos + c) / 8] &= (uint8_t)~(0x80 >> ((pos + c) % 8));
    }
}

static void flip_pair(uint8_t *chips, int k)   /* a one-bit message error */
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

/* ------------------------------------------------------- chips to message -- */

LS_CASE(chips_of_real_df17_frames_give_back_the_frame)
{
    const uint8_t *frames[] = { F_IDENT, F_POS_EVEN, F_POS_ODD, F_VEL };
    for (int i = 0; i < 4; i++) {
        uint8_t chips[MODE_S_CHIP_BYTES_LONG], msg[14];
        mode_s_chip_info_t info;
        chips_of(frames[i], 0, 112, chips, sizeof(chips), 0);
        LS_EQ_INT(112, mode_s_msg_from_chips(chips, sizeof(chips), 0, 0, msg, &info));
        LS_CHECK(memcmp(msg, frames[i], 14) == 0);
        LS_EQ_INT(0, info.bad_pairs);
        LS_EQ_INT(0, info.bad_pairs_tail);
        LS_EQ_INT(-1, info.first_bad_bit);
        /* and the samples themselves are real: the checksum agrees */
        LS_EQ_UINT(((uint32_t)frames[i][11] << 16) | (frames[i][12] << 8) | frames[i][13],
                   mode_s_checksum((uint8_t *)frames[i], 112));
    }
}

LS_CASE(the_known_ident_frame_packs_to_the_expected_chip_bytes)
{
    /* 8D = 1000 1101 -> 10 01 01 01 | 10 10 01 10  =  0x95 0xA6 ; pins the
       order independent of the helper that built it */
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_UINT(0x95, chips[0]);
    LS_EQ_UINT(0xA6, chips[1]);
}

LS_CASE(short_frames_come_back_56_bits_and_ignore_the_noise_after)
{
    uint8_t f11[14], f4[14], chips[MODE_S_CHIP_BYTES_LONG], msg[14];
    df11(f11, 0xAE1234u);
    df4(f4, 0xAE1234u);

    /* exactly 14 chip bytes */
    chips_of(f11, 0, 56, chips, MODE_S_CHIP_BYTES_SHORT, 0);
    LS_EQ_INT(56, mode_s_msg_from_chips(chips, MODE_S_CHIP_BYTES_SHORT, 0, 0, msg, NULL));
    LS_CHECK(memcmp(msg, f11, 7) == 0);

    /* 28 chip bytes, the second half junk, including invalid pairs */
    const uint8_t junk[] = { 0x00, 0xFF, 0x33, 0xCC, 0x55, 0xAA };
    for (unsigned j = 0; j < sizeof(junk); j++) {
        mode_s_chip_info_t info;
        chips_of(f4, 0, 56, chips, sizeof(chips), junk[j]);
        LS_EQ_INT(56, mode_s_msg_from_chips(chips, sizeof(chips), 0, 0, msg, &info));
        LS_CHECK(memcmp(msg, f4, 7) == 0);
        for (int i = 7; i < 14; i++) LS_EQ_UINT(0, msg[i]);
        LS_EQ_INT(0, info.bad_pairs);
        LS_EQ_INT(0, info.bad_pairs_tail);   /* not counted past the frame */
    }
}

LS_CASE(too_few_chips_for_the_frame_the_df_names_is_refused)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG], msg[14];

    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, MODE_S_CHIP_BYTES_LONG - 1, 0, 0, msg, NULL));
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, MODE_S_CHIP_BYTES_SHORT, 0, 0, msg, NULL));

    uint8_t f11[14];
    df11(f11, 0x123456u);
    chips_of(f11, 0, 56, chips, sizeof(chips), 0);
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, MODE_S_CHIP_BYTES_SHORT - 1, 0, 0, msg, NULL));
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, 1, 0, 0, msg, NULL));
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, 0, 0, 0, msg, NULL));
    LS_EQ_INT(-1, mode_s_msg_from_chips(NULL, 28, 0, 0, msg, NULL));
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, 28, 0, 0, NULL, NULL));
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, 28, 9, 0, msg, NULL));
}

LS_CASE(invalid_pairs_are_counted_where_they_fall)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG], msg[14];
    mode_s_chip_info_t info;

    /* one in the first 56 bits: 00, then 11 */
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    set_pair(chips, 20, 0);
    LS_EQ_INT(112, mode_s_msg_from_chips(chips, sizeof(chips), 0, 0, msg, &info));
    LS_EQ_INT(1, info.bad_pairs);
    LS_EQ_INT(0, info.bad_pairs_tail);
    LS_EQ_INT(20, info.first_bad_bit);
    LS_CHECK(info.bad[20 / 8] & (0x80 >> (20 % 8)));

    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    set_pair(chips, 20, 3);
    set_pair(chips, 55, 3);
    set_pair(chips, 56, 0);
    set_pair(chips, 111, 3);
    LS_EQ_INT(112, mode_s_msg_from_chips(chips, sizeof(chips), 0, 0, msg, &info));
    LS_EQ_INT(2, info.bad_pairs);
    LS_EQ_INT(2, info.bad_pairs_tail);
    LS_EQ_INT(20, info.first_bad_bit);
}

LS_CASE(the_df17_head_variant_prepends_the_four_bits_the_detector_ate)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG], msg[14];
    mode_s_chip_info_t info;

    /* chips start at message bit 4 */
    chips_of(F_IDENT, 4, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(112, mode_s_msg_from_chips(chips, 27, 4, 0x8, msg, &info));
    LS_CHECK(memcmp(msg, F_IDENT, 14) == 0);
    LS_EQ_INT(0, info.bad_pairs);

    /* 27 bytes is all a head-4 long frame needs; 26 is not */
    LS_EQ_INT(-1, mode_s_msg_from_chips(chips, 26, 4, 0x8, msg, NULL));

    /* a bad pair index is relative to the message, not to the chip stream */
    set_pair(chips, 10, 3);                 /* message bit 14 */
    LS_EQ_INT(112, mode_s_msg_from_chips(chips, 27, 4, 0x8, msg, &info));
    LS_EQ_INT(14, info.first_bad_bit);
}

/* ------------------------------------------------------------ end to end -- */

LS_CASE(an_ident_frame_from_chips_names_the_aircraft)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];
    adsb_decode_init();
    s_bursts = 0;
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);

    LS_EQ_INT(ADSB_RSSI_NONE, adsb_last_rssi_tenths_dbm());
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -712));
    LS_EQ_INT(1, s_bursts);
    LS_EQ_INT(-712, adsb_last_rssi_tenths_dbm());
    LS_EQ_INT(1, adsb_state_active_count());

    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_STR("KLM1023", a->callsign);
        LS_EQ_INT(1, a->msg_count);
        LS_EQ_INT(1, a->mt_df17_id);
    }

    /* No signal level given: the last one stays */
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, ADSB_RSSI_NONE));
    LS_EQ_INT(-712, adsb_last_rssi_tenths_dbm());
    a = tracked(0x4840D6);
    if (a) LS_EQ_INT(2, a->msg_count);
}

LS_CASE(the_df17_head_variant_decodes_end_to_end)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];
    adsb_decode_init();
    chips_of(F_IDENT, 4, 112, chips, 27, 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, 27, ADSB_CHIPS_DF17_HEAD, -800));
    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) LS_EQ_STR("KLM1023", a->callsign);
    LS_EQ_INT(ADSB_FRAME_BAD_CHIPS, adsb_on_chips(chips, 27, 7, -800));
}

LS_CASE(position_and_altitude_arrive_by_chips_as_by_iq)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];
    adsb_decode_init();
    ls_shim_time_set(1000000000LL);
    chips_of(F_POS_EVEN, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -600));
    ls_shim_time_advance(1000000LL);
    chips_of(F_POS_ODD, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -600));

    const adsb_aircraft_t *a = tracked(0x40621D);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_INT(38000, a->altitude);
        LS_CHECK(a->pos_valid);
        LS_EQ_INT(2, a->mt_df17_pos);
        /* the published even/odd pair, whichever frame is the newer */
        const int near_even = fabs(a->lat - 52.25720) < 0.01 && fabs(a->lon - 3.91937) < 0.01;
        const int near_odd  = fabs(a->lat - 52.26578) < 0.01 && fabs(a->lon - 3.93891) < 0.01;
        LS_CHECK_MSG(near_even || near_odd, "position %.5f %.5f", a->lat, a->lon);
    }
}

LS_CASE(velocity_and_df11_and_df4_by_chips)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG], f[14];
    adsb_decode_init();

    chips_of(F_VEL, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -500));
    const adsb_aircraft_t *a = tracked(0x485020);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_INT(1, a->mt_df17_vel);
        LS_CHECK(a->velocity > 0);
    }

    /* DF11: 56 chips, from a bare 14-byte read */
    df11(f, 0xAE1234u);
    chips_of(f, 0, 56, chips, MODE_S_CHIP_BYTES_SHORT, 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, MODE_S_CHIP_BYTES_SHORT, ADSB_CHIPS_FULL, -900));
    a = tracked(0xAE1234);
    LS_CHECK(a != NULL);
    if (a) LS_EQ_INT(1, a->mt_df11);

    /* DF4: AP field, recovered through the address just heard */
    df4(f, 0xAE1234u);
    chips_of(f, 0, 56, chips, sizeof(chips), 0x33);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -900));
    a = tracked(0xAE1234);
    if (a) LS_EQ_INT(1, a->mt_surv);

    /* DF4 from an address never heard: no aircraft */
    df4(f, 0x654321u);
    chips_of(f, 0, 56, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_BAD_CRC, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -900));
    LS_CHECK(tracked(0x654321) == NULL);
}

LS_CASE(an_invalid_pair_in_the_first_56_bits_is_refused_and_nothing_is_tracked)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];
    for (int pair = 0; pair <= 3; pair += 3) {       /* 00 and 11 */
        for (int k = 5; k < 56; k += 7) {
            adsb_decode_init();
            chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
            set_pair(chips, k, pair);
            LS_EQ_INT(ADSB_FRAME_BAD_CHIPS,
                      adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
            LS_EQ_INT(0, adsb_state_active_count());
        }
    }
    /* the same frame undamaged is accepted, so the refusals were the pairs */
    adsb_decode_init();
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
}

LS_CASE(an_invalid_pair_in_the_tail_is_left_to_the_crc)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];

    /* "11" where the bit is a 1 ("10"): the first chip still says 1, so the
       bit survives and the CRC passes */
    adsb_decode_init();
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    int k1 = -1, k0 = -1;
    for (int k = 60; k < 100 && (k1 < 0 || k0 < 0); k++) {
        const int one = (F_IDENT[k / 8] & (0x80 >> (k % 8))) != 0;
        if (one && k1 < 0) k1 = k;
        if (!one && k0 < 0) k0 = k;
    }
    set_pair(chips, k1, 3);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));

    /* "11" where the bit is a 0: a wrong guess, one bit off. Never heard
       clean before, so the repaired frame is refused like the RTL path's */
    adsb_decode_init();
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    set_pair(chips, k0, 3);
    LS_EQ_INT(ADSB_FRAME_BAD_CRC, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
    LS_EQ_INT(0, adsb_state_active_count());

    /* once the sender has been heard clean, the same damage is repaired */
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
    set_pair(chips, k0, 3);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) LS_EQ_INT(2, a->msg_count);
}

LS_CASE(a_one_bit_error_is_repaired_by_crc_like_the_rtl_path)
{
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];

    /* cold cache: refused, no phantom */
    int accepted = 0;
    for (int bit = 5; bit < 112; bit++) {
        adsb_decode_init();
        chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
        flip_pair(chips, bit);
        if (adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700) == ADSB_FRAME_OK)
            accepted++;
        LS_EQ_INT(0, adsb_state_active_count());
    }
    LS_EQ_INT(0, accepted);

    /* heard clean once: every one-bit error after the DF field is repaired */
    adsb_decode_init();
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
    int repaired = 0;
    for (int bit = 5; bit < 112; bit++) {
        chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
        flip_pair(chips, bit);
        if (adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700) == ADSB_FRAME_OK)
            repaired++;
    }
    LS_EQ_INT(107, repaired);
    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_INT(108, a->msg_count);
        LS_EQ_STR("KLM1023", a->callsign);   /* repaired bits did not corrupt it */
    }

    /* two bits off: not repaired (not aggressive) */
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    flip_pair(chips, 30);
    flip_pair(chips, 80);
    LS_EQ_INT(ADSB_FRAME_BAD_CRC, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
}

/* ------------------------------------------------------------- frames in -- */

LS_CASE(adsb_on_frame_takes_7_or_14_byte_frames_only)
{
    uint8_t f[14];
    adsb_decode_init();

    LS_EQ_INT(ADSB_FRAME_BAD_CHIPS, adsb_on_frame(NULL, 14, ADSB_RSSI_NONE));
    LS_EQ_INT(ADSB_FRAME_BAD_CHIPS, adsb_on_frame(F_IDENT, 0, ADSB_RSSI_NONE));
    LS_EQ_INT(ADSB_FRAME_BAD_CHIPS, adsb_on_frame(F_IDENT, 11, ADSB_RSSI_NONE));
    LS_EQ_INT(ADSB_FRAME_BAD_CHIPS, adsb_on_frame(F_IDENT, 15, ADSB_RSSI_NONE));
    /* a DF17 in a 7-byte buffer would read past it */
    LS_EQ_INT(ADSB_FRAME_BAD_CHIPS, adsb_on_frame(F_IDENT, 7, ADSB_RSSI_NONE));
    LS_EQ_INT(0, adsb_state_active_count());

    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_frame(F_IDENT, 14, -655));
    LS_EQ_INT(-655, adsb_last_rssi_tenths_dbm());
    LS_EQ_INT(1, adsb_state_active_count());

    df11(f, 0xAE1234u);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_frame(f, 7, -640));
    LS_EQ_INT(2, adsb_state_active_count());

    f[5] ^= 0x10;                                   /* damaged, unheard sender */
    f[3] ^= 0x01;
    LS_EQ_INT(ADSB_FRAME_BAD_CRC, adsb_on_frame(f, 7, -640));

    adsb_decode_init();
    LS_EQ_INT(ADSB_RSSI_NONE, adsb_last_rssi_tenths_dbm());
}

LS_CASE(the_iq_path_and_the_chip_path_agree_on_the_same_frame)
{
    /* RTL path: PPM at 2 Msps, the preamble as pulses at 0, 1, 3.5, 4.5 us. */
    static uint8_t iq[1024 * 2];
    uint8_t chips[MODE_S_CHIP_BYTES_LONG];
    static const int PREAMBLE[] = { 0, 2, 7, 9 };

    adsb_decode_init();
    memset(iq, 127, sizeof(iq));
    for (int i = 0; i < 4; i++) iq[2 * (100 + PREAMBLE[i])] = 227;
    for (int b = 0; b < 112; b++) {
        const int one = (F_IDENT[b / 8] & (0x80 >> (b % 8))) != 0;
        iq[2 * (100 + 16 + 2 * b + (one ? 0 : 1))] = 227;
    }
    adsb_on_sample(iq, (int)sizeof(iq));
    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    char iq_call[9] = "";
    int iq_count = 0;
    if (a) { memcpy(iq_call, a->callsign, 9); iq_count = a->msg_count; }

    adsb_decode_init();
    chips_of(F_IDENT, 0, 112, chips, sizeof(chips), 0);
    LS_EQ_INT(ADSB_FRAME_OK, adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL, -700));
    a = tracked(0x4840D6);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_STR(iq_call, a->callsign);
        LS_EQ_INT(iq_count, a->msg_count);
        LS_EQ_INT(1, a->good_msg_count);
    }
}
