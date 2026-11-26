/* LSX, the LakeShark link (components/apps/tui/ls_lsx.c): crypto vectors,
   discovery, sessions, objects under loss, resume after a lost session, the
   mode ladder and broadcast datagrams, two engines over the loopback. */
#include "ls_test.h"
#include "ls_lsx.h"
#include "ls_lsx_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int opened, closed, lost, objects, sent_ok, sent_lost, dgrams, probes;
    uint32_t last_len, last_crc;
    uint8_t last_port;
    uint8_t last_meta[LSX_META_MAX];
    char dgram[64];
    bool refuse_probe;
} app_t;

static bool a_probe(void *c, const ls_lsx_peer_t *p) { app_t *a = c; (void)p; a->probes++; return !a->refuse_probe; }
static void a_session(void *c, uint32_t peer, int ev)
{
    app_t *a = c;
    (void)peer;
    if (ev == LSX_EV_OPEN) a->opened++;
    if (ev == LSX_EV_CLOSED) a->closed++;
    if (ev == LSX_EV_LOST) a->lost++;
}
static void a_object(void *c, uint32_t peer, uint8_t port, const uint8_t *meta, int mlen, const uint8_t *d, uint32_t n)
{
    app_t *a = c;
    (void)peer;
    a->objects++;
    a->last_port = port;
    a->last_len = n;
    a->last_crc = lsx_crc32(0, d, n);
    memcpy(a->last_meta, meta, (size_t)mlen);
}
static void a_sent(void *c, uint32_t peer, uint8_t port, void *tag, int r)
{
    app_t *a = c;
    (void)peer; (void)port; (void)tag;
    if (r == LSX_SENT_OK) a->sent_ok++;
    else a->sent_lost++;
}
static void a_dgram(void *c, uint32_t peer, uint8_t port, const uint8_t *d, int n)
{
    app_t *a = c;
    (void)peer; (void)port;
    a->dgrams++;
    memcpy(a->dgram, d, (size_t)(n < 63 ? n : 63));
    a->dgram[n < 63 ? n : 63] = 0;
}

typedef struct {
    ls_lsx_loop_t *loop;
    ls_lsx_t *home, *field;
    app_t ah, af;
    uint32_t now;
} rig_t;

static const uint8_t KEY[16] = { 'l', 'a', 'k', 'e', 's', 'h', 'a', 'r', 'k', '-', 'h', 'o', 'm', 'e', '!', '!' };

static ls_lsx_t *mk(rig_t *r, int end, int role, uint32_t id, const char *name, app_t *a, int region)
{
    ls_lsx_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.role = (uint8_t)role;
    c.id = id;
    strncpy(c.name, name, LSX_NAME - 1);
    memcpy(c.key, KEY, 16);
    c.region = (uint8_t)region;
    c.power_dbm = 14;
    c.max_mode = LSX_M_N - 1;
    c.probe_ms = 2000;
    c.phy = ls_lsx_loop_phy(r->loop, end);
    c.app = (ls_lsx_app_t){ a, a_probe, NULL, a_session, a_object, a_sent, a_dgram, NULL };
    return ls_lsx_create(&c);
}

static void rig_init(rig_t *r, uint32_t caps_home, uint32_t caps_field, int region, uint32_t seed)
{
    memset(r, 0, sizeof(*r));
    r->loop = ls_lsx_loop_create(caps_home, caps_field, seed);
    r->home = mk(r, 0, LSX_ROLE_HOME, 0x484F4D45u, "HOME", &r->ah, region);
    r->field = mk(r, 1, LSX_ROLE_FIELD, 0x4649454Cu, "CAR-1", &r->af, region);
    r->now = 1000;
}

static void rig_free(rig_t *r)
{
    ls_lsx_destroy(r->home);
    ls_lsx_destroy(r->field);
    ls_lsx_loop_destroy(r->loop);
}

