/* lssim: PARTY's board (components/apps/tui/party/party_io.h) as a small
   town with three other members on the loopback link.

     KB1QWE  '#' on foot, a loop north-east of here; finds a Flock camera,
             later the same car VAN7 reported (a second report: AGAIN)
     VAN7    '&' in a van, a big loop at 11 m/s; reports a TPMS ID, a
             bodycam, a police fleet camera, and sends route.gpx
     MOTH    '*' standing south-west, an SX1262 (no FLRC, so files go on
             GFSK), shares a ROUGH position; reports a drone

   Each speaks the real wire format (party_proto.h) through party_loop_*,
   at a level that falls with distance, and answers this board's files
   with real ACKs. lssim_party_seed() plays six minutes before the first
   frame, so a picture shows a party under way.
     LSSIM_PARTY=new     no profiles: the first-run screen
     LSSIM_PARTY=lobby   two saved profiles, no party running
     LSSIM_GPS=nofix     no fix here
   Deterministic: the same picture every run. */
#include "party/party_core.h"
#include "party/party_io.h"
#include "party/party_proto.h"
#include "wardrive/wd_core.h"
#include "esp_timer.h"
#include "tuilib/tui_core.h"

#include <direct.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ls_shim_time_set(int64_t us);

#define LAT0 43.4445
#define LON0 -71.6473
#define T_END_MS 2000000u
#define SEED_MS 360000u
#define MPD 111194.9266

static char s_dir[200];
static uint32_t s_rand = 12345, s_t0;
static int (*s_fn)(void);

/* ------------------------------------------------------------ the board */

uint32_t party_io_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void world(uint32_t now);
static void at_loop(double ce, double cn, double r, double speed, double t_s, double phase, double *e, double *n, double *course)
{
    const double w = speed / r, a = phase + w * t_s;
    *e = ce + r * sin(a);
    *n = cn + r * cos(a);
    *course = fmod((a + M_PI / 2) * 180 / M_PI + 360, 360);
}
static void me_at(uint32_t now, double *e, double *n, double *course)
{
    at_loop(0, 0, 140, 1.3, (now - s_t0) / 1000.0, 0.3, e, n, course);
}

bool party_io_fix(party_fix_t *f)
{
    const uint32_t now = party_io_ms();
    world(now);
    double e, n, c;
    me_at(now, &e, &n, &c);
    const char *g = getenv("LSSIM_GPS");
    f->fix = !(g && strcmp(g, "fix"));
    f->lat = LAT0 + n / MPD;
    f->lon = LON0 + e / (MPD * cos(LAT0 * M_PI / 180));
    f->acc_m = 4.5f;
    f->speed_mps = 1.3f;
    f->course_deg = (float)c;
    f->heading_deg = (float)fmod(c + 8, 360);
    return f->fix;
}
int party_io_batt(void) { return 83; }
bool party_io_vibe(int ms, int strength) { (void)ms; (void)strength; return false; }
bool party_io_vibe_present(void) { return false; }
int party_io_local_hour(void) { return 15; }
const char *party_io_dir(void)
{
    if (!s_dir[0]) {
        snprintf(s_dir, sizeof(s_dir), "%s/lssim_party", getenv("TEMP") ? getenv("TEMP") : ".");
        _mkdir(s_dir);
    }
    return s_dir;
}
bool party_io_card(void) { return true; }
bool party_io_mkdir(const char *path) { return _mkdir(path) == 0; }
void *party_io_alloc(size_t bytes) { return calloc(1, bytes); }
void party_io_lock(void) { }
void party_io_unlock(void) { }
bool party_io_spawn(int (*fn)(void)) { s_fn = fn; return true; }   /* no worker: the frame ticks */
bool party_io_worker_alive(void) { return false; }
const party_link_ops_t *party_io_link(void) { return party_link_loop(); }
uint32_t party_io_random(void) { s_rand = s_rand * 1664525u + 1013904223u; return s_rand >> 8; }

