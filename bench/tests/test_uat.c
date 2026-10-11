/* LS_TEST_SOURCES: uat_decode.c, uat_stream.c, uat_weather.c, adsb_state.c. */
#include "ls_test.h"
#include "uat_stream.h"
#include "uat_weather.h"
#include "adsb_state.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static uat_stream_t stream;
static uint8_t iq[24000];
static size_t iq_n;
static double phase;
static unsigned downs, ups;
static uat_fix_t last_fix;
static uint8_t last_up[432];
static void got_down(const uat_fix_t *f, void *user) { (void)user; downs++; last_fix = *f; }
static void got_up(const uint8_t *p, void *user) { (void)user; ups++; memcpy(last_up, p, 432); }
static void sample(double increment)
{
    phase += increment;
    LS_CHECK(iq_n + 2 <= sizeof(iq));
    iq[iq_n++] = (uint8_t)lround(128 + 85 * cos(phase));
    iq[iq_n++] = (uint8_t)lround(128 + 85 * sin(phase));
}
static void fm_bit(unsigned b)
{
    /* +/-312500 Hz CPFSK; 15 kHz frequency error, arbitrary carrier phase. */
    double step = 2 * 3.141592653589793 * ((b ? 312500 : -312500) + 15000) / UAT_SAMPLE_RATE;
    sample(step); sample(step);
}
static void burst(uint64_t sync, const uint8_t *frame, unsigned n)
{
    for (int i = 0; i < 40; i++) fm_bit(i & 1);
    for (int i = 35; i >= 0; i--) fm_bit((sync >> i) & 1);
    for (unsigned i = 0; i < n * 8; i++) fm_bit((frame[i / 8] >> (7 - i % 8)) & 1);
    for (int i = 0; i < 40; i++) fm_bit(0);
}
static void reset(void)
{
    downs = ups = 0;
    iq_n = 0;
    phase = 0.73;
    uat_stream_init(&stream, got_down, got_up, NULL);
}
static void feed_split(size_t chunk)
{
    for (size_t i = 0; i < iq_n; i += chunk) {
        size_t n = iq_n - i < chunk ? iq_n - i : chunk;
        uat_stream_feed(&stream, iq + i, n);
    }
}

LS_CASE(cpfsk_short_long_phase_and_every_transfer_boundary)
{
    uint8_t data[34] = {0}, f[48];
    data[1] = 0xab; data[2] = 0xcd; data[3] = 0xef;
    data[11] = 8;
    for (int long_frame = 0; long_frame < 2; long_frame++) {
        data[0] = long_frame ? 8 : 0;
        LS_CHECK(uat_rs_encode(data, long_frame ? 34 : 18, f));
        f[5] ^= 0x39;
        for (int half = 0; half < 2; half++) {
            for (size_t chunk = 1; chunk <= 67; chunk += 11) {
                reset();
                if (half) sample(0.2);
                burst(UAT_DOWNLINK_SYNC, f, long_frame ? 48 : 30);
                feed_split(chunk);
                LS_EQ_UINT(downs, 1);
                LS_EQ_UINT(ups, 0);
                LS_EQ_UINT(last_fix.address, 0xabcdef);
                LS_EQ_UINT(stream.stats.corrected, 1);
                LS_EQ_UINT(stream.stats.long_frames, long_frame);
                LS_EQ_UINT(stream.stats.short_frames, !long_frame);
            }
        }
    }
}

static void uplink_encode(const uint8_t payload[432], uint8_t wire[552])
{
    uint8_t lane[92];
    for (int i = 0; i < 6; i++) {
        LS_CHECK(uat_rs_encode(payload + i * 72, 72, lane));
        for (int j = 0; j < 92; j++) wire[j * 6 + i] = lane[j];
    }
}

