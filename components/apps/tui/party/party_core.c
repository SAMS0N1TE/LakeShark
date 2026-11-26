/* PARTY's state and its service pass (party_core.h). */
#include "party_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "party_io.h"
#include "../wardrive/wd_core.h"
#include "../tuilib/tui_core.h"

/* WARDRIVE's finds (party_wd.c). */
int party_wd_poll(party_find_t *out, int max, uint32_t since_ms,
                  bool (*want)(uint16_t tags, const char *name, const uint8_t *addr, uint8_t kind),
                  bool *running, bool *hooked);

#define NOTICES 6
#define SENT    128
#define RX_SLOTS 2
#define TX_RECV 8

typedef struct { uint32_t at; uint8_t left; } ptx_t;
typedef struct { uint32_t h, ms; } sent_t;
typedef struct { char title[20], body[96]; uint8_t hue; bool quiet; } notice_t;

typedef struct {
    bool on;
    FILE *f;
    party_ftx_t ftx;
    party_offer_t offer;
    uint8_t rid[TX_RECV], rdone[TX_RECV];
    uint16_t rbase[TX_RECV];
    uint8_t nrecv, offers_left, passes;
    uint32_t next_ms, last_ack_ms, idle_ms;
} ftx_t;

typedef struct {
    bool on;
    FILE *f;
    party_frx_t frx;
    uint8_t from;
    char name[PARTY_FNAME_MAX + 1];
    char path[128];
    uint32_t last_chunk_ms, last_ack_ms;
    uint16_t since_ack;
    uint8_t tries, done_acks;
    int rxf;
} frx_t;

typedef struct {
    party_t pub;
    const party_link_ops_t *link;
    party_prof_t profs[PARTY_PROFILES];
    int nprof, cur;
    ptx_t ptx[PARTY_PINGS];
    sent_t sent[SENT];
    notice_t notes[NOTICES];
    int note_h, note_n;
    uint32_t bucket_ms, air_cur, air_prev;
    uint32_t link_try_ms, wd_ms, wd_since, hello_due_ms;
    double v_lat, v_lon;           /* speed and course from fixes, when the GPS gives neither */
    uint32_t v_ms;
    float v_speed, v_course;
    uint8_t hello_burst;
    uint8_t vibe_step;
    uint32_t vibe_next;
    uint8_t bulk_caps;
    ftx_t ftx;
    frx_t frx[RX_SLOTS];
    uint8_t buf[PARTY_DGRAM_MAX];
    party_find_t finds[16];
} core_t;

static core_t *C;
#define P (C->pub)

/* ------------------------------------------------------------ tables */

/* Pairs of (on, off) in 20 ms steps, 0 ends; then the strength. */
static const struct { const char *name; uint8_t s[18]; uint8_t strength; } VIBE[PARTY_VIBES] = {
    { "TAP",       { 3, 0 }, 70 },
    { "DOUBLE",    { 3, 6, 3, 0 }, 75 },
    { "TRIPLE",    { 3, 5, 3, 5, 3, 0 }, 80 },
    { "LONG",      { 25, 0 }, 70 },
    { "HEARTBEAT", { 4, 4, 8, 30, 4, 4, 8, 0 }, 85 },
    { "RISING",    { 2, 8, 4, 8, 8, 0 }, 90 },
    { "SOS",       { 3, 3, 3, 3, 3, 8, 10, 4, 10, 4, 10, 8, 3, 3, 3, 3, 3, 0 }, 100 },
    { "CAR",       { 12, 6, 3, 6, 12, 0 }, 85 },
};
const char *party_vibe_name(int p) { return p >= 0 && p < PARTY_VIBES ? VIBE[p].name : "?"; }

const char *party_style_name(int s) { return ls_alert_name(s); }

static const uint8_t CLASS_HUE[PARTY_CLASSES] = {
    TUI_WHITE, TUI_RED, TUI_YELLOW, TUI_RED, TUI_BLUE, TUI_CYAN, TUI_MAGENTA, TUI_YELLOW,
    TUI_RED, TUI_MAGENTA, TUI_GREEN, TUI_MAGENTA, TUI_GREEN, TUI_YELLOW, TUI_CYAN, TUI_RED,
};

static const uint16_t AIR_PCT[PARTY_AIRS] = { 1, 3, 10, 25 };
static const uint16_t NEAR_M[PARTY_NEARS] = { 0, 500, 2000, 10000 };
static const int8_t MIN_RSSI[PARTY_RSSIS] = { -127, -85, -75, -65 };

#define ALWAYS (PARTY_CMASK(PARTY_C_MARK) | PARTY_CMASK(PARTY_C_HELP))
static const struct { const char *name; uint16_t mask; } PRESET[] = {
    { "FLOCK + POLICE", PARTY_CMASK(PARTY_C_ALPR) | PARTY_CMASK(PARTY_C_POLICE) | PARTY_CMASK(PARTY_C_BODYCAM) | PARTY_CMASK(PARTY_C_CAMERA) },
    { "DRONES", PARTY_CMASK(PARTY_C_DRONE) },
    { "TRACKERS + FOLLOW", PARTY_CMASK(PARTY_C_TRACKER) | PARTY_CMASK(PARTY_C_FOLLOW) },
    { "VEHICLES", PARTY_CMASK(PARTY_C_VEHICLE) | PARTY_CMASK(PARTY_C_POLICE) | PARTY_CMASK(PARTY_C_FOLLOW) },
    { "SKIMMERS + HACKS", PARTY_CMASK(PARTY_C_SKIMMER) | PARTY_CMASK(PARTY_C_HACKTOOL) | PARTY_CMASK(PARTY_C_WEARABLE) },
    { "EVERYTHING", 0xFFFF },
};
const char *party_preset_name(int i) { return i >= 0 && i < party_preset_count() ? PRESET[i].name : "?"; }
int party_preset_count(void) { return (int)(sizeof(PRESET) / sizeof(PRESET[0])); }

/* ------------------------------------------------------------ helpers */

void party_geo(double lat1, double lon1, double lat2, double lon2, float *metres, float *bearing)
{
    const double k = 111194.9266;
    const double n = (lat2 - lat1) * k;
    const double e = (lon2 - lon1) * k * cos((lat1 + lat2) * 0.5 * M_PI / 180.0);
    if (metres) *metres = (float)hypot(e, n);
    if (bearing) { double b = atan2(e, n) * 180.0 / M_PI; if (b < 0) b += 360; *bearing = (float)b; }
}

uint16_t party_unc_m(int kind, int rssi, float acc_m)
{
    double ref = -40, n = 2.7, d;
    switch (kind) {
    case WD_BLE: ref = -59; n = 2.2; break;
    case WD_DRONE: ref = -45; n = 2.3; break;
    case WD_RF: ref = -30; n = 2.5; break;
    case WD_LORA: ref = -35; n = 2.4; break;
    default: break;
    }
    if (kind == WD_CELL) d = 2000;
    else if (kind == WD_AIR) d = 20000;
    else d = pow(10.0, (ref - rssi) / (10.0 * n));
    if (d < 5) d = 5;
    if (d > 5000) d = 5000;
    d += acc_m > 0 ? acc_m : 15;
    return (uint16_t)(d > 65000 ? 65000 : d);
}

static uint32_t hash_find(const party_find_t *f)
{
    static const uint8_t z[6];
    if (memcmp(f->addr, z, 6)) return party_crc32(0, f->addr, 6) ^ f->kind;
    return party_crc32(0, f->name, strlen(f->name)) ^ 0x5A5A0000u ^ f->kind;
}

static void notice(const char *title, const char *body, uint8_t hue, bool quiet)
{
    notice_t *n;
    if (C->note_n == NOTICES) { C->note_h = (C->note_h + 1) % NOTICES; C->note_n--; }
    n = &C->notes[(C->note_h + C->note_n++) % NOTICES];
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->body, sizeof(n->body), "%s", body);
    n->hue = hue;
    n->quiet = quiet;
}