static void run(rig_t *r, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        r->now++;
        ls_lsx_loop_time(r->loop, r->now);
        ls_lsx_step(r->home, r->now);
        ls_lsx_step(r->field, r->now);
    }
}

static bool run_until(rig_t *r, uint32_t ms, bool (*done)(rig_t *))
{
    for (uint32_t i = 0; i < ms; i++) {
        run(r, 1);
        if (done(r)) return true;
    }
    return false;
}

static bool opened(rig_t *r) { return r->ah.opened && r->af.opened; }
static bool opened2(rig_t *r) { return r->ah.opened >= 2 && r->af.opened >= 2; }
static bool home_got(rig_t *r) { return r->ah.objects > 0; }

static uint8_t *pattern(uint32_t n, uint32_t seed)
{
    uint8_t *p = malloc(n);
    for (uint32_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; p[i] = (uint8_t)(seed >> 16); }
    return p;
}

LS_CASE(lsx_crypto_vectors)
{
    LS_CHECK(lsx_crypto_selftest());
    uint8_t a[16], b[16];
    lsx_kdf(KEY, "x", NULL, 0, a);
    lsx_kdf(KEY, "y", NULL, 0, b);
    LS_CHECK(memcmp(a, b, 16) != 0);
    lsx_phrase_key("party-42", a);
    lsx_phrase_key("party-42", b);
    LS_CHECK(!memcmp(a, b, 16));
    lsx_phrase_key("party-43", b);
    LS_CHECK(memcmp(a, b, 16) != 0);
    /* CTR is its own inverse. */
    lsx_aes_t k = { 0 };
    lsx_aes_init(&k, KEY);
    uint8_t iv[16] = { 1, 2, 3 }, buf[40], orig[40];
    for (int i = 0; i < 40; i++) orig[i] = buf[i] = (uint8_t)i;
    lsx_ctr(&k, iv, buf, sizeof(buf));
    LS_CHECK(memcmp(buf, orig, 40) != 0);
    lsx_ctr(&k, iv, buf, sizeof(buf));
    LS_CHECK(!memcmp(buf, orig, 40));
    lsx_aes_free(&k);
}

LS_CASE(lsx_airtime_and_modes)
{
    /* LoRa SF7/125: a 50-byte frame is about 98 ms. */
    const uint32_t lora = ls_lsx_airtime_us(LSX_M_LORA, 50);
    LS_CHECK_MSG(lora > 90000 && lora < 110000, "LoRa 50 B = %u us", (unsigned)lora);
    /* GFSK 200 kbps, 255 bytes plus 64 bits: 10.5 ms. */
    LS_NEAR(ls_lsx_airtime_us(LSX_M_GFSK200, 255), 10520, 10);
    /* FLRC 1.3 Mbps CR 3/4 beats GFSK 200 by more than four times. */
    LS_CHECK(ls_lsx_airtime_us(LSX_M_FLRC975, 255) * 4 < ls_lsx_airtime_us(LSX_M_GFSK200, 255));
    for (int m = 1; m < LSX_M_N; m++)
        LS_CHECK(ls_lsx_mode_info(m)->net_bps > ls_lsx_mode_info(m - 1)->net_bps || m == LSX_M_FLRC130);
    LS_CHECK(ls_lsx_region(LSX_REGION_EU868)->modes == ((1u << LSX_M_LORA) | (1u << LSX_M_GFSK50)));
}