LS_CASE(downlink_state_vector_and_callsign_identification)
{
    uint8_t data[34] = {8, 0x12, 0x34, 0x56}, frame[48];
    unsigned alt = 241, ns = 1024 | 101, ew = 51, vr = 512 | 11;
    data[10] = alt >> 4; data[11] = (alt << 4) | 8;
    data[12] = ns >> 6; data[13] = (ns << 2) | (ew >> 9);
    data[14] = ew >> 1; data[15] = (ew << 7) | (vr >> 4); data[16] = vr << 4;
    /* Emitter category 1 followed by N123AB and two spaces, base 40. */
    unsigned words[] = {1600 + 23 * 40 + 1, 2 * 1600 + 3 * 40 + 10, 11 * 1600 + 36 * 40 + 36};
    for (int i = 0; i < 3; i++) { data[17 + i * 2] = words[i] >> 8; data[18 + i * 2] = words[i]; }
    data[26] = 2;
    uat_fix_t fix;
    LS_CHECK(uat_rs_encode(data, 34, frame));
    LS_CHECK(uat_frame_decode(frame, 48, &fix, NULL));
    LS_EQ_INT(fix.altitude, 5000);
    LS_EQ_INT(fix.ns_velocity, -100); LS_EQ_INT(fix.ew_velocity, 50);
    LS_EQ_INT(fix.vert_rate, -640);
    LS_CHECK(fix.altitude_valid && fix.velocity_valid && fix.vert_rate_valid);
    LS_EQ_STR(fix.callsign, "N123AB");
    data[26] = 0;
    LS_CHECK(uat_rs_encode(data, 34, frame));
    LS_CHECK(uat_frame_decode(frame, 48, &fix, NULL));
    LS_EQ_STR(fix.callsign, "");
}

LS_CASE(uplink_six_lanes_correct_capacity_and_do_not_publish_failures)
{
    uint8_t payload[432], wire[552], decoded[432], bad[552];
    for (unsigned i = 0; i < 432; i++) payload[i] = (uint8_t)(i * 29 + 17);
    uplink_encode(payload, wire);
    for (int lane = 0; lane < 6; lane++)
        for (int j = 0; j < 10; j++) wire[(j * 3) * 6 + lane] ^= (j + 1);
    LS_EQ_INT(uat_uplink_decode(wire, decoded), 60);
    LS_CHECK(!memcmp(payload, decoded, 432));
    reset(); burst(UAT_UPLINK_SYNC, wire, sizeof(wire)); feed_split(127);
    LS_EQ_UINT(ups, 1);
    LS_CHECK(!memcmp(last_up, payload, 432));
    LS_EQ_UINT(stream.stats.corrected, 60);
    memcpy(bad, wire, sizeof(bad));
    bad[30 * 6] ^= 0x81;
    memset(decoded, 0xa6, sizeof(decoded));
    LS_EQ_INT(uat_uplink_decode(bad, decoded), -1);
    for (unsigned i = 0; i < sizeof(decoded); i++) LS_EQ_UINT(decoded[i], 0xa6);
    reset(); burst(UAT_UPLINK_SYNC, bad, sizeof(bad)); feed_split(8192);
    LS_EQ_UINT(ups, 0);
    LS_CHECK(stream.stats.bad_frames > 0);
}

static size_t hex(const char *p, uint8_t *out)
{
    size_t n = strlen(p) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(p + i * 2, "%2x", &v); out[i] = v; }
    return n;
}
LS_CASE(faa_public_icd_text_and_nexrad_examples)
{
    /* FAA GDL90 Public ICD Rev A sections 5.1.4 and 5.2.4. Wire examples
       from a specification, not a third-party decoder implementation. */
    uint8_t text[80], radar[64];
    size_t t = hex("067441905011a02d3305832db0e70c1a04d832d71cf1d60c38c30d8b5204364cd806157c36c2008b3b1cb079c146370d30c205920b0ccb5204364cd8130d4cb5c3d79d", text);
    size_t r = hex("00FC000084A570308950111A53120930110A23451B0A0918090A1B0C1D0607061D041B0A0108", radar);
    uat_weather_reset();
    LS_CHECK(uat_weather_apdu(text, t, 100));
    uat_weather_text_t report;
    LS_CHECK(uat_weather_text_get(0, 200, &report));
    LS_EQ_STR(report.type, "TAF");
    LS_EQ_STR(report.location, "KSLE");
    LS_EQ_STR(report.issued, "260900ZAM");
    LS_CHECK(strstr(report.text, "KT") != NULL);
    LS_CHECK(uat_weather_apdu(radar, r, 100));
    uat_weather_block_t block;
    LS_EQ_INT(uat_weather_blocks_at(45.10, -123, 200, &block, 1), 1);
    LS_NEAR(block.north, 45 + 8.0 / 60, 0.00001);
    LS_NEAR(block.west, -(123 + 12.0 / 60), 0.00002);
    LS_EQ_UINT(block.number, 0x4a570);
    LS_EQ_UINT(block.product, 63);
    LS_EQ_UINT(block.pixels[0], 0);
    LS_EQ_UINT(block.pixels[7], 1);
    LS_EQ_UINT(block.pixels[24], 1);
    LS_EQ_UINT(block.pixels[25], 0);
    LS_CHECK(!uat_weather_text_get(0, UAT_WX_MAX_AGE_US + 101, &report));
    LS_EQ_INT(uat_weather_blocks_at(45.10, -123, UAT_RADAR_MAX_AGE_US + 101, &block, 1), 0);
}

