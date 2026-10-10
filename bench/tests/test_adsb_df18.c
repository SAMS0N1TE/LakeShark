/* LS_TEST_SOURCES: ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c */
/* DF18 (TIS-B, ADS-R, non-transponder extended squitter) is 112 bits and is
   decoded like a DF17 where its control field allows; and the message handed
   to the callback carries nothing from the stack it was made on.

   Vectors:
   - 959c0bf8682907751f029468c24a is a DF18 CF=5 frame as dump1090 printed it
     ("CRC: 000000 (ok) ... Control Field: 5 (TIS-B relay of ADS-B message
     with other address)"), quoted in the FlightAware forum thread
     "Bad Hexcodes" (discussions.flightaware.com/t/bad-hexcodes/16073).
   - The ADS-B vectors are the DF17 frames of the mode-s.org 1090 MHz Riddle
     (identification KLM1023 from 4840D6; the even/odd airborne position pair
     from 40621D, 52.2572 N 3.9194 E at 38000 ft). A DF18 CF=0 frame is the
     same message from a non-transponder transmitter, so they are re-framed
     here as DF18 and re-sealed; the ME field is the published one. */

#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_state.h"
#include "mode-s.h"

#include "audio_events.h"
#include "event_bus.h"
#include "esp_timer.h"
#include "perf.h"
#include "plane_audio.h"

#include <stdio.h>
#include <string.h>

uint32_t mode_s_checksum(unsigned char *msg, int bits);
int mode_s_msg_len_by_type(int type);

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
static int s_bad;
void perf_count_msg_bad(void) { s_bad++; }
void perf_count_burst(void) { }
void perf_mark_good_msg(int64_t now_us) { (void)now_us; }
void perf_mark_position(int64_t now_us) { (void)now_us; }
void perf_set_mag(int avg, int peak) { (void)avg; (void)peak; }
void perf_set_active_count(int n) { (void)n; }
int  perf_get_crc_good(void) { return 0; }
int  perf_get_crc_err(void) { return 0; }

/* ---------------------------------------------------------------- frames -- */

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static void from_hex(uint8_t m[14], const char *hex)
{
    for (int i = 0; i < 14; i++)
        m[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
}

static void seal(uint8_t *m, int bits)
{
    const int n = bits / 8;
    m[n - 3] = m[n - 2] = m[n - 1] = 0;
    const uint32_t crc = mode_s_checksum(m, bits);
    m[n - 3] = (uint8_t)(crc >> 16);
    m[n - 2] = (uint8_t)(crc >> 8);
    m[n - 1] = (uint8_t)crc;
}

/* The DF17 frame `hex`, sent as `df` with `low3` in the three bits after it
   (CF for DF18). */
static void reframe(uint8_t m[14], const char *hex, int df, int low3)
{
    from_hex(m, hex);
    m[0] = (uint8_t)((df << 3) | low3);
    seal(m, 112);
}

#define ID_17    "8D4840D6202CC371C32CE0576098"   /* KLM1023, 4840D6 */
#define EVEN_17  "8D40621D58C382D690C8AC2863A7"   /* 40621D, even */
#define ODD_17   "8D40621D58C386435CC412692AD6"   /* 40621D, odd  */
#define TISB_CF5 "959c0bf8682907751f029468c24a"   /* captured DF18 CF=5 */

#define ADDR_KLM 0x4840D6u
#define ADDR_POS 0x40621Du

static void flip(uint8_t *m, int bit) { m[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8)); }

/* ----------------------------------------------------------------- driver -- */

static mode_s_t s_ms;   /* 32 KB of address cache - not on the stack */

static bool decode(const uint8_t *frame, struct mode_s_msg *out)
{
    struct mode_s_msg mm;
    uint8_t buf[MODE_S_LONG_MSG_BYTES];
    memcpy(buf, frame, sizeof(buf));
    mode_s_decode(&s_ms, &mm, buf);
    if (out) *out = mm;
    return mm.crcok != 0;
}

/* PPM at 2 MSPS, as test_adsb_phantom does it. */
#define IQ_SAMPLES 1024
#define LEAD       100
#define PULSE      227

