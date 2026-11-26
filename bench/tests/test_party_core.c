/* PARTY's core (components/apps/tui/party/party_core.c) over the loopback
   link: a member's HELLO and POS make a roster entry, a ping it sends lands
   in the feed with an alert in the profile's style, a second member
   reporting the same car merges into one entry, classes this profile does
   not want are dropped, the board's own find goes out as a PING, a MARK
   needs a fix, and a file offered by a member arrives whole on the card. */
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "party/party_core.h"
#include "party/party_io.h"
#include "party/party_proto.h"
#include "party/party_wd_peer.h"
#include "wardrive/wd_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- the board, as the test wants it ---- */
static uint32_t s_ms = 1000000;
static bool s_fix = true;
static int s_buzz;
static char s_dir[260];
uint32_t party_io_ms(void) { return s_ms; }
bool party_io_fix(party_fix_t *f)
{
    f->fix = s_fix; f->lat = 43.0; f->lon = -71.0; f->acc_m = 5; f->speed_mps = 0; f->course_deg = -1; f->heading_deg = -1;
    return s_fix;
}
int party_io_batt(void) { return 50; }
bool party_io_vibe(int ms, int strength) { (void)ms; (void)strength; s_buzz++; return true; }
bool party_io_vibe_present(void) { return true; }
int party_io_local_hour(void) { return 12; }
const char *party_io_dir(void) { return s_dir; }
bool party_io_card(void) { return true; }
bool party_io_mkdir(const char *p) { return _mkdir(p) == 0; }
void *party_io_alloc(size_t n) { return calloc(1, n); }
void party_io_lock(void) { }
void party_io_unlock(void) { }
bool party_io_spawn(int (*fn)(void)) { (void)fn; return true; }
bool party_io_worker_alive(void) { return false; }
const party_link_ops_t *party_io_link(void) { return party_link_loop(); }
uint32_t party_io_random(void) { static uint32_t r = 7; r = r * 1664525u + 1013904223u; return r >> 8; }
int party_wd_poll(party_find_t *out, int max, uint32_t since_ms,
                  bool (*want)(uint16_t, const char *, const uint8_t *, uint8_t), bool *running, bool *hooked)
{
    (void)out; (void)max; (void)since_ms; (void)want;
    *running = false; *hooked = true;
    return 0;
}

static uint16_t s_party;
static void run(int ms) { for (int t = 0; t < ms; t += 100) { s_ms += 100; party_tick(); } }
static void inject(uint8_t *b, int n) { CHECK(n > 0); party_loop_inject(b, n, -80, 6); }
static party_hdr_t H(uint8_t src, uint8_t seq) { party_hdr_t h = { 0, src, PARTY_ALL, seq, s_party }; return h; }

static int drain(int type, party_msg_t *last, party_hdr_t *lh)
{
    uint8_t d[PARTY_DGRAM_MAX];
    int n, count = 0;
    bool bulk;
    while (party_loop_take(d, &n, &bulk)) {
        party_hdr_t h;
        party_msg_t m;
        if (party_decode(d, n, s_party, &h, &m) == type) { count++; if (last) *last = m; if (lh) *lh = h; }
    }
    return count;
}

static const party_pingrec_t *ping_labeled(const char *label)
{
    const party_t *p = party_state();
    for (int i = 0; i < PARTY_PINGS; i++) if (p->ping[i].used && !strcmp(p->ping[i].label, label)) return &p->ping[i];
    return NULL;
}