LS_CASE(lsx_discovery_and_flrc_transfer)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, 0, 7);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    ls_lsx_status_t sh, sf;
    ls_lsx_status(r.home, &sh);
    ls_lsx_status(r.field, &sf);
    LS_EQ_INT(sh.state, LSX_ST_SESSION);
    LS_EQ_INT(sf.state, LSX_ST_SESSION);
    LS_CHECK(sh.master && !sf.master);
    /* -60 dBm: straight to the top FLRC mode. */
    LS_EQ_INT(sh.mode, LSX_M_FLRC975);
    LS_EQ_INT(sf.mode, LSX_M_FLRC975);
    LS_EQ_STR(sh.peer_name, "CAR-1");
    const uint32_t n = 20000;
    uint8_t *d = pattern(n, 1);
    const uint8_t meta[4] = { 2, 0, 0, 7 };
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, meta, 4, d, n, NULL), 0);
    LS_CHECK(run_until(&r, 5000, home_got));
    LS_EQ_UINT(r.ah.last_len, n);
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    LS_EQ_INT(r.ah.last_port, 1);
    LS_EQ_INT(r.ah.last_meta[3], 7);
    run(&r, 300);
    LS_EQ_INT(r.af.sent_ok, 1);
    ls_lsx_status(r.home, &sh);
    LS_EQ_INT(sh.frames_bad, 0);
    /* Back the other way, a small object that rides inline in the BEGIN. */
    const uint8_t small[10] = "hello car";
    LS_EQ_INT(ls_lsx_send_object(r.home, 3, NULL, 0, small, sizeof(small), NULL), 0);
    run(&r, 500);
    LS_EQ_INT(r.af.objects, 1);
    LS_EQ_UINT(r.af.last_len, sizeof(small));
    /* BYE closes both ends; the field gives the radio back. */
    ls_lsx_close(r.home);
    run(&r, 500);
    LS_EQ_INT(r.ah.closed, 1);
    LS_EQ_INT(r.af.closed, 1);
    ls_lsx_status(r.field, &sf);
    LS_CHECK(!sf.held);
    free(d);
    rig_free(&r);
}

/* Deadlines that start at zero must not look 24.8 days in the future once
   uptime passes 2^31 ms: the home has to claim the radio, open a session
   and move an object exactly as it does after a fresh boot. */
LS_CASE(lsx_works_after_24_days_uptime)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, 0, 7);
    r.now = 0x80000000u + 1000u;
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    const uint32_t n = 6000;
    uint8_t *d = pattern(n, 3);
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    LS_CHECK(run_until(&r, 5000, home_got));
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    free(d);
    rig_free(&r);
}

LS_CASE(lsx_sx1262_peer_uses_gfsk)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK, 0, 9);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_EQ_INT(sh.mode, LSX_M_GFSK200);
    const uint32_t n = 6000;
    uint8_t *d = pattern(n, 2);
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    LS_CHECK(run_until(&r, 5000, home_got));
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    free(d);
    rig_free(&r);
}

LS_CASE(lsx_weak_link_starts_low_and_climbs)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, 0, 11);
    ls_lsx_loop_set(r.loop, 0, -105);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_EQ_INT(sh.mode, LSX_M_LORA);
    /* The link gets better: the master tries the next mode up, one at a time. */
    ls_lsx_loop_set(r.loop, 0, -70);
    for (int k = 0; k < 24; k++) {
        run(&r, 500);
        ls_lsx_status(r.home, &sh);
        if (getenv("LSX_DEBUG")) printf("t=%u mode=%d rssi=%d at_peer=%d turns=%u miss=%u\n", (unsigned)r.now, sh.mode, sh.rssi, sh.rssi_at_peer, (unsigned)sh.turns, (unsigned)sh.misses);
    }
    ls_lsx_status(r.home, &sh);
    LS_EQ_INT(sh.mode, LSX_M_FLRC975);
    ls_lsx_status_t sf;
    ls_lsx_status(r.field, &sf);
    LS_EQ_INT(sf.mode, LSX_M_FLRC975);
    rig_free(&r);
}

LS_CASE(lsx_transfer_survives_loss)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK, LSX_CAP_LORA | LSX_CAP_GFSK, 0, 13);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    ls_lsx_loop_set(r.loop, 50, -60);    /* one frame in five lost, both ways */
    const uint32_t n = 12000;
    uint8_t *d = pattern(n, 3);
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    LS_CHECK(run_until(&r, 30000, home_got));
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_CHECK(sh.bytes_rx == n);
    free(d);
    rig_free(&r);
}

