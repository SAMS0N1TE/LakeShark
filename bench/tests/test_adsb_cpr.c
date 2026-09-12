/* LS_TEST_SOURCES: ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c */


#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_state.h"
#include "adsb_demo.h"
#include "mode-s.h"

#include "audio_events.h"
#include "event_bus.h"
#include "esp_timer.h"
#include "perf.h"
#include "plane_audio.h"

#include <math.h>
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




static double pmod(double a, double b)
{
    const double r = fmod(a, b);
    return r < 0.0 ? r + b : r;
}


static int nl(double lat)
{
    if (lat == 0.0) return 59;
    if (fabs(lat) == 87.0) return 2;
    if (fabs(lat) > 87.0) return 1;
    const double a = 1.0 - cos(M_PI / 30.0);
    const double b = cos(M_PI / 180.0 * fabs(lat));
    return (int)(2.0 * M_PI / acos(1.0 - a / (b * b)));
}

static void cpr_encode(double lat, double lon, int odd,
                       uint32_t *yz, uint32_t *xz)
{
    const int i = odd ? 1 : 0;
    const double dlat = 360.0 / (60 - i);
    const double y = floor(131072.0 * pmod(lat, dlat) / dlat + 0.5);
    const double rlat = dlat * (y / 131072.0 + floor(lat / dlat));

    int n = nl(rlat) - i;
    if (n < 1) n = 1;
    const double dlon = 360.0 / n;
    const double x = floor(131072.0 * pmod(lon, dlon) / dlon + 0.5);

    *yz = (uint32_t)y & 0x1FFFFu;
    *xz = (uint32_t)x & 0x1FFFFu;
}



#define SENDER 0x4CA2D3u   

static void seal(uint8_t *m, int bits, uint32_t overlay)
{
    const int n = bits / 8;
    m[n - 3] = m[n - 2] = m[n - 1] = 0;
    const uint32_t crc = mode_s_checksum(m, bits) ^ overlay;
    m[n - 3] = (uint8_t)(crc >> 16);
    m[n - 2] = (uint8_t)(crc >> 8);
    m[n - 1] = (uint8_t)crc;
}


static void df17_position(uint8_t m[14], uint32_t aa, int odd,
                          uint32_t lat17, uint32_t lon17)
{
    memset(m, 0, 14);
    m[0] = (17 << 3) | 5;
    m[1] = (uint8_t)(aa >> 16);
    m[2] = (uint8_t)(aa >> 8);
    m[3] = (uint8_t)aa;
    m[4] = (11 << 3);                                  /* TC 11, SS 0, NICsb 0 */
    m[5] = 0x30;                                       /* altitude, not read here */
    m[6] = (uint8_t)(((odd ? 1u : 0u) << 2) | ((lat17 >> 15) & 3u));
    m[7] = (uint8_t)((lat17 >> 7) & 0xFFu);
    m[8] = (uint8_t)(((lat17 & 0x7Fu) << 1) | ((lon17 >> 16) & 1u));
    m[9] = (uint8_t)((lon17 >> 8) & 0xFFu);
    m[10] = (uint8_t)(lon17 & 0xFFu);
    seal(m, 112, 0);
}

static mode_s_t s_ms;


static void feed(const uint8_t *frame)
{
    struct mode_s_msg mm = {0};
    uint8_t buf[MODE_S_LONG_MSG_BYTES];
    memcpy(buf, frame, sizeof(buf));
    mode_s_decode(&s_ms, &mm, buf);
    LS_CHECK_MSG(mm.crcok, "the test built a frame that fails its own CRC");
    adsb_decode_on_message(&mm);
}

static void send_pair(double lat, double lon)
{
    uint8_t even[14], odd[14];
    uint32_t ey, ex, oy, ox;

    cpr_encode(lat, lon, 0, &ey, &ex);
    cpr_encode(lat, lon, 1, &oy, &ox);
    df17_position(even, SENDER, 0, ey, ex);
    df17_position(odd, SENDER, 1, oy, ox);

    feed(even);
    feed(odd);
}

static void send_one(double lat, double lon, int odd)
{
    uint8_t f[14];
    uint32_t y, x;
    cpr_encode(lat, lon, odd, &y, &x);
    df17_position(f, SENDER, odd, y, x);
    feed(f);
}

static double nm_apart(double lat1, double lon1, double lat2, double lon2)
{
    const double dlat = lat1 - lat2;
    const double dlon = (lon1 - lon2) * cos(lat2 * M_PI / 180.0);
    return sqrt(dlat * dlat + dlon * dlon) * 60.0;
}

