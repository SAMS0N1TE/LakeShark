#include "ls_test.h"
#include "rs41_decode.h"
#include "rs41_stream.h"
#include "rs41_store.h"
#include <string.h>
static rs41_decoder_t decoder;
static uint8_t frame[518];
/* Independent polynomial division encoder, GF(256), parity first. */
static uint8_t gm(uint8_t a, uint8_t b)
{
    unsigned x = a, v = 0;
    while (b) { if (b & 1) v ^= x; b >>= 1; x <<= 1; if (x & 256) x ^= 0x11d; }
    return (uint8_t)v;
}
static void parity(size_t n)
{
    uint8_t gen[25] = {1}, root = 1;
    for (int k = 0; k < 24; ++k) {
        for (int j = k+1; j >= 0; --j) gen[j] = (j ? gen[j-1] : 0) ^ gm(gen[j],root);
        root = gm(root,2);
    }
    for (int lane = 0; lane < 2; ++lane) {
        uint8_t cw[255] = {0};
        for (size_t i = 0; i < (n-56)/2; ++i) cw[24+i] = frame[56+2*i+lane];
        for (int i = 254; i >= 24; --i) {
            uint8_t lead = cw[i];
            for (int j = 0; j <= 24; ++j) cw[i-24+j] ^= gm(lead,gen[j]);
        }
        memcpy(frame+8+24*lane,cw,24);
    }
}
static size_t block(size_t at, uint8_t type, size_t len)
{
    frame[at] = type; frame[at+1] = (uint8_t)len;
    uint16_t crc = rs41_crc(frame+at+2,len);
    frame[at+2+len] = (uint8_t)crc; frame[at+3+len] = (uint8_t)(crc>>8);
    return at+len+4;
}
static void put32(size_t at, int32_t v)
{
    for (int i = 0; i < 4; ++i) frame[at+i] = (uint8_t)((uint32_t)v>>(8*i));
}
static void fixture(size_t n)
{
    memset(frame,0,sizeof(frame)); memcpy(frame,rs41_header,8); frame[56] = n == 320 ? 15 : 240;
    frame[59] = 42; memcpy(frame+61,"N1234567",8); frame[69] = 31;
    size_t at = block(57,0x79,40);
    for (int i = 0; i < 36; ++i) frame[at+2+i] = (uint8_t)(i+1);
    at = block(at,0x7a,42);
    at = block(at,0x7c,30); at = block(at,0x7d,89);
    put32(at+2,637913700); frame[at+14] = 250; frame[at+20] = 8;
    at = block(at,0x7b,21);
    /* Extended frames use multiple padding blocks to stay within byte lengths. */
    while (n-at > 259) at = block(at,0x76,255);
    block(at,0x76,n-at-4);
}
LS_CASE(standard_and_extended_correct_symbols)
{
    rs41_init(&decoder);
    for (int ext = 0; ext < 2; ++ext) {
        size_t n = ext ? 518 : 320; fixture(n); parity(n); rs41_whiten(frame,n);
        rs41_report_t r; LS_CHECK(rs41_decode(&decoder,frame,n,&r));
        LS_EQ_STR(r.serial,"N1234567"); LS_EQ_INT(r.frame,42); LS_CHECK(r.position && r.measurement && r.gps_info);
        LS_NEAR(r.alt,1000,0.02); LS_NEAR(r.climb,2.5,0.001); LS_NEAR(r.battery,3.1,0.001);
        frame[8] ^= 0x23; frame[33] ^= 0x71; frame[61] ^= 0xe2; frame[100] ^= 0x44;
        LS_CHECK(rs41_decode(&decoder,frame,n,&r)); LS_EQ_INT(r.corrected,4); LS_EQ_STR(r.serial,"N1234567");
        LS_NEAR(r.alt,1000,0.02);
    }
}
LS_CASE(bad_block_crc_is_not_telemetry)
{
    rs41_init(&decoder); fixture(320); frame[280] ^= 1; parity(320); rs41_whiten(frame,320);
    rs41_report_t r; LS_CHECK(rs41_decode(&decoder,frame,320,&r));
    LS_EQ_INT(r.bad_blocks,1); LS_CHECK(!r.position); LS_CHECK(r.status);
}
LS_CASE(encrypted_frame_exposes_no_telemetry)
{
    rs41_init(&decoder); fixture(320); block(101,0x80,167); parity(320); rs41_whiten(frame,320);
    rs41_report_t r; LS_CHECK(rs41_decode(&decoder,frame,320,&r));
    LS_CHECK(r.encrypted); LS_CHECK(!r.status && !r.position && !r.measurement && !r.gps_info);
    LS_EQ_STR(r.serial,"");
}
LS_CASE(ecef_known_coordinates)
{
    double lat,lon,alt;
    LS_CHECK(rs41_ecef(6378137,0,0,&lat,&lon,&alt)); LS_NEAR(lat,0,1e-8); LS_NEAR(lon,0,1e-8); LS_NEAR(alt,0,0.001);
    LS_CHECK(rs41_ecef(0,0,6356752.314245,&lat,&lon,&alt)); LS_NEAR(lat,90,1e-8); LS_NEAR(alt,0,0.001);
    LS_CHECK(rs41_ecef(4517590.878849,0,4487348.408866,&lat,&lon,&alt)); LS_NEAR(lat,45,1e-7); LS_NEAR(alt,0,0.001);
    LS_CHECK(!rs41_ecef(0,0,0,&lat,&lon,&alt)); LS_CHECK(!rs41_ecef(NAN,0,0,&lat,&lon,&alt));
}
LS_CASE(correction_limit_and_header_rejection)
{
    rs41_init(&decoder); fixture(320); parity(320); rs41_whiten(frame,320);
    for (int i = 0; i < 12; ++i) frame[8+i] ^= (uint8_t)(i+1);
    rs41_report_t r; LS_CHECK(rs41_decode(&decoder,frame,320,&r)); LS_EQ_INT(r.corrected,12);
    frame[20] ^= 67; LS_CHECK(!rs41_decode(&decoder,frame,320,&r));
    fixture(320); parity(320); rs41_whiten(frame,320); frame[0] ^= 1;
    LS_CHECK(!rs41_decode(&decoder,frame,320,&r)); LS_CHECK(!rs41_decode(&decoder,frame,319,&r));
    LS_EQ_UINT(rs41_crc((const uint8_t *)"123456789",9),0x29b1);
    uint8_t h[8]; memcpy(h,rs41_header,8); rs41_whiten(h,8);
    const uint8_t known[] = {0x10,0xb6,0xca,0x11,0x22,0x96,0x12,0xf8}; LS_CHECK(!memcmp(h,known,8));
}