LS_CASE(lsx_resume_after_lost_session)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK, LSX_CAP_LORA | LSX_CAP_GFSK, 0, 17);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    const uint32_t n = 30000;
    uint8_t *d = pattern(n, 4);
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    /* Part of the way through, MeshCore wants the field's radio back. */
    ls_lsx_status_t sh;
    for (int i = 0; i < 20000; i++) {
        run(&r, 1);
        ls_lsx_status(r.home, &sh);
        if (sh.obj_rx_done > n / 3) break;
    }
    const uint32_t first = sh.obj_rx_done;
    LS_CHECK(first > n / 3 && first < n);
    ls_lsx_loop_block(r.loop, 1, true);
    run(&r, 12000);
    LS_EQ_INT(r.af.sent_lost, 1);
    LS_EQ_INT(r.af.lost, 1);
    LS_CHECK(r.ah.lost == 1);
    LS_EQ_INT(r.ah.objects, 0);
    /* The radio comes back; the field probes, and offers the same object. */
    ls_lsx_loop_block(r.loop, 1, false);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 5000, opened2));
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    LS_CHECK(run_until(&r, 20000, home_got));
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    ls_lsx_status(r.home, &sh);
    LS_EQ_INT(sh.resumed, 1);
    /* Only what was missing crossed the second time (plus a window's worth
       sent before the resume ack came back). */
    LS_CHECK_MSG(sh.bytes_rx < n - first + 12 * 228, "second session moved %u of %u", (unsigned)sh.bytes_rx,
                 (unsigned)n);
    free(d);
    rig_free(&r);
}

LS_CASE(lsx_link_breaks_falls_to_lora_and_recovers)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, 0, 19);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    const uint32_t n = 24000;
    uint8_t *d = pattern(n, 5);
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    run(&r, 30);
    /* Everything above LoRa dies (the RSSI collapses); LoRa still works. */
    ls_lsx_loop_set(r.loop, 0, -112);
    for (int i = 0; i < 6000; i++) {
        run(&r, 1);
        ls_lsx_status_t sh, sf;
        ls_lsx_status(r.home, &sh);
        ls_lsx_status(r.field, &sf);
        if (sh.mode == LSX_M_LORA && sf.mode == LSX_M_LORA) break;
    }
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_EQ_INT(sh.state, LSX_ST_SESSION);
    ls_lsx_loop_set(r.loop, 0, -60);
    LS_CHECK(run_until(&r, 60000, home_got));
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    free(d);
    rig_free(&r);
}

/* A home phy that flips one bit in every seventh frame it receives. */
static ls_lsx_phy_t tamper_phy;
static const ls_lsx_phy_t *tamper_inner;
static int tamper_n, tamper_hits;
static int tamper_recv(void *c, uint8_t *buf, int cap, ls_lsx_rxmeta_t *m)
{
    const int n = tamper_inner->recv(c, buf, cap, m);
    if (n > 20 && ++tamper_n % 7 == 0) { buf[17] ^= 0x10; tamper_hits++; }
    return n;
}

