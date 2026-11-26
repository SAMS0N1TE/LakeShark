/* PARTY's wire format (components/apps/tui/party/party_proto.h): every
   datagram type round trips, damaged and foreign datagrams are refused, the
   party code gives the same key however it is typed, and a file cut into
   chunks comes back whole over a lossy air with ACKs and resends. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "party/party_proto.h"
#include "wardrive/wd_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void hex(const uint8_t *b, int n, char *o) { for (int i = 0; i < n; i++) sprintf(o + i * 2, "%02x", b[i]); }

static void test_hashes(void)
{
    uint8_t d[32];
    char h[65];
    party_sha256("abc", 3, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    party_sha256("", 0, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const char *l = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    party_sha256(l, strlen(l), d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK(party_crc32(0, "123456789", 9) == 0xCBF43926u);
}

static void test_key(void)
{
    party_key_t a, b, c;
    char n[32];
    party_code_norm("fox-den 7!", n, sizeof(n));
    CHECK(!strcmp(n, "FOXDEN7"));
    CHECK(party_key_derive("fox den 7", &a));
    CHECK(party_key_derive("FOX-DEN-7", &b));
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK(party_key_derive("fox den 8", &c));
    CHECK(memcmp(a.key, c.key, 16) != 0);
    CHECK(a.channel < PARTY_CHANNELS);
    CHECK(!party_key_derive("a-b", &c));
    CHECK(!party_key_derive("", &c));
    /* ids spread: 64 codes, few collisions */
    uint16_t ids[64];
    int same = 0;
    for (int i = 0; i < 64; i++) {
        char code[16];
        snprintf(code, sizeof(code), "camp%03d", i);
        party_key_derive(code, &c);
        ids[i] = c.party;
        for (int j = 0; j < i; j++) same += ids[j] == ids[i];
    }
    CHECK(same <= 1);
}

static void test_roundtrip(void)
{
    uint8_t buf[PARTY_DGRAM_MAX];
    party_hdr_t h = { 0, 0x21, PARTY_ALL, 7, 0xBEEF }, g;
    party_msg_t m;

    party_hello_t he = { "KB1QWE", '#', 3 | 8, PARTY_CAP_LORA | PARTY_CAP_FLRC, PARTY_SHARE_ROUGH, 77, 0, 0x8123 };
    int n = party_enc_hello(buf, sizeof(buf), &h, &he);
    CHECK(n == PARTY_HDR + 16);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_HELLO);
    CHECK(g.src == 0x21 && g.dst == PARTY_ALL && g.seq == 7);
    CHECK(!strcmp(m.hello.call, "KB1QWE") && m.hello.glyph == '#' && m.hello.want == 0x8123 && m.hello.batt == 77);
    CHECK(party_decode(buf, n, 0xBEEE, &g, &m) == PARTY_E_PARTY);
    CHECK(party_decode(buf, n - 1, 0xBEEF, &g, &m) == PARTY_E_BODY);
    CHECK(party_decode(buf, 3, 0xBEEF, &g, &m) == PARTY_E_SHORT);
    buf[0] = (uint8_t)((2 << 5) | PARTY_HELLO);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_E_VERSION);

    party_pos_t po = { party_deg_e7(43.4445123), party_deg_e7(-71.6473456), 12, 9, 64, 200, 55, PARTY_POS_MOVING | PARTY_POS_COURSE };
    n = party_enc_pos(buf, sizeof(buf), &h, &po);
    CHECK(n == PARTY_HDR + 15);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_POS);
    CHECK(m.pos.lat_e7 == po.lat_e7 && m.pos.lon_e7 == po.lon_e7 && m.pos.course == 64 && m.pos.flags == po.flags);
    CHECK(party_e7_deg(m.pos.lon_e7) < -71.64734 && party_e7_deg(m.pos.lon_e7) > -71.64735);

    party_ping_t pi;
    memset(&pi, 0, sizeof(pi));
    pi.id = 513; pi.cls = PARTY_C_ALPR; pi.kind = WD_WIFI; pi.tags = WD_T_ALPR | WD_T_CAMERA; pi.rssi = -71;
    pi.flags = PARTY_PF_PLACED; pi.lat_e7 = po.lat_e7; pi.lon_e7 = po.lon_e7; pi.unc_m = 60; pi.age_s = 12;
    memcpy(pi.addr, "\xb4\x1e\x52\x01\x02\x03", 6);
    memset(pi.label, 'F', PARTY_LABEL_MAX);   /* a full label */
    n = party_enc_ping(buf, sizeof(buf), &h, &pi);
    CHECK(n == PARTY_HDR + 27 + PARTY_LABEL_MAX);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_PING);
    CHECK(m.ping.id == 513 && m.ping.cls == PARTY_C_ALPR && m.ping.rssi == -71 && m.ping.unc_m == 60);
    CHECK(!memcmp(m.ping.addr, pi.addr, 6) && strlen(m.ping.label) == PARTY_LABEL_MAX);
    buf[PARTY_HDR + 2] = PARTY_CLASSES;                  /* a class from the future is refused */
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_E_BODY);

    party_offer_t of = { 0x55AA, 4000, 0x12345678, 100, "notes.txt" };
    n = party_enc_offer(buf, sizeof(buf), &h, &of);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_OFFER);
    CHECK(m.offer.size == 4000 && m.offer.crc == 0x12345678 && !strcmp(m.offer.name, "notes.txt"));

    uint8_t data[180];
    for (int i = 0; i < 180; i++) data[i] = (uint8_t)i;
    party_chunk_t ch = { 0x55AA, 39, 180, data };
    n = party_enc_chunk(buf, sizeof(buf), &h, &ch);
    CHECK(n == PARTY_HDR + 5 + 180 && n <= PARTY_DGRAM_MAX);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_CHUNK);
    CHECK(m.chunk.idx == 39 && m.chunk.len == 180 && !memcmp(m.chunk.data, data, 180));
    CHECK(party_decode(buf, n - 10, 0xBEEF, &g, &m) == PARTY_E_BODY);
    ch.len = 200;                                       /* too big for a datagram */
    CHECK(party_enc_chunk(buf, sizeof(buf), &h, &ch) == -1);

    party_ack_t ak = { 0x55AA, 17, 0xF0F0F0F0u, PARTY_ACK_DONE };
    n = party_enc_ack(buf, sizeof(buf), &h, &ak);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_ACK);
    CHECK(m.ack.base == 17 && m.ack.bits == 0xF0F0F0F0u && m.ack.flags == PARTY_ACK_DONE);

    n = party_enc_bye(buf, sizeof(buf), &h);
    CHECK(n == PARTY_HDR && party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_BYE);
    buf[0] = (uint8_t)((PARTY_PROTO_VER << 5) | 30);
    CHECK(party_decode(buf, n, 0xBEEF, &g, &m) == PARTY_E_TYPE);
    CHECK(party_enc_pos(buf, 10, &h, &po) == -1);
}