/* ------------------------------------------------------------ the others */

typedef struct {
    uint8_t id, glyph, colour, caps, share, seq;
    const char *call;
    double ce, cn, r, speed, phase;   /* their loop */
    bool vehicle;
    uint32_t join_ms, hello_ms, pos_ms, pos_every;
    party_frx_t frx;                  /* a file this board sends them */
    bool frx_on;
} sim_t;

static sim_t S[3] = {
    { 0x31, '#', TUI_RED | TUI_BRIGHT, PARTY_CAP_LORA | PARTY_CAP_GFSK | PARTY_CAP_FLRC | PARTY_CAP_FILES | PARTY_CAP_COMPASS,
      PARTY_SHARE_EXACT, 0, "KB1QWE", 260, 190, 110, 1.4, 1.0, false, 4000, 0, 0, 15000, { 0 }, false },
    { 0x47, '&', TUI_YELLOW | TUI_BRIGHT, PARTY_CAP_LORA | PARTY_CAP_GFSK | PARTY_CAP_FLRC | PARTY_CAP_FILES,
      PARTY_SHARE_EXACT, 0, "VAN7", 0, 0, 700, 11.0, 2.2, true, 9000, 0, 0, 10000, { 0 }, false },
    { 0x5C, '*', TUI_MAGENTA | TUI_BRIGHT, PARTY_CAP_LORA | PARTY_CAP_GFSK | PARTY_CAP_FILES,
      PARTY_SHARE_ROUGH, 0, "MOTH", -420, -380, 15, 0.3, 0.0, false, 16000, 0, 0, 20000, { 0 }, false },
};
static uint16_t s_party;
static uint32_t s_world_ms;

static void sim_pos(const sim_t *m, uint32_t now, double *lat, double *lon, double *course)
{
    double e, n;
    at_loop(m->ce, m->cn, m->r, m->speed, (now - s_t0) / 1000.0, m->phase, &e, &n, course);
    *lat = LAT0 + n / MPD;
    *lon = LON0 + e / (MPD * cos(LAT0 * M_PI / 180));
}

static void air(sim_t *m, const uint8_t *pkt, int n, uint32_t now)
{
    double la, lo, c, e, nn, cc;
    sim_pos(m, now, &la, &lo, &c);
    me_at(now, &e, &nn, &cc);
    const double de = (lo - LON0) * MPD * cos(LAT0 * M_PI / 180) - e, dn = (la - LAT0) * MPD - nn;
    const double d = hypot(de, dn);
    int rssi = (int)(-48 - 22 * log10(d + 1));
    int snr = (rssi + 118) / 3;
    snr = snr > 12 ? 12 : snr < -12 ? -12 : snr;
    party_loop_inject(pkt, n, (int8_t)rssi, (int8_t)snr);
}

static party_hdr_t H(sim_t *m, uint8_t dst) { party_hdr_t h = { 0, m->id, dst, ++m->seq, s_party }; return h; }

static void hello(sim_t *m, uint32_t now)
{
    uint8_t b[PARTY_DGRAM_MAX];
    party_hello_t he = { "", m->glyph, m->colour, m->caps, m->share, (uint8_t)(70 - m->id % 40), 0,
                         (uint16_t)(0xFFFF & ~(m->id == 0x5C ? PARTY_CMASK(PARTY_C_VEHICLE) : 0)) };
    snprintf(he.call, sizeof(he.call), "%s", m->call);
    party_hdr_t h = H(m, PARTY_ALL);
    air(m, b, party_enc_hello(b, sizeof(b), &h, &he), now);
    m->hello_ms = now;
}