LS_CASE(lsx_tampered_frames_are_refused)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    r.loop = ls_lsx_loop_create(LSX_CAP_LORA | LSX_CAP_GFSK, LSX_CAP_LORA | LSX_CAP_GFSK, 37);
    tamper_inner = ls_lsx_loop_phy(r.loop, 0);
    tamper_phy = *tamper_inner;
    tamper_phy.recv = tamper_recv;
    ls_lsx_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.role = LSX_ROLE_HOME;
    c.id = 0x11112222u;
    strcpy(c.name, "HOME");
    memcpy(c.key, KEY, 16);
    c.power_dbm = 14;
    c.max_mode = LSX_M_N - 1;
    c.phy = &tamper_phy;
    c.app = (ls_lsx_app_t){ &r.ah, a_probe, NULL, a_session, a_object, a_sent, a_dgram, NULL };
    r.home = ls_lsx_create(&c);
    r.field = mk(&r, 1, LSX_ROLE_FIELD, 0x33334444u, "CAR-2", &r.af, 0);
    r.now = 1000;
    for (int tries = 0; tries < 5 && !opened(&r); tries++) {
        ls_lsx_probe_now(r.field);
        run_until(&r, 3000, opened);
    }
    LS_CHECK(opened(&r));
    const uint32_t n = 9000;
    uint8_t *d = pattern(n, 6);
    LS_EQ_INT(ls_lsx_send_object(r.field, 1, NULL, 0, d, n, NULL), 0);
    LS_CHECK(run_until(&r, 20000, home_got));
    LS_EQ_UINT(r.ah.last_crc, lsx_crc32(0, d, n));
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_CHECK(tamper_hits > 0);
    LS_CHECK_MSG(sh.frames_bad >= (uint32_t)tamper_hits, "bad %u hits %d", (unsigned)sh.frames_bad, tamper_hits);
    free(d);
    rig_free(&r);
}

LS_CASE(lsx_wrong_key_never_opens)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK, LSX_CAP_LORA | LSX_CAP_GFSK, 0, 23);
    uint8_t other[16];
    lsx_phrase_key("not the home key", other);
    ls_lsx_set_key(r.field, other);
    ls_lsx_probe_now(r.field);
    run(&r, 5000);
    LS_EQ_INT(r.ah.probes, 0);
    LS_EQ_INT(r.ah.opened, 0);
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_CHECK(sh.frames_bad > 0);
    rig_free(&r);
}

LS_CASE(lsx_home_can_decline_and_broadcasts_arrive)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK, LSX_CAP_LORA | LSX_CAP_GFSK, 0, 29);
    r.ah.refuse_probe = true;
    ls_lsx_probe_now(r.field);
    run(&r, 2000);
    LS_CHECK(r.ah.probes >= 1);
    LS_EQ_INT(r.ah.opened, 0);
    /* A PARTY-style position ping: broadcast, no session. */
    const char *msg = "pos 26.64 -81.87";
    LS_EQ_INT(ls_lsx_send_dgram(r.field, LSX_BCAST, 5, (const uint8_t *)msg, (int)strlen(msg)), 0);
    run(&r, 3000);
    LS_EQ_INT(r.ah.dgrams, 1);
    LS_EQ_STR(r.ah.dgram, msg);
    rig_free(&r);
}

/* Two boards in the PARTY role (what ls_lsx_svc_party runs): both listen on
   the discovery channel, neither opens a session, and a burst of broadcast
   datagrams from each, longer than the queue, all arrive, each with the
   level the receiver measured in its status (PARTY's RSSI). */
LS_CASE(lsx_party_role_broadcasts_both_ways)
{
    rig_t r;
    memset(&r, 0, sizeof(r));
    r.loop = ls_lsx_loop_create(LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK, 37);
    r.home = mk(&r, 0, LSX_ROLE_PARTY, 0x50415231u, "ORCA", &r.ah, 0);
    r.field = mk(&r, 1, LSX_ROLE_PARTY, 0x50415232u, "MOTH", &r.af, 0);
    r.ah.refuse_probe = r.af.refuse_probe = true;
    ls_lsx_loop_set(r.loop, 0, -97);
    r.now = 1000;
    run(&r, 500);
    char msg[200];
    /* every second both have something at the same instant: the worst case for timing */
    for (int i = 0; i < 12; i++) {
        snprintf(msg, sizeof(msg), "hello %02d from orca, a party datagram padded out to look like a ping .........", i);
        LS_EQ_INT(ls_lsx_send_dgram(r.home, LSX_BCAST, 0x50, (const uint8_t *)msg, (int)strlen(msg)), 0);
        snprintf(msg, sizeof(msg), "pos %02d from moth", i);
        LS_EQ_INT(ls_lsx_send_dgram(r.field, LSX_BCAST, 0x50, (const uint8_t *)msg, (int)strlen(msg)), 0);
        run(&r, 1000);
    }
    run(&r, 2000);
    printf("party broadcasts: orca heard %d of 12, moth heard %d of 12\n", r.ah.dgrams, r.af.dgrams);
    /* Without the random waits every frame met the other board's: 0 of 12 one way. */
    LS_CHECK(r.af.dgrams >= 8);
    LS_CHECK(r.ah.dgrams >= 8);
    LS_EQ_INT(r.ah.opened + r.af.opened, 0);
    ls_lsx_status_t s;
    ls_lsx_status(r.field, &s);
    LS_CHECK(s.rssi <= -92 && s.rssi >= -102);
    LS_EQ_INT(s.state, LSX_ST_LISTEN);
    rig_free(&r);
}