bool party_notice_pop(char *title, int tcap, char *body, int bcap, uint8_t *hue, bool *quiet)
{
    if (!C) return false;
    party_io_lock();
    bool got = C->note_n > 0;
    if (got) {
        const notice_t *n = &C->notes[C->note_h];
        snprintf(title, (size_t)tcap, "%s", n->title);
        snprintf(body, (size_t)bcap, "%s", n->body);
        *hue = n->hue;
        *quiet = n->quiet;
        C->note_h = (C->note_h + 1) % NOTICES;
        C->note_n--;
    }
    party_io_unlock();
    return got;
}

static const char *compass8(float b)
{
    static const char *const D[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return D[((int)floorf((b + 22.5f) / 45.0f)) & 7];
}

static void fmt_m(char *o, size_t n, float m)
{
    if (m < 1000) snprintf(o, n, "%d m", (int)m);
    else if (m < 10000) snprintf(o, n, "%.1f km", m / 1000.0f);
    else snprintf(o, n, "%d km", (int)(m / 1000));
}

static bool quiet_now(void)
{
    if (!P.prof.quiet) return false;
    const int h = party_io_local_hour();
    if (h < 0) return false;
    const int a = P.prof.quiet_from, b = P.prof.quiet_to;
    return a <= b ? (h >= a && h < b) : (h >= a || h < b);
}

/* ------------------------------------------------------------ vibration */

void party_vibe_play(int pattern)
{
    if (!C || pattern < 0 || pattern >= PARTY_VIBES) return;
    P.vibe_pat = (int8_t)pattern;
    P.vibe_ms = party_io_ms();
    C->vibe_step = 0;
    C->vibe_next = P.vibe_ms;
}

static void vibe_step(uint32_t now)
{
    if (P.vibe_pat < 0 || (int32_t)(now - C->vibe_next) < 0) return;
    const uint8_t *s = VIBE[P.vibe_pat].s;
    const uint8_t on = C->vibe_step < 18 ? s[C->vibe_step] : 0;
    if (!on) { P.vibe_pat = -1; return; }
    const uint8_t off = C->vibe_step + 1 < 18 ? s[C->vibe_step + 1] : 0;
    P.vibe_motor = party_io_vibe(on * 20, VIBE[P.vibe_pat].strength) || P.vibe_motor;
    C->vibe_next = now + (uint32_t)(on + off) * 20u;
    C->vibe_step += 2;
    if (!off) C->vibe_next = now + on * 20u;   /* let the last buzz finish before the next pattern */
}

/* ------------------------------------------------------------ profiles */

void party_prof_defaults(party_prof_t *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "PARTY");
    snprintf(p->party, sizeof(p->party), "CAMPFIRE");
    snprintf(p->call, sizeof(p->call), "LS%02X", (unsigned)(party_io_random() & 0xFF));
    p->glyph = '@';
    p->colour = TUI_CYAN | TUI_BRIGHT;
    p->share = PARTY_SHARE_EXACT;
    p->rate = PARTY_RATE_SMART;
    party_prof_preset(p, 0);
    p->send_mask |= PARTY_CMASK(PARTY_C_WATCH) | PARTY_CMASK(PARTY_C_CUSTOM) | PARTY_CMASK(PARTY_C_DRONE) | PARTY_CMASK(PARTY_C_FOLLOW);
    p->recv_mask = p->send_mask | PARTY_CMASK(PARTY_C_TRACKER);
    for (int c = 0; c < PARTY_CLASSES; c++) { p->style[c] = PARTY_STYLE_BANNER; p->vibe[c] = 0; }
    p->style[PARTY_C_ALPR] = PARTY_STYLE_VIBE;    p->vibe[PARTY_C_ALPR] = 1;
    p->style[PARTY_C_POLICE] = PARTY_STYLE_VIBE;  p->vibe[PARTY_C_POLICE] = 7;
    p->style[PARTY_C_BODYCAM] = PARTY_STYLE_VIBE; p->vibe[PARTY_C_BODYCAM] = 2;
    p->style[PARTY_C_DRONE] = PARTY_STYLE_VIBE;   p->vibe[PARTY_C_DRONE] = 5;
    p->style[PARTY_C_FOLLOW] = PARTY_STYLE_VIBE;  p->vibe[PARTY_C_FOLLOW] = 4;
    p->style[PARTY_C_HELP] = PARTY_STYLE_SOUND;   p->vibe[PARTY_C_HELP] = 6;
    p->quiet_from = 22; p->quiet_to = 7;
    p->mode = PARTY_MODE_AUTO;
    p->airtime = PARTY_AIR_10;
    p->files = 1;
}

void party_prof_preset(party_prof_t *p, int preset)
{
    if (preset < 0 || preset >= party_preset_count()) return;
    p->send_mask = p->recv_mask = (uint16_t)(PRESET[preset].mask | ALWAYS);
}

int party_prof_count(void) { return C ? C->nprof : 0; }
int party_prof_cur(void) { return C ? C->cur : 0; }
void party_prof_select(int i) { if (C && i >= 0 && i < C->nprof) C->cur = i; }
party_prof_t *party_prof(int i) { return C && i >= 0 && i < C->nprof ? &C->profs[i] : NULL; }

int party_prof_new(const char *name)
{
    if (!C || C->nprof >= PARTY_PROFILES) return -1;
    party_prof_t *p = &C->profs[C->nprof];
    if (C->nprof) {                    /* start from the one in use: same call sign, same tastes */
        *p = C->profs[C->cur];
        p->code[0] = 0;
    } else party_prof_defaults(p);
    snprintf(p->name, sizeof(p->name), "%s", name && *name ? name : "PARTY");
    C->cur = C->nprof++;
    return C->cur;
}

void party_prof_delete(int i)
{
    if (!C || i < 0 || i >= C->nprof) return;
    memmove(&C->profs[i], &C->profs[i + 1], (size_t)(C->nprof - i - 1) * sizeof(party_prof_t));
    C->nprof--;
    if (C->cur >= C->nprof) C->cur = C->nprof ? C->nprof - 1 : 0;
}

#define PROF_MAGIC 0x50504C31u   /* "1LPP" */
bool party_prof_save(void)
{
    if (!C || !party_io_card()) return false;
    char path[96];
    party_io_mkdir(party_io_dir());
    snprintf(path, sizeof(path), "%s/profiles.bin", party_io_dir());
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t hdr[4] = { PROF_MAGIC, (uint32_t)sizeof(party_prof_t), (uint32_t)C->nprof, (uint32_t)C->cur };
    bool ok = fwrite(hdr, sizeof(hdr), 1, f) == 1;
    if (C->nprof) ok = ok && fwrite(C->profs, sizeof(party_prof_t), (size_t)C->nprof, f) == (size_t)C->nprof;
    return fclose(f) == 0 && ok;
}

void party_prof_load(void)
{
    if (!C) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/profiles.bin", party_io_dir());
    FILE *f = party_io_card() ? fopen(path, "rb") : NULL;
    uint32_t hdr[4];
    if (f && fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == PROF_MAGIC && hdr[1] == sizeof(party_prof_t) &&
        hdr[2] <= PARTY_PROFILES && fread(C->profs, sizeof(party_prof_t), hdr[2], f) == hdr[2]) {
        C->nprof = (int)hdr[2];
        C->cur = hdr[3] < hdr[2] ? (int)hdr[3] : 0;
        for (int i = 0; i < C->nprof; i++) {   /* nothing read from a card is trusted to be terminated */
            party_prof_t *p = &C->profs[i];
            p->name[sizeof(p->name) - 1] = p->party[sizeof(p->party) - 1] = 0;
            p->code[sizeof(p->code) - 1] = p->call[sizeof(p->call) - 1] = 0;
            for (int k = 0; k < PARTY_KW; k++) p->kw[k][12] = 0;
            if (p->glyph < 33 || p->glyph > 126) p->glyph = '@';
        }
    }
    if (f) fclose(f);
}