static void reset(void)
{
    mode_s_init(&s_ms);
    adsb_state_init();
    adsb_decode_init();
}



LS_CASE(an_even_odd_pair_decodes_to_where_the_aircraft_is)
{
    
    static const struct { double lat, lon; } sky[] = {
        { 42.36, -71.06 },   /* Boston */
        { 43.20, -71.50 },   /* Concord */
        { 42.00, -73.00 },
        { 43.64, -70.30 },   /* Portland */
        { 44.47, -73.15 },   /* Burlington */
        { 41.72, -72.65 },   /* Hartford */
    };

    int wrong = 0;
    for (unsigned i = 0; i < sizeof(sky) / sizeof(sky[0]); i++) {
        reset();
        send_pair(sky[i].lat, sky[i].lon);

        adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
        const double off = a->pos_valid
            ? nm_apart(a->lat, a->lon, sky[i].lat, sky[i].lon) : -1.0;

        if (!a->pos_valid || off > 1.0) {
            wrong++;
            ls_note("  %.2f, %.2f decoded as %.2f, %.2f%s",
                    sky[i].lat, sky[i].lon, a->lat, a->lon,
                    a->pos_valid ? "" : " (no position)");
        }
    }
    LS_CHECK_MSG(wrong == 0, "%d of %u positions over the receiver decoded wrong",
                 wrong, (unsigned)(sizeof(sky) / sizeof(sky[0])));
}

LS_CASE(a_decoded_position_is_on_the_planet)
{
    
    reset();
    send_pair(42.36, -71.06);

    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    if (a->pos_valid) {
        LS_CHECK_MSG(a->lat >= -90.0 && a->lat <= 90.0,
                     "latitude %.2f is not on the planet", a->lat);
        LS_CHECK_MSG(a->lon >= -180.0 && a->lon <= 180.0,
                     "longitude %.2f is not on the planet", a->lon);
    }
}

LS_CASE(the_far_side_of_the_world_decodes_too)
{
    
    static const struct { double lat, lon; } sky[] = {
        { -33.87, 151.21 },  /* Sydney */
        {  35.68, 139.69 },  /* Tokyo */
        {  55.75,  37.62 },  /* Moscow */
        { -22.91, -43.17 },  /* Rio */
    };

    int wrong = 0;
    for (unsigned i = 0; i < sizeof(sky) / sizeof(sky[0]); i++) {
        reset();
        send_pair(sky[i].lat, sky[i].lon);

        adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
        const double off = a->pos_valid
            ? nm_apart(a->lat, a->lon, sky[i].lat, sky[i].lon) : -1.0;
        if (!a->pos_valid || off > 1.0) {
            wrong++;
            ls_note("  %.2f, %.2f decoded as %.2f, %.2f%s",
                    sky[i].lat, sky[i].lon, a->lat, a->lon,
                    a->pos_valid ? "" : " (no position)");
        }
    }
    LS_CHECK_MSG(wrong == 0, "%d of %u positions decoded wrong",
                 wrong, (unsigned)(sizeof(sky) / sizeof(sky[0])));
}

LS_CASE(one_frame_carries_a_track_when_the_pair_has_gone_stale)
{
    
    reset();
    ls_shim_time_set(1000000);
    send_pair(43.20, -71.50);

    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    LS_CHECK_MSG(a->pos_valid, "the pair did not give a position to start from");

    
    ls_shim_time_advance(11000000);
    send_one(43.35, -71.25, 1);

    const double off = nm_apart(a->lat, a->lon, 43.35, -71.25);
    LS_CHECK_MSG(off < 1.0,
                 "the track sat at %.4f, %.4f, %.1f nm behind the frame the "
                 "aircraft just sent", a->lat, a->lon, off);
}

LS_CASE(one_frame_alone_is_not_a_position)
{
    
    reset();
    send_one(43.20, -71.50, 0);

    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    LS_CHECK_MSG(!a->pos_valid,
                 "a lone frame produced %.4f, %.4f out of nowhere",
                 a->lat, a->lon);
}

LS_CASE(a_fresh_pair_corrects_a_wrong_local_anchor)
{
    reset();
    ls_shim_time_set(1000000);
    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    a->pos_valid = true;
    a->pos_ts_us = esp_timer_get_time();
    a->lat = 43.20f;
    a->lon = -71.50f + 360.0f / 43.0f;
    send_pair(43.20, -71.50);
    LS_CHECK_MSG(nm_apart(a->lat, a->lon, 43.20, -71.50) < 0.1,
                 "valid pair retained alias %.5f, %.5f", a->lat, a->lon);
}