LS_CASE(lsx_eu_region_stays_narrow)
{
    rig_t r;
    rig_init(&r, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC,
             LSX_REGION_EU868, 31);
    ls_lsx_probe_now(r.field);
    LS_CHECK(run_until(&r, 3000, opened));
    run(&r, 8000);
    ls_lsx_status_t sh;
    ls_lsx_status(r.home, &sh);
    LS_CHECK(sh.mode == LSX_M_LORA || sh.mode == LSX_M_GFSK50);
    LS_EQ_UINT(sh.hz, 869460000u);
    /* 10 % of a minute at most, whatever moves. */
    LS_CHECK(sh.airtime_ms_60s <= 6000);
    rig_free(&r);
}

/* ---- HOME BASE sync (ls_lsx_sync.c) over the same loopback -------------- */

#include "ls_lsx_sync.h"

typedef struct {
    ls_lsx_loop_t *loop;
    ls_lsx_t *home, *field;
    ls_lsx_sync_t *sh, *sf;
    const ls_lsx_store_t *store_h, *store_f;
    uint32_t now;
} srig_t;

static ls_lsx_t *smk(srig_t *r, int end, int role, uint32_t id, const char *name, ls_lsx_sync_t *s)
{
    ls_lsx_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.role = (uint8_t)role;
    c.id = id;
    strncpy(c.name, name, LSX_NAME - 1);
    memcpy(c.key, KEY, 16);
    c.power_dbm = 14;
    c.max_mode = LSX_M_N - 1;
    c.probe_ms = 3000;
    c.phy = ls_lsx_loop_phy(r->loop, end);
    c.app = ls_lsx_sync_app(s);
    ls_lsx_t *x = ls_lsx_create(&c);
    ls_lsx_sync_bind(s, x);
    return x;
}

static void srun(srig_t *r, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        r->now++;
        ls_lsx_loop_time(r->loop, r->now);
        ls_lsx_step(r->home, r->now);
        ls_lsx_sync_tick(r->sh, r->now);
        ls_lsx_step(r->field, r->now);
        ls_lsx_sync_tick(r->sf, r->now);
    }
}