/* ------------------------------------------------------------ sending */

static uint32_t air_used(uint32_t now)
{
    if (now - C->bucket_ms >= 60000) {
        C->air_prev = now - C->bucket_ms >= 120000 ? 0 : C->air_cur;
        C->air_cur = 0;
        C->bucket_ms = now;
    }
    const float frac = (now - C->bucket_ms) / 60000.0f;
    return (uint32_t)(C->air_prev * (1.0f - frac)) + C->air_cur;
}

/* prio 0 beacons, pings, ACKs: the whole budget; 1 file chunks: 80 % of it. */
static bool send(int len, uint8_t dst, bool bulk, int prio)
{
    if (len <= 0 || !P.link_up) return false;
    const uint32_t now = party_io_ms();
    const uint32_t air = C->link->airtime_ms(len, bulk);
    const uint32_t budget = AIR_PCT[P.prof.airtime < PARTY_AIRS ? P.prof.airtime : 2] * 600u;
    const uint32_t used = air_used(now);
    if (used + air > (prio ? budget * 4 / 5 : budget)) return false;
    if (!C->link->send(C->buf, len, dst, bulk)) return false;
    C->air_cur += air;
    P.air_used_ms = used + air;
    return true;
}

static party_hdr_t hdr(uint8_t dst)
{
    party_hdr_t h = { 0, P.me, dst, ++P.seq, P.key.party };
    return h;
}

static uint8_t my_caps(void)
{
    return (uint8_t)((P.link.caps & (PARTY_CAP_LORA | PARTY_CAP_GFSK | PARTY_CAP_FLRC)) | PARTY_CAP_FILES |
                     (P.heading >= 0 ? PARTY_CAP_COMPASS : 0));
}

static void send_hello(void)
{
    party_hello_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.call, sizeof(m.call), "%s", P.prof.call);
    m.glyph = P.prof.glyph; m.colour = P.prof.colour; m.caps = my_caps(); m.share = P.prof.share;
    m.want = P.prof.recv_mask;
    const int b = party_io_batt();
    m.batt = b < 0 ? 255 : (uint8_t)b;
    m.flags = P.help ? PARTY_POS_HELP : 0;
    party_hdr_t h = hdr(PARTY_ALL);
    if (send(party_enc_hello(C->buf, sizeof(C->buf), &h, &m), PARTY_ALL, false, 0)) P.hello_tx_ms = party_io_ms();
}

static bool send_pos(void)
{
    party_pos_t m;
    memset(&m, 0, sizeof(m));
    const bool rough = P.prof.share == PARTY_SHARE_ROUGH;
    m.lat_e7 = party_deg_e7(rough ? party_rough(P.lat, 0.01) : P.lat);
    m.lon_e7 = party_deg_e7(rough ? party_rough(P.lon, 0.01) : P.lon);
    m.acc_m = rough ? 800 : (uint16_t)(P.acc_m > 0 ? (P.acc_m > 65000 ? 65000 : P.acc_m) : 0);
    const float sp = rough || P.speed < 0 ? 0 : P.speed;
    m.speed_half = (uint8_t)(sp * 2 > 254 ? 255 : sp * 2 + 0.5f);
    m.course = (uint8_t)(P.course >= 0 && !rough ? (int)(P.course * 256 / 360) & 0xFF : 0);
    m.heading = (uint8_t)(P.heading >= 0 && !rough ? (int)(P.heading * 256 / 360) & 0xFF : 0);
    const int b = party_io_batt();
    m.batt = b < 0 ? 255 : (uint8_t)b;
    m.flags = (uint8_t)((rough ? PARTY_POS_ROUGH : 0) | (P.speed > 0.7f ? PARTY_POS_MOVING : 0) |
                        (P.prof.vehicle ? PARTY_POS_VEHICLE : 0) | (P.help ? PARTY_POS_HELP : 0) |
                        (!rough && P.course >= 0 ? PARTY_POS_COURSE : 0) | (!rough && P.heading >= 0 ? PARTY_POS_HEADING : 0));
    party_hdr_t h = hdr(PARTY_ALL);
    if (!send(party_enc_pos(C->buf, sizeof(C->buf), &h, &m), PARTY_ALL, false, 0)) return false;
    P.pos_tx_ms = party_io_ms();
    P.pos_tx_lat = P.lat; P.pos_tx_lon = P.lon;
    return true;
}

static void send_ping(int slot, bool repeat)
{
    const party_pingrec_t *r = &P.ping[slot];
    party_ping_t m;
    memset(&m, 0, sizeof(m));
    m.id = r->id; m.cls = r->cls; m.kind = r->kind; m.tags = r->tags; m.rssi = r->rssi;
    m.flags = (uint8_t)(r->flags | (repeat ? PARTY_PF_REPEAT : 0));
    double lat = r->lat, lon = r->lon;
    if (P.prof.share == PARTY_SHARE_ROUGH) { lat = party_rough(lat, 0.005); lon = party_rough(lon, 0.005); m.flags |= PARTY_PF_ROUGH; }
    m.lat_e7 = party_deg_e7(lat); m.lon_e7 = party_deg_e7(lon);
    m.unc_m = m.flags & PARTY_PF_ROUGH ? (uint16_t)(r->unc_m + 400) : r->unc_m;
    const uint32_t age = (party_io_ms() - r->seen_ms) / 1000u;
    m.age_s = (uint16_t)(age > 65535 ? 65535 : age);
    memcpy(m.addr, r->addr, 6);
    snprintf(m.label, sizeof(m.label), "%s", r->label);
    party_hdr_t h = hdr(PARTY_ALL);
    if (send(party_enc_ping(C->buf, sizeof(C->buf), &h, &m), PARTY_ALL, false, 0)) P.pings_out++;
}

/* ------------------------------------------------------------ members */

static party_member_t *member(uint8_t id, bool make)
{
    party_member_t *free_m = NULL, *old = NULL;
    for (int i = 0; i < PARTY_MEMBERS; i++) {
        party_member_t *m = &P.member[i];
        if (m->used && m->id == id) return m;
        if (!m->used && !free_m) free_m = m;
        if (m->used && (!old || m->last_ms < old->last_ms)) old = m;
    }
    if (!make) return NULL;
    party_member_t *m = free_m ? free_m : old;
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->id = id;
    snprintf(m->call, sizeof(m->call), "#%02X", id);
    m->glyph = '?';
    m->colour = TUI_WHITE;
    m->batt = 255;
    m->speed = m->course = m->heading = -1;
    m->join_ms = party_io_ms();
    return m;
}

int party_member_count(void)
{
    int n = 0;
    if (C) for (int i = 0; i < PARTY_MEMBERS; i++) n += P.member[i].used;
    return n;
}

static const char *who(uint8_t id)
{
    if (id == P.me) return P.prof.call;
    const party_member_t *m = member(id, false);
    return m ? m->call : "?";
}

/* ------------------------------------------------------------ pings */

static int ping_slot(void)
{
    int best = 0;
    for (int i = 0; i < PARTY_PINGS; i++) {
        if (!P.ping[i].used) return i;
        if (P.ping[i].rx_ms < P.ping[best].rx_ms) best = i;
    }
    return best;
}

static int ping_by_addr(const uint8_t addr[6], uint8_t kind)
{
    static const uint8_t z[6];
    if (!memcmp(addr, z, 6)) return -1;
    for (int i = 0; i < PARTY_PINGS; i++)
        if (P.ping[i].used && P.ping[i].kind == kind && !memcmp(P.ping[i].addr, addr, 6)) return i;
    return -1;
}