static uint8_t s_iq[IQ_SAMPLES * 2];

static void build_iq(const uint8_t *frame, int bits)
{
    static const int PREAMBLE[] = { 0, 2, 7, 9 };
    memset(s_iq, 127, sizeof(s_iq));
    for (int i = 0; i < 4; i++) s_iq[2 * (LEAD + PREAMBLE[i])] = PULSE;
    for (int b = 0; b < bits; b++) {
        const bool one = (frame[b / 8] & (0x80 >> (b % 8))) != 0;
        s_iq[2 * (LEAD + 16 + 2 * b + (one ? 0 : 1))] = PULSE;
    }
}

static void on_air(const uint8_t *frame, int bits)
{
    build_iq(frame, bits);
    adsb_on_sample(s_iq, (int)sizeof(s_iq));
}

static const adsb_aircraft_t *tracked(uint32_t icao)
{
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (a && a->active && a->icao == icao) return a;
    }
    return NULL;
}

/* ----------------------------------------------------------------- cases -- */

LS_CASE(downlink_formats_16_and_up_are_112_bits)
{
    for (int df = 0; df < 16; df++)
        LS_EQ_INT(56, mode_s_msg_len_by_type(df));
    for (int df = 16; df < 32; df++) {
        LS_CHECK_MSG(mode_s_msg_len_by_type(df) == 112,
                     "DF%d is %d bits, not 112", df, mode_s_msg_len_by_type(df));
    }
}

LS_CASE(the_vectors_are_what_they_claim_to_be)
{
    /* So a failure below is the decoder's and not a mistyped constant. */
    uint8_t m[14];
    const char *plain[] = { ID_17, EVEN_17, ODD_17, TISB_CF5 };
    for (int i = 0; i < 4; i++) {
        from_hex(m, plain[i]);
        LS_CHECK_MSG(mode_s_checksum(m, 112) ==
                     (((uint32_t)m[11] << 16) | ((uint32_t)m[12] << 8) | m[13]),
                     "vector %d fails its own CRC", i);
    }
}

LS_CASE(a_captured_df18_frame_passes_its_crc)
{
    uint8_t m[14];
    from_hex(m, TISB_CF5);
    mode_s_init(&s_ms);

    struct mode_s_msg mm;
    LS_CHECK_MSG(decode(m, &mm),
                 "the captured DF18 frame was rejected (msgbits %d)", mm.msgbits);
    LS_EQ_INT(18, mm.msgtype);
    LS_EQ_INT(112, mm.msgbits);
    LS_EQ_INT(5, mm.cf);
}

LS_CASE(a_non_icao_df18_is_never_an_icao_aircraft)
{
    /* CF=5 carries an address that is not an ICAO one. The frame is good; it
       is not an aircraft in a table keyed on ICAO addresses, and it is not
       evidence that the address is one. */
    uint8_t m[14];
    from_hex(m, TISB_CF5);
    mode_s_init(&s_ms);

    struct mode_s_msg mm;
    LS_CHECK(decode(m, &mm));
    LS_CHECK(mm.addr_nonicao);
    LS_CHECK(mm.rebroadcast);

    adsb_decode_init();
    on_air(m, 112);
    LS_EQ_INT(0, adsb_state_active_count());

    /* ADS-B with some other address (CF=1) is the same. */
    uint8_t c1[14];
    reframe(c1, ID_17, 18, 1);
    adsb_decode_init();
    on_air(c1, 112);
    LS_EQ_INT(0, adsb_state_active_count());

    /* And it did not teach the cache the address: after a clean CF=1 frame,
       a damaged CF=0 frame from the same bits is still from an address never
       heard as an ICAO one. */
    uint8_t c0[14];
    reframe(c0, ID_17, 18, 0);
    flip(c0, 60);
    mode_s_init(&s_ms);
    LS_CHECK(decode(c1, NULL));
    LS_CHECK(!decode(c0, NULL));
}