static unsigned received;
static void took(const rs41_report_t *r, void *arg)
{
    (void)arg; ++received; LS_EQ_STR(r->serial,"N1234567");
}
static uint8_t reverse(uint8_t b)
{
    uint8_t v = 0; for (int j = 0; j < 8; ++j) { v = (uint8_t)((v<<1)|(b&1)); b >>= 1; } return v;
}
LS_CASE(stream_chunks_polarity_restart_and_resync)
{
    static rs41_stream_t stream;
    rs41_init(&decoder);
    for (int inv = 0; inv < 2; ++inv) for (int ext = 0; ext < 2; ++ext) {
        size_t n = ext ? 518 : 320; fixture(n); parity(n); rs41_whiten(frame,n);
        uint8_t radio[518]; for (size_t i = 0; i < n; ++i) radio[i] = reverse(frame[i])^(inv ? 255 : 0);
        received = 0; rs41_stream_reset(&stream,true);
        for (size_t i = 8; i < n;) {
            size_t take = n-i > 19 ? 19 : n-i;
            rs41_stream_feed(&stream,&decoder,radio+i,take,inv,took,NULL); i += take;
        }
        LS_EQ_INT(received,1);
        rs41_stream_feed(&stream,&decoder,radio,n,inv,took,NULL); LS_EQ_INT(received,2);
        rs41_stream_reset(&stream,false); rs41_stream_feed(&stream,&decoder,radio+40,n-40,inv,took,NULL);
        LS_EQ_INT(received,2);
        rs41_stream_feed(&stream,&decoder,radio,n,inv,took,NULL); LS_EQ_INT(received,3);
    }
}
LS_CASE(track_identity_retention_thinning_and_encryption)
{
    static rs41_sonde_t s;
    rs41_store_clear(); rs41_set_keep_hours(1);
    rs41_report_t r = {.status=true,.position=true,.lat=45,.lon=-71,.alt=1000,.climb=3};
    memcpy(r.serial,"N1234567",9);
    int64_t now = 1000000;
    for (int i = 0; i < 300; ++i) { r.frame = (uint16_t)i; rs41_store_put(&r,now); now += 10000000; }
    LS_CHECK(rs41_store_copy(0,&s,now)); LS_CHECK(s.count <= 128); LS_EQ_INT(s.track[0].us,1000000);
    int64_t fix = s.fix_us;
    r.position = false; ++r.frame; rs41_store_put(&r,now);
    LS_CHECK(rs41_store_copy(0,&s,now)); LS_EQ_INT(s.fix_us,fix); LS_CHECK(s.report.position);
    r.encrypted = true; rs41_store_put(&r,now+10000000);
    LS_CHECK(rs41_store_copy(0,&s,now)); LS_EQ_INT(s.heard_us,now);
    LS_CHECK(!rs41_store_copy(0,&s,now+3600000001LL));
    rs41_set_keep_hours(6);
}