static void pos(sim_t *m, uint32_t now)
{
    uint8_t b[PARTY_DGRAM_MAX];
    double la, lo, c;
    sim_pos(m, now, &la, &lo, &c);
    const bool rough = m->share == PARTY_SHARE_ROUGH;
    party_pos_t p = { party_deg_e7(rough ? party_rough(la, 0.01) : la), party_deg_e7(rough ? party_rough(lo, 0.01) : lo),
                      (uint16_t)(rough ? 800 : 5), (uint8_t)(m->speed * 2 + 0.5), (uint8_t)(c * 256 / 360), 0,
                      (uint8_t)(70 - m->id % 40),
                      (uint8_t)((rough ? PARTY_POS_ROUGH : PARTY_POS_COURSE) | (m->speed > 0.7 ? PARTY_POS_MOVING : 0) |
                                (m->vehicle ? PARTY_POS_VEHICLE : 0)) };
    party_hdr_t h = H(m, PARTY_ALL);
    air(m, b, party_enc_pos(b, sizeof(b), &h, &p), now);
    m->pos_ms = now;
}

static void ping(sim_t *m, uint32_t now, uint16_t id, int cls, int kind, uint16_t tags, int rssi, const char *addr, const char *label, double off_m)
{
    uint8_t b[PARTY_DGRAM_MAX];
    double la, lo, c;
    sim_pos(m, now, &la, &lo, &c);
    party_ping_t p;
    memset(&p, 0, sizeof(p));
    p.id = id; p.cls = (uint8_t)cls; p.kind = (uint8_t)kind; p.tags = tags; p.rssi = (int8_t)rssi;
    p.flags = PARTY_PF_PLACED | (cls == PARTY_C_MARK ? PARTY_PF_MANUAL : 0);
    p.lat_e7 = party_deg_e7(la + off_m / MPD);
    p.lon_e7 = party_deg_e7(lo + off_m / (MPD * cos(LAT0 * M_PI / 180)));
    p.unc_m = party_unc_m(kind, rssi, 5);
    p.age_s = 3;
    if (addr) memcpy(p.addr, addr, 6);
    snprintf(p.label, sizeof(p.label), "%s", label);
    party_hdr_t h = H(m, PARTY_ALL);
    air(m, b, party_enc_ping(b, sizeof(b), &h, &p), now);
}

/* VAN7's file: route.gpx, sent with its own sender state. */
static char s_gpx[3200];
static int s_gpx_len;
static party_ftx_t s_vtx;
static uint32_t s_vtx_next, s_vtx_start;
static bool s_vtx_on;

static void van_file_step(uint32_t now)
{
    sim_t *m = &S[1];
    uint8_t b[PARTY_DGRAM_MAX];
    if (!s_vtx_on || (int32_t)(now - s_vtx_next) < 0) return;
    if (now - s_vtx_start < 10) {
        party_offer_t o = { 0x7A11, (uint32_t)s_gpx_len, party_crc32(0, s_gpx, (size_t)s_gpx_len), 120, "route.gpx" };
        party_hdr_t h = H(m, PARTY_ALL);
        air(m, b, party_enc_offer(b, sizeof(b), &h, &o), now);
        party_ftx_begin(&s_vtx, o.fid, o.size, o.chunk);
        s_vtx_next = now + 1200;
        return;
    }
    const int idx = party_ftx_next(&s_vtx);
    if (idx < 0) { s_vtx_next = now + 300; return; }
    const uint32_t off = (uint32_t)idx * 120;
    party_chunk_t c = { 0x7A11, (uint16_t)idx, (uint8_t)(s_gpx_len - off < 120 ? s_gpx_len - off : 120), (const uint8_t *)s_gpx + off };
    party_hdr_t h = H(m, PARTY_ALL);
    const int n = party_enc_chunk(b, sizeof(b), &h, &c);
    static bool lost;
    if (idx != 7 || lost) air(m, b, n, now);   /* chunk 7 is lost the first time */
    else lost = true;
    s_vtx_next = now + 60;
}

/* What this board sent: ACKs for VAN7's file, and a file of its own for
   KB1QWE to take and answer. */
