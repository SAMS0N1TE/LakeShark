#include "ls_test.h"
#include "ais.h"
#include "ais_store.h"
#include "fm_mode_label.h"
#include "esp_timer.h"
#include <stdlib.h>

static const char *v1 = "04574a67014007b5cf3d506cd4c609297500000000";
static const char *v5 = "14574a6700000000017103c72cf43012c54c80522c000000000000000046000000000000000023d350f38000000000000000000000";
static const char *v18 = "48574a6700004b2aea5401c9c3804d23d800000000";
static const char *v24a = "60574a67004c50605054d4000000000000000000";
static const char *v24b = "60574a6701250000000000010b1cb3d35000000000";
static unsigned unhex(const char *hex, uint8_t *out)
{
    unsigned n = strlen(hex)/2;
    for (unsigned i = 0; i < n; ++i) { char b[] = {hex[2*i], hex[2*i+1], 0}; out[i] = strtoul(b, NULL, 16); }
    return n;
}
static void put(uint8_t *p, unsigned at, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; ++i) {
        unsigned pos = at+i, mask = 1u<<(7-pos%8);
        p[pos/8] = (p[pos/8]&~mask) | (((v>>(n-i-1))&1) ? mask : 0);
    }
}
LS_CASE(known_position_static_and_class_b_vectors)
{
    uint8_t p[64]; ais_vessel_t o;
    unsigned n = unhex(v1, p); LS_CHECK(ais_parse(p, n*8, &o));
    LS_EQ_UINT(366123456, o.mmsi); LS_EQ_INT(5, o.nav_status);
    LS_CHECK(o.position); LS_NEAR(-33.5, o.lat, 1e-6); LS_NEAR(-122.5, o.lon, 1e-6);
    LS_EQ_INT(123, o.sog); LS_EQ_INT(2345, o.cog); LS_EQ_INT(234, o.heading);
    n = unhex(v5, p); LS_CHECK(ais_parse(p, n*8, &o));
    LS_EQ_STR("LAKESHARK", o.name); LS_EQ_STR("WDC1234", o.callsign);
    LS_EQ_INT(70, o.ship_type); LS_EQ_STR("BOSTON", o.destination);
    n = unhex(v18, p); LS_CHECK(ais_parse(p, n*8, &o));
    LS_NEAR(50, o.lat, 1e-6); LS_NEAR(150, o.lon, 1e-6);
    LS_EQ_INT(75, o.sog); LS_EQ_INT(1234, o.cog); LS_EQ_INT(123, o.heading);
    n = unhex(v24a, p); LS_CHECK(ais_parse(p, n*8, &o)); LS_EQ_STR("SEA TEST", o.name);
    n = unhex(v24b, p); LS_CHECK(ais_parse(p, n*8, &o));
    LS_EQ_INT(37, o.ship_type); LS_EQ_STR("AB12345", o.callsign);
    LS_CHECK(!ais_parse(p, 159, &o)); put(p, 38, 2, 2); LS_CHECK(!ais_parse(p, n*8, &o));
}
LS_CASE(coordinates_unavailable_and_message_lengths)
{
    uint8_t p[64]; ais_vessel_t o;
    unhex(v1, p); put(p, 61, 28, 181*600000); put(p, 89, 27, 91*600000);
    LS_CHECK(ais_parse(p, 168, &o)); LS_CHECK(!o.position);
    put(p, 61, 28, 180*600000); put(p, 89, 27, -90*600000);
    LS_CHECK(ais_parse(p, 168, &o)); LS_CHECK(o.position);
    LS_NEAR(-90, o.lat, 1e-6); LS_NEAR(180, o.lon, 1e-6);
    LS_CHECK(!ais_parse(p, 167, &o)); LS_CHECK(!ais_parse(p, 176, &o));
    for (unsigned t = 2; t <= 3; ++t) { put(p, 0, 6, t); LS_CHECK(ais_parse(p, 168, &o)); LS_EQ_INT(t, o.message); }
    memset(p, 0, sizeof(p)); put(p, 0, 6, 4); put(p, 8, 30, 123456789);
    put(p, 79, 28, -600000); put(p, 107, 27, 1200000);
    LS_CHECK(ais_parse(p, 168, &o)); LS_NEAR(2, o.lat, 1e-6); LS_NEAR(-1, o.lon, 1e-6);
    memset(p, 0, sizeof(p)); put(p, 0, 6, 19); put(p, 8, 30, 123456789);
    put(p, 57, 28, 600000); put(p, 85, 27, -1200000); put(p, 143, 6, 1); put(p, 263, 8, 30);
    LS_CHECK(ais_parse(p, 312, &o)); LS_EQ_STR("A", o.name); LS_EQ_INT(30, o.ship_type);
    LS_NEAR(-2, o.lat, 1e-6); LS_NEAR(1, o.lon, 1e-6);
}
static ais_vessel_t received[8];
static int received_count;
static void receive(const ais_vessel_t *p, void *user)
{ (void)user; if (received_count < 8) received[received_count++] = *p; }
/* Independent HDLC fixture: bytes go LSB first, zero is an NRZI transition. */
static uint16_t fixture_crc(const uint8_t *p, unsigned n)
{
    unsigned crc = 65535;
    for (unsigned j = 0; j < n; ++j) for (unsigned k = 0; k < 8; ++k) {
        unsigned feedback = (crc ^ (p[j]>>k)) & 1;
        crc >>= 1; if (feedback) crc ^= 0x8408;
    }
    return crc ^ 65535;
}
static unsigned frame_bits(const char *hex, uint8_t *out, bool corrupt)
{
    uint8_t data[128]; unsigned n = unhex(hex, data), at = 0, ones = 0;
    uint16_t fcs = fixture_crc(data, n); data[n++] = fcs; data[n++] = fcs>>8;
    if (corrupt) data[4] ^= 1;
    for (unsigned i = 0; i < 24; ++i) out[at++] = i&1;
    for (unsigned i = 0; i < 8; ++i) out[at++] = (0x7e>>i)&1;
    for (unsigned j = 0; j < n; ++j) for (unsigned i = 0; i < 8; ++i) {
        unsigned b = (data[j]>>i)&1; out[at++] = b;
        if (b) { if (++ones == 5) { out[at++] = 0; ones = 0; } } else ones = 0;
    }
    for (unsigned i = 0; i < 8; ++i) out[at++] = (0x7e>>i)&1;
    return at;
}
static void symbols(ais_ctx_t *ctx, const char *hex, bool corrupt, bool invert)
{
    uint8_t bits[1024]; unsigned n = frame_bits(hex, bits, corrupt); bool level = invert;
    for (unsigned i = 0; i < n; ++i) { if (!bits[i]) level = !level; ais_symbol(ctx, 0, level); }
}
LS_CASE(hdlc_nrzi_stuffing_crc_and_polarity)
{
    LS_EQ_UINT(0x906e, (uint16_t)~ais_fcs((const uint8_t *)"123456789", 9));
    ais_ctx_t *ctx = ais_create(receive, NULL); LS_CHECK(ctx); received_count = 0;
    symbols(ctx, v1, false, false); LS_EQ_INT(1, received_count);
    LS_EQ_UINT(366123456, received[0].mmsi);
    ais_reset(ctx); symbols(ctx, v1, true, false); LS_EQ_INT(1, received_count);
    ais_reset(ctx); symbols(ctx, v5, false, true); LS_EQ_INT(2, received_count); LS_EQ_STR("LAKESHARK", received[1].name);
    /* All-one field exercises repeated stuffed zeroes in the real frame. */
    uint8_t data[64]; unhex(v1, data); memset(data+5, 255, 14);
    char hex[43]; for (int i = 0; i < 21; ++i) snprintf(hex+2*i, 3, "%02x", data[i]);
    ais_reset(ctx); symbols(ctx, hex, false, false); LS_EQ_INT(3, received_count);
    ais_destroy(ctx);
}
/* Gaussian pulse shaping at BT=0.4, continuous phase with h=0.5. */
static float shaped(const uint8_t *bits, unsigned n, double t)
{
    int center = (int)floor(t); float sum = 0;
    const double a = 2*M_PI*0.4/sqrt(2*log(2.0));
    for (int j = center-3; j <= center+3; ++j) {
        int at = j < 0 ? 0 : j >= (int)n ? (int)n-1 : j;
        sum += (bits[at] ? 1 : -1)*0.5*(erf(a*(t-j))-erf(a*(t-j-1)));
    }
    return sum;
}
static void nrzi(uint8_t *p, unsigned n)
{
    bool level = false;
    for (unsigned i = 0; i < n; ++i) { if (!p[i]) level = !level; p[i] = level; }
}
static void gmsk(ais_ctx_t *ctx, bool dual, double offset, double clock_error, unsigned block)
{
    uint8_t a[1024], b[1024], iq[4096];
    unsigned na = frame_bits(v1, a, false), nb = frame_bits(v5, b, false);
    nrzi(a, na); nrzi(b, nb);
    double pa = 0, pb = 0;
    unsigned samples = (unsigned)((nb+16)*AIS_SAMPLE_RATE/9600.0), used = 0;
    for (unsigned i = 0; i < samples; ++i) {
        double t = (double)i*9600*(1+clock_error)/AIS_SAMPLE_RATE-3.27;
        pa += 2*M_PI*(-25000+offset+2400*shaped(a, na, t))/AIS_SAMPLE_RATE;
        pb += 2*M_PI*(25000-offset+2400*shaped(b, nb, t))/AIS_SAMPLE_RATE;
        double real = 127.5+45*cos(pa)+(dual ? 45*cos(pb) : 0);
        double imag = 127.5+45*sin(pa)+(dual ? 45*sin(pb) : 0);
        iq[used++] = (uint8_t)lround(real); iq[used++] = (uint8_t)lround(imag);
        if (used >= block) { ais_process(ctx, iq, used); used = 0; }
    }
    if (used) ais_process(ctx, iq, used);
}
LS_CASE(synthesized_dual_channel_gmsk_iq)
{
    ais_ctx_t *ctx = ais_create(receive, NULL); LS_CHECK(ctx);
    received_count = 0; gmsk(ctx, true, 0, 0, 4096);
    LS_EQ_INT(2, received_count);
    LS_EQ_INT(0, received[0].channel); LS_EQ_INT(1, received[1].channel);
    LS_EQ_UINT(366123456, received[0].mmsi); LS_EQ_STR("LAKESHARK", received[1].name);
    ais_reset(ctx); received_count = 0; gmsk(ctx, true, 450, 0.00008, 74);
    LS_EQ_INT(2, received_count); LS_NEAR(-33.5, received[0].lat, 1e-6);
    ais_destroy(ctx);
}
LS_CASE(vessel_merge_age_capacity_and_fresh_gps)
{
    uint8_t p[64]; ais_vessel_t v, out[AIS_VESSELS];
    ais_options_t o = {1, 10, true, true}; ais_options_set(&o);
    ls_shim_time_set(20000000); ais_store_clear();
    unsigned n = unhex(v1, p); ais_parse(p, n*8, &v); ais_store_receive(&v, NULL);
    ls_shim_time_advance(59000000);
    n = unhex(v5, p); ais_parse(p, n*8, &v); ais_store_receive(&v, NULL);
    LS_EQ_INT(1, ais_store_snapshot(out, AIS_VESSELS, esp_timer_get_time()));
    LS_EQ_STR("LAKESHARK", out[0].name); LS_CHECK(out[0].position); LS_EQ_INT(123, out[0].sog);
    double km, brg; int64_t now = esp_timer_get_time();
    LS_CHECK(!ais_visible(out, &o, now, true, 0, 0, now, &km, &brg));
    LS_CHECK(ais_visible(out, &o, now, true, 0, 0, now-11000000, &km, &brg)); LS_CHECK(km < 0);
    LS_CHECK(ais_visible(out, &o, now, true, -33.5, -122.5, now, &km, &brg)); LS_NEAR(0, km, 1e-6);
    ls_shim_time_advance(2000000); now = esp_timer_get_time();
    LS_CHECK(ais_visible(out, &o, now, true, 0, 0, now, &km, &brg)); LS_CHECK(km < 0);
    n = unhex(v1, p); put(p, 61, 28, 181*600000); ais_parse(p, n*8, &v); ais_store_receive(&v, NULL);
    ais_store_snapshot(out, AIS_VESSELS, now); LS_CHECK(!out[0].position); LS_EQ_STR("LAKESHARK", out[0].name);
    LS_EQ_INT(0, ais_store_snapshot(out, AIS_VESSELS, now+61000000));
    ais_store_clear(); n = unhex(v24a, p); ais_parse(p, n*8, &v); ais_store_receive(&v, NULL);
    n = unhex(v24b, p); ais_parse(p, n*8, &v); ais_store_receive(&v, NULL);
    ais_store_snapshot(out, AIS_VESSELS, now); LS_EQ_STR("SEA TEST", out[0].name); LS_EQ_STR("AB12345", out[0].callsign);
    for (unsigned i = 1; i < AIS_VESSELS+10; ++i) { v.mmsi = i; ais_store_receive(&v, NULL); }
    LS_EQ_INT(AIS_VESSELS, ais_store_snapshot(out, AIS_VESSELS, now)); LS_EQ_UINT(AIS_VESSELS+9, out[0].mmsi);
    o.keep_minutes = 30; o.max_km = 0; ais_options_set(&o); ais_store_clear();
}
LS_CASE(mode_id_labels_and_flipper_refusal)
{
    fm_mode_t mode;
    LS_EQ_INT(10, FM_MODE_AIS); LS_EQ_INT(11, FM_MODE_COUNT);
    LS_CHECK(fm_mode_parse("AIS", &mode)); LS_EQ_INT(FM_MODE_AIS, mode);
    LS_CHECK(fm_mode_parse("10", &mode)); LS_EQ_STR("AIS", fm_mode_label(mode)); LS_EQ_STR("ais", fm_mode_command_name(mode));
    LS_CHECK(!fm_mode_parse("6", &mode));
    LS_CHECK(!fm_mode_flipper_allowed(FM_MODE_AIS)); LS_CHECK(!fm_mode_flipper_allowed(FM_MODE_SAME));
    LS_CHECK(!fm_mode_flipper_allowed(FM_MODE_APRS)); LS_CHECK(fm_mode_flipper_allowed(FM_MODE_LISTEN));
}
