/* LSX, the LakeShark link: discovery, sessions, turns, objects and the mode
   ladder (ls_lsx.h has the design). No radio, clock or allocator of the
   board in here except the PSRAM preference; ls_lsx_svc.c gives it a radio. */
#include "ls_lsx.h"
#include "ls_lsx_crypto.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *lsx_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#else
static void *lsx_alloc(size_t n) { return calloc(1, n); }
#endif

/* ---- modes and regions -------------------------------------------------- */

/* min_rssi is the datasheet sensitivity plus about 10 dB, to be tuned on the
   bench: SX1262 GFSK 50 kbps -104, 200 kbps -97; LR2021 FLRC 260 kbps CR 1/2
   -111, 650 kbps CR 1/2 -104 (estimated), 1.3 Mbps CR 3/4 -97 (estimated).
   LoRa SF7/125 kHz decodes to about -124 dBm and has no floor here. */
static const ls_lsx_mode_info_t MODES[LSX_M_N] = {
    { "LORA",    LSX_K_LORA, 5469,    4375,   0,      125000,  0, 0, -128, 2 },
    { "GFSK50",  LSX_K_GFSK, 50000,   50000,  25000,  156200,  0, 0, -94,  8 },
    { "GFSK200", LSX_K_GFSK, 200000,  200000, 50000,  373600,  0, 0, -87,  8 },
    { "FLRC130", LSX_K_FLRC, 260000,  130000, 0,      307000,  8, 0, -101, 8 },
    { "FLRC325", LSX_K_FLRC, 650000,  325000, 0,      740000,  5, 0, -94,  8 },
    { "FLRC975", LSX_K_FLRC, 1300000, 975000, 0,      1333000, 3, 1, -87,  12 },
};

const ls_lsx_mode_info_t *ls_lsx_mode_info(int mode)
{
    return mode >= 0 && mode < LSX_M_N ? &MODES[mode] : &MODES[0];
}

static const ls_lsx_region_t REGIONS[LSX_REGION_N] = {
    { "US915", 919100000u, { 917500000u, 923300000u, 926300000u }, 3, 300, 20, 0x3F },
    { "EU868", 869460000u, { 869460000u, 869460000u, 869460000u }, 1, 100, 14,
      (1u << LSX_M_LORA) | (1u << LSX_M_GFSK50) },
};

const ls_lsx_region_t *ls_lsx_region(int region)
{
    return region >= 0 && region < LSX_REGION_N ? &REGIONS[region] : &REGIONS[0];
}

uint32_t ls_lsx_airtime_us(int mode, int len)
{
    const ls_lsx_mode_info_t *m = ls_lsx_mode_info(mode);
    if (m->kind == LSX_K_LORA) {
        /* SF7, 125 kHz, CR 4/5, explicit header, CRC, preamble 8, no LDRO
           (Semtech AN1200.13). */
        const int sf = 7, cr = 1, crc = 1;
        int num = 8 * len - 4 * sf + 28 + 16 * crc;
        int ceil_term = num > 0 ? (num + 4 * sf - 1) / (4 * sf) : 0;
        const int symbols = 8 + ceil_term * (cr + 4);
        const uint32_t ts_us = 1024;   /* 2^7 / 125 kHz */
        return ts_us * 8 + ts_us * 425 / 100 + ts_us * (uint32_t)symbols;
    }
    (void)len;
    if (m->kind == LSX_K_GFSK)   /* 32-bit preamble, 32-bit sync, 255 bytes */
        return (uint32_t)(((32u + 32u + 8u * LSX_FRAME_MAX) * 1000000ull + m->bitrate - 1) / m->bitrate);
    /* FLRC, datasheet 18.3: preamble 32, timing preamble 21 and sync 32 bits
       uncoded, then payload and the 6-bit tail through the coding rate. */
    static const uint8_t num[4] = { 6, 9, 12, 8 };
    const uint32_t coded = (12u * (8u * LSX_FRAME_MAX + 6u) + num[m->flrc_cr] - 1) / num[m->flrc_cr];
    return (uint32_t)(((32u + 21u + 32u + coded) * 1000000ull + m->bitrate - 1) / m->bitrate);
}

/* ---- wire --------------------------------------------------------------- */

enum {
    T_PROBE = 1, T_WELCOME, T_BEACON, T_DGRAM,
    T_POLL = 8, T_PONG, T_BEGIN, T_DATA, T_ACK, T_MODE, T_BYE, T_SDGRAM,
};
#define F_SESS  0x01    /* session keys (else the network's)         */
#define F_POLL  0x02    /* master: the slave's turn after this frame */
#define F_OVER  0x04    /* slave: the master's turn after this frame */
#define F_MORE  0x08    /* the sender has more queued than it sent   */

#define A_DONE    1
#define A_UNKNOWN 2
#define A_REJECT  4

#define CHUNK     (LSX_BODY_MAX - 4)                        /* 228 */
#define NCH_MAX   ((LSX_OBJ_MAX + CHUNK - 1) / CHUNK)        /* 144 */
#define BITS_LEN  ((NCH_MAX + 7) / 8)
#define TXQ       4
#define DGQ       4
#define PEERS     8
#define PLAN_MAX  12
#define BEGIN_FIXED 12  /* id 2, port 1, len 4, crc 4, mlen 1 */