static void alert(const party_pingrec_t *r, const char *what)
{
    const int c = r->cls;
    int style = P.prof.style[c];
    if (style == PARTY_STYLE_OFF) return;
    float d = -1, b = 0;
    if (P.fix) party_geo(P.lat, P.lon, r->lat, r->lon, &d, &b);
    const uint16_t near = NEAR_M[P.prof.near < PARTY_NEARS ? P.prof.near : 0];
    if (near && d >= 0 && d > near + r->unc_m && c != PARTY_C_HELP) return;
    const bool q = quiet_now();
    if (style == PARTY_STYLE_SOUND && q) style = PARTY_STYLE_VIBE;
    char where[40], dist[16], unc[16], body[96];
    if (d >= 0) { fmt_m(dist, sizeof(dist), d); fmt_m(unc, sizeof(unc), r->unc_m); snprintf(where, sizeof(where), "%s %s +-%s", dist, compass8(b), unc); }
    else snprintf(where, sizeof(where), "no fix here");
    snprintf(body, sizeof(body), "%s%s %s  %s", what, party_class_name(c), r->label, where);
    if (ls_alert_buzz(style)) party_vibe_play(P.prof.vibe[c]);
    if (ls_alert_banner(style)) notice(r->who, body, CLASS_HUE[c], !ls_alert_sound(style));
}

static void add_reporter(party_pingrec_t *r, uint8_t id, double lat, double lon)
{
    for (int i = 0; i < r->nrep && i < 4; i++) if (r->rep[i] == id) return;
    if (r->nrep < 4) r->rep[r->nrep] = id;
    if (r->nrep < 255) r->nrep++;
    float d;
    party_geo(r->lat, r->lon, lat, lon, &d, NULL);
    if (d > r->spread_m) r->spread_m = d;
}

static void offer_find(const party_find_t *f)
{
    const int cls = party_class_of(f->tags, f->name, (const char (*)[13])P.prof.kw, PARTY_KW);
    if (cls < 0) return;
    P.finds_seen++;
    const uint32_t now = party_io_ms();
    const double lat = f->placed ? f->lat : P.lat, lon = f->placed ? f->lon : P.lon;
    if (!f->placed && !P.fix) return;
    /* Something a member already reported: say so, once. */
    const int k = ping_by_addr(f->addr, f->kind);
    if (k >= 0 && !P.ping[k].mine) {
        party_pingrec_t *r = &P.ping[k];
        const uint8_t before = r->nrep;
        add_reporter(r, P.me, lat, lon);
        if (r->nrep != before) { r->rx_ms = now; if (P.prof.recv_mask & PARTY_CMASK(cls)) alert(r, "NEAR YOU: "); }
    }
    if (!(P.prof.send_mask & PARTY_CMASK(cls))) return;
    if (f->rssi < MIN_RSSI[P.prof.min_rssi < PARTY_RSSIS ? P.prof.min_rssi : 0] && cls != PARTY_C_MARK) return;
    /* Once per ten minutes per device. */
    const uint32_t h = hash_find(f);
    int slot = -1, oldest = 0;
    for (int i = 0; i < SENT; i++) {
        if (C->sent[i].h == h && C->sent[i].ms && now - C->sent[i].ms < 600000u) return;
        if (C->sent[i].h == h) slot = i;
        if (C->sent[i].ms < C->sent[oldest].ms) oldest = i;
    }
    if (slot < 0) slot = oldest;
    C->sent[slot].h = h;
    C->sent[slot].ms = now ? now : 1;
    /* Nobody in the party wants this class: keep it here, send nothing. */
    uint16_t want = 0;
    int nmem = 0;
    for (int i = 0; i < PARTY_MEMBERS; i++) if (P.member[i].used) { want |= P.member[i].want; nmem++; }
    const bool wanted = !nmem || (want & PARTY_CMASK(cls));
    const int s = k >= 0 && P.ping[k].mine ? k : ping_slot();
    party_pingrec_t *r = &P.ping[s];
    memset(r, 0, sizeof(*r));
    r->used = r->mine = true;
    r->from = P.me;
    snprintf(r->who, sizeof(r->who), "%s", P.prof.call);
    r->id = ++P.ping_seq;
    r->cls = (uint8_t)cls; r->kind = f->kind; r->tags = f->tags; r->rssi = f->rssi;
    r->flags = (uint8_t)((f->placed ? PARTY_PF_PLACED : 0) | (cls == PARTY_C_MARK || cls == PARTY_C_HELP ? PARTY_PF_MANUAL : 0));
    r->lat = lat; r->lon = lon;
    r->unc_m = cls == PARTY_C_MARK || cls == PARTY_C_HELP ? (uint16_t)(P.acc_m > 0 ? P.acc_m : 10) : party_unc_m(f->kind, f->rssi, P.acc_m);
    r->seen_ms = f->ms && (int32_t)(now - f->ms) >= 0 ? f->ms : now;
    r->rx_ms = now;
    memcpy(r->addr, f->addr, 6);
    static const char *const KIND[] = { "WI-FI", "BT", "CELL", "DRONE", "RF", "AIR", "LORA" };   /* WARDRIVE's words: no call into its module */
    snprintf(r->label, sizeof(r->label), "%.24s", f->name[0] ? f->name : f->kind < 7 ? KIND[f->kind] : "?");
    r->nrep = 1;
    r->rep[0] = P.me;
    C->ptx[s].at = now;
    C->ptx[s].left = wanted ? 2 : 0;
}

void party_mark(int cls, const char *label)
{
    if (!C || !P.running || cls < 0 || cls >= PARTY_CLASSES) return;
    party_io_lock();
    const uint32_t now = party_io_ms();
    const int s = ping_slot();
    party_pingrec_t *p = &P.ping[s];
    memset(p, 0, sizeof(*p));
    p->used = p->mine = true;
    p->from = P.me;
    snprintf(p->who, sizeof(p->who), "%s", P.prof.call);
    p->id = ++P.ping_seq;
    p->cls = (uint8_t)cls;
    p->kind = WD_RF;
    p->flags = PARTY_PF_MANUAL;
    p->lat = P.lat; p->lon = P.lon;
    p->unc_m = (uint16_t)(P.acc_m > 0 ? P.acc_m : 10);
    p->seen_ms = p->rx_ms = now;
    snprintf(p->label, sizeof(p->label), "%s", label && *label ? label : party_class_name(cls));
    p->nrep = 1;
    p->rep[0] = P.me;
    /* a mark is a place: without a fix there is nothing to send */
    C->ptx[s].at = now;
    C->ptx[s].left = P.fix ? 2 : 0;
    party_io_unlock();
}

void party_set_help(bool on)
{
    if (!C || !P.running) return;
    P.help = on;
    if (on) party_mark(PARTY_C_HELP, "HELP");
    party_io_lock();
    send_hello();
    party_io_unlock();
}

/* Under wd_lock: cheap, no copies. A find is wanted when its class is one
   this profile sends, or when a member already reported the same device. */
static bool wd_want(uint16_t tags, const char *name, const uint8_t *addr, uint8_t kind)
{
    const int cls = party_class_of(tags, name, (const char (*)[13])P.prof.kw, PARTY_KW);
    if (cls < 0) return false;
    return (P.prof.send_mask & PARTY_CMASK(cls)) || ping_by_addr(addr, kind) >= 0;
}

void party_offer_find(const party_find_t *f)
{
    if (!C) return;
    party_io_lock();
    if (P.running) offer_find(f);
    party_io_unlock();
}

/* ------------------------------------------------------------ files */