int main(void)
{
    snprintf(s_dir, sizeof(s_dir), "%s/test_party_core", getenv("TEMP") ? getenv("TEMP") : ".");
    _mkdir(s_dir);
    char path[300];
    snprintf(path, sizeof(path), "%s/profiles.bin", s_dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/notes.txt", s_dir);
    remove(path);
    CHECK(party_init());
    CHECK(!party_start());                       /* no profile yet */
    const int pi = party_prof_new("TEST");
    party_prof_t *pr = party_prof(pi);
    snprintf(pr->code, sizeof(pr->code), "fox den 7");
    snprintf(pr->call, sizeof(pr->call), "ME");
    pr->recv_mask &= (uint16_t)~PARTY_CMASK(PARTY_C_SKIMMER);
    pr->recv_mask |= PARTY_CMASK(PARTY_C_VEHICLE);
    pr->style[PARTY_C_VEHICLE] = PARTY_STYLE_BANNER;
    pr->style[PARTY_C_ALPR] = PARTY_STYLE_VIBE;
    party_key_t k;
    CHECK(party_key_derive("fox den 7", &k));
    s_party = k.party;
    CHECK(party_start());
    run(500);
    const party_t *st = party_state();
    CHECK(st->running && st->link_up);
    CHECK(drain(PARTY_HELLO, NULL, NULL) >= 1);  /* a burst of HELLOs on joining */

    /* a member arrives */
    uint8_t b[PARTY_DGRAM_MAX];
    party_hello_t he = { "KB1QWE", '#', 1, PARTY_CAP_LORA | PARTY_CAP_GFSK, PARTY_SHARE_EXACT, 77, 0, 0xFFFF };
    party_hdr_t h = H(0x31, 1);
    inject(b, party_enc_hello(b, sizeof(b), &h, &he));
    party_pos_t po = { party_deg_e7(43.001), party_deg_e7(-71.0), 5, 3, 0, 0, 77, PARTY_POS_MOVING };
    h = H(0x31, 3);                               /* seq 2 lost */
    inject(b, party_enc_pos(b, sizeof(b), &h, &po));
    run(200);
    CHECK(party_member_count() == 1);
    const party_member_t *m = &st->member[0];
    CHECK(!strcmp(m->call, "KB1QWE") && m->pos && m->lost == 1 && m->batt == 77);
    float d, br;
    party_geo(st->lat, st->lon, m->lat, m->lon, &d, &br);
    CHECK(d > 100 && d < 120 && br < 1);

    /* a ping: stored, alerted by vibration only (no banner) */
    char t[24], body[128];
    uint8_t hue;
    bool quiet;
    while (party_notice_pop(t, sizeof(t), body, sizeof(body), &hue, &quiet)) { }
    party_ping_t pg;
    memset(&pg, 0, sizeof(pg));
    pg.id = 1; pg.cls = PARTY_C_ALPR; pg.kind = WD_WIFI; pg.tags = WD_T_ALPR; pg.rssi = -70;
    pg.lat_e7 = party_deg_e7(43.002); pg.lon_e7 = party_deg_e7(-71.0); pg.unc_m = 40; pg.age_s = 10;
    snprintf(pg.label, sizeof(pg.label), "Flock-1");
    h = H(0x31, 4);
    inject(b, party_enc_ping(b, sizeof(b), &h, &pg));
    s_buzz = 0;
    run(300);
    const party_pingrec_t *r = ping_labeled("Flock-1");
    CHECK(r && r->from == 0x31 && r->cls == PARTY_C_ALPR && st->pings_in == 1);
    CHECK(r && st->start_ms && (s_ms - r->seen_ms) >= 10000);
    CHECK(s_buzz >= 2);                           /* DOUBLE */
    CHECK(!party_notice_pop(t, sizeof(t), body, sizeof(body), &hue, &quiet));
    /* the repeat of the same id is ignored */
    pg.flags = PARTY_PF_REPEAT;
    h = H(0x31, 5);
    inject(b, party_enc_ping(b, sizeof(b), &h, &pg));
    run(200);
    CHECK(st->pings_in == 1);

    /* a class this profile does not take is dropped */
    pg.id = 2; pg.cls = PARTY_C_SKIMMER; pg.flags = 0;
    snprintf(pg.label, sizeof(pg.label), "Skim");
    h = H(0x31, 6);
    inject(b, party_enc_ping(b, sizeof(b), &h, &pg));
    run(200);
    CHECK(!ping_labeled("Skim"));

    /* the same car from two members: one entry, two reporters, a banner each time */
    pg.id = 3; pg.cls = PARTY_C_VEHICLE; pg.kind = WD_RF; pg.tags = WD_T_VEHICLE;
    memcpy(pg.addr, "\x00\x00\x00\x3a\x91\xf2", 6);
    snprintf(pg.label, sizeof(pg.label), "TPMS 3a91f2");
    h = H(0x31, 7);
    inject(b, party_enc_ping(b, sizeof(b), &h, &pg));
    run(200);
    CHECK(party_notice_pop(t, sizeof(t), body, sizeof(body), &hue, &quiet) && quiet && strstr(body, "VEHICLE"));
    he.call[0] = 'V'; he.call[1] = '7'; he.call[2] = 0;
    h = H(0x47, 1);
    inject(b, party_enc_hello(b, sizeof(b), &h, &he));
    pg.id = 1; pg.lat_e7 = party_deg_e7(43.010);
    h = H(0x47, 2);
    inject(b, party_enc_ping(b, sizeof(b), &h, &pg));
    run(200);
    r = ping_labeled("TPMS 3a91f2");
    CHECK(r && r->nrep == 2 && r->spread_m > 800 && r->spread_m < 950);
    int n = 0;
    for (int i = 0; i < PARTY_PINGS; i++) n += st->ping[i].used && !strcmp(st->ping[i].label, "TPMS 3a91f2");
    CHECK(n == 1);
    bool again = false;
    while (party_notice_pop(t, sizeof(t), body, sizeof(body), &hue, &quiet)) again |= strstr(body, "AGAIN (2)") != NULL;
    CHECK(again);
    CHECK(party_member_count() == 2 && !strcmp(st->member[1].call, "V7"));

    /* this board's own find goes out as a PING, once, and again as a repeat */
    drain(0, NULL, NULL);
    party_find_t f;
    memset(&f, 0, sizeof(f));
    memcpy(f.addr, "\xb4\x1e\x52\x77\xb1\xe0", 6);
    f.kind = WD_WIFI; f.tags = WD_T_ALPR; f.rssi = -60; f.ms = s_ms;
    snprintf(f.name, sizeof(f.name), "Flock-mine");
    party_offer_find(&f);
    party_offer_find(&f);                         /* the same device again: held back */
    run(300);
    party_msg_t last;
    party_hdr_t lh;
    CHECK(drain(PARTY_PING, &last, &lh) == 1 && !strcmp(last.ping.label, "Flock-mine") && last.ping.cls == PARTY_C_ALPR);
    run(8500);
    CHECK(drain(PARTY_PING, &last, &lh) == 1 && (last.ping.flags & PARTY_PF_REPEAT));
    CHECK(st->pings_out == 2);

    /* a MARK without a fix stays here */
    s_fix = false;
    run(200);
    party_mark(PARTY_C_MARK, "here");
    run(300);
    CHECK(ping_labeled("here") && drain(PARTY_PING, NULL, NULL) == 0);
    s_fix = true;

    /* a file from V7 arrives on the card */
    static char text[1500];
    for (int i = 0; i < (int)sizeof(text) - 1; i++) text[i] = (char)('a' + i % 26);
    party_offer_t of = { 0x7A11, sizeof(text) - 1, party_crc32(0, text, sizeof(text) - 1), 100, "notes.txt" };
    h = H(0x47, 3);
    inject(b, party_enc_offer(b, sizeof(b), &h, &of));
    run(200);
    CHECK(drain(PARTY_ACK, &last, &lh) >= 1 && lh.dst == 0x47 && last.ack.fid == 0x7A11 && !(last.ack.flags & PARTY_ACK_REFUSED));
    uint8_t seq = 4;
    for (int i = 0; i < 15; i++) {
        if (i == 4) continue;                     /* lost */
        party_chunk_t c = { 0x7A11, (uint16_t)i, (uint8_t)(i == 14 ? 99 : 100), (const uint8_t *)text + i * 100 };
        h = H(0x47, seq++);
        inject(b, party_enc_chunk(b, sizeof(b), &h, &c));
        run(100);
    }
    run(2000);
    CHECK(drain(PARTY_ACK, &last, &lh) >= 1 && last.ack.base == 4 && !(last.ack.flags & PARTY_ACK_DONE));
    party_chunk_t c = { 0x7A11, 4, 100, (const uint8_t *)text + 400 };
    h = H(0x47, seq++);
    inject(b, party_enc_chunk(b, sizeof(b), &h, &c));
    run(300);
    CHECK(drain(PARTY_ACK, &last, &lh) >= 1 && (last.ack.flags & PARTY_ACK_DONE));
    snprintf(path, sizeof(path), "%s/notes.txt", s_dir);
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (fp) {
        static char got[2000];
        const size_t gn = fread(got, 1, sizeof(got), fp);
        fclose(fp);
        CHECK(gn == sizeof(text) - 1 && !memcmp(got, text, gn));
        remove(path);
    }
    CHECK(st->nrxf == 1 && st->rxf[0].state == PARTY_X_DONE && !strcmp(st->rxf[0].from, "V7"));

    /* members' finds as WARDRIVE sightings: peers only, tagged, placed, never a WiGLE kind */
    {
        party_find_t pf[16];
        wd_obs_t ob[16];
        const int pn = party_peer_since(pf, 16, 0);
        CHECK(pn >= 2);
        const int on = party_peers_to_obs(pf, pn, ob, 16);
        CHECK(on == pn);
        bool flock = false, car = false;
        for (int i = 0; i < on; i++) {
            CHECK(ob[i].radio == WD_RADIO_PEER && (ob[i].tags & WD_T_PEER) && ob[i].own_pos);
            CHECK(ob[i].kind < WD_KINDS);                       /* finds keep their kind; wd_core keeps peers out of the CSV */
            CHECK(!strstr(ob[i].name, "Flock-mine"));          /* this board's own finds stay out */
            if (strstr(ob[i].name, "Flock-1")) flock = (ob[i].tags & WD_T_ALPR) != 0;
            if (strstr(ob[i].name, "TPMS")) car = ob[i].kind == WD_RF && (ob[i].tags & WD_T_VEHICLE) && ob[i].addr[5] == 0xf2;
        }
        CHECK(flock && car);
        CHECK(party_peer_since(pf, 16, s_ms) == 0);
    }

    /* a long transfer in one slot while nine short files pass through the
       other: the long one's FILES row falls off the 8-row list, and its
       chunks must stop touching the list instead of writing past it (the
       sanitizer build traps on the out-of-range row) */
    {
        static char big[3000];
        for (int i = 0; i < (int)sizeof(big); i++) big[i] = (char)('A' + i % 26);
        party_offer_t bo = { 0x5150, sizeof(big), party_crc32(0, big, sizeof(big)), 100, "big.bin" };
        h = H(0x47, seq++);
        inject(b, party_enc_offer(b, sizeof(b), &h, &bo));
        run(200);
        drain(0, NULL, NULL);
        uint8_t s31 = 100;
        const char tiny[] = "0123456789";
        for (int f = 0; f < 9; f++) {
            party_offer_t to = { (uint16_t)(0x6000 + f), 10, party_crc32(0, tiny, 10), 100, "" };
            snprintf(to.name, sizeof(to.name), "t%d.txt", f);
            h = H(0x31, s31++);
            inject(b, party_enc_offer(b, sizeof(b), &h, &to));
            run(200);
            party_chunk_t tc = { to.fid, 0, 10, (const uint8_t *)tiny };
            h = H(0x31, s31++);
            inject(b, party_enc_chunk(b, sizeof(b), &h, &tc));
            run(300);
            /* keep the long one alive while the finished slot lets go */
            for (int sec = 0; sec < 22; sec++) {
                party_chunk_t bc = { 0x5150, (uint16_t)(sec % 2), 100, (const uint8_t *)big + (sec % 2) * 100 };
                h = H(0x47, seq++);
                inject(b, party_enc_chunk(b, sizeof(b), &h, &bc));
                run(1000);
            }
            drain(0, NULL, NULL);
            snprintf(path, sizeof(path), "%s/t%d.txt", s_dir, f);
            remove(path);
        }
        CHECK(st->nrxf == PARTY_RXFILES);
        CHECK(!strcmp(st->rxf[0].name, "t8.txt") && st->rxf[0].state == PARTY_X_DONE);
        for (int i = 0; i < PARTY_RXFILES; i++) CHECK(strcmp(st->rxf[i].name, "big.bin") != 0);
        /* a new chunk of the long one: its row is gone, nothing is written */
        party_chunk_t bc = { 0x5150, 5, 100, (const uint8_t *)big + 500 };
        h = H(0x47, seq++);
        inject(b, party_enc_chunk(b, sizeof(b), &h, &bc));
        run(300);
        CHECK(st->nrxf == PARTY_RXFILES);
        drain(0, NULL, NULL);
    }

    /* two members send a file with the same name at once: each lands whole */
    {
        run(26000);                               /* the long one above gives up and frees its slot */
        drain(0, NULL, NULL);
        static char a1[200], a2[200];
        for (int i = 0; i < 200; i++) { a1[i] = (char)('a' + i % 26); a2[i] = (char)('0' + i % 10); }
        party_offer_t o1 = { 0x7101, 200, party_crc32(0, a1, 200), 100, "dup.txt" };
        party_offer_t o2 = { 0x7202, 200, party_crc32(0, a2, 200), 100, "dup.txt" };
        uint8_t s31 = 200;
        h = H(0x31, s31++);
        inject(b, party_enc_offer(b, sizeof(b), &h, &o1));
        h = H(0x47, seq++);
        inject(b, party_enc_offer(b, sizeof(b), &h, &o2));
        run(300);
        for (int k = 0; k < 2; k++) {
            party_chunk_t c1 = { 0x7101, (uint16_t)k, 100, (const uint8_t *)a1 + k * 100 };
            party_chunk_t c2 = { 0x7202, (uint16_t)k, 100, (const uint8_t *)a2 + k * 100 };
            h = H(0x31, s31++);
            inject(b, party_enc_chunk(b, sizeof(b), &h, &c1));
            h = H(0x47, seq++);
            inject(b, party_enc_chunk(b, sizeof(b), &h, &c2));
            run(200);
        }
        run(500);
        CHECK(st->rxf[0].state == PARTY_X_DONE && st->rxf[1].state == PARTY_X_DONE);
        char p1[300], p2[300];
        snprintf(p1, sizeof(p1), "%s/dup.txt", s_dir);
        snprintf(p2, sizeof(p2), "%s/1-dup.txt", s_dir);
        static char g1[300], g2[300];
        FILE *f1 = fopen(p1, "rb"), *f2 = fopen(p2, "rb");
        CHECK(f1 && f2);
        size_t n1 = 0, n2 = 0;
        if (f1) { n1 = fread(g1, 1, sizeof(g1), f1); fclose(f1); }
        if (f2) { n2 = fread(g2, 1, sizeof(g2), f2); fclose(f2); }
        CHECK(n1 == 200 && n2 == 200);
        CHECK((!memcmp(g1, a1, 200) && !memcmp(g2, a2, 200)) || (!memcmp(g1, a2, 200) && !memcmp(g2, a1, 200)));
        remove(p1);
        remove(p2);
        drain(0, NULL, NULL);
    }

    /* leaving says BYE */
    drain(0, NULL, NULL);
    party_stop();
    CHECK(!party_running());

    if (fails) { printf("test_party_core: %d failed\n", fails); return 1; }
    printf("test_party_core: ok\n");
    return 0;
}