LS_CASE(a_df18_ads_b_identification_is_tracked_and_named)
{
    /* The frame that used to be read as 56 bits, fail its CRC and be dropped. */
    uint8_t m[14];
    reframe(m, ID_17, 18, 0);

    mode_s_init(&s_ms);
    struct mode_s_msg mm;
    LS_CHECK_MSG(decode(m, &mm), "DF18 CF=0 rejected (msgbits %d)", mm.msgbits);
    LS_CHECK(!mm.addr_nonicao);
    LS_CHECK(!mm.rebroadcast);
    LS_EQ_INT(4, mm.metype);

    adsb_decode_init();
    on_air(m, 112);
    LS_EQ_INT(1, adsb_state_active_count());

    const adsb_aircraft_t *a = tracked(ADDR_KLM);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_STR("KLM1023", a->callsign);
        LS_EQ_INT(1, a->mt_df17_id);
        LS_EQ_INT(0, a->rebroadcast);
    }
}

LS_CASE(a_df18_ads_b_position_pair_gives_a_fix_and_altitude)
{
    uint8_t even[14], odd[14];
    reframe(even, EVEN_17, 18, 0);
    reframe(odd, ODD_17, 18, 0);

    adsb_decode_init();
    ls_shim_time_set(1000000);
    on_air(odd, 112);
    ls_shim_time_advance(1000000);
    on_air(even, 112);        /* the newer frame sets the fix: the even one */

    const adsb_aircraft_t *a = tracked(ADDR_POS);
    LS_CHECK(a != NULL);
    if (a) {
        LS_CHECK(a->pos_valid);
        LS_NEAR(a->lat, 52.25720, 0.0005);
        LS_NEAR(a->lon, 3.91937, 0.0005);
        LS_EQ_INT(38000, a->altitude);
        LS_EQ_INT(2, a->mt_df17_pos);
    }
}

LS_CASE(only_the_df18_layouts_that_can_be_trusted_are_tracked)
{
    uint8_t m[14];

    /* ADS-R (CF=6) position, IMF clear: an ICAO address, relayed. */
    from_hex(m, EVEN_17);
    m[0] = (18 << 3) | 6;
    m[4] &= (uint8_t)~1u;
    seal(m, 112);
    adsb_decode_init();
    on_air(m, 112);
    const adsb_aircraft_t *a = tracked(ADDR_POS);
    LS_CHECK(a != NULL);
    if (a) LS_EQ_INT(1, a->rebroadcast);

    /* The same with IMF set: the address is not an ICAO one. */
    m[4] |= 1;
    seal(m, 112);
    adsb_decode_init();
    on_air(m, 112);
    LS_EQ_INT(0, adsb_state_active_count());

    /* Fine TIS-B (CF=2) identification: the IMF bit is not defined where it
       would be read, so nothing is believed. */
    reframe(m, ID_17, 18, 2);
    adsb_decode_init();
    on_air(m, 112);
    LS_EQ_INT(0, adsb_state_active_count());

    /* Coarse TIS-B (CF=3) and TIS-B management (CF=4): other layouts. */
    for (int cf = 3; cf <= 4; cf++) {
        reframe(m, EVEN_17, 18, cf);
        adsb_decode_init();
        on_air(m, 112);
        LS_EQ_INT(0, adsb_state_active_count());
    }
}

LS_CASE(df18_repairs_follow_the_df17_rules)
{
    uint8_t f18[14];
    reframe(f18, ID_17, 18, 0);

    /* Never heard clean: a one-bit-damaged frame is refused. */
    int accepted = 0;
    for (int bit = 5; bit < 112; bit++) {
        uint8_t m[14];
        memcpy(m, f18, sizeof(m));
        flip(m, bit);
        mode_s_init(&s_ms);
        if (decode(m, NULL)) accepted++;
    }
    LS_EQ_INT(0, accepted);

    /* Heard clean once: the same damage is repaired to the sender. */
    mode_s_init(&s_ms);
    LS_CHECK(decode(f18, NULL));
    int wrong = 0;
    for (int bit = 5; bit < 112; bit++) {
        uint8_t m[14];
        memcpy(m, f18, sizeof(m));
        flip(m, bit);
        struct mode_s_msg mm;
        if (!decode(m, &mm) || mm.errorbit != bit ||
            ((mm.aa1 << 16) | (mm.aa2 << 8) | mm.aa3) != (int)ADDR_KLM)
            wrong++;
    }
    LS_EQ_INT(0, wrong);
}