static void tx_view_update(void)
{
    ftx_t *t = &C->ftx;
    party_xfer_view_t *v = &P.tx;
    v->nchunks = t->ftx.nchunks;
    v->receivers = t->nrecv;
    uint16_t lo = t->ftx.nchunks;
    uint8_t done = 0;
    for (int i = 0; i < t->nrecv; i++) {
        if (t->rdone[i]) { done++; continue; }
        if (t->rbase[i] < lo) lo = t->rbase[i];
    }
    v->done = done;
    v->acked = t->nrecv ? lo : 0;
}

static void tx_end(int state, const char *why)
{
    ftx_t *t = &C->ftx;
    if (t->f) fclose(t->f);
    t->f = NULL;
    t->on = false;
    P.tx.state = (uint8_t)state;
    P.tx.end_ms = party_io_ms();
    snprintf(P.tx.why, sizeof(P.tx.why), "%s", why ? why : "");
    char body[96];
    snprintf(body, sizeof(body), "%s: %s%s%s", P.tx.name, state == PARTY_X_DONE ? "delivered" : "not sent",
             why && *why ? ", " : "", why ? why : "");
    notice("PARTY FILE", body, state == PARTY_X_DONE ? TUI_GREEN : TUI_RED, true);
}

bool party_send_file(const char *path, char *why, int cap)
{
    if (!C || !P.running) { snprintf(why, (size_t)cap, "start the party"); return false; }
    if (C->ftx.on) { snprintf(why, (size_t)cap, "one file at a time"); return false; }
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, (size_t)cap, "cannot open it"); return false; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size <= 0 || size > PARTY_FILE_MAX) { fclose(f); snprintf(why, (size_t)cap, size <= 0 ? "empty" : "over 64 KB"); return false; }
    fseek(f, 0, SEEK_SET);
    uint32_t crc = 0;
    uint8_t b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) crc = party_crc32(crc, b, n);
    party_io_lock();
    ftx_t *t = &C->ftx;
    memset(t, 0, sizeof(*t));
    t->on = true;
    t->f = f;
    t->offer.fid = (uint16_t)(party_io_random() | 1);
    t->offer.size = (uint32_t)size;
    t->offer.crc = crc;
    t->offer.chunk = party_chunk_size(P.link.mtu ? P.link.mtu : PARTY_DGRAM_MAX);
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(t->offer.name, sizeof(t->offer.name), "%s", base);
    party_ftx_begin(&t->ftx, t->offer.fid, t->offer.size, t->offer.chunk);
    t->offers_left = 3;
    t->next_ms = party_io_ms();
    memset(&P.tx, 0, sizeof(P.tx));
    P.tx.state = PARTY_X_OFFER;
    snprintf(P.tx.name, sizeof(P.tx.name), "%s", t->offer.name);
    P.tx.size = t->offer.size;
    P.tx.start_ms = party_io_ms();
    tx_view_update();
    party_io_unlock();
    return true;
}

static void tx_step(uint32_t now)
{
    ftx_t *t = &C->ftx;
    if (!t->on || (int32_t)(now - t->next_ms) < 0) return;
    if (t->offers_left) {
        party_hdr_t h = hdr(PARTY_ALL);
        if (send(party_enc_offer(C->buf, sizeof(C->buf), &h, &t->offer), PARTY_ALL, false, 0)) t->offers_left--;
        t->next_ms = now + (t->offers_left ? 800 : 1500);
        t->idle_ms = now;
        return;
    }
    P.tx.state = PARTY_X_SEND;
    const int idx = party_ftx_next(&t->ftx);
    if (idx < 0) {
        /* A pass is out: wait for the ACKs to say what is missing. */
        P.tx.state = PARTY_X_WAIT;
        if (t->nrecv && P.tx.done == t->nrecv) { tx_end(PARTY_X_DONE, NULL); return; }
        const uint32_t last = t->last_ack_ms > t->idle_ms ? t->last_ack_ms : t->idle_ms;
        if (now - last > 8000) {
            if (!t->nrecv) tx_end(PARTY_X_FAIL, "nobody took it");
            else if (++t->passes >= 6) tx_end(P.tx.done ? PARTY_X_DONE : PARTY_X_FAIL, P.tx.done ? "some missed it" : "no answer");
            else {                                 /* ask again: resend what the slowest still needs */
                for (int i = 0; i < t->nrecv; i++)
                    if (!t->rdone[i]) {
                        party_ack_t a = { t->ftx.fid, t->rbase[i], 0, 0 };
                        party_ftx_ack(&t->ftx, &a);
                    }
                t->idle_ms = now;
            }
        }
        t->next_ms = now + 200;
        return;
    }
    party_chunk_t c;
    uint8_t data[180];
    const uint32_t off = (uint32_t)idx * t->offer.chunk;
    const uint32_t len = t->offer.size - off < t->offer.chunk ? t->offer.size - off : t->offer.chunk;
    if (fseek(t->f, (long)off, SEEK_SET) || fread(data, 1, len, t->f) != len) { tx_end(PARTY_X_FAIL, "card read"); return; }
    c.fid = t->offer.fid; c.idx = (uint16_t)idx; c.len = (uint8_t)len; c.data = data;
    party_hdr_t h = hdr(PARTY_ALL);
    if (send(party_enc_chunk(C->buf, sizeof(C->buf), &h, &c), PARTY_ALL, true, 1)) {
        P.tx.sent++;
        t->idle_ms = now;
        t->next_ms = now + C->link->airtime_ms(len + 11, true) + 5;
    } else {
        /* Over the airtime, or the link is busy: put it back and wait. */
        party_ack_t a = { t->ftx.fid, (uint16_t)idx, 0xFFFFFFFEu, 0 };
        party_ftx_ack(&t->ftx, &a);
        t->next_ms = now + 250;
    }
}

static void on_ack(uint8_t from, const party_ack_t *a, uint32_t now)
{
    ftx_t *t = &C->ftx;
    if (!t->on || a->fid != t->offer.fid) return;
    int i = 0;
    while (i < t->nrecv && t->rid[i] != from) i++;
    if (i == t->nrecv) {
        if (t->nrecv == TX_RECV) return;
        t->rid[i] = from; t->rdone[i] = 0; t->rbase[i] = 0;
        t->nrecv++;
    }
    t->rbase[i] = a->base;
    if (a->flags & (PARTY_ACK_DONE | PARTY_ACK_REFUSED)) { t->rdone[i] = 1; t->rbase[i] = t->ftx.nchunks; }
    party_ftx_ack(&t->ftx, a);
    t->last_ack_ms = now;
    tx_view_update();
}

static void send_ack(frx_t *r, uint8_t flags)
{
    party_ack_t a;
    party_frx_ack(&r->frx, &a);
    a.flags |= flags;
    party_hdr_t h = hdr(r->from);
    if (send(party_enc_ack(C->buf, sizeof(C->buf), &h, &a), r->from, true, 0)) {
        r->last_ack_ms = party_io_ms();
        r->since_ack = 0;
    }
}

static void clean_name(char *s)
{
    for (char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) *p = '_';
    if (!s[0] || s[0] == '.') s[0] = '_';
}

