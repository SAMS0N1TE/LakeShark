/* LS_TEST_SOURCES: ${APP}/adsb/adsb_demo.c ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c ${FW}/components/apps/tui/ls_follow.c ${FW}/components/apps/tui/ls_map_motion.c */


#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_state.h"
#include "adsb_demo.h"
#include "mode-s.h"
#include "ls_follow.h"
#include "ls_map_motion.h"

#include "audio_events.h"
#include "event_bus.h"
#include "esp_timer.h"
#include "perf.h"
#include "plane_audio.h"

#include <math.h>
#include <string.h>

uint32_t mode_s_checksum(unsigned char *msg, int bits);
uint32_t icao_cache_has_addr(uint32_t addr);



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
    m[5] = 0xc3;                                       /* Q=1, 38000 ft */
    m[6] = (uint8_t)(0x80 | ((odd ? 1u : 0u) << 2) | ((lat17 >> 15) & 3u));
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

LS_CASE(a_fresh_pair_reacquires_after_the_local_anchor_expires)
{
    reset();
    ls_shim_time_set(1000000);
    adsb_aircraft_t *a = adsb_state_find_or_create(SENDER);
    a->pos_valid = true;
    a->pos_ts_us = esp_timer_get_time();
    a->lat = 43.20f;
    a->lon = -71.50f + 360.0f / 43.0f;
    ls_shim_time_advance(60000000);
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

/* Exercise both production entrances, including the RTL preamble detector.
   Frame CRCs are real; damage is a single RF bit error, not a fake message. */
static void receive_frame(uint8_t frame[14], bool rtl)
{
    if (rtl) {
        uint8_t iq[512];
        memset(iq, 127, sizeof(iq));
        const int preamble[] = { 0, 2, 7, 9 };
        for (unsigned i = 0; i < sizeof(preamble) / sizeof(preamble[0]); i++)
            iq[2 * preamble[i]] = 255;
        for (int bit = 0; bit < 112; bit++) {
            int one = (frame[bit / 8] >> (7 - bit % 8)) & 1;
            iq[2 * (16 + 2 * bit + !one)] = 255;
        }
        adsb_on_sample(iq, sizeof(iq));
    } else {
        uint8_t chips[28] = {0};
        for (int bit = 0; bit < 112; bit++) {
            int one = (frame[bit / 8] >> (7 - bit % 8)) & 1;
            int high = 2 * bit + !one;
            chips[high / 8] |= (uint8_t)(0x80 >> (high % 8));
        }
        LS_EQ_INT(adsb_on_chips(chips, sizeof(chips), ADSB_CHIPS_FULL,
                              ADSB_RSSI_NONE), ADSB_FRAME_OK);
    }
}

static void receive_position(uint32_t id, double lat, double lon, int odd,
                             bool damage, bool rtl)
{
    uint8_t frame[14];
    uint32_t y, x;
    cpr_encode(lat, lon, odd, &y, &x);
    df17_position(frame, id, odd, y, x);
    if (damage) frame[10] ^= 1; /* repair a position bit */
    receive_frame(frame, rtl);
}

static void receive_motion(uint32_t id, int aircraft, bool rtl)
{
    uint8_t frame[14] = {0x8d, (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id};
    /* TC19 subtype 3 supplies a valid heading. Subtype 1 then supplies
       distinct north/east ground velocities for dead reckoning. */
    const int heading = 25 + aircraft;
    frame[4] = (19 << 3) | 3;
    frame[5] = (uint8_t)(4 | (heading >> 5));
    frame[6] = (uint8_t)((heading & 31) << 3);
    seal(frame, 112, 0);
    receive_frame(frame, rtl);
    const int ew = 471 + aircraft * 20, ns = 156 + aircraft * 7;
    frame[4] = (19 << 3) | 1;
    frame[5] = (uint8_t)(ew >> 8);
    frame[6] = (uint8_t)ew;
    frame[7] = (uint8_t)(ns >> 3);
    frame[8] = (uint8_t)((ns & 7) << 5);
    seal(frame, 112, 0);
    receive_frame(frame, rtl);
}

static uint32_t colliding_sender(void)
{
    for (uint32_t id = 1; id < 0xFFFFFF; id++)
        if (id != SENDER && icao_cache_has_addr(id) == icao_cache_has_addr(SENDER))
            return id;
    return 0;
}

static void two_tracks(bool rtl, double base_lat)
{
    reset(); ls_shim_time_set(1000000);
    const uint32_t ids[] = { SENDER, colliding_sender() };
    LS_CHECK(ids[1] != 0);
    LS_EQ_UINT(icao_cache_has_addr(ids[0]), icao_cache_has_addr(ids[1]));
    receive_position(ids[0], base_lat, -71.50, 0, false, rtl);
    receive_position(ids[0], base_lat, -71.50, 1, false, rtl);
    adsb_select_set_icao(ids[0]);
    ls_follow_view_t selected, all;
    ls_follow_clear(&selected); ls_follow_clear(&all);
    ls_map_glide_t glide[2] = {0};
    int64_t previous[2] = {0};
    int trail_count[2] = {0};
    for (int step = 0; step < 70; step++) {
        ls_shim_time_set(1000000LL + step * 5000000LL);
        const int n = step >= 6 ? 2 : 1; /* newcomer starts exactly 30 s later */
        for (int i = 0; i < n; i++) {
            const double lat = base_lat + step * 0.002 - i * 0.03;
            const double lon = -71.50 + step * 0.008 - i * 0.03;
            adsb_aircraft_t peer = *adsb_state_get(1 - i);
            /* Do not refresh A's clean-address evidence after B arrives:
               its weak position reports must stand on its earlier evidence. */
            if (i == 1 || step <= 6) receive_motion(ids[i], i, rtl);
            /* The first receiver is weaker: subsequent frames require repair.
               Each aircraft alternates parity, with opposite parity to its peer. */
            receive_position(ids[i], lat, lon, (step + i) & 1,
                             i == 0 && step > 6, rtl);
            LS_CHECK(memcmp(&peer, adsb_state_get(1 - i), sizeof(peer)) == 0);
        }
        adsb_periodic_age(esp_timer_get_time());
        LS_EQ_INT(adsb_state_active_count(), n);
        LS_EQ_UINT(adsb_select_get()->icao, ids[0]);
        /* MAP/ADS-B read these exact slot records, rather than a packed or
           double-buffered aircraft snapshot. Copy all slots as the UI does. */
        adsb_aircraft_t snapshot[ADSB_MAX_TRACKED];
        ls_follow_pt_t points[2];
        int positioned = 0;
        for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
            snapshot[slot] = *adsb_state_get(slot);
            const adsb_aircraft_t *a = &snapshot[slot];
            if (!a->active || !a->pos_valid) continue;
            LS_CHECK(positioned < n);
            int i = a->icao == ids[0] ? 0 : 1;
            LS_EQ_UINT(a->icao, ids[i]);
            LS_EQ_INT(a->altitude, 38000);
            /* The later subtype 1 now publishes ground track, about 71.7 degrees. */
            LS_EQ_INT(a->heading, 71);
            LS_CHECK(a->heading_valid && a->heading_is_track);
            LS_EQ_INT(a->ew_velocity, 470 + i * 20);
            LS_EQ_INT(a->ns_velocity, 155 + i * 7);
            LS_CHECK(a->velocity > 400);
            LS_CHECK(a->ns_velocity > 0 && a->ew_velocity > 0);
            if (step > 0 && (i == 0 || step > 6)) {
                LS_CHECK(a->pos_ts_us > previous[i]);
                LS_NEAR(a->lat, base_lat + step * 0.002 - i * 0.03, 0.0001);
                LS_NEAR(a->lon, -71.50 + step * 0.008 - i * 0.03, 0.001);
            }
            previous[i] = a->pos_ts_us;
            adsb_trail_pt_t trail[ADSB_TRAIL_N];
            int count = adsb_state_trail(slot, trail, ADSB_TRAIL_N);
            LS_CHECK(count >= trail_count[i]);
            if (i == 0 ? step > 0 : step > 7) LS_CHECK(count > 0);
            if (count) {
                LS_EQ_INT(trail[count - 1].ts_us, a->pos_ts_us);
                LS_NEAR(trail[count - 1].lat, a->lat, 0.00001);
                for (int k = 1; k < count; k++)
                    LS_CHECK(trail[k].ts_us > trail[k - 1].ts_us);
            }
            trail_count[i] = count;
            double lat, lon;
            ls_map_glide(&glide[i], a->icao, a->pos_ts_us, a->lat, a->lon,
                         esp_timer_get_time() + 900000, &lat, &lon);
            points[positioned++] = (ls_follow_pt_t){a->icao, lat, lon};
        }
        LS_EQ_INT(positioned, step == 6 ? 1 : n); /* B needs its own pair */
        ls_follow_in_t in = { .mode = LS_FOLLOW_SELECTED, .pw = 480, .ph = 640,
            .tile_px = 256, .zoom = 11, .zoom_lo = 0, .zoom_hi = 15,
            .air = points, .n = positioned, .selected = ids[0],
            .now_us = esp_timer_get_time() };
        ls_follow_step(&selected, &in);
        LS_EQ_UINT(selected.target, ids[0]);
        in.mode = LS_FOLLOW_ALL;
        ls_follow_step(&all, &in);
        LS_EQ_INT(all.count, positioned);
    }
    LS_EQ_INT(trail_count[0], ADSB_TRAIL_N);
    LS_EQ_INT(trail_count[1], ADSB_TRAIL_N);
}

LS_CASE(lr_chip_interleaved_colliding_aircraft_keep_both_tracks) { two_tracks(false, 43.20); }
LS_CASE(rtl_interleaved_colliding_aircraft_keep_both_tracks) { two_tracks(true, 43.20); }
/* Both cross NL=44/43 at 42.80914012, starting a few km apart. */
LS_CASE(lr_chip_two_tracks_cross_cpr_zone_boundary) { two_tracks(false, 42.792); }
LS_CASE(rtl_two_tracks_cross_cpr_zone_boundary) { two_tracks(true, 42.792); }

LS_CASE(recent_own_fix_precedes_a_motion_aliased_pair)
{
    reset(); ls_shim_time_set(1000000);
    send_pair(42.36, -71.06);
    ls_shim_time_advance(9000000);
    send_one(42.42, -71.06, 0);
    const adsb_aircraft_t *a = adsb_state_get(0);
    LS_NEAR(a->lat, 42.42, 0.0001);
    LS_NEAR(a->lon, -71.06, 0.001);
}