static size_t dlac(const char *text, uint8_t *out)
{
    static const char alphabet[] = "\003ABCDEFGHIJKLMNOPQRSTUVWXYZ\032\t\036\n| !\"#$%&'()*+,-./0123456789:;<=>?";
    size_t bit = 0;
    memset(out, 0, 418);
    for (size_t i = 0; ; i++) {
        unsigned c = text[i] ? (unsigned)(strchr(alphabet, text[i]) - alphabet) : 29;
        for (int j = 5; j >= 0; j--, bit++) out[bit / 8] |= ((c >> j) & 1) << (7 - bit % 8);
        if (!text[i]) break;
    }
    return (bit + 7) / 8;
}
LS_CASE(product_framework_station_header_and_text_records)
{
    uat_weather_reset();
    uint8_t payload[432] = {0};
    payload[5] = 1; payload[6] = 32 | 7; payload[7] = 0xa0;
    const char *types[] = {"METAR KJFK 101200Z CLEAR", "TAF KJFK 101130Z VFR", "NOTAM KJFK 101000Z RUNWAY CLOSED", "PIREP KJFK 101201Z TURBULENCE"};
    for (int i = 0; i < 4; i++) {
        size_t n = dlac(types[i], payload + 14);
        payload[8] = (n + 4) >> 1;
        payload[9] = ((n + 4) & 1) << 7;
        payload[10] = 6; payload[11] = 0x74; payload[12] = payload[13] = 0;
        memset(payload + 14 + n, 0, sizeof(payload) - 14 - n);
        uat_weather_uplink(payload, 1000 + i);
        uat_weather_text_t r;
        LS_CHECK(uat_weather_text_get(i, 2000, &r));
        LS_EQ_STR(r.text, types[i]);
    }
    uat_ground_station_t g;
    LS_CHECK(uat_weather_station_get(0, 2000, &g));
    LS_CHECK(g.position_valid);
    LS_EQ_UINT(g.site, 10);
    LS_EQ_UINT(g.slot, 7);
    const char *capture = getenv("LS_UAT_TEST_IQ");
    if (capture && capture[0]) {
        uint8_t wire[552], data[18] = {0}, frame[30];
        data[1] = 0xab; data[2] = 0xcd; data[3] = 0xef;
        data[11] = 8;
        LS_CHECK(uat_rs_encode(data, 18, frame));
        uplink_encode(payload, wire);
        reset();
        burst(UAT_DOWNLINK_SYNC, frame, sizeof(frame));
        burst(UAT_UPLINK_SYNC, wire, sizeof(wire));
        FILE *fp = fopen(capture, "wb");
        LS_CHECK(fp != NULL);
        if (fp) { LS_EQ_UINT(fwrite(iq, 1, iq_n, fp), iq_n); fclose(fp); }
    }
    /* Application data invalid must not parse otherwise valid APDUs. */
    payload[6] = 0;
    uat_weather_uplink(payload, 2001);
    uat_weather_stats_t stats;
    uat_weather_stats(&stats);
    LS_EQ_UINT(stats.text_records, 4);
    payload[6] = 32; payload[8] = 255; payload[9] = 128;
    uat_weather_uplink(payload, 2002);
    uat_weather_stats(&stats);
    LS_CHECK(stats.malformed > 0);
}