static void hear(uint32_t now)
{
    uint8_t d[PARTY_DGRAM_MAX], b[PARTY_DGRAM_MAX];
    int n;
    bool bulk;
    while (party_loop_take(d, &n, &bulk)) {
        party_hdr_t h;
        party_msg_t m;
        const int t = party_decode(d, n, s_party, &h, &m);
        if (t == PARTY_ACK && h.dst == S[1].id && m.ack.fid == 0x7A11) {
            party_ftx_ack(&s_vtx, &m.ack);
            if (m.ack.flags & PARTY_ACK_DONE) s_vtx_on = false;
        } else if (t == PARTY_OFFER) {
            S[0].frx_on = party_frx_begin(&S[0].frx, &m.offer);
            if (S[0].frx_on) {
                party_ack_t a;
                party_frx_ack(&S[0].frx, &a);
                party_hdr_t hh = H(&S[0], h.src);
                air(&S[0], b, party_enc_ack(b, sizeof(b), &hh, &a), now);
            }
        } else if (t == PARTY_CHUNK && S[0].frx_on) {
            if (party_frx_put(&S[0].frx, &m.chunk, NULL) >= 0 &&
                (party_frx_complete(&S[0].frx) || S[0].frx.got % 16 == 0)) {
                party_ack_t a;
                party_frx_ack(&S[0].frx, &a);
                party_hdr_t hh = H(&S[0], h.src);
                air(&S[0], b, party_enc_ack(b, sizeof(b), &hh, &a), now);
                if (party_frx_complete(&S[0].frx)) S[0].frx_on = false;
            }
        }
    }
}

static void world(uint32_t now)
{
    if (!s_party || now == s_world_ms) return;
    const uint32_t prev = s_world_ms;
    s_world_ms = now;
    const uint32_t t = now - s_t0, tp = prev - s_t0;
#define AT(sec) (tp < (sec) * 1000u && t >= (sec) * 1000u)
    for (int i = 0; i < 3; i++) {
        sim_t *m = &S[i];
        if (t < m->join_ms) continue;
        if (!m->hello_ms || now - m->hello_ms >= 60000) hello(m, now);
        else if (!m->pos_ms || now - m->pos_ms >= m->pos_every) pos(m, now);
    }
    if (AT(40)) ping(&S[0], now, 1, PARTY_C_ALPR, WD_WIFI, WD_T_ALPR | WD_T_CAMERA, -68, "\xb4\x1e\x52\x4f\x2a\x9c", "Flock-4f2a9c", 40);
    if (AT(70)) ping(&S[1], now, 1, PARTY_C_VEHICLE, WD_RF, WD_T_VEHICLE, -60, "\x00\x00\x00\x3a\x91\xf2", "TPMS 3a91f2", 0);
    if (AT(100)) ping(&S[2], now, 1, PARTY_C_DRONE, WD_DRONE, WD_T_DRONE, -75, "\x60\x60\x1f\x22\x10\x05", "DJI Mini 4 Pro", 120);
    if (AT(130)) ping(&S[1], now, 2, PARTY_C_BODYCAM, WD_BLE, WD_T_BODYCAM | WD_T_POLICE, -70, "\x00\x25\xdf\x81\x00\x11", "Axon Body 3 X81", 10);
    if (AT(150)) {
        party_find_t f;
        memset(&f, 0, sizeof(f));
        memcpy(f.addr, "\xb4\x1e\x52\x77\xb1\xe0", 6);
        f.kind = WD_WIFI; f.tags = WD_T_ALPR | WD_T_CAMERA; f.rssi = -64;
        snprintf(f.name, sizeof(f.name), "Flock-77b1e0");
        f.ms = now;
        party_offer_find(&f);
    }
    if (AT(200)) ping(&S[0], now, 2, PARTY_C_VEHICLE, WD_RF, WD_T_VEHICLE, -66, "\x00\x00\x00\x3a\x91\xf2", "TPMS 3a91f2", 0);
    if (AT(220)) { s_vtx_on = true; s_vtx_start = s_vtx_next = now; }
    if (AT(260)) ping(&S[1], now, 3, PARTY_C_POLICE, WD_WIFI, WD_T_POLICE | WD_T_VEHICLE, -72, "\x00\x30\x1a\x07\x07\x07", "FleetCam-07", 0);
    if (AT(330)) ping(&S[0], now, 3, PARTY_C_MARK, WD_RF, 0, 0, NULL, "LOOK HERE", 0);
    if (AT(358)) ping(&S[2], now, 2, PARTY_C_TRACKER, WD_BLE, WD_T_TRACKER, -80, "\xc8\x12\x55\x01\x02\x03", "AirTag (lost mode)", 30);
#undef AT
    van_file_step(now);
    hear(now);
}