static void test_classes(void)
{
    const char kw[3][13] = { "", "bodycam", "AXON" };
    CHECK(party_class_of(WD_T_ALPR | WD_T_CAMERA, "x", NULL, 0) == PARTY_C_ALPR);
    CHECK(party_class_of(WD_T_VEHICLE | WD_T_POLICE, "x", NULL, 0) == PARTY_C_POLICE);
    CHECK(party_class_of(WD_T_CAMERA, "x", NULL, 0) == PARTY_C_CAMERA);
    CHECK(party_class_of(0, "Axon Body 3 X9", kw, 3) == PARTY_C_CUSTOM);
    CHECK(party_class_of(0, "Ring Doorbell", kw, 3) == -1);
    CHECK(party_class_of(0, "Ring Doorbell", NULL, 0) == -1);
    CHECK(!strcmp(party_class_tag(PARTY_C_DRONE), "UAS"));
    CHECK(party_rough(43.4445, 0.01) > 43.444 && party_rough(43.4445, 0.01) < 43.446);
    CHECK(party_chunk_size(255) == 180 && party_chunk_size(64) == 64 && party_chunk_size(120) == 109);
}

/* Sender and two receivers over an air that loses one chunk in five for
   the first, and every third for the second. */
static void test_files(void)
{
    enum { SIZE = 9999, CHUNK = 100 };
    static uint8_t src[SIZE], dst[2][SIZE];
    for (int i = 0; i < SIZE; i++) src[i] = (uint8_t)(i * 31 + 7);
    party_offer_t o = { 0x1234, SIZE, party_crc32(0, src, SIZE), CHUNK, "map.pmt" };
    party_frx_t rx[2];
    party_ftx_t tx;
    CHECK(party_frx_begin(&rx[0], &o) && party_frx_begin(&rx[1], &o));
    CHECK(rx[0].nchunks == 100);
    party_ftx_begin(&tx, o.fid, o.size, o.chunk);
    CHECK(party_ftx_pending(&tx) == 100);
    int sent = 0, rounds = 0;
    unsigned lose = 0;
    while (!(party_frx_complete(&rx[0]) && party_frx_complete(&rx[1])) && rounds < 40) {
        rounds++;
        int idx;
        while ((idx = party_ftx_next(&tx)) >= 0) {
            sent++;
            const uint32_t off = (uint32_t)idx * CHUNK;
            party_chunk_t c = { o.fid, (uint16_t)idx, (uint8_t)(SIZE - off < CHUNK ? SIZE - off : CHUNK), src + off };
            for (int r = 0; r < 2; r++) {
                lose++;
                if (r == 0 && lose % 5 == 0) continue;
                if (r == 1 && lose % 3 == 0) continue;
                uint32_t at;
                if (party_frx_put(&rx[r], &c, &at) == 1) memcpy(dst[r] + at, c.data, c.len);
            }
        }
        for (int r = 0; r < 2; r++) {
            party_ack_t a;
            party_frx_ack(&rx[r], &a);
            party_ftx_ack(&tx, &a);
        }
    }
    CHECK(party_frx_complete(&rx[0]) && party_frx_complete(&rx[1]));
    CHECK(!memcmp(dst[0], src, SIZE) && !memcmp(dst[1], src, SIZE));
    CHECK(party_crc32(0, dst[1], SIZE) == o.crc);
    printf("files: %d chunks sent for 100 over %d rounds\n", sent, rounds);
    CHECK(sent < 300);
    /* the last chunk is short; a wrong length or index is refused, a repeat is a duplicate */
    party_chunk_t bad = { o.fid, 99, 100, src };
    CHECK(party_frx_put(&rx[0], &bad, NULL) == -1);
    bad.idx = 100; bad.len = 99;
    CHECK(party_frx_put(&rx[0], &bad, NULL) == -1);
    bad.idx = 99;
    CHECK(party_frx_put(&rx[0], &bad, NULL) == 0);
    party_ack_t a;
    party_frx_ack(&rx[0], &a);
    CHECK(a.flags & PARTY_ACK_DONE);
    party_offer_t big = o;
    big.size = PARTY_FILE_MAX + 1;
    CHECK(!party_frx_begin(&rx[0], &big));
    big.size = 100; big.chunk = 10;
    CHECK(!party_frx_begin(&rx[0], &big));
}

int main(void)
{
    test_hashes();
    test_key();
    test_roundtrip();
    test_classes();
    test_files();
    if (fails) { printf("test_party_proto: %d failed\n", fails); return 1; }
    printf("test_party_proto: ok\n");
    return 0;
}