static void on_offer(uint8_t from, const party_offer_t *o, uint32_t now)
{
    for (int i = 0; i < RX_SLOTS; i++) if (C->frx[i].on && C->frx[i].frx.fid == o->fid && C->frx[i].from == from) return;
    frx_t *r = NULL;
    for (int i = 0; i < RX_SLOTS && !r; i++) if (!C->frx[i].on) r = &C->frx[i];
    party_frx_t tmp;
    const bool ok = P.prof.files && party_io_card() && r && party_frx_begin(&tmp, o);
    if (!ok) {
        frx_t no;
        memset(&no, 0, sizeof(no));
        no.from = from;
        no.frx.fid = o->fid;
        send_ack(&no, PARTY_ACK_REFUSED);
        return;
    }
    memset(r, 0, sizeof(*r));
    r->frx = tmp;
    r->from = from;
    snprintf(r->name, sizeof(r->name), "%s", o->name);
    clean_name(r->name);
    party_io_mkdir(party_io_dir());
    /* one .part per slot: two members sending the same name must not share a file */
    snprintf(r->path, sizeof(r->path), "%s/%s.%d.part", party_io_dir(), r->name, (int)(r - C->frx));
    r->f = fopen(r->path, "wb+");
    if (!r->f) return;
    r->on = true;
    r->last_chunk_ms = now;
    /* the list on FILES: newest first */
    memmove(&P.rxf[1], &P.rxf[0], sizeof(P.rxf[0]) * (PARTY_RXFILES - 1));
    if (P.nrxf < PARTY_RXFILES) P.nrxf++;
    for (int i = 0; i < RX_SLOTS; i++)
        if (C->frx[i].on && &C->frx[i] != r && C->frx[i].rxf >= 0 && ++C->frx[i].rxf >= PARTY_RXFILES)
            C->frx[i].rxf = -1;        /* its row fell off the list */
    r->rxf = 0;
    party_rxfile_t *v = &P.rxf[0];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", r->name);
    snprintf(v->from, sizeof(v->from), "%s", who(from));
    v->size = o->size;
    v->ms = now;
    v->state = PARTY_X_SEND;
    send_ack(r, 0);                    /* "I am taking it" */
}

static void rx_finish(frx_t *r, uint32_t now)
{
    party_rxfile_t *v = r->rxf >= 0 && r->rxf < PARTY_RXFILES ? &P.rxf[r->rxf] : NULL;
    uint32_t crc = 0;
    uint8_t b[256];
    size_t n;
    fflush(r->f);
    fseek(r->f, 0, SEEK_SET);
    uint32_t left = r->frx.size;
    while (left && (n = fread(b, 1, left < sizeof(b) ? left : sizeof(b), r->f)) > 0) { crc = party_crc32(crc, b, n); left -= (uint32_t)n; }
    fclose(r->f);
    r->f = NULL;
    char final[128];
    char body[96];
    if (crc != r->frx.crc) {
        remove(r->path);
        if (v) v->state = PARTY_X_FAIL;
        snprintf(body, sizeof(body), "%s from %s arrived damaged", r->name, who(r->from));
        notice("PARTY FILE", body, TUI_RED, true);
    } else {
        snprintf(final, sizeof(final), "%s/%s", party_io_dir(), r->name);
        for (int k = 1; k < 100; k++) {
            FILE *e = fopen(final, "rb");
            if (!e) break;
            fclose(e);
            snprintf(final, sizeof(final), "%s/%d-%s", party_io_dir(), k, r->name);
        }
        rename(r->path, final);
        if (v) { v->state = PARTY_X_DONE; v->got_bytes = v->size; }
        snprintf(body, sizeof(body), "%s from %s, %u bytes", r->name, who(r->from), (unsigned)r->frx.size);
        notice("PARTY FILE", body, TUI_GREEN, true);
    }
    (void)now;
    send_ack(r, 0);
    r->done_acks = 1;
}

static void on_chunk(uint8_t from, const party_chunk_t *c, uint32_t now)
{
    for (int i = 0; i < RX_SLOTS; i++) {
        frx_t *r = &C->frx[i];
        if (!r->on || r->from != from || r->frx.fid != c->fid) continue;
        if (!r->f) {                   /* done already: the sender missed our DONE */
            if (now - r->last_ack_ms > 1000) send_ack(r, 0);
            return;
        }
        uint32_t off;
        if (party_frx_put(&r->frx, c, &off) == 1) {
            if (fseek(r->f, (long)off, SEEK_SET) || fwrite(c->data, 1, c->len, r->f) != c->len) {
                fclose(r->f); r->f = NULL; remove(r->path); r->on = false;
                if (r->rxf >= 0) P.rxf[r->rxf].state = PARTY_X_FAIL;
                return;
            }
            r->since_ack++;
            r->tries = 0;
            if (r->rxf >= 0) P.rxf[r->rxf].got_bytes = (uint32_t)r->frx.got * r->frx.chunk;
        }
        r->last_chunk_ms = now;
        if (party_frx_complete(&r->frx)) rx_finish(r, now);
        else if (r->since_ack >= 16) send_ack(r, 0);
        return;
    }
}

static void rx_step(uint32_t now)
{
    for (int i = 0; i < RX_SLOTS; i++) {
        frx_t *r = &C->frx[i];
        if (!r->on) continue;
        if (!r->f) {                   /* finished: a second DONE, then let go */
            if (r->done_acks < 2 && now - r->last_ack_ms > 1500) { send_ack(r, 0); r->done_acks++; }
            if (now - r->last_ack_ms > 20000) r->on = false;
            continue;
        }
        if (now - r->last_chunk_ms > 1500 && now - r->last_ack_ms > 1500) {
            if (++r->tries > 15) {
                fclose(r->f); r->f = NULL; remove(r->path); r->on = false;
                if (r->rxf >= 0) P.rxf[r->rxf].state = PARTY_X_FAIL;
                continue;
            }
            send_ack(r, 0);
        }
    }
}

/* ------------------------------------------------------------ receiving */

static void on_ping(party_member_t *m, const party_ping_t *p, uint32_t now)
{
    for (int i = 0; i < PARTY_PINGS; i++)
        if (P.ping[i].used && !P.ping[i].mine && P.ping[i].from == m->id && P.ping[i].id == p->id) return;
    const double lat = party_e7_deg(p->lat_e7), lon = party_e7_deg(p->lon_e7);
    const int k = ping_by_addr(p->addr, p->kind);
    if (k >= 0) {
        party_pingrec_t *r = &P.ping[k];
        const uint8_t before = r->nrep;
        add_reporter(r, m->id, lat, lon);
        if (r->nrep == before) return;
        /* the newest place: a car moves */
        r->lat = lat; r->lon = lon; r->unc_m = p->unc_m;
        r->rx_ms = now;
        r->seen_ms = now - p->age_s * 1000u;
        P.pings_in++;
        char what[24];
        snprintf(what, sizeof(what), "AGAIN (%u): ", r->nrep);
        if (P.prof.recv_mask & PARTY_CMASK(r->cls)) alert(r, what);
        return;
    }
    if (!(P.prof.recv_mask & PARTY_CMASK(p->cls))) return;
    const int s = ping_slot();
    party_pingrec_t *r = &P.ping[s];
    memset(r, 0, sizeof(*r));
    C->ptx[s].left = 0;
    r->used = true;
    r->from = m->id;
    snprintf(r->who, sizeof(r->who), "%s", m->call);
    r->id = p->id; r->cls = p->cls; r->kind = p->kind; r->flags = p->flags; r->tags = p->tags; r->rssi = p->rssi;
    r->lat = lat; r->lon = lon; r->unc_m = p->unc_m;
    r->seen_ms = now - p->age_s * 1000u;
    r->rx_ms = now;
    memcpy(r->addr, p->addr, 6);
    snprintf(r->label, sizeof(r->label), "%s", p->label);
    r->nrep = 1;
    r->rep[0] = m->id;
    P.pings_in++;
    alert(r, "");
}