LS_CASE(a_repair_that_changes_the_format_is_refused)
{
    /* A DF19 frame with bit 3 flipped reads as DF17; flipping it back is a
       valid codeword for a known address, but it is not the message the
       length and the rest of the decode were chosen for. */
    uint8_t f17[14], f19[14];
    reframe(f17, ID_17, 17, 5);
    reframe(f19, ID_17, 19, 5);
    flip(f19, 3);
    LS_EQ_INT(17, f19[0] >> 3);

    mode_s_init(&s_ms);
    LS_CHECK(decode(f17, NULL));            /* the address is known */
    LS_CHECK(!decode(f19, NULL));
}

/* ------------------------------------------------------------- clean msg -- */

static int s_cb_calls;
static int s_cb_dirty;
static char s_cb_why[96];

static void on_message(mode_s_t *self, struct mode_s_msg *mm)
{
    (void)self;
    s_cb_calls++;
    /* A DF11 sets none of the extended-squitter fields, so every one of them
       must still be zero. */
    static const char zero_flight[9] = { 0 };
    const char *why = NULL;
    if (memcmp(mm->flight, zero_flight, sizeof(zero_flight)) != 0) why = "flight";
    else if (mm->heading_is_valid || mm->heading) why = "heading";
    else if (mm->aircraft_type) why = "aircraft_type";
    else if (mm->fflag || mm->tflag) why = "fflag/tflag";
    else if (mm->raw_latitude || mm->raw_longitude) why = "raw position";
    else if (mm->ew_dir || mm->ew_velocity || mm->ns_dir || mm->ns_velocity) why = "ground velocity";
    else if (mm->vert_rate_source || mm->vert_rate_sign || mm->vert_rate) why = "vert_rate";
    else if (mm->velocity) why = "velocity";
    else if (mm->altitude || mm->unit) why = "altitude";
    if (why) {
        s_cb_dirty++;
        snprintf(s_cb_why, sizeof(s_cb_why), "%s", why);
    }
}

static void __attribute__((noinline)) dirty_the_stack(void)
{
    volatile unsigned char junk[24 * 1024];
    for (unsigned i = 0; i < sizeof(junk); i++) junk[i] = 0xA5;
}

LS_CASE(the_callback_message_carries_nothing_from_the_stack)
{
    /* An all-call reply (DF11): its decode touches only the fields it has. */
    uint8_t f11[14] = { 0 };
    f11[0] = (11 << 3) | 5;
    f11[1] = 0xAE; f11[2] = 0x12; f11[3] = 0x34;
    seal(f11, 56);

    static uint16_t mag[IQ_SAMPLES];
    build_iq(f11, 56);
    mode_s_compute_magnitude_vector(s_iq, mag, sizeof(s_iq));

    mode_s_init(&s_ms);
    s_cb_calls = s_cb_dirty = 0;
    dirty_the_stack();
    mode_s_detect(&s_ms, mag, IQ_SAMPLES, on_message);

    LS_CHECK_MSG(s_cb_calls >= 1, "the detector never called back");
    LS_CHECK_MSG(s_cb_dirty == 0,
                 "%d callback(s) saw stale stack in the message (%s)",
                 s_cb_dirty, s_cb_why);
}

LS_CASE(mode_s_decode_does_not_depend_on_what_the_struct_held)
{
    uint8_t f11[14] = { 0 };
    f11[0] = (11 << 3) | 5;
    f11[1] = 0xAE; f11[2] = 0x12; f11[3] = 0x34;
    seal(f11, 56);

    struct mode_s_msg mm;
    memset(&mm, 0xA5, sizeof(mm));
    mode_s_init(&s_ms);
    mode_s_decode(&s_ms, &mm, f11);
    LS_CHECK(mm.crcok);
    LS_EQ_INT(0, mm.flight[0]);
    LS_EQ_INT(0, mm.velocity);
    LS_EQ_INT(0, mm.heading_is_valid);
}