LS_CASE(local_tracking_crosses_the_dateline)
{
    reset();
    ls_shim_time_set(1000000);
    send_pair(43.20, 179.95);
    ls_shim_time_advance(11000000);
    send_one(43.20, -179.95, 1);
    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    LS_CHECK_MSG(fabs(a->lon + 179.95) < 0.001,
                 "dateline crossing stuck at %.5f", a->lon);
}

LS_CASE(demo_positions_have_timestamps)
{
    reset();
    ls_shim_time_set(123456789);
    adsb_demo_set(1);
    adsb_aircraft_t *a = adsb_state_find_or_create(0xF00000u);
    LS_CHECK(a->pos_valid);
    LS_EQ_INT(a->pos_ts_us, 123456789);
    adsb_demo_set(0);
}

LS_CASE(local_even_position_at_87_degrees)
{
    reset();
    ls_shim_time_set(1000000);
    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    a->pos_valid = true;
    a->pos_ts_us = esp_timer_get_time();
    a->lat = 86.99f;
    a->lon = 1.0f;
    send_one(87.0, 1.0, 0);
    LS_NEAR(a->lat, 87.0, 0.0001);
    LS_NEAR(a->lon, 1.0, 0.001);
}

LS_CASE(published_cpr_vector_matches_encoder_and_decoder)
{
    /* The 1090 MHz Riddle, airborne position example. */
    reset();
    ls_shim_time_set(1000000);
    uint8_t even[] = {0x8d,0x40,0x62,0x1d,0x58,0xc3,0x82,0xd6,0x90,0xc8,0xac,0x28,0x63,0xa7};
    uint8_t odd[] = {0x8d,0x40,0x62,0x1d,0x58,0xc3,0x86,0x43,0x5c,0xc4,0x12,0x69,0x2a,0xd6};
    feed(odd);
    ls_shim_time_advance(1000000);
    feed(even);
    adsb_aircraft_t *a = adsb_state_find_or_create(0x40621d);
    LS_CHECK(a->pos_valid);
    LS_NEAR(a->lat, 52.2572021484375, 0.00001);
    LS_NEAR(a->lon, 3.91937255859375, 0.00001);
    uint32_t y, x;
    cpr_encode(52.2572021484375, 3.91937255859375, 0, &y, &x);
    LS_EQ_UINT(y, 93000); LS_EQ_UINT(x, 51372);
    cpr_encode(52.26578017412606, 360.0 / 35 * (50194.0 / 131072), 1, &y, &x);
    LS_EQ_UINT(y, 74158); LS_EQ_UINT(x, 50194);
}

LS_CASE(invalid_global_latitude_is_rejected)
{
    reset();
    uint8_t frame[14];
    df17_position(frame, SENDER, 0, 0, 0); feed(frame);
    df17_position(frame, SENDER, 1, 65536, 0); feed(frame);
    LS_CHECK(!adsb_state_find_or_create(SENDER)->pos_valid);
}

LS_CASE(local_anchor_age_is_bounded)
{
    reset(); ls_shim_time_set(1000000);
    send_pair(43.20, -71.50);
    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    int64_t ts = a->pos_ts_us;
    ls_shim_time_advance(60000001);
    send_one(43.35, -71.25, 1);
    LS_EQ_INT(a->pos_ts_us, ts);
}

LS_CASE(local_displacement_is_bounded)
{
    reset(); ls_shim_time_set(1000000);
    send_pair(43.20, -71.50);
    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    int64_t ts = a->pos_ts_us;
    ls_shim_time_advance(11000000);
    send_one(44.30, -71.50, 1);
    LS_EQ_INT(a->pos_ts_us, ts);
}

LS_CASE(global_positions_cover_both_parities_and_hemispheres)
{
    for (int parity = 0; parity < 2; parity++) {
        for (int lat = -85; lat <= 85; lat += 5) {
            for (int lon = -175; lon <= 175; lon += 5) {
                reset(); ls_shim_time_set(1000000);
                send_one(lat + 0.13, lon + 0.27, !parity);
                ls_shim_time_advance(1000000);
                send_one(lat + 0.13, lon + 0.27, parity);
                adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
                LS_CHECK(a->pos_valid);
                LS_NEAR(a->lat, lat + 0.13, 0.0001);
                LS_NEAR(a->lon, lon + 0.27, 0.003);
            }
        }
    }
}
