/* LS_TEST_SOURCES: ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c */
/* Aircraft status messages (DF17 type code 28, subtype 1): the emergency
   state and the Mode A code they carry reach the aircraft table, each
   accepted once two reports agree and stamped when last heard. The frames
   are built here from the layout and sealed with the decoder's own CRC. */

#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_state.h"
#include "mode-s.h"

#include "audio_events.h"
#include "event_bus.h"
#include "esp_timer.h"
#include "perf.h"
#include "plane_audio.h"

#include <string.h>

uint32_t mode_s_checksum(unsigned char *msg, int bits);

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
void perf_count_burst(void) { }
void perf_mark_good_msg(int64_t now_us) { (void)now_us; }
void perf_mark_position(int64_t now_us) { (void)now_us; }
void perf_set_mag(int avg, int peak) { (void)avg; (void)peak; }
void perf_set_active_count(int n) { (void)n; }
int  perf_get_crc_good(void) { return 0; }
int  perf_get_crc_err(void) { return 0; }

/* A Mode A code (decimal digits 0-7) in the 13-bit identity layout:
   C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4. */
static void put_identity(uint8_t *hi, uint8_t *lo, int code)
{
    const int a = code / 1000 % 10, b = code / 100 % 10, c = code / 10 % 10, d = code % 10;
    *hi = (uint8_t)(((c & 1) << 4) | ((a & 1) << 3) | ((c & 2) << 1) | (a & 2) | ((c & 4) >> 2));
    *lo = (uint8_t)(((a & 4) << 5) | ((b & 1) << 5) | ((d & 1) << 4) | ((b & 2) << 2) |
                    ((d & 2) << 1) | ((b & 4) >> 1) | ((d & 4) >> 2));
}

static void status_frame(uint8_t m[14], uint32_t icao, int state, int code)
{
    memset(m, 0, 14);
    m[0] = (17 << 3) | 5;
    m[1] = (uint8_t)(icao >> 16);
    m[2] = (uint8_t)(icao >> 8);
    m[3] = (uint8_t)icao;
    m[4] = (28 << 3) | 1;
    uint8_t hi, lo;
    put_identity(&hi, &lo, code);
    m[5] = (uint8_t)((state << 5) | hi);
    m[6] = lo;
    const uint32_t crc = mode_s_checksum(m, 112);
    m[11] = (uint8_t)(crc >> 16);
    m[12] = (uint8_t)(crc >> 8);
    m[13] = (uint8_t)crc;
}

#define IQ_SAMPLES 1024
#define LEAD       100
#define PULSE      227
static uint8_t s_iq[IQ_SAMPLES * 2];

static void on_air(const uint8_t *frame)
{
    static const int PREAMBLE[] = { 0, 2, 7, 9 };
    memset(s_iq, 127, sizeof(s_iq));
    for (int i = 0; i < 4; i++) s_iq[2 * (LEAD + PREAMBLE[i])] = PULSE;
    for (int b = 0; b < 112; b++) {
        const bool one = (frame[b / 8] & (0x80 >> (b % 8))) != 0;
        s_iq[2 * (LEAD + 16 + 2 * b + (one ? 0 : 1))] = PULSE;
    }
    adsb_on_sample(s_iq, (int)sizeof(s_iq));
}

static int slot_of(uint32_t icao)
{
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (a && a->active && a->icao == icao) return i;
    }
    return -1;
}

LS_CASE(the_status_frame_decodes_state_and_code)
{
    uint8_t m[14];
    status_frame(m, 0xABCDEF, 1, 7700);
    static mode_s_t s_ms;
    mode_s_init(&s_ms);
    struct mode_s_msg mm;
    mode_s_decode(&s_ms, &mm, m);
    LS_CHECK(mm.crcok);
    LS_EQ_INT(28, mm.metype);
    LS_CHECK(mm.emergency_ok);
    LS_EQ_INT(1, mm.emergency_state);
    LS_EQ_INT(7700, mm.status_identity);
    /* Other type codes carry no status. */
    status_frame(m, 0xABCDEF, 0, 1200);
    m[4] = (19 << 3) | 1;
    m[11] = m[12] = m[13] = 0;
    const uint32_t crc = mode_s_checksum(m, 112);
    m[11] = (uint8_t)(crc >> 16);
    m[12] = (uint8_t)(crc >> 8);
    m[13] = (uint8_t)crc;
    mode_s_decode(&s_ms, &mm, m);
    LS_CHECK(!mm.emergency_ok);
}

LS_CASE(every_mode_a_digit_round_trips)
{
    static mode_s_t s_ms;
    mode_s_init(&s_ms);
    const int codes[] = { 0, 1200, 7500, 7600, 7700, 4567, 1234, 777, 7000, 7777 };
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        uint8_t m[14];
        struct mode_s_msg mm;
        status_frame(m, 0x123456, 0, codes[i]);
        mode_s_decode(&s_ms, &mm, m);
        LS_EQ_INT(codes[i], mm.status_identity);
    }
}

LS_CASE(emergency_and_squawk_need_two_agreeing_status_messages)
{
    ls_shim_time_set(5000000);
    adsb_decode_init();
    uint8_t m[14];
    status_frame(m, 0xABCDEF, 1, 7700);
    on_air(m);
    const int slot = slot_of(0xABCDEF);
    LS_CHECK(slot >= 0);
    int64_t ts = -1;
    LS_EQ_INT(0, adsb_state_emergency(slot, &ts));
    LS_EQ_INT(0, adsb_state_squawk(slot));
    on_air(m);
    LS_EQ_INT(1, adsb_state_emergency(slot, &ts));
    LS_CHECK(ts != 0);
    int64_t sts = 0;
    LS_EQ_INT(7700, adsb_state_squawk_at(slot, &sts));
    LS_CHECK(sts != 0);
}

LS_CASE(a_reserved_state_and_an_empty_code_are_ignored)
{
    adsb_decode_init();
    uint8_t m[14];
    status_frame(m, 0x00BEEF, 7, 0);
    on_air(m);
    on_air(m);
    const int slot = slot_of(0x00BEEF);
    LS_CHECK(slot >= 0);
    LS_EQ_INT(0, adsb_state_emergency(slot, NULL));
    LS_EQ_INT(0, adsb_state_squawk(slot));
}

LS_CASE(a_cleared_state_replaces_an_emergency_after_two_reports)
{
    adsb_decode_init();
    uint8_t m[14], clear[14];
    status_frame(m, 0xABCDEF, 4, 7600);
    status_frame(clear, 0xABCDEF, 0, 7600);
    on_air(m);
    on_air(m);
    const int slot = slot_of(0xABCDEF);
    LS_EQ_INT(4, adsb_state_emergency(slot, NULL));
    on_air(clear);
    LS_EQ_INT(4, adsb_state_emergency(slot, NULL));
    on_air(clear);
    LS_EQ_INT(0, adsb_state_emergency(slot, NULL));
}
