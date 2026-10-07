#include "ls_test.h"
#include "aprs.h"
#include "aprs_store.h"
#include "fm_dsp.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static aprs_packet_t heard;
static int received;
static void receive(const aprs_packet_t *p, void *user) { (void)user; heard = *p; ++received; }
static bool parse(const char *dst, const char *text, aprs_packet_t *out)
{ return aprs_parse("N0CALL-7", dst, (const uint8_t *)text, strlen(text), out); }
static size_t frame(uint8_t *out, const char *info)
{
    const char *calls[] = {"APRS  ", "N0CALL"};
    for (int a = 0; a < 2; ++a) {
        for (int i = 0; i < 6; ++i) out[a*7+i] = calls[a][i] << 1;
        out[a*7+6] = a ? 0x6f : 0x60;
    }
    out[14] = 3; out[15] = 0xf0;
    size_t n = strlen(info); memcpy(out+16, info, n); n += 16;
    uint16_t crc = aprs_crc(out, n) ^ 0xffff;
    out[n++] = crc & 255; out[n++] = crc >> 8; return n;
}
static uint8_t bits[6000];
static int nbits;
static void flag(void) { for (int i = 0; i < 8; ++i) bits[nbits++] = (0x7e >> i) & 1; }
static int encode(const uint8_t *p, size_t n)
{
    nbits = 0; for (int i = 0; i < 32; ++i) flag();
    int ones = 0, stuffed = 0;
    for (size_t i = 0; i < n; ++i) for (int j = 0; j < 8; ++j) {
        unsigned b = (p[i] >> j) & 1; bits[nbits++] = b; ones = b ? ones+1 : 0;
        if (ones == 5) { bits[nbits++] = 0; ones = 0; ++stuffed; }
    }
    for (int i = 0; i < 4; ++i) flag();
    return stuffed;
}
LS_CASE(stuffed_hdlc_and_corrupt_fcs)
{
    uint8_t p[512]; size_t n = frame(p, ">~~~~~ ready");
    received = 0; aprs_ctx_t *s = aprs_create(receive, NULL); LS_CHECK(s != NULL);
    LS_CHECK(encode(p, n) > 0);
    for (int i = 0; i < nbits; ++i) aprs_bit(s, bits[i]);
    LS_EQ_INT(1, received); LS_EQ_STR("N0CALL-7", heard.call); LS_EQ_STR("~~~~~ ready", heard.text);
    p[18] ^= 1; aprs_reset(s); encode(p, n);
    for (int i = 0; i < nbits; ++i) aprs_bit(s, bits[i]);
    LS_EQ_INT(1, received); LS_CHECK(!aprs_frame(s, p, n)); aprs_destroy(s);
}
LS_CASE(synthesized_bell202_stream_across_blocks_and_phases)
{
    uint8_t p[512]; size_t n = frame(p, "!4903.50N/07201.75W>Mobile"); encode(p, n);
    for (int offset = 0; offset < 13; offset += 3) {
        received = 0; aprs_ctx_t *s = aprs_create(receive, NULL); LS_CHECK(s != NULL);
        int16_t pcm[37]; int used = 0, tone = 1; double phase = 0;
        int total = (int)ceil(nbits*16000.0/1200)+offset;
        int last = -1;
        for (int k = 0; k < total; ++k) {
            int b = (int)floor((k-offset)*1200.0/16000);
            if (b >= 0 && b < nbits && b != last) { if (!bits[b]) tone ^= 1; last = b; }
            phase += 6.283185307179586*(tone ? 1200 : 2200)/16000;
            pcm[used++] = k < offset ? 0 : (int16_t)(14000*sin(phase+0.7));
            if (used == 37) { aprs_process(s, pcm, used); used = 0; }
        }
        aprs_process(s, pcm, used);
        LS_EQ_INT(1, received); LS_CHECK(fabs(heard.lat-49.0583333) < 0.00001);
        LS_CHECK(fabs(heard.lon+72.0291667) < 0.00001); aprs_destroy(s);
    }
}
LS_CASE(compressed_and_mice_positions)
{
    /* APRS101's compressed coordinate example: 49.5 N, 72.75 W. */
    aprs_packet_t p;
    LS_CHECK(parse("APRS", "!/5L!!<*e7>7P[comment", &p));
    LS_CHECK(fabs(p.lat-49.5) < 0.00001); LS_CHECK(fabs(p.lon+72.75) < 0.00001);
    /* Destination digits 490350, north, no +100 offset, west. */
    const uint8_t mic[] = {'`', 100, 29, 103, 28, 28, 28, '>', '/'};
    LS_CHECK(aprs_parse("N0CALL", "TYPS5P", mic, sizeof(mic), &p));
    LS_CHECK(fabs(p.lat-49.0583333) < 0.00001); LS_CHECK(fabs(p.lon+72.0291667) < 0.00001);
    LS_EQ_INT('>', p.symbol);
    LS_CHECK(parse("APRS", "!4903.5 N/07201.7 W>", &p)); LS_CHECK(p.ambiguous);
    LS_CHECK(fabs(p.lat-49.0591667) < 0.00001);
    LS_CHECK(parse("APRS", "!d5L!!<*e7>7P[overlay", &p)); LS_EQ_INT('d', p.table);
    LS_CHECK(!parse("APRS", "!/5L!!<*~7>7P[", &p));
    LS_CHECK(!parse("APRS", "!4903.50N/07261.75W>", &p));
}
LS_CASE(bell202_through_the_firmware_audio_conversion)
{
    uint8_t p[512]; size_t n = frame(p, "!4903.50N/07201.75W>through NFM audio"); encode(p, n);
    static fm_dsp_t dsp; fm_dsp_init(&dsp);
    received = 0; aprs_ctx_t *s = aprs_create(receive, NULL); LS_CHECK(s != NULL);
    float demod[37]; int16_t pcm[20]; int used = 0, tone = 1, last = -1;
    double phase = 0;
    int total = (int)ceil(nbits*32000.0/1200);
    for (int k = 0; k < total; ++k) {
        int b = (int)floor(k*1200.0/32000);
        if (b < nbits && b != last) { if (!bits[b]) tone ^= 1; last = b; }
        phase += 6.283185307179586*(tone ? 1200 : 2200)/32000;
        demod[used++] = 0.8*sin(phase)+0.2;
        if (used == 37) {
            int na = fm_demod_to_audio(&dsp, demod, used, pcm, 20);
            aprs_process(s, pcm, na); used = 0;
        }
    }
    int na = fm_demod_to_audio(&dsp, demod, used, pcm, 20); aprs_process(s, pcm, na);
    LS_EQ_INT(1, received); LS_EQ_STR("through NFM audio", heard.text); aprs_destroy(s);
}
LS_CASE(weather_objects_items_messages_and_status)
{
    aprs_packet_t p;
    LS_CHECK(parse("APRS", "!4903.50N/07201.75W_180/010g020t-05r000h00b10132", &p));
    LS_CHECK(p.weather && p.position && p.temperature_valid && p.humidity_valid && p.wind_valid);
    LS_EQ_INT(-5, p.temperature_f); LS_EQ_INT(100, p.humidity); LS_EQ_INT(10, p.wind_mph);
    LS_CHECK(p.gust_valid && p.rain_valid && p.pressure_valid);
    LS_EQ_INT(20, p.gust_mph); LS_EQ_INT(0, p.rain_hour_hundredths); LS_EQ_INT(10132, p.pressure_tenths_hpa);
    LS_CHECK(parse("APRS", "_10071234c090s005t072h50", &p)); LS_CHECK(p.weather && !p.position);
    LS_CHECK(parse("APRS", ";FIRE     *071234z4903.50N/07201.75W>scene", &p));
    LS_EQ_STR("FIRE", p.name); LS_EQ_INT(APRS_OBJECT, p.kind); LS_CHECK(!p.killed);
    LS_CHECK(parse("APRS", ")FIRE_4903.50N/07201.75W>ended", &p)); LS_CHECK(p.killed);
    LS_CHECK(parse("APRS", ":ANYONE   :hello{12", &p)); LS_EQ_STR("ANYONE", p.recipient);
    LS_EQ_STR("hello{12", p.text); LS_CHECK(parse("APRS", ">available", &p)); LS_EQ_INT(APRS_STATUS, p.kind);
    for (size_t n = 0; n < 19; ++n)
        LS_CHECK(!aprs_parse("N0CALL", "APRS", (const uint8_t *)"!4903.50N/07201.75W>", n, &p));
}
LS_CASE(capped_station_store_age_and_fresh_gps_limits)
{
    ls_shim_time_set(20000000);
    aprs_store_clear(); aprs_options_t o; aprs_options_get(&o);
    o.keep_minutes = 1; o.max_km = 10; o.messages = true; aprs_options_set(&o);
    aprs_packet_t p; LS_CHECK(parse("APRS", "!4903.50N/07201.75W>mobile", &p));
    for (int i = 0; i < APRS_STATIONS+5; ++i) {
        snprintf(p.call, sizeof(p.call), "CALL%d", i); aprs_store_receive(&p, NULL);
    }
    int64_t now = esp_timer_get_time(); LS_EQ_INT(APRS_STATIONS, aprs_store_count(now));
    LS_CHECK(aprs_store_get(0, now, &p)); LS_EQ_STR("CALL68", p.call);
    double km, brg;
    LS_CHECK(!aprs_visible(&p, &o, now, true, 0, 0, now, &km, &brg));
    LS_CHECK(aprs_visible(&p, &o, now, true, 0, 0, now-11000000, &km, &brg)); LS_CHECK(km < 0);
    LS_CHECK(aprs_visible(&p, &o, now, true, p.lat, p.lon, now, &km, &brg)); LS_CHECK(km < 0.001);
    LS_EQ_INT(0, aprs_store_count(now+61000000));
    o.max_km = 0; o.keep_minutes = 30; aprs_options_set(&o); aprs_store_clear();
}
LS_CASE(messages_preserve_position_but_cannot_refresh_its_age_and_objects_can_end)
{
    ls_shim_time_set(20000000); aprs_store_clear();
    aprs_options_t o; aprs_options_get(&o); o.keep_minutes = 1; o.messages = true; aprs_options_set(&o);
    aprs_packet_t p;
    LS_CHECK(parse("APRS", "!4903.50N/07201.75W>mobile", &p)); aprs_store_receive(&p, NULL);
    ls_shim_time_advance(59000000);
    LS_CHECK(parse("APRS", ":ANYONE   :still listening", &p)); aprs_store_receive(&p, NULL);
    LS_CHECK(aprs_store_get(0, esp_timer_get_time(), &p)); LS_CHECK(p.position); LS_EQ_INT(APRS_MESSAGE, p.kind);
    double km;
    ls_shim_time_advance(2000000);
    LS_CHECK(aprs_visible(&p, &o, esp_timer_get_time(), true, p.lat, p.lon, esp_timer_get_time(), &km, NULL));
    LS_CHECK(km < 0);
    o.messages = false; aprs_options_set(&o);
    LS_CHECK(!aprs_visible(&p, &o, esp_timer_get_time(), false, 0, 0, 0, NULL, NULL));
    LS_CHECK(parse("APRS", ";FIRE     *071234z4903.50N/07201.75W>scene", &p)); aprs_store_receive(&p, NULL);
    LS_EQ_INT(2, aprs_store_count(esp_timer_get_time()));
    LS_CHECK(parse("APRS", ";FIRE     _071234z4903.50N/07201.75W>ended", &p)); aprs_store_receive(&p, NULL);
    LS_EQ_INT(1, aprs_store_count(esp_timer_get_time()));
    o.messages = true; o.keep_minutes = 30; aprs_options_set(&o); aprs_store_clear();
}