/* ------------------------------------------------------------ the seed */

static void write_file(const char *path, const char *text, int repeat)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < repeat; i++) fputs(text, f);
    fclose(f);
}

void lssim_party_seed(void)
{
    const char *mode = getenv("LSSIM_PARTY");
    ls_shim_time_set((int64_t)(T_END_MS - SEED_MS) * 1000);
    char p[260];
    snprintf(p, sizeof(p), "%s/profiles.bin", party_io_dir());
    remove(p);                         /* the same picture every run */
    snprintf(p, sizeof(p), "%s/route.gpx", party_io_dir());
    remove(p);
    snprintf(p, sizeof(p), "%s/share", party_io_dir());
    _mkdir(p);
    snprintf(p, sizeof(p), "%s/share/meet-points.txt", party_io_dir());
    write_file(p, "North lot, by the grain elevator. 18:30.\n", 40);
    snprintf(p, sizeof(p), "%s/share/flock-list.csv", party_io_dir());
    write_file(p, "b4:1e:52:4f:2a:9c,Flock,43.4461,-71.6449\n", 120);
    party_init();
    if (mode && !strcmp(mode, "new")) return;
    int i = party_prof_new("COMMUTE");
    party_prof_t *pr = party_prof(i);
    snprintf(pr->party, sizeof(pr->party), "NIGHT OWLS");
    snprintf(pr->code, sizeof(pr->code), "fox den 7");
    snprintf(pr->call, sizeof(pr->call), "ORCA");
    pr->glyph = '@';
    pr->colour = TUI_CYAN | TUI_BRIGHT;
    pr->recv_mask |= PARTY_CMASK(PARTY_C_VEHICLE) | PARTY_CMASK(PARTY_C_TRACKER);
    pr->send_mask |= PARTY_CMASK(PARTY_C_VEHICLE);
    snprintf(pr->kw[0], sizeof(pr->kw[0]), "axon");
    i = party_prof_new("WEEKEND");
    pr = party_prof(i);
    snprintf(pr->party, sizeof(pr->party), "TRAIL CREW");
    snprintf(pr->code, sizeof(pr->code), "pine ridge");
    pr->share = PARTY_SHARE_ROUGH;
    party_prof_preset(pr, 1);
    party_prof_select(0);
    if (mode && !strcmp(mode, "lobby")) return;
    party_start();
    {
        party_key_t k;
        party_key_derive("fox den 7", &k);
        s_party = k.party;
    }
    s_t0 = party_io_ms();
    s_world_ms = s_t0;
    for (int n = 0; n < (int)sizeof(s_gpx) - 80 && s_gpx_len < 3000;)
        n = s_gpx_len += snprintf(s_gpx + s_gpx_len, sizeof(s_gpx) - (size_t)s_gpx_len,
                                  "<trkpt lat=\"%.5f\" lon=\"%.5f\"/>\n", LAT0 + s_gpx_len * 1e-6, LON0 - s_gpx_len * 1e-6);
    for (uint32_t t = 0; t <= SEED_MS; t += 100) {
        ls_shim_time_set((int64_t)(T_END_MS - SEED_MS + t) * 1000);
        party_tick();
    }
}