LS_CASE(concurrent_snapshot_time_and_duplicate_fix_age)
{
    static rs41_sonde_t s;
    rs41_store_clear();
    rs41_report_t r = {.status=true,.position=true,.lat=45,.lon=-71,.alt=1000,.frame=1};
    memcpy(r.serial,"N1234567",9); rs41_store_put(&r,1000001);
    LS_CHECK(rs41_store_copy(0,&s,1000000)); LS_EQ_INT(s.fix_us,1000001);
    r.alt = 9999; rs41_store_put(&r,2000000);
    LS_CHECK(rs41_store_copy(0,&s,2000000)); LS_NEAR(s.report.alt,1000,0.01); LS_EQ_INT(s.fix_us,1000001);
    r.frame = 2; rs41_store_put(&r,3000000);
    LS_CHECK(rs41_store_copy(0,&s,3000000)); LS_NEAR(s.report.alt,9999,0.01); LS_EQ_INT(s.fix_us,3000000);
}

LS_CASE(both_interleaves_correct_twelve_symbols_at_arbitrary_positions)
{
    rs41_init(&decoder);
    for (int ext = 0; ext < 2; ++ext) {
        size_t n = ext ? 518 : 320;
        int symbols = 24+(int)(n-56)/2;
        for (int trial = 0; trial < 20; ++trial) {
            fixture(n); parity(n); rs41_whiten(frame,n);
            for (int lane = 0; lane < 2; ++lane) for (int j = 0; j < 12; ++j) {
                int pos = (trial*7+j*13)%symbols;
                int at = pos < 24 ? 8+24*lane+pos : 56+2*(pos-24)+lane;
                frame[at] ^= (uint8_t)(1+j*17);
            }
            rs41_report_t r; LS_CHECK(rs41_decode(&decoder,frame,n,&r)); LS_EQ_INT(r.corrected,24);
            LS_EQ_STR(r.serial,"N1234567"); LS_CHECK(r.position);
        }
    }
}
LS_CASE(western_hemisphere_position_and_radial_climb)
{
    rs41_init(&decoder); fixture(320);
    const double phi = 42*3.141592653589793/180, lam = -71*3.141592653589793/180;
    double v = 6378137/sqrt(1-0.0066943799901413165*sin(phi)*sin(phi));
    put32(276,(int32_t)lround((v+12000)*cos(phi)*cos(lam)*100));
    put32(280,(int32_t)lround((v+12000)*cos(phi)*sin(lam)*100));
    put32(284,(int32_t)lround((v*(1-0.0066943799901413165)+12000)*sin(phi)*100));
    const int16_t velocity[3] = {121,-351,335};
    for (int i = 0; i < 3; ++i) { frame[288+2*i] = (uint8_t)velocity[i]; frame[289+2*i] = (uint8_t)((uint16_t)velocity[i]>>8); }
    block(274,0x7b,21); parity(320); rs41_whiten(frame,320);
    rs41_report_t r; LS_CHECK(rs41_decode(&decoder,frame,320,&r)); LS_CHECK(r.position);
    LS_NEAR(r.lat,42,1e-7); LS_NEAR(r.lon,-71,1e-7); LS_NEAR(r.alt,12000,.01); LS_NEAR(r.climb,5,.01);
}
LS_CASE(stream_recovers_bit_alignment)
{
    static rs41_stream_t stream;
    uint8_t shifted[321] = {0};
    rs41_init(&decoder); fixture(320); parity(320); rs41_whiten(frame,320);
    /* Three unrelated bits before the first LSB-first frame byte. */
    shifted[0] = 0xa0;
    for (int i = 0; i < 320*8; ++i) if ((frame[i/8]>>(i%8))&1) shifted[(i+3)/8] |= (uint8_t)(0x80>>((i+3)%8));
    received = 0; rs41_stream_reset(&stream,false);
    rs41_stream_feed(&stream,&decoder,shifted,sizeof(shifted),false,took,NULL);
    LS_EQ_INT(received,1);
}