LS_CASE(radar_bounds_empty_updates_resolutions_and_malformed_runs)
{
    uat_weather_reset();
    uint8_t r[] = {0, 0xfc, 0, 0, 0x84, 0xa5, 0x70, 0xf9, 0xf9, 0xf9, 0xf9};
    LS_CHECK(uat_weather_apdu(r, sizeof(r), 10));
    uat_weather_block_t b;
    LS_EQ_INT(uat_weather_blocks_at(45.1, -123, 10, &b, 1), 1);
    LS_EQ_UINT(b.pixels[127], 1);
    r[7] = 0xff;
    LS_CHECK(uat_weather_apdu(r, sizeof(r), 11));
    r[10] = 0xe8; /* only 127 bins, reject without altering the store */
    LS_CHECK(!uat_weather_apdu(r, sizeof(r), 12));
    LS_EQ_INT(uat_weather_blocks_at(45.1, -123, 12, &b, 1), 1);
    LS_EQ_UINT(b.pixels[0], 7);
    r[4] = 4; r[7] = 0; /* empty base block */
    LS_CHECK(uat_weather_apdu(r, 8, 13));
    LS_EQ_INT(uat_weather_blocks_at(45.1, -123, 13, &b, 1), 1);
    LS_EQ_UINT(b.pixels[0], 0);
    r[0] = 1; r[1] = 0; /* product 64 */
    r[4] = 0x94; r[5] = 0xaa; r[6] = 0xb5;
    r[7] = r[8] = r[9] = r[10] = 0xfa;
    LS_CHECK(uat_weather_apdu(r, sizeof(r), 14));
    uat_weather_block_t blocks[2];
    LS_EQ_INT(uat_weather_blocks_at(45.1, -123, 14, blocks, 2), 2);
    LS_EQ_UINT(blocks[1].product, 64);
    LS_NEAR(blocks[1].width, 4, 0.00001);
    r[1] |= 2; /* segmented APDU not misinterpreted as raw radar */
    LS_CHECK(!uat_weather_apdu(r, sizeof(r), 15));
    r[1] &= ~2; r[0] |= 0x80;
    LS_CHECK(!uat_weather_apdu(r, sizeof(r), 15));
    LS_EQ_INT(uat_weather_blocks_at(NAN, 0, 15, &b, 1), 0);
}

LS_CASE(aircraft_merge_namespace_selection_and_source_age)
{
    adsb_state_init();
    adsb_aircraft_t *m = adsb_state_find_or_create(0x123456);
    m->sources = 1; m->source_seen_us[0] = 1; m->last_seen_us = 1;
    uat_fix_t f = {.address = 0x123456, .qualifier = 2, .position = true,
        .lat = 40, .lon = -70, .altitude_valid = true, .altitude = 5000,
        .velocity_valid = true, .ns_velocity = 100, .ew_velocity = 0};
    LS_CHECK(adsb_state_update_uat(&f, 100) == m);
    LS_EQ_STR(adsb_state_source_label(m, 200), "BOTH");
    LS_EQ_UINT(m->uat_qualifier, 2);
    LS_CHECK(m->altitude_valid && m->velocity_valid && m->heading_valid && m->pos_valid);
    f.qualifier = 1;
    adsb_aircraft_t *other = adsb_state_update_uat(&f, 200);
    LS_CHECK(other != m);
    LS_EQ_INT(adsb_state_active_count(), 2);
    adsb_select_set_icao(adsb_state_key(other));
    LS_CHECK(adsb_select_get() == other);
    adsb_select_next();
    LS_CHECK(adsb_select_get() == m);
    LS_EQ_STR(adsb_state_source_label(m, ADSB_FIELD_STALE_US + 50), "UAT");
    adsb_state_age_out(120000101, 120000000, NULL, NULL);
    LS_CHECK(!m->active && other->active);
}

LS_CASE(radar_scaled_anchor_high_latitude_and_prime_meridian_bitmap)
{
    uat_weather_reset();
    uint8_t r[8] = {0, 0xfc, 0, 0};
    unsigned number = 900 * 450 + 448;
    r[4] = number >> 16; r[5] = number >> 8; r[6] = number; r[7] = 0x10;
    LS_CHECK(uat_weather_apdu(r, sizeof(r), 1));
    uat_weather_block_t b;
    LS_EQ_INT(uat_weather_blocks_at(60.03, -0.5, 1, &b, 1), 1);
    LS_NEAR(b.width, 1.6, 0.00001);
    LS_EQ_INT(uat_weather_blocks_at(60.03, 0.5, 1, &b, 1), 1);
    LS_EQ_UINT(b.number, 900 * 450);
    /* CONUS medium resolution: northwest anchor row 679, column 300. */
    number = 679 * 450 + 300;
    r[0] = 1; r[1] = 0;
    r[4] = 0x10 | (number >> 16); r[5] = number >> 8; r[6] = number; r[7] = 0x10;
    LS_CHECK(uat_weather_apdu(r, sizeof(r), 2));
    LS_EQ_INT(uat_weather_blocks_at(45.1, -119, 2, &b, 1), 1);
    LS_NEAR(b.north, 680.0 / 15, 0.00001);
    LS_NEAR(b.height, 5.0 / 15, 0.00001);
    LS_EQ_INT(uat_weather_blocks_at(45.1, -115, 2, &b, 1), 1);
    LS_EQ_UINT(b.number, number + 5);
    r[6]--; /* column no longer aligned to a medium resolution anchor */
    LS_CHECK(!uat_weather_apdu(r, sizeof(r), 3));
}