LS_CASE(published_ground_velocity_has_physical_units_and_valid_track)
{
    uint8_t m[14];
    from_hex(m, "8D485020994409940838175B284F");
    adsb_decode_init();
    ls_shim_time_set(1000000);
    struct mode_s_msg mm;
    mode_s_decode(&s_ms, &mm, m);
    LS_CHECK(mm.crcok);
    LS_EQ_INT(8, mm.ew_velocity);
    LS_EQ_INT(159, mm.ns_velocity);
    LS_EQ_INT(832, mm.vert_rate);
    LS_CHECK(mm.heading_is_valid && mm.heading_is_track);
    adsb_decode_on_message(&mm);
    const adsb_aircraft_t *a = tracked(0x485020);
    LS_CHECK(a && a->heading_valid);
    LS_EQ_INT(182, a->heading);
    LS_EQ_INT(-832, a->vert_rate);
    LS_EQ_INT(159, a->velocity);
    m[4] = (19 << 3) | 2;
    seal(m, 112);
    mode_s_decode(&s_ms, &mm, m);
    LS_EQ_INT(32, mm.ew_velocity);
    LS_EQ_INT(636, mm.ns_velocity);
    LS_EQ_INT(832, mm.vert_rate);
}

LS_CASE(velocity_zero_and_unavailable_are_distinct)
{
    uint8_t m[14] = {0x8D, 0x48, 0x50, 0x20, 0x99, 0, 1, 0, 0x20, 4};
    adsb_decode_init();
    ls_shim_time_set(1000000);
    seal(m, 112);
    adsb_on_frame(m, 14, ADSB_RSSI_NONE);
    const adsb_aircraft_t *a = tracked(0x485020);
    LS_CHECK(a && a->velocity_valid && a->vert_rate_valid);
    LS_EQ_INT(0, a->velocity);
    LS_EQ_INT(0, a->vert_rate);
    LS_CHECK(!a->heading_valid);
    m[6] = m[8] = m[9] = 0;
    seal(m, 112);
    struct mode_s_msg mm;
    mode_s_decode(&s_ms, &mm, m);
    LS_CHECK(!mm.velocity_is_valid && !mm.vert_rate_is_valid);
    ls_shim_time_advance(20000000);
    adsb_decode_on_message(&mm);
    LS_EQ_INT(1000000, a->velocity_ts_us);
    LS_CHECK(a->last_seen_us > a->velocity_ts_us + ADSB_FIELD_STALE_US);
}

LS_CASE(airspeed_heading_and_vertical_rate_decode_for_both_subtypes)
{
    uint8_t m[14];
    from_hex(m, "8DA05F219B06B6AF189400CBC33F");
    struct mode_s_msg mm;
    mode_s_init(&s_ms);
    mode_s_decode(&s_ms, &mm, m);
    LS_CHECK(mm.crcok);
    LS_CHECK(mm.heading_is_valid && !mm.heading_is_track);
    LS_EQ_INT(243, mm.heading);
    LS_EQ_INT(375, mm.airspeed);
    LS_EQ_INT(1, mm.airspeed_type);
    LS_EQ_INT(2304, mm.vert_rate);
    LS_CHECK(mm.vert_rate_sign);
    m[4] = (19 << 3) | 4;
    seal(m, 112);
    mode_s_decode(&s_ms, &mm, m);
    LS_EQ_INT(1500, mm.airspeed);
    m[7] = 0; m[8] &= 31;
    seal(m, 112);
    mode_s_decode(&s_ms, &mm, m);
    LS_CHECK(!mm.airspeed_is_valid);
}