LS_CASE(lsx_home_sync_pulls_then_pushes)
{
    srig_t r;
    memset(&r, 0, sizeof(r));
    r.loop = ls_lsx_loop_create(LSX_CAP_LORA | LSX_CAP_GFSK | LSX_CAP_FLRC, LSX_CAP_LORA | LSX_CAP_GFSK, 41);
    r.sh = ls_lsx_sync_create(LSX_ROLE_HOME);
    r.sf = ls_lsx_sync_create(LSX_ROLE_FIELD);
    r.store_h = ls_lsx_test_store(50, 2);      /* what other cars found */
    r.store_f = ls_lsx_test_store(2000, 1);    /* this drive: 64 KB, four objects */
    ls_lsx_sync_set_store(r.sh, r.store_h);
    ls_lsx_sync_set_store(r.sf, r.store_f);
    r.home = smk(&r, 0, LSX_ROLE_HOME, 0xAAAA0001u, "HOME", r.sh);
    r.field = smk(&r, 1, LSX_ROLE_FIELD, 0xBBBB0002u, "CAR-SX", r.sf);
    r.now = 5000;
    /* Not forced: an unknown unit is always answered. */
    bool saw_progress = false;
    int last_pct = -1;
    bool monotonic = true;
    for (int i = 0; i < 40000; i++) {
        srun(&r, 1);
        ls_lsx_sync_status_t st;
        ls_lsx_sync_status(r.sh, &st);
        if (st.progress_pct > 0 && st.progress_pct < 100) saw_progress = true;
        if (st.progress_pct >= 0) {
            if (st.progress_pct + 15 < last_pct) monotonic = false;
            last_pct = st.progress_pct;
        }
        if (ls_lsx_test_store_applied(r.store_f) == 50) break;
    }
    srun(&r, 2000);
    LS_EQ_UINT(ls_lsx_test_store_applied(r.store_h), 2000);
    LS_EQ_UINT(ls_lsx_test_store_applied(r.store_f), 50);
    LS_CHECK(saw_progress);
    LS_CHECK(monotonic);
    ls_lsx_unit_t u[4];
    LS_EQ_INT(ls_lsx_sync_units(r.sh, u, 4), 1);
    LS_EQ_UINT(u[0].pulled, 2000);
    LS_EQ_UINT(u[0].pushed, 50);
    LS_EQ_UINT(u[0].recs_in, 2000);
    LS_EQ_UINT(u[0].recs_out, 50);
    LS_EQ_INT(u[0].syncs, 1);
    LS_EQ_STR(u[0].name, "CAR-SX");
    LS_CHECK(ls_lsx_sync_take_dirty(r.sh));
    ls_lsx_sync_status_t fs;
    ls_lsx_sync_status(r.sf, &fs);
    LS_CHECK(fs.last_sync_s > 0);
    LS_EQ_STR(fs.home_name, "HOME");
    LS_CHECK(fs.last_home_ms < 5000);
    ls_lsx_status_t ls;
    ls_lsx_status(r.field, &ls);
    LS_CHECK(!ls.held);          /* MeshCore has its radio back */
    /* Nothing new on either side: the next probes are not answered. */
    const uint32_t sessions = ls.sessions;
    srun(&r, 15000);
    ls_lsx_status(r.field, &ls);
    LS_EQ_UINT(ls.sessions, sessions);
    /* 'lsx sync' on the car: answered anyway, and nothing moves twice. */
    ls_lsx_sync_force(r.sf);
    srun(&r, 8000);
    ls_lsx_status(r.field, &ls);
    LS_EQ_UINT(ls.sessions, sessions + 1);
    LS_EQ_UINT(ls_lsx_test_store_applied(r.store_h), 2000);
    LS_EQ_INT(ls_lsx_sync_units(r.sh, u, 4), 1);
    LS_EQ_INT(u[0].syncs, 2);
    /* The table survives a reboot through its blob. */
    uint8_t blob[1024];
    const size_t bl = ls_lsx_sync_units_blob(r.sh, blob, sizeof(blob));
    LS_CHECK(bl > 0);
    ls_lsx_sync_t *again = ls_lsx_sync_create(LSX_ROLE_HOME);
    ls_lsx_sync_units_load(again, blob, bl);
    LS_EQ_INT(ls_lsx_sync_units(again, u, 4), 1);
    LS_EQ_UINT(u[0].pulled, 2000);
    ls_lsx_sync_destroy(again);
    ls_lsx_destroy(r.home);
    ls_lsx_destroy(r.field);
    ls_lsx_sync_destroy(r.sh);
    ls_lsx_sync_destroy(r.sf);
    ls_lsx_loop_destroy(r.loop);
}