static void on_rx(const party_rx_t *rx, uint32_t now)
{
    party_hdr_t h;
    party_msg_t m;
    const int t = party_decode(rx->data, rx->len, P.key.party, &h, &m);
    if (t < 0) return;
    P.heard_ms[P.heard_i++ & 15] = now ? now : 1;
    if (h.src == P.me) {
        /* Another board picked my number: the one with the other call sign keeps it. */
        if (t == PARTY_HELLO && strncmp(m.hello.call, P.prof.call, PARTY_CALL_MAX)) {
            P.me = (uint8_t)(1 + party_io_random() % 254);
            C->hello_burst = 2;
            C->hello_due_ms = now;
        }
        return;
    }
    if (h.dst != PARTY_ALL && h.dst != P.me) return;
    if (t == PARTY_BYE) {
        party_member_t *mm = member(h.src, false);
        if (mm) {
            char body[48];
            snprintf(body, sizeof(body), "%s left the party", mm->call);
            notice("PARTY", body, TUI_YELLOW, true);
            mm->used = false;
        }
        return;
    }
    const bool known = member(h.src, false) != NULL;
    party_member_t *mm = member(h.src, true);
    if (mm->heard) {
        const uint8_t gap = (uint8_t)(h.seq - mm->seq);
        if (gap > 1 && gap < 64) mm->lost += gap - 1;
    }
    mm->seq = h.seq;
    if (mm->heard < 65535) mm->heard++;
    mm->last_ms = now;
    mm->rssi = rx->rssi; mm->snr = rx->snr; mm->via = rx->via;
    if (!known && now - P.hello_tx_ms > 5000) { C->hello_due_ms = now; if (!C->hello_burst) C->hello_burst = 1; }   /* a newcomer: tell them who I am */
    switch (t) {
    case PARTY_HELLO: {
        const bool first = mm->call[0] == '#';
        snprintf(mm->call, sizeof(mm->call), "%s", m.hello.call);
        mm->glyph = m.hello.glyph >= 33 && m.hello.glyph <= 126 ? m.hello.glyph : '?';
        mm->colour = m.hello.colour & 0x0F ? m.hello.colour & 0x0F : TUI_WHITE;
        mm->caps = m.hello.caps; mm->share = m.hello.share; mm->want = m.hello.want;
        if (m.hello.batt != 255) mm->batt = m.hello.batt;
        mm->pflags = (uint8_t)((mm->pflags & ~PARTY_POS_HELP) | (m.hello.flags & PARTY_POS_HELP));
        if (first) {
            char body[48];
            snprintf(body, sizeof(body), "%s joined %s", mm->call, P.prof.party);
            notice("PARTY", body, TUI_GREEN, true);
        }
        break;
    }
    case PARTY_POS: {
        const double lat = party_e7_deg(m.pos.lat_e7), lon = party_e7_deg(m.pos.lon_e7);
        if (mm->pos) {
            float d;
            party_geo(mm->lat, mm->lon, lat, lon, &d, NULL);
            if (d > 3) {
                memmove(&mm->tlat[1], &mm->tlat[0], sizeof(double) * (PARTY_TRAIL - 1));
                memmove(&mm->tlon[1], &mm->tlon[0], sizeof(double) * (PARTY_TRAIL - 1));
                mm->tlat[0] = mm->lat; mm->tlon[0] = mm->lon;
                if (mm->ntrail < PARTY_TRAIL) mm->ntrail++;
            }
        }
        mm->pos = true;
        mm->lat = lat; mm->lon = lon; mm->acc_m = m.pos.acc_m;
        mm->speed = m.pos.speed_half / 2.0f;
        mm->course = m.pos.flags & PARTY_POS_COURSE ? m.pos.course * 360.0f / 256 : -1;
        mm->heading = m.pos.flags & PARTY_POS_HEADING ? m.pos.heading * 360.0f / 256 : -1;
        if (m.pos.batt != 255) mm->batt = m.pos.batt;
        const bool help_was = mm->pflags & PARTY_POS_HELP;
        mm->pflags = m.pos.flags;
        mm->pos_ms = now;
        if ((mm->pflags & PARTY_POS_HELP) && !help_was) party_vibe_play(P.prof.vibe[PARTY_C_HELP]);
        break;
    }
    case PARTY_PING: on_ping(mm, &m.ping, now); break;
    case PARTY_OFFER: on_offer(h.src, &m.offer, now); break;
    case PARTY_CHUNK: on_chunk(h.src, &m.chunk, now); break;
    case PARTY_ACK: if (h.dst == P.me) on_ack(h.src, &m.ack, now); break;
    default: break;
    }
}

/* ------------------------------------------------------------ the pass */

static uint32_t pos_interval(void)
{
    switch (P.prof.rate) {
    case PARTY_RATE_10S: return 10000;
    case PARTY_RATE_30S: return 30000;
    case PARTY_RATE_1M: return 60000;
    case PARTY_RATE_5M: return 300000;
    default:
        if (P.prof.vehicle || P.speed > 5) return 10000;
        if (P.speed > 0.7f) return 30000;
        return 120000;
    }
}

static party_link_cfg_t link_cfg(void)
{
    party_link_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.key, P.key.key, sizeof(c.key));
    c.party = P.key.party;
    c.channel = P.key.channel;
    c.mode = P.prof.mode;
    c.airtime_pct = (uint8_t)AIR_PCT[P.prof.airtime < PARTY_AIRS ? P.prof.airtime : 2];
    c.bulk_caps = C->bulk_caps;
    return c;
}

static void find_step(uint32_t now)
{
    if (!P.find_kind || !P.fix) return;
    double lat, lon;
    if (P.find_kind == 1) {
        const party_member_t *m = member((uint8_t)P.find_id, false);
        if (!m || !m->pos) return;
        lat = m->lat; lon = m->lon;
    } else {
        if (P.find_id >= PARTY_PINGS || !P.ping[P.find_id].used) { P.find_kind = 0; return; }
        lat = P.ping[P.find_id].lat; lon = P.ping[P.find_id].lon;
    }
    party_geo(P.lat, P.lon, lat, lon, &P.find_m, &P.find_brg);
    /* HUNT's ticking, felt: every 6 s far away, every 0.4 s within 10 m. */
    float iv = 300.0f + P.find_m * 12.0f;
    if (iv < 400) iv = 400;
    if (iv > 6000) iv = 6000;
    if (P.vibe_pat < 0 && now - P.find_buzz_ms >= (uint32_t)iv) {
        P.find_buzz_ms = now;
        party_vibe_play(P.find_m < 15 ? 1 : 0);
    }
}

void party_find_set(int kind, int id)
{
    if (!C) return;
    party_io_lock();
    P.find_kind = (uint8_t)(kind == 1 || kind == 2 ? kind : 0);
    P.find_id = (uint16_t)id;
    P.find_buzz_ms = 0;
    P.find_m = -1;
    party_io_unlock();
}