LS_CASE(zero_altitude_and_level_flight_update_without_refreshing_position)
{
    adsb_decode_init();
    ls_shim_time_set(1000000);
    struct mode_s_msg mm = {0};
    mm.crcok = 1; mm.aa1 = 0xAA;
    mm.altitude_is_valid = mm.vert_rate_is_valid = 1;
    mm.altitude = 100; mm.vert_rate = 128;
    adsb_decode_on_message(&mm);
    const adsb_aircraft_t *a = tracked(0xAA0000);
    mm.altitude = mm.vert_rate = 0;
    ls_shim_time_advance(1000000);
    adsb_decode_on_message(&mm);
    LS_EQ_INT(0, a->altitude);
    LS_EQ_INT(0, a->vert_rate);
    LS_EQ_INT(2000000, a->altitude_ts_us);
    adsb_aircraft_t *w = adsb_state_find_or_create(0xAA0000);
    w->pos_valid = true; w->pos_ts_us = 2000000;
    mm.altitude_is_valid = mm.vert_rate_is_valid = 0;
    mm.me_ok = 1; mm.metype = 4;
    strcpy(mm.flight, "TEST123");
    ls_shim_time_advance(20000000);
    adsb_decode_on_message(&mm);
    LS_EQ_INT(2000000, a->pos_ts_us);
    LS_EQ_INT(2000000, a->vert_rate_ts_us);
    LS_EQ_INT(22000000, a->last_seen_us);
}

LS_CASE(corrupt_addresses_only_increment_receiver_quality)
{
    adsb_decode_init();
    struct mode_s_msg mm = {0};
    mm.aa1 = 0xFF;
    int before = s_bad;
    adsb_decode_on_message(&mm);
    LS_EQ_INT(before + 1, s_bad);
    LS_EQ_INT(0, adsb_state_active_count());
}

LS_CASE(stream_retains_every_frame_split_and_decodes_adjacent_frames_once)
{
    uint8_t m[14];
    from_hex(m, ID_17);
    build_iq(m, 112);
    /* Two immediately adjacent frames. */
    memcpy(s_iq + 2 * (LEAD + 240), s_iq + 2 * LEAD, 480);
    for (int split = 1; split < IQ_SAMPLES; split++) {
        adsb_decode_init();
        adsb_on_sample(s_iq, split * 2);
        adsb_on_sample(s_iq + split * 2, (IQ_SAMPLES - split) * 2);
        const adsb_aircraft_t *a = tracked(0x4840D6);
        LS_CHECK_MSG(a && a->msg_count == 2, "split %d count %d",
                     split, a ? a->msg_count : 0);
    }
}

LS_CASE(stream_reset_discards_old_partial_frame)
{
    uint8_t m[14];
    from_hex(m, ID_17);
    build_iq(m, 112);
    adsb_decode_init();
    adsb_on_sample(s_iq, (LEAD + 120) * 2);
    adsb_decode_reset_stream();
    adsb_on_sample(s_iq + (LEAD + 120) * 2,
                   (IQ_SAMPLES - LEAD - 120) * 2);
    LS_EQ_INT(0, adsb_state_active_count());
}

LS_CASE(rejected_frame_counts_receiver_crc_without_creating_aircraft)
{
    uint8_t m[14];
    from_hex(m, ID_17);
    adsb_decode_init();
    m[1] ^= 0x55; m[2] ^= 0xAA; m[3] ^= 0x33;
    int before = s_bad;
    LS_EQ_INT(ADSB_FRAME_BAD_CRC, adsb_on_frame(m, 14, ADSB_RSSI_NONE));
    LS_EQ_INT(before + 1, s_bad);
    LS_EQ_INT(0, adsb_state_active_count());
}

LS_CASE(production_iq_boundary_preserves_a_long_frame)
{
    static uint8_t iq[16384];
    uint8_t m[14];
    from_hex(m, ID_17);
    build_iq(m, 112);
    memset(iq, 127, sizeof(iq));
    memcpy(iq + 3980 * 2, s_iq + LEAD * 2, 480);
    adsb_decode_init();
    adsb_on_sample(iq, 8192);
    LS_EQ_INT(0, adsb_state_active_count());
    adsb_on_sample(iq + 8192, 8192);
    const adsb_aircraft_t *a = tracked(0x4840D6);
    LS_CHECK(a);
    LS_EQ_INT(1, a->msg_count);
}