#define PROBE_LISTEN_MS  320
#define SILENCE_MS       1500
#define DEAD_MS          10000
#define KEEPALIVE_MS     250
#define BEACON_MS        60000

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static bool after(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

typedef struct {
    bool used, begun;
    uint8_t port, mlen;
    uint16_t id, nch;
    const uint8_t *data;
    uint32_t len, crc;
    uint8_t meta[LSX_META_MAX];
    uint8_t acked[BITS_LEN];
    void *tag;
} txobj_t;

typedef struct {
    bool active;
    uint32_t peer, len, crc;
    uint16_t id, nch, have;
    uint8_t port, mlen;
    uint8_t meta[LSX_META_MAX];
    uint8_t *buf;
    uint8_t bits[BITS_LEN];
} rxobj_t;

typedef struct {
    ls_lsx_peer_t p;
    uint32_t ctr_seen[8];
    uint8_t ctr_at;
} peer_t;

enum { I_ACK = 1, I_MODE, I_BYE, I_DGRAM, I_BEGIN, I_DATA, I_POLL, I_PONG };
enum { AFTER_NONE, AFTER_WELCOME, AFTER_BYE, AFTER_SWITCH };

struct ls_lsx {
    ls_lsx_cfg_t cfg;
    uint32_t now;
    lsx_aes_t g_enc, g_mac, s_enc, s_mac;
    uint32_t gctr;
    uint8_t state;
    bool held, txing, probe_now, closing_req;
    int tuned_mode;
    uint32_t tuned_hz;
    uint8_t after;
    uint32_t t_claim, t_next_probe, t_probe_end, t_beacon;
    uint32_t t_bcast;           /* no broadcast datagram before this: ALOHA spacing */
    bool probe_sent;
    uint8_t probe_nonce[8];
    uint8_t rx_drop;
    uint32_t rng;

    /* A probe being answered (listener). */
    struct {
        bool due;
        uint32_t at, peer;
        uint8_t nf[8], caps, role;
        int8_t rssi;
        char name[LSX_NAME];
    } wel;

    struct {
        bool on, master;
        uint32_t peer, t_open, hz;
        char name[LSX_NAME];
        uint8_t peer_caps;
        uint16_t sid;
        uint8_t ladder[LSX_M_N];
        int nlad, li, probe_from;
        uint32_t t_probe, t_mode;
        uint32_t blocked_until[LSX_M_N];
        uint32_t tx_ctr, rx_max;
        uint64_t rx_win;
        bool rx_any;
        bool my_turn, got_frame;
        uint32_t deadline, t_rx_last, t_next_poll;
        int miss_run;
        uint8_t hist;
        bool peer_more;
        bool mode_pending, mode_probe;
        int mode_target;
        bool switch_on_over;
        int slave_target;
        bool slave_probe;
        bool bye_pending;
        bool ack_due, ack_unknown;
        uint16_t unknown_id;
        uint16_t done_ids[4];
        uint8_t done_at;
        uint16_t last_done, reject_id;
        bool ack_reject;
        uint32_t t_turn_ok;
        int8_t rssi, snr, rssi_at_peer;
        uint8_t plan[PLAN_MAX];
        uint16_t plan_arg[PLAN_MAX];
        int plan_n, plan_i;
        uint16_t next_id;
        uint8_t begun_turn;
    } s;

    txobj_t tx[TXQ];
    rxobj_t rx, partial;
    struct { bool used; uint32_t dst; uint8_t port, len; uint8_t d[LSX_BODY_MAX]; } dq[DGQ];
    peer_t peers[PEERS];

    uint32_t air_us[60];
    uint32_t air_sec;
    ls_lsx_status_t st;
    uint32_t last_heard;
    bool heard_any;
    bool clock_set;             /* the deadlines above hold real times, not 0 */
    uint8_t txbuf[LSX_FRAME_MAX];
    uint8_t rxbuf[LSX_FRAME_MAX];
};

static uint32_t rnd(ls_lsx_t *x)
{
    if (x->cfg.rand32) return x->cfg.rand32();
    x->rng ^= x->rng << 13;
    x->rng ^= x->rng >> 17;
    x->rng ^= x->rng << 5;
    return x->rng;
}

static void keys_group(ls_lsx_t *x)
{
    uint8_t k[16];
    lsx_kdf(x->cfg.key, "LSX group enc", NULL, 0, k);
    lsx_aes_init(&x->g_enc, k);
    lsx_kdf(x->cfg.key, "LSX group mac", NULL, 0, k);
    lsx_aes_init(&x->g_mac, k);
    memset(k, 0, sizeof(k));
}

ls_lsx_t *ls_lsx_create(const ls_lsx_cfg_t *cfg)
{
    if (!cfg || !cfg->phy || !cfg->id || cfg->id == LSX_BCAST) return NULL;
    ls_lsx_t *x = lsx_alloc(sizeof(*x));
    if (!x) return NULL;
    x->cfg = *cfg;
    x->cfg.name[LSX_NAME - 1] = 0;
    if (!x->cfg.probe_ms) x->cfg.probe_ms = 8000;
    if (x->cfg.max_mode >= LSX_M_N) x->cfg.max_mode = LSX_M_N - 1;
    x->rng = cfg->id ^ 0x9E3779B9u;
    x->gctr = rnd(x);
    x->tuned_mode = -1;
    x->state = cfg->role ? LSX_ST_WAIT_RADIO : LSX_ST_OFF;
    x->st.role = cfg->role;
    x->st.last_heard_ms = UINT32_MAX;
    keys_group(x);
    return x;
}

static void session_end(ls_lsx_t *x, int event);

void ls_lsx_destroy(ls_lsx_t *x)
{
    if (!x) return;
    if (x->s.on) session_end(x, LSX_EV_CLOSED);
    if (x->held) x->cfg.phy->claim(x->cfg.phy->ctx, false);
    free(x->rx.buf);
    free(x->partial.buf);
    lsx_aes_free(&x->g_enc);
    lsx_aes_free(&x->g_mac);
    lsx_aes_free(&x->s_enc);
    lsx_aes_free(&x->s_mac);
    free(x);
}

void ls_lsx_set_key(ls_lsx_t *x, const uint8_t key[16])
{
    memcpy(x->cfg.key, key, 16);
    keys_group(x);
}

void ls_lsx_set_power(ls_lsx_t *x, int8_t dbm) { x->cfg.power_dbm = dbm; x->tuned_mode = -1; }
void ls_lsx_set_max_mode(ls_lsx_t *x, int mode) { x->cfg.max_mode = (uint8_t)(mode < 0 ? 0 : mode >= LSX_M_N ? LSX_M_N - 1 : mode); }
void ls_lsx_probe_now(ls_lsx_t *x) { x->probe_now = true; }
void ls_lsx_close(ls_lsx_t *x) { x->closing_req = true; }
void ls_lsx_set_rx_drop(ls_lsx_t *x, uint8_t share) { x->rx_drop = share; }

const char *ls_lsx_state_name(int state)
{
    static const char *const n[LSX_ST_N] = { "OFF", "WAIT RADIO", "LISTEN", "PROBE", "SESSION" };
    return state >= 0 && state < LSX_ST_N ? n[state] : "?";
}

/* ---- airtime -------------------------------------------------------------- */

static void air_roll(ls_lsx_t *x)
{
    const uint32_t sec = x->now / 1000;
    if (sec - x->air_sec >= 60) memset(x->air_us, 0, sizeof(x->air_us));
    else
        for (uint32_t s = x->air_sec + 1; s <= sec; s++) x->air_us[s % 60] = 0;
    x->air_sec = sec;
}

static uint32_t air_used_ms(const ls_lsx_t *x)
{
    uint64_t t = 0;
    for (int i = 0; i < 60; i++) t += x->air_us[i];
    return (uint32_t)(t / 1000);
}

static bool air_ok(ls_lsx_t *x, uint32_t us)
{
    const uint32_t cap_ms = 60u * ls_lsx_region(x->cfg.region)->duty_permille;
    return air_used_ms(x) + us / 1000 <= cap_ms;
}

/* ---- radio ---------------------------------------------------------------- */

static int8_t power(const ls_lsx_t *x)
{
    const int8_t m = ls_lsx_region(x->cfg.region)->max_dbm;
    return x->cfg.power_dbm > m ? m : x->cfg.power_dbm;
}

static int tune(ls_lsx_t *x, int mode, uint32_t hz)
{
    if (x->tuned_mode == mode && x->tuned_hz == hz) return 0;
    const int e = x->cfg.phy->tune(x->cfg.phy->ctx, mode, hz, power(x));
    x->tuned_mode = e ? -1 : mode;
    x->tuned_hz = hz;
    return e;
}

static void release(ls_lsx_t *x)
{
    if (x->held) x->cfg.phy->claim(x->cfg.phy->ctx, false);
    x->held = false;
    x->txing = false;
    x->tuned_mode = -1;
}

/* Seal and send: header, CTR body, CMAC tag, cipher-noise padding. */
static int send_frame(ls_lsx_t *x, uint8_t type, uint8_t flags, uint32_t dst, const uint8_t *body, int blen)
{
    if (blen < 0 || blen > LSX_BODY_MAX || x->txing || x->tuned_mode < 0) return -1;
    const bool sess = (flags & F_SESS) != 0;
    const bool fixed = ls_lsx_mode_info(x->tuned_mode)->kind != LSX_K_LORA;
    const int len = fixed ? LSX_FRAME_MAX : LSX_HDR + blen + LSX_TAG;
    const uint32_t us = ls_lsx_airtime_us(x->tuned_mode, len);
    if (!air_ok(x, us)) return -1;
    uint8_t *f = x->txbuf;
    const uint32_t ctr = sess ? x->s.tx_ctr++ : x->gctr++;
    f[0] = (uint8_t)(0x40 | type);
    f[1] = flags;
    put32(f + 2, x->cfg.id);
    put32(f + 6, dst);
    put32(f + 10, ctr);
    f[14] = (uint8_t)blen;
    if (blen) memcpy(f + LSX_HDR, body, (size_t)blen);
    uint8_t iv[16];
    memcpy(iv, f + 2, 4);
    memcpy(iv + 4, f + 10, 4);
    memcpy(iv + 8, "LSX1", 4);
    memset(iv + 12, 0, 4);
    lsx_aes_t *enc = sess ? &x->s_enc : &x->g_enc, *mac = sess ? &x->s_mac : &x->g_mac;
    lsx_ctr(enc, iv, f + LSX_HDR, (size_t)blen);
    uint8_t tag[16];
    lsx_cmac(mac, f, (size_t)(LSX_HDR + blen), tag);
    memcpy(f + LSX_HDR + blen, tag, LSX_TAG);
    const int used = LSX_HDR + blen + LSX_TAG;
    if (len > used) {
        memset(f + used, 0, (size_t)(len - used));
        iv[12] = 0x80;     /* a keystream the body never uses */
        lsx_ctr(enc, iv, f + used, (size_t)(len - used));
    }
    if (x->cfg.phy->send(x->cfg.phy->ctx, f, len) != 0) return -1;
    x->txing = true;
    x->air_us[(x->now / 1000) % 60] += us;
    x->st.frames_tx++;
    return 0;
}

/* ---- peers -------------------------------------------------------------- */

static peer_t *peer_get(ls_lsx_t *x, uint32_t id, bool make)
{
    peer_t *old = NULL;
    for (int i = 0; i < PEERS; i++) {
        peer_t *p = &x->peers[i];
        if (p->p.id == id) return p;
        if (!old || (old->p.id && (!p->p.id || (int32_t)(p->p.heard_ms - old->p.heard_ms) < 0))) old = p;
    }
    if (!make) return NULL;
    memset(old, 0, sizeof(*old));
    old->p.id = id;
    return old;
}

/* Network-key frames have no session to count against: refuse only a
   counter this peer used in its last eight. */
static bool group_fresh(peer_t *p, uint32_t ctr)
{
    for (int i = 0; i < 8; i++)
        if (p->ctr_seen[i] == ctr && (p->ctr_seen[i] || p->ctr_at)) return false;
    p->ctr_seen[p->ctr_at++ & 7] = ctr;
    return true;
}

static bool session_fresh(ls_lsx_t *x, uint32_t ctr)
{
    if (!x->s.rx_any) {
        x->s.rx_any = true;
        x->s.rx_max = ctr;
        x->s.rx_win = 1;
        return true;
    }
    if (ctr > x->s.rx_max) {
        const uint32_t d = ctr - x->s.rx_max;
        x->s.rx_win = d >= 64 ? 1 : (x->s.rx_win << d) | 1;
        x->s.rx_max = ctr;
        return true;
    }
    const uint32_t d = x->s.rx_max - ctr;
    if (d >= 64 || (x->s.rx_win >> d) & 1) return false;
    x->s.rx_win |= 1ull << d;
    return true;
}

/* ---- objects ------------------------------------------------------------ */

static bool bit(const uint8_t *b, int i) { return (b[i >> 3] >> (i & 7)) & 1; }
static void setbit(uint8_t *b, int i) { b[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static txobj_t *tx_cur(ls_lsx_t *x) { return x->tx[0].used ? &x->tx[0] : NULL; }

static void tx_pop(ls_lsx_t *x, int result)
{
    txobj_t done = x->tx[0];
    memmove(&x->tx[0], &x->tx[1], sizeof(x->tx[0]) * (TXQ - 1));
    memset(&x->tx[TXQ - 1], 0, sizeof(x->tx[0]));
    x->st.obj_tx_done = x->st.obj_tx_len = 0;
    if (x->cfg.app.on_sent) x->cfg.app.on_sent(x->cfg.app.ctx, x->s.peer, done.port, done.tag, result);
}

int ls_lsx_send_object(ls_lsx_t *x, uint8_t port, const uint8_t *meta, int mlen,
                       const uint8_t *data, uint32_t len, void *tag)
{
    if (!x || !x->s.on || len > LSX_OBJ_MAX || mlen < 0 || mlen > LSX_META_MAX || (len && !data)) return -1;
    for (int i = 0; i < TXQ; i++) {
        txobj_t *t = &x->tx[i];
        if (t->used) continue;
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->port = port;
        t->mlen = (uint8_t)mlen;
        if (mlen) memcpy(t->meta, meta, (size_t)mlen);
        t->data = data;
        t->len = len;
        t->crc = lsx_crc32(0, data, len);
        t->nch = (uint16_t)((len + CHUNK - 1) / CHUNK);
        t->id = x->s.next_id++;
        t->tag = tag;
        return 0;
    }
    return -1;
}

static bool bcast_queued(const ls_lsx_t *x);

int ls_lsx_send_dgram(ls_lsx_t *x, uint32_t dst, uint8_t port, const uint8_t *data, int len)
{
    if (!x || len < 0 || len > LSX_BODY_MAX - 1) return -1;
    if (dst != LSX_BCAST && (!x->s.on || dst != x->s.peer)) return -1;
    if (dst == LSX_BCAST && !bcast_queued(x)) {
        /* Boards that queue at the same moment (a beacon each on the same
           tick) would start together and lose both frames: a random wait of
           up to a few of this frame's air times first. */
        const uint32_t a = ls_lsx_airtime_us(LSX_M_LORA, LSX_HDR + 1 + len + LSX_TAG) / 1000u;
        const uint32_t t = x->now + rnd(x) % (3 * a + 50);
        if (after(t, x->t_bcast)) x->t_bcast = t;
    }
    for (int i = 0; i < DGQ; i++)
        if (!x->dq[i].used) {
            x->dq[i].used = true;
            x->dq[i].dst = dst;
            x->dq[i].port = port;
            x->dq[i].len = (uint8_t)len;
            if (len) memcpy(x->dq[i].d, data, (size_t)len);
            return 0;
        }
    return -1;
}

static int inline_cap(const txobj_t *t) { return LSX_BODY_MAX - BEGIN_FIXED - t->mlen; }

static void rx_drop_obj(rxobj_t *r)
{
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

/* Cut off with chunks still missing: kept, so the same object offered again
   (by CRC and length) picks up where it stopped. */
static void rx_park(ls_lsx_t *x)
{
    if (!x->rx.active) return;
    if (x->rx.have && x->rx.have < x->rx.nch) {
        rx_drop_obj(&x->partial);
        x->partial = x->rx;
        memset(&x->rx, 0, sizeof(x->rx));
    } else {
        rx_drop_obj(&x->rx);
    }
}

static void rx_complete(ls_lsx_t *x)
{
    rxobj_t *r = &x->rx;
    const bool ok = lsx_crc32(0, r->buf, r->len) == r->crc;
    x->s.ack_due = true;
    if (ok) {
        x->s.done_ids[x->s.done_at++ & 3] = r->id;
        x->s.last_done = r->id;
        if (x->cfg.app.on_object)
            x->cfg.app.on_object(x->cfg.app.ctx, r->peer, r->port, r->meta, r->mlen, r->buf, r->len);
    } else {          /* tell the sender to start over */
        x->s.ack_reject = true;
        x->s.reject_id = r->id;
    }
    x->st.obj_rx_done = x->st.obj_rx_len = 0;
    rx_drop_obj(r);
}

static bool done_recently(ls_lsx_t *x, uint16_t id)
{
    for (int i = 0; i < 4; i++)
        if (x->s.done_ids[i] == id && id != 0xFFFF) return true;
    return false;
}

static void on_begin(ls_lsx_t *x, const uint8_t *b, int n)
{
    if (n < BEGIN_FIXED) return;
    const uint16_t id = get16(b);
    const uint8_t port = b[2], mlen = b[11];
    const uint32_t len = get32(b + 3), crc = get32(b + 7);
    if (mlen > LSX_META_MAX || BEGIN_FIXED + mlen > n || len > LSX_OBJ_MAX) return;
    x->s.ack_due = true;
    if (x->s.ack_unknown && x->s.unknown_id == id) x->s.ack_unknown = false;
    if (done_recently(x, id) || (x->rx.active && x->rx.id == id)) return;
    rx_park(x);
    rxobj_t *r = &x->rx;
    const int inl = n - BEGIN_FIXED - mlen;
    if (x->partial.active && x->partial.peer == x->s.peer && x->partial.crc == crc && x->partial.len == len) {
        *r = x->partial;           /* resume */
        memset(&x->partial, 0, sizeof(x->partial));
        r->id = id;
        x->st.resumed++;
    } else {
        memset(r, 0, sizeof(*r));
        r->buf = lsx_alloc(len ? len : 1);
        if (!r->buf) return;
        r->peer = x->s.peer;
        r->len = len;
        r->crc = crc;
        r->id = id;
        r->nch = (uint16_t)((len + CHUNK - 1) / CHUNK);
    }
    r->active = true;
    r->port = port;
    r->mlen = mlen;
    memcpy(r->meta, b + BEGIN_FIXED, mlen);
    x->st.obj_rx_len = len;
    if (inl > 0 && (uint32_t)inl == len) {
        memcpy(r->buf, b + BEGIN_FIXED + mlen, len);
        r->have = r->nch;
        for (int i = 0; i < r->nch; i++) setbit(r->bits, i);
    }
    x->st.obj_rx_done = r->have >= r->nch ? len : (uint32_t)r->have * CHUNK;
    if (r->have >= r->nch) rx_complete(x);
}

static void on_data(ls_lsx_t *x, const uint8_t *b, int n)
{
    if (n < 5) return;
    const uint16_t id = get16(b), ch = get16(b + 2);
    rxobj_t *r = &x->rx;
    x->s.ack_due = true;
    if (!r->active || r->id != id) {
        if (!done_recently(x, id)) {
            x->s.ack_unknown = true;
            x->s.unknown_id = id;
        }
        return;
    }
    if (ch >= r->nch) return;
    const uint32_t off = (uint32_t)ch * CHUNK;
    const uint32_t want = r->len - off < CHUNK ? r->len - off : CHUNK;
    if ((uint32_t)(n - 4) < want) return;
    if (!bit(r->bits, ch)) {
        memcpy(r->buf + off, b + 4, want);
        setbit(r->bits, ch);
        r->have++;
        x->st.bytes_rx += want;
        x->st.obj_rx_done += want;
    }
    if (r->have >= r->nch) rx_complete(x);
}

static void on_ack(ls_lsx_t *x, const uint8_t *b, int n)
{
    if (n < 10) return;
    const uint16_t id = get16(b), base = get16(b + 2);
    const uint32_t bits = get32(b + 4);
    const uint8_t fl = b[8];
    x->s.rssi_at_peer = (int8_t)b[9];
    txobj_t *t = tx_cur(x);
    if (!t || t->id != id) return;
    if (fl & A_DONE) { tx_pop(x, LSX_SENT_OK); return; }
    if (fl & A_REJECT) { memset(t->acked, 0, sizeof(t->acked)); t->begun = false; return; }
    if (fl & A_UNKNOWN) { t->begun = false; return; }
    t->begun = true;
    for (int i = 0; i < base && i < t->nch; i++) setbit(t->acked, i);
    for (int j = 0; j < 32; j++)
        if ((bits >> j) & 1 && base + 1 + j < t->nch) setbit(t->acked, base + 1 + j);
    uint32_t done = 0;
    for (int i = 0; i < t->nch; i++)
        if (bit(t->acked, i)) done += i == t->nch - 1 ? t->len - (uint32_t)i * CHUNK : CHUNK;
    x->st.obj_tx_done = done;
}

static int build_ack(ls_lsx_t *x, uint8_t *b)
{
    uint16_t id = 0, base = 0;
    uint32_t bits = 0;
    uint8_t fl = 0;
    if (x->s.ack_unknown) {
        id = x->s.unknown_id;
        fl = A_UNKNOWN;
        x->s.ack_unknown = false;
    } else if (x->s.ack_reject) {
        id = x->s.reject_id;
        fl = A_REJECT;
        x->s.ack_reject = false;
    } else if (x->rx.active) {
        rxobj_t *r = &x->rx;
        id = r->id;
        while (base < r->nch && bit(r->bits, base)) base++;
        for (int j = 0; j < 32; j++)
            if (base + 1 + j < r->nch && bit(r->bits, base + 1 + j)) bits |= 1u << j;
    } else {
        id = x->s.last_done;
        fl = id == 0xFFFF ? 0 : A_DONE;
    }
    put16(b, id);
    put16(b + 2, base);
    put32(b + 4, bits);
    b[8] = fl;
    b[9] = (uint8_t)x->s.rssi;
    return 10;
}

/* ---- sessions ------------------------------------------------------------ */

static uint32_t reply_timeout(int mode)
{
    const ls_lsx_mode_info_t *m = ls_lsx_mode_info(mode);
    /* The peer's whole window at full size, a few milliseconds of SPI and
       turnaround a frame on the board, and slack. */
    return (m->window * (ls_lsx_airtime_us(mode, LSX_FRAME_MAX) + 6000u)) / 1000u + 60u;
}

static int cur_mode(const ls_lsx_t *x) { return x->s.ladder[x->s.li]; }

/* The slave's patience before it drops to LoRa: longer than three of the
   master's turn timeouts, so a master that has just changed mode is not
   left behind. A mode being tried is given up after half of it. */
static uint32_t silence(int mode)
{
    const uint32_t t = 3 * reply_timeout(mode) + 500;
    return t > SILENCE_MS ? t : SILENCE_MS;
}

/* Idle polls: four a second, slower on LoRa where each costs ~40 ms. */
static uint32_t keepalive(int mode) { return ls_lsx_mode_info(mode)->kind == LSX_K_LORA ? 1500 : KEEPALIVE_MS; }

/* Before answering a turn: the far end has just finished sending and needs
   a moment to be listening again. */
static uint32_t turnaround(int mode) { return ls_lsx_mode_info(mode)->kind == LSX_K_LORA ? 6 : 3; }

static void session_tune(ls_lsx_t *x)
{
    x->tuned_mode = -1;
    tune(x, cur_mode(x), x->s.hz);
    x->s.t_mode = x->now;
    x->st.mode = (uint8_t)cur_mode(x);
    x->st.mode_changes++;
    if (x->cfg.app.on_session) x->cfg.app.on_session(x->cfg.app.ctx, x->s.peer, LSX_EV_MODE);
}

static void session_keys(ls_lsx_t *x, const uint8_t nf[8], const uint8_t nh[8], uint32_t slave, uint32_t master,
                         uint16_t sid)
{
    uint8_t ctx[26], ks[16], k[16];
    memcpy(ctx, nf, 8);
    memcpy(ctx + 8, nh, 8);
    put32(ctx + 16, slave);
    put32(ctx + 20, master);
    put16(ctx + 24, sid);
    lsx_kdf(x->cfg.key, "LSX session", ctx, sizeof(ctx), ks);
    lsx_kdf(ks, "enc", NULL, 0, k);
    lsx_aes_init(&x->s_enc, k);
    lsx_kdf(ks, "mac", NULL, 0, k);
    lsx_aes_init(&x->s_mac, k);
    memset(ks, 0, sizeof(ks));
    memset(k, 0, sizeof(k));
}

static uint8_t ladder_mask(const ls_lsx_t *x, uint8_t peer_caps)
{
    const uint8_t mine = (uint8_t)x->cfg.phy->caps, both = mine & peer_caps;
    uint8_t m = 1u << LSX_M_LORA;
    if (both & LSX_CAP_FLRC) m |= (1u << LSX_M_FLRC130) | (1u << LSX_M_FLRC325) | (1u << LSX_M_FLRC975);
    else if (both & LSX_CAP_GFSK) m |= (1u << LSX_M_GFSK50) | (1u << LSX_M_GFSK200);
    m &= ls_lsx_region(x->cfg.region)->modes;
    for (int i = x->cfg.max_mode + 1; i < LSX_M_N; i++) m &= (uint8_t)~(1u << i);
    return m | 1u;
}

static void session_open(ls_lsx_t *x, bool master, uint32_t peer, const char *name, uint8_t caps,
                         uint8_t mask, int start_mode, uint32_t hz, uint16_t sid)
{
    memset(&x->s, 0, sizeof(x->s));
    x->s.on = true;
    x->s.master = master;
    x->s.peer = peer;
    memcpy(x->s.name, name, LSX_NAME);
    x->s.name[LSX_NAME - 1] = 0;
    x->s.peer_caps = caps;
    x->s.sid = sid;
    x->s.hz = hz;
    x->s.probe_from = -1;
    x->s.t_open = x->now;
    x->s.t_rx_last = x->now;
    /* Not 0: after() would read 0 as 24.8 days ahead past 2^31 ms uptime. */
    x->s.t_turn_ok = x->s.deadline = x->s.t_next_poll = x->s.t_probe = x->s.t_mode = x->now;
    for (int m = 0; m < LSX_M_N; m++) x->s.blocked_until[m] = x->now;
    x->s.rssi_at_peer = -128;
    for (int m = 0; m < LSX_M_N; m++)
        if (mask & (1u << m)) {
            if (m == start_mode) x->s.li = x->s.nlad;
            x->s.ladder[x->s.nlad++] = (uint8_t)m;
        }
    for (int i = 0; i < 4; i++) x->s.done_ids[i] = 0xFFFF;
    x->s.last_done = 0xFFFF;
    x->s.my_turn = master;
    x->state = LSX_ST_SESSION;
    x->st.sessions++;
    x->st.peer = peer;
    memcpy(x->st.peer_name, x->s.name, LSX_NAME);
    x->st.master = master;
    x->st.bytes_rx = x->st.bytes_tx = 0;
    x->st.mode_changes = 0;
    session_tune(x);
    x->st.mode_changes = 0;
    if (x->cfg.app.on_session) x->cfg.app.on_session(x->cfg.app.ctx, peer, LSX_EV_OPEN);
}

static void session_end(ls_lsx_t *x, int event)
{
    if (!x->s.on) return;
    const uint32_t peer = x->s.peer;
    x->s.on = false;
    rx_park(x);
    while (tx_cur(x)) tx_pop(x, LSX_SENT_LOST);
    for (int i = 0; i < DGQ; i++)
        if (x->dq[i].dst != LSX_BCAST) x->dq[i].used = false;
    x->st.session_ms = x->now - x->s.t_open;
    x->after = AFTER_NONE;
    x->tuned_mode = -1;
    if (x->cfg.role == LSX_ROLE_FIELD) {
        release(x);
        x->t_next_probe = x->now + (event == LSX_EV_LOST ? 2000u : x->cfg.probe_ms);
        x->state = LSX_ST_WAIT_RADIO;
    } else {
        x->state = LSX_ST_LISTEN;
    }
    if (x->cfg.app.on_session) x->cfg.app.on_session(x->cfg.app.ctx, peer, event);
}

/* A turn's frames, planned when the turn starts so the last can carry
   POLL or OVER. */
static void plan_turn(ls_lsx_t *x)
{
    int n = 0;
    const int w = ls_lsx_mode_info(cur_mode(x))->window;
    if (x->s.ack_due || x->s.ack_unknown || x->s.ack_reject) { x->s.plan[n++] = I_ACK; x->s.ack_due = false; }
    if (x->s.master && x->s.mode_pending && n < w) x->s.plan[n++] = I_MODE;
    if (x->s.master && x->s.bye_pending && n < w) { x->s.plan[n++] = I_BYE; }
    for (int i = 0; i < DGQ && n < w && !x->s.bye_pending; i++)
        if (x->dq[i].used && x->dq[i].dst == x->s.peer) { x->s.plan[n] = I_DGRAM; x->s.plan_arg[n++] = (uint16_t)i; }
    txobj_t *t = tx_cur(x);
    if (t && !x->s.bye_pending && n < w) {
        const bool inl = (int)t->len <= inline_cap(t);
        int missing = 0;
        for (int i = 0; !inl && i < t->nch; i++) missing += !bit(t->acked, i);
        /* BEGIN until it is known; again when every chunk is acked but the
           DONE was lost, which makes the receiver say DONE again. */
        if (!t->begun || !missing) x->s.plan[n++] = I_BEGIN;
        for (int i = 0; !inl && i < t->nch && n < w; i++)
            if (!bit(t->acked, i)) { x->s.plan[n] = I_DATA; x->s.plan_arg[n++] = (uint16_t)i; }
    }
    if (!n) x->s.plan[n++] = x->s.master ? I_POLL : I_PONG;
    x->s.plan_n = n;
    x->s.plan_i = 0;
}

static bool more_queued(const ls_lsx_t *x)
{
    if (x->tx[0].used) return true;
    for (int i = 0; i < DGQ; i++)
        if (x->dq[i].used && x->dq[i].dst != LSX_BCAST) return true;
    return false;
}

static void send_plan_item(ls_lsx_t *x)
{
    const int i = x->s.plan_i;
    const bool last = i == x->s.plan_n - 1;
    uint8_t fl = F_SESS;
    if (last) fl |= x->s.master ? F_POLL : F_OVER;
    uint8_t b[LSX_BODY_MAX];
    int n = 0;
    uint8_t type = T_POLL;
    txobj_t *t = tx_cur(x);
    switch (x->s.plan[i]) {
    case I_ACK: type = T_ACK; n = build_ack(x, b); break;
    case I_MODE:
        type = T_MODE;
        b[0] = (uint8_t)x->s.ladder[x->s.mode_target];
        b[1] = x->s.mode_probe;
        n = 2;
        x->s.mode_pending = false;
        x->s.switch_on_over = true;
        break;
    case I_BYE: type = T_BYE; n = 0; break;
    case I_DGRAM: {
        const int k = x->s.plan_arg[i];
        type = T_SDGRAM;
        b[0] = x->dq[k].port;
        memcpy(b + 1, x->dq[k].d, x->dq[k].len);
        n = 1 + x->dq[k].len;
        x->dq[k].used = false;
        break;
    }
    case I_BEGIN:
        if (!t) break;
        type = T_BEGIN;
        put16(b, t->id);
        b[2] = t->port;
        put32(b + 3, t->len);
        put32(b + 7, t->crc);
        b[11] = t->mlen;
        memcpy(b + BEGIN_FIXED, t->meta, t->mlen);
        n = BEGIN_FIXED + t->mlen;
        if ((int)t->len <= inline_cap(t)) {
            if (t->len) memcpy(b + n, t->data, t->len);   /* an empty object has no data pointer */
            n += (int)t->len;
        }
        break;
    case I_DATA: {
        /* The plan was made at the start of the turn; an ACK DONE heard
           since can have moved a smaller object to the head of the queue. */
        if (!t || x->s.plan_arg[i] >= t->nch) break;
        const uint16_t ch = x->s.plan_arg[i];
        const uint32_t off = (uint32_t)ch * CHUNK;
        const uint32_t k = t->len - off < CHUNK ? t->len - off : CHUNK;
        type = T_DATA;
        put16(b, t->id);
        put16(b + 2, ch);
        memcpy(b + 4, t->data + off, k);
        n = 4 + (int)k;
        x->st.bytes_tx += k;
        break;
    }
    case I_POLL:
    case I_PONG:
        type = x->s.plan[i] == I_POLL ? T_POLL : T_PONG;
        b[0] = (uint8_t)x->s.rssi;
        n = 1;
        break;
    }
    if (last && more_queued(x)) fl |= F_MORE;
    if (send_frame(x, type, fl, x->s.peer, b, n) != 0) return;   /* tried again next step */
    if (x->s.plan[i] == I_BYE) x->after = AFTER_BYE;
    x->s.plan_i++;
    if (x->s.plan_i >= x->s.plan_n) x->s.my_turn = false;
}

static void master_fall(ls_lsx_t *x)
{
    /* Three silent turns. Back from a mode being tried; otherwise to LoRa,
       where the slave's own silence timer also takes it. */
    if (x->s.probe_from >= 0) {
        x->s.blocked_until[cur_mode(x)] = x->now + 20000;
        x->s.li = x->s.probe_from;
    } else {
        x->s.li = 0;
    }
    x->s.probe_from = -1;
    x->s.miss_run = 0;
    x->s.switch_on_over = false;
    x->s.mode_pending = false;
    x->s.t_next_poll = x->now;
    session_tune(x);
}

static void master_adapt(ls_lsx_t *x)
{
    if (x->s.mode_pending || x->s.switch_on_over || x->s.probe_from >= 0 || x->s.bye_pending) return;
    if (x->now - x->s.t_mode < 1000 || (x->s.hist & 0x03) != 0x03) return;
    const int8_t worst = x->s.rssi < x->s.rssi_at_peer ? x->s.rssi : x->s.rssi_at_peer;
    if (x->s.li + 1 < x->s.nlad) {
        const int up = x->s.ladder[x->s.li + 1];
        if (worst >= ls_lsx_mode_info(up)->min_rssi + 3 && after(x->now, x->s.blocked_until[up])) {
            x->s.mode_pending = true;
            x->s.mode_probe = true;
            x->s.mode_target = x->s.li + 1;
            return;
        }
    }
    if (x->s.li > 0 && worst < ls_lsx_mode_info(cur_mode(x))->min_rssi - 3) {
        x->s.mode_pending = true;
        x->s.mode_probe = false;
        x->s.mode_target = x->s.li - 1;
    }
}

static void master_switch(ls_lsx_t *x)
{
    x->s.switch_on_over = false;
    const int from = x->s.li;
    x->s.li = x->s.mode_target;
    x->s.probe_from = x->s.mode_probe ? from : -1;
    x->s.t_probe = x->now;
    x->s.miss_run = 0;
    x->s.t_next_poll = x->now;     /* the slave is waiting to hear the new mode */
    session_tune(x);
}

static void session_rx(ls_lsx_t *x, uint8_t type, uint8_t flags, const uint8_t *b, int n, const ls_lsx_rxmeta_t *m)
{
    x->s.t_rx_last = x->now;
    x->s.got_frame = true;
    x->s.rssi = m->rssi;
    x->s.snr = m->snr;
    if (x->s.probe_from >= 0 && !x->s.master) x->s.probe_from = -1;   /* the new mode carries */
    switch (type) {
    case T_POLL:
    case T_PONG:
        if (n >= 1) x->s.rssi_at_peer = (int8_t)b[0];
        break;
    case T_BEGIN: on_begin(x, b, n); break;
    case T_DATA: on_data(x, b, n); break;
    case T_ACK: on_ack(x, b, n); break;
    case T_SDGRAM:
        if (n >= 1 && x->cfg.app.on_dgram) x->cfg.app.on_dgram(x->cfg.app.ctx, x->s.peer, b[0], b + 1, n - 1);
        break;
    case T_MODE:
        if (!x->s.master && n >= 2) {
            for (int i = 0; i < x->s.nlad; i++)
                if (x->s.ladder[i] == b[0]) {
                    x->s.slave_target = i + 1;    /* 0 means none */
                    x->s.slave_probe = b[1] != 0;
                }
        }
        break;
    case T_BYE:
        if (!x->s.master) { session_end(x, LSX_EV_CLOSED); return; }
        break;
    }
    x->s.peer_more = (flags & F_MORE) != 0;
    if (!x->s.master && (flags & F_POLL)) {
        x->s.my_turn = true;
        x->s.t_turn_ok = x->now + turnaround(cur_mode(x));
        plan_turn(x);
        if (x->s.slave_target) x->after = AFTER_SWITCH;
    }
    if (x->s.master && (flags & F_OVER) && !x->s.my_turn) {
        x->s.hist = (uint8_t)((x->s.hist << 1) | 1);
        x->s.miss_run = 0;
        x->st.turns++;
        if (x->s.probe_from >= 0) x->s.probe_from = -1;
        if (x->s.switch_on_over) master_switch(x);
        x->s.my_turn = true;
        x->s.plan_n = 0;
        x->s.t_turn_ok = x->now + turnaround(cur_mode(x));
    }
    if (x->s.master && !(flags & F_OVER)) x->s.deadline = x->now + reply_timeout(cur_mode(x)) / 2 + 30;
}

static void session_tick(ls_lsx_t *x)
{
    if (x->tuned_mode < 0 && tune(x, cur_mode(x), x->s.hz) != 0) return;
    if (!after(x->now, x->s.t_turn_ok)) return;      /* the far end turns its radio round */
    if (x->s.master) {
        if (x->closing_req) { x->closing_req = false; x->s.bye_pending = true; }
        if (!x->s.my_turn && after(x->now, x->s.deadline)) {
            x->s.hist = (uint8_t)(x->s.hist << 1);
            x->st.turns++;
            if (!x->s.got_frame) { x->s.miss_run++; x->st.misses++; }
            if (x->s.switch_on_over) x->s.mode_pending = true;   /* say it again */
            x->s.switch_on_over = false;
            x->s.my_turn = true;
            x->s.plan_n = 0;
            if (x->s.miss_run >= 3 && (x->s.li > 0 || x->s.probe_from >= 0)) master_fall(x);
        }
        if (x->now - x->s.t_rx_last > DEAD_MS) { session_end(x, LSX_EV_LOST); return; }
        if (x->s.my_turn && !x->txing) {
            if (!x->s.plan_n) {
                /* Idle: poll at the keepalive rate unless something moves. */
                const bool busy = more_queued(x) || x->s.peer_more || x->s.ack_due || x->rx.active ||
                                  x->s.mode_pending || x->s.bye_pending || x->s.miss_run;
                if (!busy && !after(x->now, x->s.t_next_poll)) return;
                x->s.got_frame = false;
                master_adapt(x);
                plan_turn(x);
            }
            send_plan_item(x);
            if (!x->s.my_turn) {
                x->s.deadline = x->now + reply_timeout(cur_mode(x));
                x->s.t_next_poll = x->now + keepalive(cur_mode(x));
            }
        }
    } else {
        if (x->closing_req) x->closing_req = false;   /* only the master says goodbye */
        if (x->s.probe_from >= 0 && x->now - x->s.t_probe > silence(cur_mode(x)) / 2) {
            x->s.li = x->s.probe_from;
            x->s.probe_from = -1;
            x->s.t_rx_last = x->now;
            session_tune(x);
        }
        const uint32_t quiet = x->now - x->s.t_rx_last;
        if (x->s.li > 0 && quiet > silence(cur_mode(x))) {
            x->s.li = 0;
            x->s.probe_from = -1;
            x->s.t_rx_last = x->now;
            session_tune(x);
        } else if (quiet > DEAD_MS) {
            session_end(x, LSX_EV_LOST);
            return;
        }
        if (x->s.my_turn && !x->txing && x->s.plan_i < x->s.plan_n) send_plan_item(x);
    }
}

/* ---- discovery ------------------------------------------------------------ */

static int probe_body(ls_lsx_t *x, uint8_t *b)
{
    /* Top bit: asked for by hand ('lsx sync'), answer even if up to date. */
    b[0] = (uint8_t)(x->cfg.role | (x->probe_now ? 0x80 : 0));
    b[1] = (uint8_t)x->cfg.phy->caps;
    memcpy(b + 2, x->probe_nonce, 8);
    put32(b + 10, x->cfg.app.hint ? x->cfg.app.hint(x->cfg.app.ctx) : 0);
    b[14] = (uint8_t)power(x);
    memcpy(b + 15, x->cfg.name, 12);
    return 27;
}

static void heard(ls_lsx_t *x, uint32_t id, uint8_t role, uint8_t caps, const char *name, uint32_t hint,
                  const ls_lsx_rxmeta_t *m)
{
    peer_t *p = peer_get(x, id, true);
    p->p.role = role;
    p->p.caps = caps;
    if (name) { memcpy(p->p.name, name, 12); p->p.name[12] = 0; }
    p->p.hint = hint;
    p->p.rssi = m->rssi;
    p->p.snr = m->snr;
    p->p.heard_ms = x->now;
    if (x->cfg.app.on_peer) x->cfg.app.on_peer(x->cfg.app.ctx, &p->p);
}

static void group_rx(ls_lsx_t *x, uint8_t type, uint32_t src, uint32_t dst, const uint8_t *b, int n,
                     const ls_lsx_rxmeta_t *m)
{
    switch (type) {
    case T_PROBE: {
        if (n < 27) return;
        char name[LSX_NAME] = { 0 };
        memcpy(name, b + 15, 12);
        heard(x, src, b[0], b[1], name, get32(b + 10), m);
        if ((x->cfg.role != LSX_ROLE_HOME && x->cfg.role != LSX_ROLE_PARTY) || x->s.on || x->state != LSX_ST_LISTEN)
            return;
        peer_t *p = peer_get(x, src, false);
        if (x->cfg.app.on_probe && p && !x->cfg.app.on_probe(x->cfg.app.ctx, &p->p)) return;
        x->wel.due = true;
        x->wel.at = x->now + 4 + rnd(x) % 20;
        x->wel.peer = src;
        memcpy(x->wel.nf, b + 2, 8);
        x->wel.caps = b[1];
        x->wel.role = b[0];
        x->wel.rssi = m->rssi;
        memcpy(x->wel.name, name, LSX_NAME);
        break;
    }
    case T_WELCOME: {
        if (n < 35 || dst != x->cfg.id || x->state != LSX_ST_PROBE || x->s.on) return;
        if (memcmp(b, x->probe_nonce, 8)) return;
        char name[LSX_NAME] = { 0 };
        memcpy(name, b + 23, 12);
        heard(x, src, b[22], b[21], name, 0, m);
        const uint16_t sid = get16(b + 16);
        const uint8_t mask = b[18], start = b[19], chan = b[20];
        const ls_lsx_region_t *rg = ls_lsx_region(x->cfg.region);
        session_keys(x, x->probe_nonce, b + 8, x->cfg.id, src, sid);
        session_open(x, false, src, name, b[21], (uint8_t)(mask | 1), start, rg->data_hz[chan % rg->n_data], sid);
        x->s.rssi = m->rssi;
        break;
    }
    case T_BEACON:
        if (n >= 18) {
            char name[LSX_NAME] = { 0 };
            memcpy(name, b + 2, 12);
            heard(x, src, b[0], b[1], name, get32(b + 14), m);
        }
        break;
    case T_DGRAM:
        if (n >= 1) {
            peer_t *p = peer_get(x, src, true);
            p->p.heard_ms = x->now;
            p->p.rssi = m->rssi;
            /* someone else just finished: do not all answer at once */
            const uint32_t t = x->now + 5 + rnd(x) % 40;
            if (!after(x->t_bcast, t)) x->t_bcast = t;
            if (x->cfg.app.on_dgram) x->cfg.app.on_dgram(x->cfg.app.ctx, src, b[0], b + 1, n - 1);
        }
        break;
    }
}

static void send_welcome(ls_lsx_t *x)
{
    uint8_t b[40], nh[8];
    for (int i = 0; i < 8; i += 4) put32(nh + i, rnd(x));
    const uint16_t sid = (uint16_t)rnd(x);
    const uint8_t mask = ladder_mask(x, x->wel.caps);
    int start = LSX_M_LORA;
    for (int m = 0; m < LSX_M_N; m++)
        if ((mask & (1u << m)) && x->wel.rssi - 6 >= ls_lsx_mode_info(m)->min_rssi) start = m;
    const ls_lsx_region_t *rg = ls_lsx_region(x->cfg.region);
    const uint8_t chan = (uint8_t)(sid % rg->n_data);
    memcpy(b, x->wel.nf, 8);
    memcpy(b + 8, nh, 8);
    put16(b + 16, sid);
    b[18] = mask;
    b[19] = (uint8_t)start;
    b[20] = chan;
    b[21] = (uint8_t)x->cfg.phy->caps;
    b[22] = x->cfg.role;
    memcpy(b + 23, x->cfg.name, 12);
    x->wel.due = false;
    if (send_frame(x, T_WELCOME, 0, x->wel.peer, b, 35) != 0) return;
    session_keys(x, x->wel.nf, nh, x->wel.peer, x->cfg.id, sid);
    /* Opened when the welcome has gone (AFTER_WELCOME). */
    x->after = AFTER_WELCOME;
    x->s.sid = sid;
    x->s.peer = x->wel.peer;
    x->s.ladder[0] = mask;          /* parked here until then */
    x->s.ladder[1] = (uint8_t)start;
    x->s.hz = rg->data_hz[chan];
}

/* Broadcasts have no turns: several PARTY boards on the discovery channel
   send whenever they have something, with no carrier sense on this phy. So
   each waits a random time after its own frame (up to twice that frame's
   air time) and a little after hearing someone else's: queued at the same
   instant, two boards no longer collide on every frame. */
static bool send_bcast_dgram(ls_lsx_t *x)
{
    if (!after(x->now, x->t_bcast)) return false;
    for (int i = 0; i < DGQ; i++)
        if (x->dq[i].used && x->dq[i].dst == LSX_BCAST) {
            uint8_t b[LSX_BODY_MAX];
            b[0] = x->dq[i].port;
            memcpy(b + 1, x->dq[i].d, x->dq[i].len);
            if (send_frame(x, T_DGRAM, 0, LSX_BCAST, b, 1 + x->dq[i].len) == 0) {
                x->dq[i].used = false;
                const uint32_t a = ls_lsx_airtime_us(LSX_M_LORA, LSX_HDR + 1 + x->dq[i].len + LSX_TAG) / 1000u;
                x->t_bcast = x->now + a + rnd(x) % (2 * a + 20);
            }
            return true;
        }
    return false;
}

static bool bcast_queued(const ls_lsx_t *x)
{
    for (int i = 0; i < DGQ; i++)
        if (x->dq[i].used && x->dq[i].dst == LSX_BCAST) return true;
    return false;
}

/* ---- receive ------------------------------------------------------------ */

static void receive(ls_lsx_t *x)
{
    for (int k = 0; k < 6; k++) {
        ls_lsx_rxmeta_t m = { -128, 127 };
        const int n = x->cfg.phy->recv(x->cfg.phy->ctx, x->rxbuf, sizeof(x->rxbuf), &m);
        if (n == 0) return;
        if (n < 0) { x->st.frames_bad++; continue; }
        if (x->rx_drop && (rnd(x) & 0xFF) < x->rx_drop) continue;
        uint8_t *f = x->rxbuf;
        const int blen = f[14];
        if (n < LSX_HDR + LSX_TAG || (f[0] & 0xC0) != 0x40 || LSX_HDR + blen + LSX_TAG > n) { x->st.frames_bad++; continue; }
        const uint8_t type = f[0] & 0x3F, flags = f[1];
        const uint32_t src = get32(f + 2), dst = get32(f + 6), ctr = get32(f + 10);
        if (src == x->cfg.id || (dst != x->cfg.id && dst != LSX_BCAST)) continue;
        const bool sess = (flags & F_SESS) != 0;
        if (sess && (!x->s.on || src != x->s.peer)) continue;
        lsx_aes_t *enc = sess ? &x->s_enc : &x->g_enc, *mac = sess ? &x->s_mac : &x->g_mac;
        uint8_t tag[16], diff = 0;
        lsx_cmac(mac, f, (size_t)(LSX_HDR + blen), tag);
        for (int i = 0; i < LSX_TAG; i++) diff |= (uint8_t)(tag[i] ^ f[LSX_HDR + blen + i]);
        if (diff) { x->st.frames_bad++; continue; }
        if (sess ? !session_fresh(x, ctr) : !group_fresh(peer_get(x, src, true), ctr)) continue;
        uint8_t iv[16];
        memcpy(iv, f + 2, 4);
        memcpy(iv + 4, f + 10, 4);
        memcpy(iv + 8, "LSX1", 4);
        memset(iv + 12, 0, 4);
        lsx_ctr(enc, iv, f + LSX_HDR, (size_t)blen);
        x->st.frames_rx++;
        x->last_heard = x->now;
        x->heard_any = true;
        x->st.rssi = m.rssi;
        x->st.snr = m.snr;
        memmove(x->st.rssi_hist, x->st.rssi_hist + 1, sizeof(x->st.rssi_hist) - 1);
        const int h = m.rssi + 160;
        x->st.rssi_hist[sizeof(x->st.rssi_hist) - 1] = (uint8_t)(h < 1 ? 1 : h > 255 ? 255 : h);
        if (sess) session_rx(x, type, flags, f + LSX_HDR, blen, &m);
        else group_rx(x, type, src, dst, f + LSX_HDR, blen, &m);
    }
}

/* ---- the step ------------------------------------------------------------ */

static void after_tx(ls_lsx_t *x)
{
    const uint8_t a = x->after;
    x->after = AFTER_NONE;
    if (a == AFTER_WELCOME) {
        const uint8_t mask = x->s.ladder[0], start = x->s.ladder[1];
        const uint32_t hz = x->s.hz, peer = x->s.peer;
        const uint16_t sid = x->s.sid;
        session_open(x, true, peer, x->wel.name, x->wel.caps, mask, start, hz, sid);
        x->s.rssi = x->wel.rssi;
    } else if (a == AFTER_BYE) {
        session_end(x, LSX_EV_CLOSED);
    } else if (a == AFTER_SWITCH && x->s.on && !x->s.master) {
        if (x->s.my_turn) { x->after = AFTER_SWITCH; return; }   /* after the last frame of the turn */
        const int from = x->s.li;
        x->s.li = x->s.slave_target - 1;
        x->s.probe_from = x->s.slave_probe ? from : -1;
        x->s.t_probe = x->now;
        x->s.slave_target = 0;
        x->s.t_rx_last = x->now;
        session_tune(x);
    }
}

static bool wants_radio(ls_lsx_t *x)
{
    if (x->cfg.role == LSX_ROLE_HOME || x->cfg.role == LSX_ROLE_PARTY) return true;
    if (x->cfg.role == LSX_ROLE_FIELD) return x->probe_now || bcast_queued(x) || after(x->now, x->t_next_probe);
    return false;
}

uint32_t ls_lsx_step(ls_lsx_t *x, uint32_t now)
{
    if (!x) return 100;
    x->now = now;
    if (!x->clock_set) {
        /* after() is a half-range compare: a deadline left at 0 reads as
           24.8 days ahead once uptime passes 2^31 ms. Start them at now. */
        x->clock_set = true;
        x->t_claim = x->t_next_probe = x->t_probe_end = x->t_beacon = x->t_bcast = now;
    }
    air_roll(x);
    x->st.airtime_ms_60s = air_used_ms(x);
    x->st.last_heard_ms = x->heard_any ? now - x->last_heard : UINT32_MAX;
    const ls_lsx_phy_t *phy = x->cfg.phy;

    if (x->cfg.role == LSX_ROLE_OFF) {
        if (x->s.on) session_end(x, LSX_EV_CLOSED);
        release(x);
        x->state = LSX_ST_OFF;
        return 200;
    }
    if (!x->held) {
        if (!wants_radio(x)) { x->state = LSX_ST_WAIT_RADIO; return 50; }
        if (!after(now, x->t_claim)) return x->t_claim - now;
        x->held = phy->claim(phy->ctx, true);
        if (!x->held) {
            x->state = LSX_ST_WAIT_RADIO;
            x->t_claim = now + 200;
            return 50;
        }
        x->tuned_mode = -1;
        x->probe_sent = false;
        x->state = x->cfg.role == LSX_ROLE_FIELD ? LSX_ST_PROBE : LSX_ST_LISTEN;
    }
    if (phy->yield(phy->ctx) && !x->txing) {
        /* Another radio app or a MeshCore send: give the chip back now. */
        if (x->s.on) session_end(x, LSX_EV_LOST);
        release(x);
        x->state = LSX_ST_WAIT_RADIO;
        x->t_claim = now + 500;
        if (x->cfg.role == LSX_ROLE_FIELD && after(now, x->t_next_probe)) x->t_next_probe = now + 3000;
        return 50;
    }
    if (x->txing) {
        const int r = phy->tx_done(phy->ctx);
        if (r == 0) return 1;
        x->txing = false;
        after_tx(x);
    }
    receive(x);
    if (!x->held) return 20;   /* a session ended in there */

    const ls_lsx_region_t *rg = ls_lsx_region(x->cfg.region);
    switch (x->state) {
    case LSX_ST_LISTEN:
        tune(x, LSX_M_LORA, rg->disc_hz);
        if (x->txing) break;
        if (x->wel.due && after(now, x->wel.at)) { send_welcome(x); break; }
        if (send_bcast_dgram(x)) break;
        if (x->cfg.role == LSX_ROLE_HOME && after(now, x->t_beacon)) {
            uint8_t b[18];
            b[0] = x->cfg.role;
            b[1] = (uint8_t)phy->caps;
            memcpy(b + 2, x->cfg.name, 12);
            put32(b + 14, x->cfg.app.hint ? x->cfg.app.hint(x->cfg.app.ctx) : 0);
            if (send_frame(x, T_BEACON, 0, LSX_BCAST, b, 18) == 0) x->t_beacon = now + BEACON_MS + rnd(x) % 5000;
        }
        return x->wel.due ? 1 : 10;
    case LSX_ST_PROBE:
        tune(x, LSX_M_LORA, rg->disc_hz);
        if (x->txing) break;
        if (!x->probe_sent) {
            if (send_bcast_dgram(x)) break;
            for (int i = 0; i < 8; i += 4) put32(x->probe_nonce + i, rnd(x));
            uint8_t b[32];
            const int n = probe_body(x, b);
            if (send_frame(x, T_PROBE, 0, LSX_BCAST, b, n) == 0) {
                x->probe_sent = true;
                x->probe_now = false;
                x->t_probe_end = now + (uint32_t)(ls_lsx_airtime_us(LSX_M_LORA, LSX_HDR + n + LSX_TAG) / 1000) +
                                 PROBE_LISTEN_MS;
            } else {
                release(x);    /* over the airtime cap: later */
                x->t_next_probe = now + x->cfg.probe_ms;
            }
            break;
        }
        if (after(now, x->t_probe_end)) {
            release(x);
            /* Closer probes while a home was heard lately. */
            const bool near = x->heard_any && now - x->last_heard < 60000;
            x->t_next_probe = now + (near ? x->cfg.probe_ms / 2 : x->cfg.probe_ms) + rnd(x) % 1000;
            x->state = LSX_ST_WAIT_RADIO;
            return 50;
        }
        return 5;
    case LSX_ST_SESSION:
        session_tick(x);
        break;
    default:
        break;
    }
    return 1;
}

void ls_lsx_status(const ls_lsx_t *x, ls_lsx_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!x) { out->last_heard_ms = UINT32_MAX; return; }
    *out = x->st;
    out->state = x->state;
    out->role = x->cfg.role;
    out->held = x->held;
    if (x->rx.active) {
        memcpy(out->obj_rx_meta, x->rx.meta, x->rx.mlen);
        out->obj_rx_mlen = x->rx.mlen;
    }
    if (x->s.on) {
        out->rssi_at_peer = x->s.rssi_at_peer;
        out->hz = x->s.hz;
        out->mode = (uint8_t)cur_mode(x);
        out->session_ms = x->now - x->s.t_open;
        out->rssi = x->s.rssi;
        out->snr = x->s.snr;
    } else {
        out->hz = x->tuned_hz;
        out->mode = LSX_M_LORA;
        out->peer = 0;
    }
}