int party_tick(void)
{
    if (!C) return 0;
    party_io_lock();
    if (!P.running) { party_io_unlock(); return 0; }
    const uint32_t now = party_io_ms();
    /* where I am */
    party_fix_t fx;
    memset(&fx, 0, sizeof(fx));
    fx.speed_mps = fx.course_deg = fx.heading_deg = -1;
    P.fix = party_io_fix(&fx);
    if (P.fix) {
        P.lat = fx.lat; P.lon = fx.lon; P.acc_m = fx.acc_m;
        if (fx.speed_mps < 0) {
            if (!C->v_ms) { C->v_lat = fx.lat; C->v_lon = fx.lon; C->v_ms = now; C->v_speed = C->v_course = -1; }
            else if (now - C->v_ms >= 4000) {
                float d, b;
                party_geo(C->v_lat, C->v_lon, fx.lat, fx.lon, &d, &b);
                C->v_speed = d * 1000.0f / (float)(now - C->v_ms);
                C->v_course = d > 4 ? b : C->v_course;
                C->v_lat = fx.lat; C->v_lon = fx.lon; C->v_ms = now;
            }
            fx.speed_mps = C->v_speed;
            if (fx.course_deg < 0) fx.course_deg = C->v_course;
        }
        P.speed = fx.speed_mps; P.course = fx.course_deg;
    }
    P.heading = fx.heading_deg;
    /* the link */
    if (!P.link_up && now - C->link_try_ms >= 10000) {
        C->link_try_ms = now;
        const party_link_cfg_t cfg = link_cfg();
        P.link_up = C->link->open(&cfg);
        if (P.link_up) { C->hello_burst = 3; C->hello_due_ms = now; }
    }
    C->link->stats(&P.link);
    snprintf(P.link_why, sizeof(P.link_why), "%s", P.link_up ? "" : P.link.why ? P.link.why : "no radio");
    if (P.link_up) {
        party_rx_t rx;
        for (int n = 0; n < 16 && C->link->poll(&rx); n++) on_rx(&rx, now);
        /* what every member's radio can do decides the bulk mode */
        uint8_t caps = P.link.caps & (PARTY_CAP_FLRC | PARTY_CAP_GFSK);
        for (int i = 0; i < PARTY_MEMBERS; i++) if (P.member[i].used && P.member[i].caps) caps &= P.member[i].caps;
        if (caps != C->bulk_caps) { C->bulk_caps = caps; const party_link_cfg_t cfg = link_cfg(); C->link->reconfig(&cfg); }
        /* hello: a burst on joining, then every minute */
        if ((C->hello_burst || now - P.hello_tx_ms >= 60000) && (int32_t)(now - C->hello_due_ms) >= 0) {
            send_hello();
            if (C->hello_burst) C->hello_burst--;
            C->hello_due_ms = now + (C->hello_burst ? 2500 : 60000);
        }
        /* position */
        if (P.prof.share != PARTY_SHARE_OFF && P.fix) {
            float moved = 0;
            if (P.pos_tx_ms) party_geo(P.pos_tx_lat, P.pos_tx_lon, P.lat, P.lon, &moved, NULL);
            const uint32_t since = now - P.pos_tx_ms;
            if (!P.pos_tx_ms || since >= pos_interval() || (P.prof.share == PARTY_SHARE_EXACT && moved > 50 && since >= 5000))
                send_pos();
        }
        /* pings waiting to go (and their one repeat) */
        for (int i = 0; i < PARTY_PINGS; i++) {
            ptx_t *q = &C->ptx[i];
            if (!q->left || (int32_t)(now - q->at) < 0 || !P.ping[i].used || !P.ping[i].mine) continue;
            send_ping(i, q->left == 1);
            q->left--;
            q->at = now + 8000;
        }
        tx_step(now);
        rx_step(now);
    }
    /* WARDRIVE's finds, every two seconds */
    if (now - C->wd_ms >= 2000) {
        C->wd_ms = now;
        bool running = false, hooked = false;
        const int n = party_wd_poll(C->finds, 16, C->wd_since, wd_want, &running, &hooked);
        C->wd_since = now;
        P.wd_running = running;
        P.wd_hooked = hooked;
        for (int i = 0; i < n; i++) offer_find(&C->finds[i]);
    }
    find_step(now);
    vibe_step(now);
    P.air_used_ms = air_used(now);
    const int wait = P.vibe_pat >= 0 ? 20 : 100;
    party_io_unlock();
    return wait;
}

/* ------------------------------------------------------------ life */

bool party_init(void)
{
    if (C) return true;
    C = party_io_alloc(sizeof(core_t));
    if (!C) return false;
    P.vibe_pat = -1;
    C->link = party_io_link();
    party_prof_load();
    return true;
}

const party_t *party_state(void) { return C ? &P : NULL; }
bool party_running(void) { return C && P.running; }

bool party_start(void)
{
    if (!C || P.running || !C->nprof) return false;
    const party_prof_t *pr = &C->profs[C->cur];
    party_key_t k;
    if (!party_key_derive(pr->code, &k)) return false;
    party_io_lock();
    const bool motor = P.vibe_motor;
    memset(&P, 0, sizeof(P));
    P.vibe_motor = motor;
    P.vibe_pat = -1;
    P.prof = *pr;
    P.key = k;
    P.me = (uint8_t)(1 + party_io_random() % 254);
    P.speed = P.course = P.heading = -1;
    P.start_ms = party_io_ms();
    P.running = true;
    memset(C->ptx, 0, sizeof(C->ptx));
    memset(C->sent, 0, sizeof(C->sent));
    memset(C->frx, 0, sizeof(C->frx));
    memset(&C->ftx, 0, sizeof(C->ftx));
    C->bucket_ms = P.start_ms;
    C->air_cur = C->air_prev = 0;
    C->wd_ms = 0;
    C->wd_since = P.start_ms;
    C->bulk_caps = 0;
    C->link_try_ms = P.start_ms - 10000;
    C->v_ms = 0;
    party_io_unlock();
    return party_io_spawn(party_tick);
}

void party_prof_applied(void)
{
    if (!C || !P.running) return;
    party_io_lock();
    const party_prof_t *pr = &C->profs[C->cur];
    /* the code is the party: a new one is a new party, which STOP + START makes */
    char code[sizeof(P.prof.code)];
    memcpy(code, P.prof.code, sizeof(code));
    P.prof = *pr;
    memcpy(P.prof.code, code, sizeof(code));
    if (P.link_up) { const party_link_cfg_t cfg = link_cfg(); C->link->reconfig(&cfg); }
    C->hello_due_ms = party_io_ms();
    C->hello_burst = 1;
    party_io_unlock();
}

void party_stop(void)
{
    if (!C || !P.running) return;
    party_io_lock();
    if (P.link_up) {
        party_hdr_t h = hdr(PARTY_ALL);
        send(party_enc_bye(C->buf, sizeof(C->buf), &h), PARTY_ALL, false, 0);
        C->link->close();
    }
    if (C->ftx.f) fclose(C->ftx.f);
    C->ftx.f = NULL;
    C->ftx.on = false;
    for (int i = 0; i < RX_SLOTS; i++) {
        if (C->frx[i].f) { fclose(C->frx[i].f); remove(C->frx[i].path); }
        C->frx[i].f = NULL;
        C->frx[i].on = false;
    }
    P.running = false;
    P.link_up = false;
    P.find_kind = 0;
    party_io_unlock();
}

int party_peer_since(party_find_t *out, int max, uint32_t since_ms)
{
    if (!C || !P.running) return 0;
    int n = 0;
    party_io_lock();
    for (int i = 0; i < PARTY_PINGS && n < max; i++) {
        const party_pingrec_t *r = &P.ping[i];
        if (!r->used || r->mine || (int32_t)(r->rx_ms - since_ms) <= 0) continue;
        party_find_t *f = &out[n++];
        memset(f, 0, sizeof(*f));
        memcpy(f->addr, r->addr, 6);
        f->kind = r->kind;
        static const uint16_t CLASS_TAG[PARTY_CLASSES] = {
            0, WD_T_ALPR, WD_T_CAMERA, WD_T_BODYCAM, WD_T_POLICE, WD_T_DRONE, WD_T_TRACKER, WD_T_VEHICLE,
            0, WD_T_SKIMMER, WD_T_HACKTOOL, WD_T_WEARABLE, WD_T_INFRA, WD_T_WATCH, 0, 0,
        };
        /* FOLLOW, REGULAR and NEW HERE were the member's view of its own route, not this board's */
        const uint16_t mine_only = (uint16_t)(WD_T_FOLLOW | WD_T_REGULAR | WD_T_NEWHERE);
        f->tags = (uint16_t)(((r->tags | CLASS_TAG[r->cls < PARTY_CLASSES ? r->cls : 0]) & ~mine_only) | WD_T_PEER);
        f->rssi = r->rssi;
        snprintf(f->name, sizeof(f->name), "%s:%s", r->who, r->label);
        f->placed = true;
        f->lat = r->lat; f->lon = r->lon;
        f->ms = r->rx_ms;
    }
    party_io_unlock();
    return n;
}
