/* MESHTASTIC: be a Meshtastic node on the board's LoRa radio.
   - hears every packet on the chosen region / preset and reads up to four
     channels (the primary plus three more, each with its own key)
   - CHAT (channel and direct messages, delivery reports), NODES (names,
     signal, battery, position, distance and bearing, traceroute), AIR
   - sends text, direct messages with acks (public-key DMs when PRIVATE DMS
     is on and the other node has published a key), node info, position and
     device telemetry when TRANSMIT is on; relays others' packets (RELAY),
     adding itself to traceroutes it carries
   - nodes with a position go on the board's MAP
   - keeps running in the background when Meshtastic is the board's primary
     mesh (MESH > OPTIONS > AT BOOT > PRIMARY MESH) or KEEP RUNNING = ALWAYS */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "mt_pki.h"
#include "mt_proto.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MSGS   40
#define MAX_NODES  48
#define MAX_AIR    24
#define MAX_SEEN   96
#define HISTORY    40
#define TEXT_MAX   200
#define TXQ        6
#define NCH        4
#define NODEINFO_EVERY_MS (3u * 3600u * 1000u)
#define ACK_WAIT_MS 20000u
#define ACK_TRIES   3
#define TRACE_WAIT_MS 60000u

/* ---- saved settings ---------------------------------------------------- */
typedef struct {                    /* v1, the first release: read once, then cfg2 */
    uint8_t region, preset, psk, tx_on;
    int8_t power_dbm;
    uint8_t reserved[3];
    uint32_t node;
    char channel[12], long_name[32], short_name[6];
} cfg1_t;
typedef struct {
    uint8_t region, preset, psk, tx_on;     /* psk: v0.2's primary key, read once into chans */
    int8_t power_dbm;
    uint8_t pos_mode;       /* 0 off, 1 exact, 2 approximate */
    uint8_t pos_every;      /* index into POS_MIN */
    uint8_t relay;          /* repeat others' packets */
    uint8_t keep;           /* 0 in the background when primary, 1 always, 2 never */
    uint8_t fixed;          /* use fixed_lat/lon when there is no GPS fix */
    uint8_t tele;           /* index into TELE_MIN (0 = hourly; v0.2 left it 0) */
    uint8_t pki;            /* public-key direct messages */
    uint32_t node;
    char channel[12], long_name[32], short_name[6];
    int32_t fixed_lat, fixed_lon;   /* degrees * 1e7 */
} cfg_t;
static cfg_t cfg;
static const uint16_t POS_MIN[] = { 15, 30, 60, 120 };
static const uint16_t TELE_MIN[] = { 60, 30, 180, 0 };

/* Channel keys: [0] the primary (its name is cfg.channel), [1..3] more
   channels, each with its own name and key. */
typedef struct {
    char name[12];
    uint8_t psk_len, on;
    uint8_t psk[32];
    uint8_t pad[2];
} chan_cfg_t;
static chan_cfg_t chans[NCH];
static struct { uint8_t key[32]; int key_len; uint8_t hash; } crt[NCH];

typedef struct { uint8_t priv[32], pub[32]; uint32_t magic; } pki_t;
#define PKI_MAGIC 0x4B504D31u
static pki_t pki;
static bool pki_ready;

/* Saves go one at a time (the board refuses a second while one is pending),
   each at most 4 KB: what changed is marked and written in turn. */
enum { SV_CFG = 1, SV_CHANS = 2, SV_PKI = 4, SV_NODES = 8 };
static unsigned dirty;

static void cfg_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.psk = 1;             /* the default key, "AQ==" */
    cfg.power_dbm = 17;
    cfg.pos_every = 2;       /* hourly */
    cfg.pki = 1;             /* Meshtastic 2.7 sends no DM to a node without a key */
    do cfg.node = lap_random(); while (cfg.node < 16 || cfg.node == MT_BROADCAST);
    snprintf(cfg.long_name, sizeof(cfg.long_name), "LakeShark %04X", (unsigned)(cfg.node & 0xFFFF));
    snprintf(cfg.short_name, sizeof(cfg.short_name), "L%03X", (unsigned)(cfg.node & 0xFFF));
}
static void cfg_load(void)
{
    if (lap_load("cfg2", &cfg, sizeof(cfg)) == (int)sizeof(cfg) && cfg.node >= 16 &&
        cfg.region < MT_REGION_COUNT && cfg.preset < MT_PRESET_COUNT) return;
    cfg1_t old;
    cfg_defaults();
    if (lap_load("cfg", &old, sizeof(old)) == (int)sizeof(old) && old.node >= 16 &&
        old.region < MT_REGION_COUNT && old.preset < MT_PRESET_COUNT) {
        cfg.region = old.region; cfg.preset = old.preset; cfg.psk = old.psk; cfg.tx_on = old.tx_on;
        cfg.power_dbm = old.power_dbm; cfg.node = old.node;
        memcpy(cfg.channel, old.channel, sizeof(cfg.channel));
        memcpy(cfg.long_name, old.long_name, sizeof(cfg.long_name));
        memcpy(cfg.short_name, old.short_name, sizeof(cfg.short_name));
    }
    dirty |= SV_CFG;
}
static void chans_load(void)
{
    if (lap_load("chans", chans, sizeof(chans)) == (int)sizeof(chans) && chans[0].psk_len <= 32) return;
    memset(chans, 0, sizeof(chans));
    chans[0].on = 1;
    chans[0].psk_len = 1;
    chans[0].psk[0] = cfg.psk;      /* 1 = AQ==, 0 = none */
    /* First run after v0.2: private DMs on, as for a new node (a 2.7 node
       refuses to send a DM to one that has published no key). */
    cfg.pki = 1;
    dirty |= SV_CHANS | SV_CFG;
}
static void pki_load(void)
{
    pki_ready = lap_load("pki", &pki, sizeof(pki)) == (int)sizeof(pki) && pki.magic == PKI_MAGIC;
}
static void pki_make(void)
{
    if (pki_ready) return;
    for (int i = 0; i < 8; i++) {
        const uint32_t r = lap_random();
        memcpy(pki.priv + 4 * i, &r, 4);
    }
    mt_x25519(pki.pub, pki.priv, NULL);
    pki.magic = PKI_MAGIC;
    pki_ready = true;
    dirty |= SV_PKI;
}

/* ---- what was heard ---------------------------------------------------- */
enum { ST_NONE, ST_QUEUED, ST_SENT, ST_RELAYED, ST_DELIVERED, ST_FAILED };
typedef struct {
    uint32_t from, to, id, at_ms;
    int16_t rssi, snr;      /* tenths */
    uint8_t hops, mine, state, tries, ch, pki;
    char text[TEXT_MAX + 1];
} msg_t;
typedef struct {
    uint32_t num, last_ms, packets, asked_ms;
    int16_t rssi, snr;
    uint8_t hops, battery, ch;
    bool has_pos, named, has_dev, has_key;
    float voltage, ch_util, air_util;
    double lat, lon;
    char short_name[6], long_name[32];
    char last[10];          /* what it sent last */
    uint8_t pubkey[32];
} node_t;
typedef struct {
    uint32_t from, to, at_ms;
    int16_t rssi, snr;
    uint16_t len;
    uint8_t channel, hops;
    char what[12];
} air_t;
typedef struct {
    uint8_t frame[MT_HEADER_LEN + MT_MAX_PAYLOAD];
    uint16_t len;
    uint32_t due_ms, from, id;
    bool used, relay;
} txq_t;

static msg_t msgs[MAX_MSGS];
static int msg_n, msg_head;
static node_t nodes[MAX_NODES];
static int node_n;
static air_t air[MAX_AIR];
static int air_n, air_head;
static struct { uint32_t from, id; } seen[MAX_SEEN];
static int seen_head;
static int rssi_hist[HISTORY];
static int hist_n;
static uint32_t heard, decoded, foreign, sent, gaps, relayed, unread, pki_in, pki_bad;
static txq_t txq[TXQ];
/* The shared key with each node, worked out once (an X25519 takes a while). */
static struct { uint32_t num; uint8_t key[32]; } shared[MAX_NODES];

/* ---- radio ------------------------------------------------------------- */
static int radio = -1, rxh, txh, tx_error, rx_error, tx_slot = -1;
static uint64_t freq_hz;
static int slot, slots, send_ch;
static uint32_t last_nodeinfo_ms, last_pos_ms, last_tele_ms, last_rx_ms, nodes_saved_ms;
static bool nodeinfo_due, loaded, primary, svc_on;
static int gps_lease;
/* Channel time: 10-second slices for the last minute (everything heard or
   sent) and 10-minute slices for the last hour (what this node sent). */
static uint32_t ch_busy[6], tx_busy[6], ch_epoch, tx_epoch;
static uint32_t trace_id, trace_to, trace_ms;
static char trace_fwd[72], trace_back[72];

/* ---- screen ------------------------------------------------------------ */
enum { TAB_CHAT, TAB_NODES, TAB_AIR, TABS };
static int tab, scroll, sel;
static int dialog = -1, dialog_kind;    /* 1 text, 2 name, 3 channel, 4 DM, 5 fixed, 6 key, 7..9 channel 2..4 */
static uint32_t dm_to;
static char status_line[64];
static uint32_t status_ms;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_line, sizeof(status_line), fmt, ap);
    va_end(ap);
    status_ms = lap_millis();
}

static void print(int x, int y, uint8_t attr, const char *fmt, ...)
{
    char t[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    lap_put_str(x, y, t, attr);
}

static const char *preset_name(void) { return MT_PRESETS[cfg.preset].name; }
static const char *channel_name(void) { return cfg.channel[0] ? cfg.channel : preset_name(); }
static const char *ch_name(int c) { return c == 0 ? channel_name() : chans[c].name; }
static bool ch_on(int c) { return c == 0 || (chans[c].on && chans[c].name[0]); }
static int ch_count(void) { int n = 0; for (int c = 0; c < NCH; c++) n += ch_on(c); return n; }

static void apply_channel(void)
{
    for (int c = 0; c < NCH; c++) {
        crt[c].key_len = mt_channel_key(chans[c].psk, chans[c].psk_len, crt[c].key);
        crt[c].hash = mt_channel_hash(ch_name(c), crt[c].key, crt[c].key_len);
    }
    if (!ch_on(send_ch)) send_ch = 0;
    freq_hz = mt_frequency_hz(cfg.region, cfg.preset, cfg.channel, &slot, &slots);
}

static void key_text(int c, char *out, int cap)
{
    if (chans[c].psk_len == 0 || (chans[c].psk_len == 1 && chans[c].psk[0] == 0)) snprintf(out, (size_t)cap, "none");
    else mt_b64_encode(chans[c].psk, chans[c].psk_len, out, cap);
}

static lap_packet_config_t lora_cfg(void)
{
    const mt_preset_t *p = &MT_PRESETS[cfg.preset];
    return (lap_packet_config_t){ .size = sizeof(lap_packet_config_t), .version = 1,
        .frequency_hz = freq_hz, .bandwidth_hz = p->bw_hz, .sync_word = MT_SYNC_WORD,
        .preamble = MT_PREAMBLE, .modulation = LAP_MOD_LORA, .sf = p->sf, .coding_rate = p->cr,
        .crc = 1, .payload_bytes = 0, .power_dbm = cfg.power_dbm };
}

/* Time on air of one packet (explicit header, CRC on). */
static uint32_t airtime_ms(int len)
{
    const mt_preset_t *p = &MT_PRESETS[cfg.preset];
    const float tsym = (float)(1u << p->sf) * 1000.0f / (float)p->bw_hz;
    const int de = tsym > 16.0f, cr = p->cr > 4 ? p->cr - 4 : 1;
    int n = (int)ceilf((8.0f * len - 4.0f * p->sf + 28 + 16) / (4.0f * (p->sf - 2 * de)));
    if (n < 0) n = 0;
    return (uint32_t)((MT_PREAMBLE + 4.25f + 8 + n * (cr + 4)) * tsym);
}

static void util_roll(void)
{
    const uint32_t now = lap_millis(), ce = now / 10000u, te = now / 600000u;
    if (ce - ch_epoch > 6) { memset(ch_busy, 0, sizeof(ch_busy)); ch_epoch = ce; }
    while (ch_epoch < ce) ch_busy[++ch_epoch % 6] = 0;
    if (te - tx_epoch > 6) { memset(tx_busy, 0, sizeof(tx_busy)); tx_epoch = te; }
    while (tx_epoch < te) tx_busy[++tx_epoch % 6] = 0;
}
static void util_add(int len, bool mine)
{
    util_roll();
    const uint32_t t = airtime_ms(len);
    ch_busy[ch_epoch % 6] += t;
    if (mine) tx_busy[tx_epoch % 6] += t;
}
static float ch_util(void)
{
    util_roll();
    uint32_t s = 0;
    for (int i = 0; i < 6; i++) s += ch_busy[i];
    return (float)s / 600.0f;           /* of 60 s, in percent */
}
static float air_util(void)
{
    util_roll();
    uint32_t s = 0;
    for (int i = 0; i < 6; i++) s += tx_busy[i];
    return (float)s / 36000.0f;         /* of an hour, in percent */
}

static void rx_close(void) { if (rxh > 0) lap_packet_close(rxh); rxh = 0; }
static void rx_open(void)
{
    if (radio < 0 || rxh > 0 || txh > 0) return;
    lap_packet_config_t c = lora_cfg();
    rxh = lap_packet_open(radio, &c);
    static int last_err;
    if (rxh < 0) {
        if (rxh != last_err) lap_log("receive refused (%d) on radio %d", rxh, radio);
        last_err = rx_error = rxh; rxh = 0;
    } else { rx_error = last_err = 0; }
}
static void retune(void) { apply_channel(); rx_close(); rx_open(); }

static void find_radio(void)
{
    lap_caps_t caps;
    if (lap_caps(&caps, sizeof(caps)) < 0 || !(caps.hardware & LAP_HW_PACKET)) return;
    for (int i = 0; i < caps.radio_count; i++) {
        lap_radio_modes_t m;
        if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA)) { radio = i; return; }
    }
}

static bool read_primary(void)
{
    char v[16];
    return lap_value_text("mesh.primary", v, sizeof(v)) && !strcmp(v, "meshtastic");
}

/* ---- nodes ------------------------------------------------------------- */
static node_t *node_get(uint32_t num, bool make)
{
    for (int i = 0; i < node_n; i++) if (nodes[i].num == num) return &nodes[i];
    if (!make) return NULL;
    int at = node_n < MAX_NODES ? node_n++ : -1;
    if (at < 0) {                                  /* full: reuse the stalest */
        at = 0;
        for (int i = 1; i < MAX_NODES; i++) if (nodes[i].last_ms < nodes[at].last_ms) at = i;
    }
    memset(&nodes[at], 0, sizeof(node_t));
    nodes[at].num = num;
    snprintf(nodes[at].short_name, sizeof(nodes[at].short_name), "%04x", (unsigned)(num & 0xFFFF));
    return &nodes[at];
}

static const char *who(uint32_t num)
{
    if (num == cfg.node) return cfg.short_name;
    if (num == MT_BROADCAST) return "?";
    node_t *n = node_get(num, false);
    static char hex[4][6];
    static int flip;
    if (n) return n->short_name;
    flip = (flip + 1) & 3;
    snprintf(hex[flip], sizeof(hex[flip]), "%04x", (unsigned)(num & 0xFFFF));
    return hex[flip];
}

static uint8_t hue_of(uint32_t num)
{
    static const uint8_t H[] = { LAP_GREEN | LAP_BRIGHT, LAP_YELLOW | LAP_BRIGHT, LAP_MAGENTA | LAP_BRIGHT,
                                 LAP_BLUE | LAP_BRIGHT, LAP_RED | LAP_BRIGHT, LAP_WHITE | LAP_BRIGHT };
    if (num == cfg.node) return LAP_CYAN | LAP_BRIGHT;
    return H[(num ^ (num >> 11) ^ (num >> 23)) % sizeof(H)];
}

/* Every node on the board's MAP (drawn when it has a position). */
static void publish(const node_t *n)
{
    lap_map_node_t m;
    memset(&m, 0, sizeof(m));
    m.size = sizeof(m); m.version = 1; m.id = n->num;
    snprintf(m.name, sizeof(m.name), "%s", n->long_name[0] ? n->long_name : n->short_name);
    snprintf(m.short_name, sizeof(m.short_name), "%s", n->short_name);
    m.has_pos = n->has_pos; m.lat = n->lat; m.lon = n->lon;
    m.rssi_tenths = n->rssi; m.snr_tenths = n->snr; m.hops = n->hops;
    lap_map_node(&m);
}

/* The node list as saved: 76 bytes a node, 48 nodes inside one 4 KB save. */
typedef struct {
    uint32_t num;
    int32_t lat, lon;
    uint8_t flags, battery, hops, ch;
    char short_name[5], long_name[23];
    uint8_t pubkey[32];
} rec_t;
enum { RF_POS = 1, RF_NAMED = 2, RF_KEY = 4, RF_DEV = 8 };
static rec_t recs[MAX_NODES];

static int nodes_write(void)
{
    for (int i = 0; i < node_n; i++) {
        const node_t *n = &nodes[i];
        rec_t *r = &recs[i];
        memset(r, 0, sizeof(*r));
        r->num = n->num; r->lat = (int32_t)(n->lat * 1e7); r->lon = (int32_t)(n->lon * 1e7);
        r->flags = (uint8_t)((n->has_pos ? RF_POS : 0) | (n->named ? RF_NAMED : 0) | (n->has_key ? RF_KEY : 0) |
                             (n->has_dev ? RF_DEV : 0));
        r->battery = n->battery; r->hops = n->hops; r->ch = n->ch;
        snprintf(r->short_name, sizeof(r->short_name), "%s", n->short_name);
        snprintf(r->long_name, sizeof(r->long_name), "%s", n->long_name);
        if (n->has_key) memcpy(r->pubkey, n->pubkey, 32);
    }
    return lap_save("nodes2", recs, sizeof(rec_t) * (size_t)node_n);
}
static void nodes_load(void)
{
    const int got = lap_load("nodes2", recs, sizeof(recs));
    node_n = 0;
    for (int i = 0; got > 0 && i < got / (int)sizeof(rec_t) && i < MAX_NODES; i++) {
        const rec_t *r = &recs[i];
        node_t *n = &nodes[node_n++];
        memset(n, 0, sizeof(*n));
        n->num = r->num; n->lat = r->lat * 1e-7; n->lon = r->lon * 1e-7;
        n->has_pos = r->flags & RF_POS; n->named = r->flags & RF_NAMED; n->has_key = r->flags & RF_KEY;
        n->has_dev = r->flags & RF_DEV;
        n->battery = r->battery; n->hops = r->hops; n->ch = r->ch < NCH ? r->ch : 0;
        memcpy(n->short_name, r->short_name, sizeof(r->short_name)); n->short_name[5] = 0;
        memcpy(n->long_name, r->long_name, sizeof(r->long_name)); n->long_name[23] = 0;
        if (n->has_key) memcpy(n->pubkey, r->pubkey, 32);
        publish(n);
    }
}
static void nodes_dirty(void) { dirty |= SV_NODES; nodes_saved_ms = lap_millis(); }

/* lap_save hands back a card job: it has to be polled to finish, and the
   board has four. Unpolled, they ran out after a few saves and nothing more
   reached the card (v0.2 lost settings that way). One job at a time here;
   a failed one marks its slot again. */
static int save_job = -1;
static unsigned save_what;
static void save_pump(void)
{
    static uint32_t last;
    if (save_job >= 0) {
        const int r = lap_sd_poll(save_job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0) dirty |= save_what;
        save_job = -1;
    }
    if (!dirty || lap_millis() - last < 300) return;
    last = lap_millis();
    unsigned what = 0;
    int id = LAP_SD_EBUSY;
    if (dirty & SV_CFG) { what = SV_CFG; id = lap_save("cfg2", &cfg, sizeof(cfg)); }
    else if (dirty & SV_CHANS) { what = SV_CHANS; id = lap_save("chans", chans, sizeof(chans)); }
    else if (dirty & SV_PKI) { what = SV_PKI; id = lap_save("pki", &pki, sizeof(pki)); }
    else if (dirty & SV_NODES) { what = SV_NODES; id = nodes_write(); }
    if (id >= 0) { dirty &= ~what; save_job = id; save_what = what; }
}

static const uint8_t *shared_key(const node_t *n)
{
    int at = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (shared[i].num == n->num) return shared[i].key;
        if (!shared[i].num && at < 0) at = i;
    }
    if (at < 0) at = (int)(lap_random() % MAX_NODES);
    mt_pki_shared(pki.priv, n->pubkey, shared[at].key);
    shared[at].num = n->num;
    return shared[at].key;
}
static void shared_forget(uint32_t num)
{
    for (int i = 0; i < MAX_NODES; i++) if (shared[i].num == num) shared[i].num = 0;
}

/* ---- messages and air log ---------------------------------------------- */
static msg_t *msg_add(uint32_t from, uint32_t to, uint32_t id, const char *text, size_t len,
                      int16_t rssi, int16_t snr, uint8_t hops, bool mine)
{
    msg_t *m = &msgs[msg_head];
    msg_head = (msg_head + 1) % MAX_MSGS;
    if (msg_n < MAX_MSGS) msg_n++;
    memset(m, 0, sizeof(*m));
    m->from = from; m->to = to; m->id = id; m->at_ms = lap_millis();
    m->rssi = rssi; m->snr = snr; m->hops = hops; m->mine = mine;
    size_t j = 0;
    for (size_t i = 0; i < len && j < TEXT_MAX; i++) {
        const uint8_t c = (uint8_t)text[i];
        if (c >= 32 && c < 127) m->text[j++] = (char)c;
        else if (c >= 0xC0) m->text[j++] = '?';  /* first byte of a UTF-8 letter or emoji */
    }
    m->text[j] = 0;
    if (tab == TAB_CHAT) scroll = 0;
    return m;
}

static msg_t *msg_by_id(uint32_t id)
{
    for (int k = 0; k < msg_n; k++) {
        msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (m->mine && m->id == id) return m;
    }
    return NULL;
}

static void air_add(const mt_header_t *h, const lap_packet_meta_t *meta, const char *what)
{
    air_t *a = &air[air_head];
    air_head = (air_head + 1) % MAX_AIR;
    if (air_n < MAX_AIR) air_n++;
    a->from = h->from; a->to = h->dest; a->at_ms = lap_millis();
    a->rssi = meta->rssi_tenths_dbm; a->snr = meta->snr_tenths_db; a->len = meta->length;
    a->channel = h->channel; a->hops = (uint8_t)(h->hop_start >= h->hop_limit ? h->hop_start - h->hop_limit : 0);
    snprintf(a->what, sizeof(a->what), "%s", what);
}

static bool seen_before(uint32_t from, uint32_t id)
{
    for (int i = 0; i < MAX_SEEN; i++) if (seen[i].from == from && seen[i].id == id) return true;
    seen[seen_head].from = from; seen[seen_head].id = id;
    seen_head = (seen_head + 1) % MAX_SEEN;
    return false;
}

/* ---- transmit queue ---------------------------------------------------- */
static txq_t *txq_free(void)
{
    for (int i = 0; i < TXQ; i++) if (!txq[i].used) return &txq[i];
    return NULL;
}

/* Set just before a send: the reply delay, the channel, and "channel key
   even if a public key is known". send_packet puts them back. */
static uint32_t reply_delay;
static int send_on;
static bool send_plain, last_pki;

/* Public-key DMs carry text and the like; node info, position, acks and
   traceroutes always go on the channel, as the protocol has them. */
static bool pki_for(uint32_t dest, uint32_t port)
{
    if (!cfg.pki || !pki_ready || send_plain || dest == MT_BROADCAST) return false;
    if (port == MT_PORT_NODEINFO || port == MT_PORT_POSITION || port == MT_PORT_ROUTING || port == MT_PORT_TRACEROUTE)
        return false;
    const node_t *n = node_get(dest, false);
    return n && n->has_key;
}

/* Builds, encrypts and queues one packet. Returns its id, 0 when refused. */
static uint32_t send_packet(uint32_t dest, uint32_t port, const uint8_t *payload, size_t len,
                            bool want_ack, bool want_response, uint32_t request_id, uint32_t reuse_id)
{
    const int c = ch_on(send_on) ? send_on : 0;
    const bool pk = pki_for(dest, port);
    const uint32_t delay = reply_delay;
    reply_delay = 0; send_on = 0; send_plain = false; last_pki = false;
    if (!cfg.tx_on) { say("TRANSMIT is off: OPTIONS > TRANSMIT"); return 0; }
    if (radio < 0) { say("no LoRa radio"); return 0; }
    txq_t *q = txq_free();
    if (!q) { say("send queue full"); return 0; }
    mt_header_t h = { dest, cfg.node, reuse_id ? reuse_id : lap_random(), 3, 3, want_ack, 0,
                      pk ? 0 : crt[c].hash, 0, (uint8_t)(cfg.node & 0xFF) };
    if (!h.id) h.id = 1;
    mt_header_pack(&h, q->frame);
    mt_data_t d = { port, payload, len, want_response, request_id };
    static uint8_t plain[MT_MAX_PAYLOAD];
    const int n = mt_data_encode(&d, pk ? plain : q->frame + MT_HEADER_LEN, pk ? MT_MAX_PAYLOAD - MT_PKI_OVERHEAD : MT_MAX_PAYLOAD);
    if (n < 0) { say("too long to send"); return 0; }
    int total = n;
    if (pk) {
        total = mt_pki_seal(shared_key(node_get(dest, false)), h.from, h.id, lap_random(), plain, (size_t)n,
                            q->frame + MT_HEADER_LEN);
        if (total < 0) { say("could not seal"); return 0; }
        last_pki = true;
    } else {
        mt_crypt(crt[c].key, crt[c].key_len, h.from, h.id, q->frame + MT_HEADER_LEN, (size_t)n);
    }
    q->len = (uint16_t)(MT_HEADER_LEN + total);
    /* Replies wait a moment, as a node's channel access does: the sender's
       radio is still turning round from transmit to receive. */
    q->due_ms = lap_millis() + delay;
    q->from = h.from; q->id = h.id; q->relay = false; q->used = true;
    if (!reuse_id) seen_before(h.from, h.id);
    return h.id;
}

static void queue_relay(const uint8_t *buf, int n, const mt_header_t *h, int16_t snr)
{
    txq_t *q = txq_free();
    if (!q || n > (int)sizeof(q->frame)) return;
    memcpy(q->frame, buf, (size_t)n);
    q->frame[12] = (uint8_t)((q->frame[12] & ~7) | ((h->hop_limit - 1) & 7));
    q->frame[15] = (uint8_t)(cfg.node & 0xFF);
    q->len = (uint16_t)n;
    /* Far nodes (low SNR) go first, near ones wait and usually stand down. */
    int s = snr == INT16_MIN ? 0 : snr / 10;
    if (s < -20) s = -20;
    if (s > 15) s = 15;
    q->due_ms = lap_millis() + 150u + (uint32_t)(s + 20) * 45u + (lap_random() % 300u);
    q->from = h->from; q->id = h->id; q->relay = true; q->used = true;
}

static void cancel_relay(uint32_t from, uint32_t id)
{
    for (int i = 0; i < TXQ; i++)
        if (txq[i].used && txq[i].relay && txq[i].from == from && txq[i].id == id && i != tx_slot) txq[i].used = false;
}

static void ack_to(uint32_t dest, uint32_t id, int ch)
{
    uint8_t ack[4];
    const int an = mt_routing_ack(ack, sizeof(ack));
    reply_delay = 700u + lap_random() % 500u;
    send_on = ch;
    send_packet(dest, MT_PORT_ROUTING, ack, (size_t)an, false, false, id, 0);
}

static void send_nodeinfo_to(uint32_t dest, bool ask)
{
    mt_user_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.id, sizeof(u.id), "!%08x", (unsigned)cfg.node);
    snprintf(u.long_name, sizeof(u.long_name), "%s", cfg.long_name);
    snprintf(u.short_name, sizeof(u.short_name), "%s", cfg.short_name);
    u.hw_model = 255;                    /* PRIVATE_HW */
    u.role = cfg.relay ? 0 : 1;          /* CLIENT, or CLIENT_MUTE when it does not relay */
    if (cfg.pki && pki_ready) { memcpy(u.public_key, pki.pub, 32); u.has_key = true; }
    uint8_t mac[6] = { 0x02, 0x4C, (uint8_t)(cfg.node >> 24), (uint8_t)(cfg.node >> 16),
                       (uint8_t)(cfg.node >> 8), (uint8_t)cfg.node };
    uint8_t buf[140];
    const int n = mt_user_encode(&u, mac, buf, sizeof(buf));
    if (n > 0 && send_packet(dest, MT_PORT_NODEINFO, buf, (size_t)n, false, ask, 0, 0) && dest == MT_BROADCAST) {
        last_nodeinfo_ms = lap_millis();
        say("node info queued");
    }
}
static void send_nodeinfo(bool ask) { send_nodeinfo_to(MT_BROADCAST, ask); }

/* Where this node is: the GPS fix, else the FIXED POSITION when one is set. */
static bool my_position(double *lat, double *lon, int32_t *alt, uint32_t *t)
{
    lap_gps_t g;
    if (lap_gps(&g) && g.fix) { *lat = g.lat; *lon = g.lon; *alt = (int32_t)g.alt_m; *t = g.utc > 0 ? (uint32_t)g.utc : 0; return true; }
    if (!cfg.fixed) return false;
    lap_time_t now; lap_time(&now);
    *lat = cfg.fixed_lat * 1e-7; *lon = cfg.fixed_lon * 1e-7; *alt = 0; *t = now.synced ? (uint32_t)now.unix : 0;
    return true;
}

static bool send_position(uint32_t dest, uint32_t request_id)
{
    double lat, lon; int32_t alt; uint32_t t;
    if (!cfg.pos_mode || !my_position(&lat, &lon, &alt, &t)) return false;
    uint8_t buf[48];
    const int n = mt_position_encode(lat, lon, alt, t, cfg.pos_mode == 2 ? 15 : 32, buf, sizeof(buf));
    if (n <= 0) return false;
    if (!send_packet(dest, MT_PORT_POSITION, buf, (size_t)n, false, false, request_id, 0)) return false;
    last_pos_ms = lap_millis();
    return true;
}

/* Device metrics: battery, channel use, airtime, uptime. */
static bool send_telemetry(uint32_t dest, uint32_t request_id)
{
    mt_device_t d;
    memset(&d, 0, sizeof(d));
    lap_battery_t b;
    if (lap_battery(&b)) {
        d.battery = b.charging && b.percent >= 100 ? 101 : (uint32_t)(b.percent > 0 ? b.percent : 0);
        d.voltage = b.millivolts / 1000.0f;
    }
    d.ch_util = ch_util();
    d.air_util = air_util();
    float up = 0;
    d.uptime = lap_value_float("sys.uptime", &up) && up > 0 ? (uint32_t)up : lap_millis() / 1000u;
    lap_time_t now;
    lap_time(&now);
    uint8_t buf[64];
    const int n = mt_telemetry_encode(now.synced ? (uint32_t)now.unix : 0, &d, buf, sizeof(buf));
    if (n <= 0 || !send_packet(dest, MT_PORT_TELEMETRY, buf, (size_t)n, false, false, request_id, 0)) return false;
    last_tele_ms = lap_millis();
    return true;
}

static void send_text(uint32_t dest, const char *text)
{
    const size_t len = strlen(text);
    if (!len) return;
    if (!last_nodeinfo_ms) send_nodeinfo(true);   /* introduce ourselves first */
    const bool dm = dest != MT_BROADCAST;
    const int c = dm ? 0 : send_ch;
    send_on = c;
    const uint32_t id = send_packet(dest, MT_PORT_TEXT, (const uint8_t *)text, len, dm, false, 0, 0);
    if (!id) return;
    msg_t *m = msg_add(cfg.node, dest, id, text, len, INT16_MIN, INT16_MIN, 0, true);
    m->state = ST_QUEUED;
    m->tries = 1;
    m->ch = (uint8_t)c;
    m->pki = last_pki;
    say(dm ? (last_pki ? "sending to %s (private)" : "sending to %s") : "sending", who(dest));
}

static void resend(msg_t *m, bool plain)
{
    /* A public-key DM that got no ack, or a refusal, goes once more on the
       channel key: under a new id, since the other side may have marked the
       old one as seen. */
    if (plain) send_plain = true;
    send_on = m->ch;
    const uint32_t id = send_packet(m->to, MT_PORT_TEXT, (const uint8_t *)m->text, strlen(m->text), true, false, 0,
                                    plain ? 0 : m->id);
    if (!id) return;
    m->id = id;
    m->pki = last_pki;
    m->tries++;
    m->state = ST_QUEUED;
}

/* Direct messages wait for their ack and go again, up to ACK_TRIES. */
static void ack_service(void)
{
    const uint32_t now = lap_millis();
    for (int k = 0; k < msg_n; k++) {
        msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (!m->mine || m->to == MT_BROADCAST || m->state != ST_SENT || now - m->at_ms < ACK_WAIT_MS) continue;
        if (m->tries >= ACK_TRIES) { m->state = ST_FAILED; continue; }
        resend(m, m->pki && m->tries == ACK_TRIES - 1);
    }
}

/* ---- traceroute -------------------------------------------------------- */
static void send_trace(uint32_t to)
{
    uint8_t buf[8];
    mt_route_t r;
    memset(&r, 0, sizeof(r));
    const int n = mt_route_encode(&r, buf, sizeof(buf));
    const uint32_t id = send_packet(to, MT_PORT_TRACEROUTE, buf, (size_t)(n < 0 ? 0 : n), false, true, 0, 0);
    if (!id) return;
    trace_id = id; trace_to = to; trace_ms = lap_millis();
    snprintf(trace_fwd, sizeof(trace_fwd), "tracing %s ...", who(to));
    trace_back[0] = 0;
    say("traceroute to %s", who(to));
}

static int8_t q_snr(int16_t tenths)
{
    if (tenths == INT16_MIN) return MT_SNR_UNKNOWN;
    const int q = tenths * 4 / 10;
    return (int8_t)(q < -127 ? -127 : q > 127 ? 127 : q);
}

/* A hop adds itself (or only its SNR, at either end) to the list for the
   way the packet is going, after a gap for hops that left no mark. */
static void route_update(mt_route_t *r, const mt_header_t *h, int16_t snr, bool towards, bool snr_only)
{
    uint32_t *ids = towards ? r->route : r->back;
    uint8_t *nid = towards ? &r->n_route : &r->n_back;
    int8_t *snrs = towards ? r->snr_to : r->snr_back;
    uint8_t *nsnr = towards ? &r->n_snr_to : &r->n_snr_back;
    const int taken = h->hop_start > h->hop_limit ? h->hop_start - h->hop_limit : 0;
    while (h->hop_start && *nid < taken && *nid < MT_ROUTE_MAX) {
        ids[(*nid)++] = MT_BROADCAST;
        if (*nsnr < MT_ROUTE_MAX + 1) snrs[(*nsnr)++] = MT_SNR_UNKNOWN;
    }
    while (*nsnr < *nid) snrs[(*nsnr)++] = MT_SNR_UNKNOWN;
    if (!snr_only && *nid < MT_ROUTE_MAX) ids[(*nid)++] = cfg.node;
    if (*nsnr < MT_ROUTE_MAX + 1) snrs[(*nsnr)++] = q_snr(snr);
}

static void hop_text(char *out, size_t cap, size_t *at, uint32_t num, int8_t snr)
{
    if (*at >= cap) return;
    char s[8];
    if (snr == MT_SNR_UNKNOWN) snprintf(s, sizeof(s), "?");
    else snprintf(s, sizeof(s), "%d", snr / 4);
    const int w = snprintf(out + *at, cap - *at, ">%s(%s)", num == MT_BROADCAST ? "?" : who(num), s);
    if (w > 0) *at += (size_t)w;
}

static void trace_done(const mt_route_t *r)
{
    size_t a = 0;
    a += (size_t)snprintf(trace_fwd, sizeof(trace_fwd), "%s", cfg.short_name);
    for (int i = 0; i < r->n_route; i++) hop_text(trace_fwd, sizeof(trace_fwd), &a, r->route[i], i < r->n_snr_to ? r->snr_to[i] : MT_SNR_UNKNOWN);
    hop_text(trace_fwd, sizeof(trace_fwd), &a, trace_to, r->n_snr_to > r->n_route ? r->snr_to[r->n_route] : MT_SNR_UNKNOWN);
    a = (size_t)snprintf(trace_back, sizeof(trace_back), "%s", who(trace_to));
    for (int i = 0; i < r->n_back; i++) hop_text(trace_back, sizeof(trace_back), &a, r->back[i], i < r->n_snr_back ? r->snr_back[i] : MT_SNR_UNKNOWN);
    hop_text(trace_back, sizeof(trace_back), &a, cfg.node, r->n_snr_back > r->n_back ? r->snr_back[r->n_back] : MT_SNR_UNKNOWN);
    trace_id = 0;
    say("trace: %d hop%s there, %d back", r->n_route + 1, r->n_route ? "s" : "", r->n_back + 1);
}

/* ---- receive ----------------------------------------------------------- */
/* Tries the channels whose hash matches; the Data must parse to count. */
static int decode_channel(const mt_header_t *h, uint8_t *pl, size_t pn, mt_data_t *d)
{
    static uint8_t copy[256];
    for (int c = 0; c < NCH; c++) {
        if (!ch_on(c) || crt[c].hash != h->channel) continue;
        memcpy(copy, pl, pn);
        mt_crypt(crt[c].key, crt[c].key_len, h->from, h->id, copy, pn);
        if (mt_data_decode(copy, pn, d)) { memcpy(pl, copy, pn); return c; }
    }
    return -1;
}

static void on_packet(uint8_t *buf, int n, const lap_packet_meta_t *meta)
{
    mt_header_t h;
    if (!mt_header_unpack(buf, (size_t)n, &h)) return;
    heard++;
    last_rx_ms = lap_millis();
    util_add(n, false);
    const int16_t rssi = meta->rssi_tenths_dbm, snr = meta->snr_tenths_db;
    if (rssi != INT16_MIN) {
        if (hist_n < HISTORY) rssi_hist[hist_n++] = rssi;
        else { memmove(rssi_hist, rssi_hist + 1, sizeof(int) * (HISTORY - 1)); rssi_hist[HISTORY - 1] = rssi; }
    }
    const uint8_t hops = (uint8_t)(h.hop_start >= h.hop_limit ? h.hop_start - h.hop_limit : 0);
    if (h.from == cfg.node) {                       /* someone repeated ours: implicit ack */
        msg_t *m = msg_by_id(h.id);
        if (m && m->to == MT_BROADCAST && m->state == ST_SENT) m->state = ST_RELAYED;
        air_add(&h, meta, "MY ECHO");
        return;
    }
    const bool dup = seen_before(h.from, h.id);
    if (dup) cancel_relay(h.from, h.id);           /* someone else repeated it first */
    node_t *nd = node_get(h.from, true);
    nd->last_ms = lap_millis(); nd->packets++;
    if (!dup || rssi > nd->rssi) { nd->rssi = rssi; nd->snr = snr; nd->hops = hops; }
    const bool to_me = h.dest == cfg.node;
    if (dup) {
        /* A retry addressed to us: our ack was lost, send it again. */
        if (h.want_ack && to_me && cfg.tx_on) ack_to(h.from, h.id, nd->ch);
        air_add(&h, meta, "REPEAT");
        return;
    }
    const bool relay = cfg.relay && cfg.tx_on && h.hop_limit > 0 && !to_me;
    static uint8_t raw[MT_HEADER_LEN + MT_MAX_PAYLOAD + 16];
    const int rn = n > (int)sizeof(raw) ? (int)sizeof(raw) : n;
    memcpy(raw, buf, (size_t)rn);

    uint8_t *pl = buf + MT_HEADER_LEN;
    size_t pn = (size_t)n - MT_HEADER_LEN;
    mt_data_t d;
    int ch = -1;
    bool via_pki = false;
    static uint8_t plain[256];
    if (to_me && h.channel == 0 && pn > MT_PKI_OVERHEAD && cfg.pki && pki_ready) {
        if (!nd->has_key) {
            air_add(&h, meta, "PKI NO KEY");
            if (cfg.tx_on && lap_millis() - nd->asked_ms > 300000u) {   /* ask who it is: its key comes back */
                nd->asked_ms = lap_millis();
                reply_delay = 900u + lap_random() % 900u;
                send_nodeinfo_to(h.from, true);
            }
            publish(nd);
            return;
        }
        const int got = mt_pki_open(shared_key(nd), h.from, h.id, pl, pn, plain);
        if (got > 0 && mt_data_decode(plain, (size_t)got, &d)) { via_pki = true; ch = 0; pki_in++; }
        else pki_bad++;
    }
    if (!via_pki) ch = decode_channel(&h, pl, pn, &d);
    if (ch < 0) {
        if (relay) queue_relay(raw, rn, &h, snr);   /* the still encrypted bytes */
        foreign++;
        const bool known = h.channel == 0 ? false : true;
        air_add(&h, meta, to_me && h.channel == 0 ? (cfg.pki ? "PKI FAIL" : "PRIVATE") : known ? "OTHER CH" : "PRIVATE");
        snprintf(nd->last, sizeof(nd->last), "%s", h.channel ? "other ch" : "private");
        publish(nd);
        return;
    }
    decoded++;
    nd->ch = (uint8_t)ch;
    air_add(&h, meta, via_pki ? "PKI DM" : mt_port_name(d.portnum));
    snprintf(nd->last, sizeof(nd->last), "%s", mt_port_name(d.portnum));

    /* Relay: as received, except a traceroute, which this hop signs. */
    if (relay) {
        mt_route_t r;
        if (d.portnum == MT_PORT_TRACEROUTE && !via_pki && mt_route_decode(d.payload, d.payload_len, &r)) {
            route_update(&r, &h, snr, d.request_id == 0, false);
            uint8_t rb[96], frame[MT_HEADER_LEN + MT_MAX_PAYLOAD];
            const int rl = mt_route_encode(&r, rb, sizeof(rb));
            mt_data_t nd2 = { d.portnum, rb, (size_t)(rl < 0 ? 0 : rl), d.want_response, d.request_id };
            const int dl = rl < 0 ? -1 : mt_data_encode(&nd2, frame + MT_HEADER_LEN, MT_MAX_PAYLOAD);
            if (dl > 0) {
                memcpy(frame, raw, MT_HEADER_LEN);
                mt_crypt(crt[ch].key, crt[ch].key_len, h.from, h.id, frame + MT_HEADER_LEN, (size_t)dl);
                queue_relay(frame, MT_HEADER_LEN + dl, &h, snr);
            }
        } else {
            queue_relay(raw, rn, &h, snr);
        }
    }

    if (d.portnum == MT_PORT_TEXT && (h.dest == MT_BROADCAST || to_me)) {
        msg_t *m = msg_add(h.from, h.dest, h.id, (const char *)d.payload, d.payload_len, rssi, snr, hops, false);
        m->ch = (uint8_t)ch;
        m->pki = via_pki;
        if (h.want_ack && to_me && cfg.tx_on) ack_to(h.from, h.id, ch);
        lap_sound(LAP_SOUND_CHIRP);
        lap_haptic(LAP_HAPTIC_TICK);
        if (!lap_screen_open() || tab != TAB_CHAT) {
            char t[96];
            snprintf(t, sizeof(t), "%s%s: %.70s", to_me ? "DM " : "", who(h.from), m->text);
            lap_notify("MESHTASTIC", t);
            unread++;
        }
    } else if (d.portnum == MT_PORT_ROUTING && to_me) {
        uint32_t err = 0;
        msg_t *m = d.request_id ? msg_by_id(d.request_id) : NULL;
        if (m && mt_routing_decode(d.payload, d.payload_len, &err)) {
            if (!err) m->state = ST_DELIVERED;
            else if (m->pki && m->tries < ACK_TRIES) resend(m, true);   /* refused the private copy */
            else m->state = ST_FAILED;
        }
    } else if (d.portnum == MT_PORT_NODEINFO) {
        mt_user_t u;
        if (mt_user_decode(d.payload, d.payload_len, &u)) {
            if (u.short_name[0]) snprintf(nd->short_name, sizeof(nd->short_name), "%s", u.short_name);
            if (u.long_name[0]) snprintf(nd->long_name, sizeof(nd->long_name), "%s", u.long_name);
            nd->named = true;
            if (u.has_key && (!nd->has_key || memcmp(nd->pubkey, u.public_key, 32))) {
                memcpy(nd->pubkey, u.public_key, 32);
                nd->has_key = true;
                shared_forget(nd->num);
            }
            nodes_dirty();
        }
        if (d.want_response && cfg.tx_on) {                  /* it asked who we are */
            if (to_me) { reply_delay = 900u + lap_random() % 900u; send_nodeinfo_to(h.from, false); }
            else nodeinfo_due = true;
        }
    } else if (d.portnum == MT_PORT_POSITION) {
        mt_position_t p;
        if (mt_position_decode(d.payload, d.payload_len, &p) && (p.lat != 0 || p.lon != 0)) {
            nd->has_pos = true; nd->lat = p.lat; nd->lon = p.lon;
            nodes_dirty();
        }
        if (d.want_response && to_me && cfg.tx_on) { reply_delay = 800u + lap_random() % 600u; send_on = ch; send_position(h.from, h.id); }
    } else if (d.portnum == MT_PORT_TELEMETRY) {
        mt_device_t dev;
        if (mt_telemetry_decode(d.payload, d.payload_len, &dev)) {
            nd->has_dev = true; nd->battery = (uint8_t)(dev.battery > 101 ? 101 : dev.battery);
            nd->voltage = dev.voltage; nd->ch_util = dev.ch_util; nd->air_util = dev.air_util;
        }
        if (d.want_response && to_me && cfg.tx_on) { reply_delay = 800u + lap_random() % 600u; send_on = ch; send_telemetry(h.from, h.id); }
    } else if (d.portnum == MT_PORT_TRACEROUTE && to_me) {
        mt_route_t r;
        if (mt_route_decode(d.payload, d.payload_len, &r)) {
            if (d.request_id && d.request_id == trace_id) {          /* the answer to ours */
                route_update(&r, &h, snr, false, true);
                trace_done(&r);
            } else if (!d.request_id && cfg.tx_on) {                 /* a trace to us: answer it */
                route_update(&r, &h, snr, true, true);
                uint8_t rb[96];
                const int rl = mt_route_encode(&r, rb, sizeof(rb));
                reply_delay = 800u + lap_random() % 600u;
                send_on = ch;
                if (rl >= 0) send_packet(h.from, MT_PORT_TRACEROUTE, rb, (size_t)rl, false, false, h.id, 0);
            }
        }
    }
    publish(nd);
}

static void tx_service(void)
{
    if (txh > 0) {
        lap_io_status_t s = { .size = sizeof(s), .version = 1 };
        if (!lap_tx_status(txh, &s) && s.state >= LAP_IO_DONE) {
            tx_error = s.state == LAP_IO_DONE ? 0 : (s.error ? s.error : -1);
            if (tx_slot >= 0) {
                txq_t *q = &txq[tx_slot];
                msg_t *m = q->relay ? NULL : msg_by_id(q->id);
                if (m && m->state == ST_QUEUED) m->state = tx_error ? ST_FAILED : ST_SENT;
                if (m && !tx_error) m->at_ms = lap_millis();   /* the ack clock starts now */
                if (!tx_error) { if (q->relay) relayed++; else sent++; util_add(q->len, true); }
                q->used = false;
            }
            if (tx_error) say("send failed (%d)", (int)tx_error);
            txh = 0; tx_slot = -1;
            rx_open();
        }
        return;
    }
    const uint32_t now = lap_millis();
    if (nodeinfo_due) { nodeinfo_due = false; reply_delay = 900u + lap_random() % 1200u; send_nodeinfo(false); }
    if (cfg.tx_on && last_nodeinfo_ms && now - last_nodeinfo_ms > NODEINFO_EVERY_MS) send_nodeinfo(false);
    if (cfg.tx_on && cfg.pos_mode && now - last_pos_ms > POS_MIN[cfg.pos_every % 4] * 60000u) {
        if (!send_position(MT_BROADCAST, 0)) last_pos_ms = now - POS_MIN[cfg.pos_every % 4] * 60000u + 60000u;
    }
    const uint32_t te = TELE_MIN[cfg.tele % 4] * 60000u;
    if (cfg.tx_on && te && last_nodeinfo_ms && now - last_tele_ms > te && now > 120000u) {
        if (!send_telemetry(MT_BROADCAST, 0)) last_tele_ms = now - te + 60000u;
    }
    if (trace_id && now - trace_ms > TRACE_WAIT_MS) {
        trace_id = 0;
        snprintf(trace_fwd, sizeof(trace_fwd), "no answer from %s", who(trace_to));
    }
    ack_service();
    int pick = -1;
    for (int i = 0; i < TXQ; i++)
        if (txq[i].used && (int32_t)(now - txq[i].due_ms) >= 0 && (pick < 0 || txq[i].due_ms < txq[pick].due_ms)) pick = i;
    if (pick < 0 || radio < 0) return;
    rx_close();
    lap_packet_config_t c = lora_cfg();
    txh = lap_tx_packet(radio, &c, txq[pick].frame, txq[pick].len);
    tx_slot = pick;
    if (txh < 0) {
        tx_error = txh; txh = 0; say("send refused (%d)", (int)tx_error);
        msg_t *m = txq[pick].relay ? NULL : msg_by_id(txq[pick].id);
        if (m) m->state = ST_FAILED;
        txq[pick].used = false; tx_slot = -1;
        rx_open();
    }
}

/* The board parks its GPS unless someone wants it: this app does while it is
   on screen (distances) or while it shares its position. */
static void gps_update(void)
{
    const bool want = cfg.pos_mode || lap_screen_open();
    if (want && gps_lease <= 0) gps_lease = lap_gps_acquire(0);
    else if (!want && gps_lease > 0) { lap_gps_release(gps_lease); gps_lease = 0; }
}

/* Radio work: from draw while the screen is open, from the service tick
   while it is not. */
static void poll(void)
{
    static uint32_t gps_ms;
    if (lap_millis() - gps_ms > 1000) { gps_ms = lap_millis(); gps_update(); }
    save_pump();
    if (radio < 0) {
        /* At boot the app starts before the board has listed its radios:
           look again (the screen's draw used to be the only one that did,
           so a boot start heard nothing until the app was opened). */
        static uint32_t look_ms;
        if (lap_millis() - look_ms < 1000) return;
        look_ms = lap_millis();
        find_radio();
        if (radio < 0) return;
        lap_log("LoRa radio %d found", radio);
    }
    if (rxh > 0) {
        uint8_t buf[256];
        lap_packet_meta_t meta;
        for (int i = 0; i < 6; i++) {
            meta.size = sizeof(meta);
            const int n = lap_packet_read(rxh, buf, sizeof(buf), &meta);
            if (n <= 0) break;
            on_packet(buf, n, &meta);
        }
        /* A receive session can end under us (the boot start hands the app
           from its screen to the background, another app takes the chip):
           a gone, ended or never-started session is opened again. */
        static uint32_t queued_ms;
        lap_io_status_t s = { .size = sizeof(s), .version = 1 };
        const int st = lap_packet_status(rxh, &s);
        if (st) {
            lap_log("rx session gone (%d), opening again", st);
            rx_close();
        } else {
            rx_error = s.error; gaps = s.dropped;
            if (s.state >= LAP_IO_DONE) {
                lap_log("rx session ended (state %d, error %d), opening again", (int)s.state, (int)s.error);
                rx_close();
            } else if (s.state == LAP_IO_QUEUED) {
                if (!queued_ms) queued_ms = lap_millis() | 1;
                else if (lap_millis() - queued_ms > 5000) {
                    lap_log("rx session never started, opening again");
                    rx_close();
                    queued_ms = 0;
                }
            } else queued_ms = 0;
        }
    } else if (txh <= 0) {
        rx_open();
    }
    tx_service();
    if (lap_millis() - nodes_saved_ms > 300000u && node_n) nodes_dirty();
}

/* ---- background service ------------------------------------------------ */
static void tick(void) { poll(); }
static void stopped(void)
{
    rx_close(); nodes_dirty(); save_pump(); loaded = false; svc_on = false;
    if (gps_lease > 0) lap_gps_release(gps_lease);
    gps_lease = 0;
}
static const lap_service_t SVC = { tick, stopped, 100 };

static bool want_background(void) { return cfg.keep == 1 || (cfg.keep == 0 && primary); }
static void service_update(void)
{
    if (want_background() && !lap_service_running()) {
        const int r = lap_service_start(&SVC);
        svc_on = r >= 0;
        if (r < 0) lap_log("background service refused: %d", r);
    } else if (!want_background() && lap_service_running()) {
        lap_service_stop();
        svc_on = false;
    }
}

/* ---- OPTIONS ----------------------------------------------------------- */
enum { OPT_REGION = 1, OPT_PRESET, OPT_TX, OPT_POWER, OPT_KEY, OPT_CHANNEL, OPT_NAME, OPT_POS, OPT_POSEVERY,
       OPT_RELAY, OPT_KEEP, OPT_HELLO, OPT_CLEAR, OPT_FIXED, OPT_SENDPOS, OPT_TELE, OPT_PKI, OPT_TELENOW,
       OPT_CH2, OPT_CH3, OPT_CH4 };
static const char *region_names[8], *preset_names[8];
static const char *const POS_NAMES[] = { "OFF", "EXACT", "APPROX ~1KM" };
static const char *const EVERY_NAMES[] = { "15 MIN", "30 MIN", "1 HOUR", "2 HOURS" };
static const char *const TELE_NAMES[] = { "1 HOUR", "30 MIN", "3 HOURS", "OFF" };
static const char *const KEEP_NAMES[] = { "WHEN PRIMARY", "ALWAYS", "NEVER" };
/* Rows that edit text (a name or a key) are actions labelled with the value
   ("NAME LongFast"): a label holds 23 characters. */
static char lbl[6][24];

static double opt_get(int id)
{
    switch (id) {
    case OPT_REGION: return cfg.region;
    case OPT_PRESET: return cfg.preset;
    case OPT_TX: return cfg.tx_on;
    case OPT_POWER: return cfg.power_dbm;
    case OPT_POS: return cfg.pos_mode;
    case OPT_POSEVERY: return cfg.pos_every % 4;
    case OPT_TELE: return cfg.tele % 4;
    case OPT_RELAY: return cfg.relay;
    case OPT_PKI: return cfg.pki;
    case OPT_KEEP: return cfg.keep % 3;
    default: return 0;
    }
}

static void open_text(int kind, const char *prompt, const char *init, int max)
{
    if (dialog >= 0) return;
    dialog = lap_text_input(prompt, init, max, LAP_TEXT_LOWER);
    dialog_kind = kind;
}

static void opt_set(int id, double v)
{
    char t[64];
    switch (id) {
    case OPT_REGION: cfg.region = (uint8_t)v; retune(); break;
    case OPT_PRESET: cfg.preset = (uint8_t)v; retune(); break;
    case OPT_TX: cfg.tx_on = v != 0; if (cfg.tx_on) send_nodeinfo(true); break;
    case OPT_POWER: cfg.power_dbm = (int8_t)v; break;
    case OPT_KEY: key_text(0, t, sizeof(t)); open_text(6, "PRIMARY KEY base64 (AQ== default, empty = none)", t, 48); break;
    case OPT_CHANNEL: open_text(3, "PRIMARY CHANNEL NAME (empty = preset)", cfg.channel, 11); break;
    case OPT_CH2: case OPT_CH3: case OPT_CH4: {
        const int c = id - OPT_CH2 + 1;
        char k[48];
        key_text(c, k, sizeof(k));
        if (ch_on(c)) snprintf(t, sizeof(t), "%s %s", chans[c].name, k);
        else t[0] = 0;
        char prompt[48];
        snprintf(prompt, sizeof(prompt), "CHANNEL %d: name key (empty = off)", c + 1);
        open_text(7 + c - 1, prompt, t, 60);
        break;
    }
    case OPT_NAME: open_text(2, "YOUR NAME ON THE MESH", cfg.long_name, 31); break;
    case OPT_POS: cfg.pos_mode = (uint8_t)v; last_pos_ms = 0; break;
    case OPT_POSEVERY: cfg.pos_every = (uint8_t)v; break;
    case OPT_TELE: cfg.tele = (uint8_t)v; break;
    case OPT_RELAY: cfg.relay = v != 0; if (cfg.tx_on) send_nodeinfo(false); break;
    case OPT_PKI:
        cfg.pki = v != 0;
        if (cfg.pki) pki_make();
        if (cfg.tx_on) send_nodeinfo(false);            /* publish or withdraw the key */
        break;
    case OPT_KEEP: cfg.keep = (uint8_t)v; service_update(); break;
    case OPT_HELLO: send_nodeinfo(true); break;
    case OPT_TELENOW: say(send_telemetry(MT_BROADCAST, 0) ? "telemetry queued" : "telemetry not sent"); break;
    case OPT_FIXED: {
        t[0] = 0;
        if (cfg.fixed) snprintf(t, sizeof(t), "%.5f,%.5f", cfg.fixed_lat * 1e-7, cfg.fixed_lon * 1e-7);
        open_text(5, "FIXED POSITION lat,lon (empty = GPS only)", t, 30);
        break;
    }
    case OPT_SENDPOS:
        if (!cfg.pos_mode) say("SHARE POSITION is off");
        else if (!send_position(MT_BROADCAST, 0)) say("no GPS fix and no fixed position");
        else say("position queued");
        break;
    case OPT_CLEAR:
        msg_n = msg_head = 0; node_n = 0; air_n = air_head = 0; hist_n = 0;
        memset(shared, 0, sizeof(shared));
        dirty |= SV_NODES;
        break;
    }
    dirty |= SV_CFG;
}

static const lap_opt_t RADIO_ROWS[] = {
    { "REGION", LAP_OPT_CYCLE, OPT_REGION, region_names, 0, 0, 0, 0, NULL, NULL },
    { "PRESET", LAP_OPT_CYCLE, OPT_PRESET, preset_names, 0, 0, 0, 0, NULL, NULL },
    { "POWER", LAP_OPT_LEVEL, OPT_POWER, NULL, 0, 0, 22, 1, "dBm", NULL },
};
static lap_opt_t radio_rows[3];
static lap_opt_t chan_rows[] = {
    { lbl[0], LAP_OPT_ACTION, OPT_CHANNEL, NULL, 0, 0, 0, 0, NULL, NULL },
    { lbl[1], LAP_OPT_ACTION, OPT_KEY, NULL, 0, 0, 0, 0, NULL, NULL },
    { lbl[2], LAP_OPT_ACTION, OPT_CH2, NULL, 0, 0, 0, 0, NULL, NULL },
    { lbl[3], LAP_OPT_ACTION, OPT_CH3, NULL, 0, 0, 0, 0, NULL, NULL },
    { lbl[4], LAP_OPT_ACTION, OPT_CH4, NULL, 0, 0, 0, 0, NULL, NULL },
};
static const lap_opt_t POS_ROWS[] = {
    { "SHARE POSITION", LAP_OPT_CYCLE, OPT_POS, POS_NAMES, 3, 0, 0, 0, NULL, NULL },
    { "POSITION EVERY", LAP_OPT_CYCLE, OPT_POSEVERY, EVERY_NAMES, 4, 0, 0, 0, NULL, NULL },
    { "FIXED POSITION", LAP_OPT_ACTION, OPT_FIXED, NULL, 0, 0, 0, 0, NULL, NULL },
    { "SEND POSITION NOW", LAP_OPT_ACTION, OPT_SENDPOS, NULL, 0, 0, 0, 0, NULL, NULL },
};
static lap_opt_t NODE_ROWS[] = {
    { lbl[5], LAP_OPT_ACTION, OPT_NAME, NULL, 0, 0, 0, 0, NULL, NULL },
    { "PRIVATE DMS (PKI)", LAP_OPT_TOGGLE, OPT_PKI, NULL, 0, 0, 0, 0, NULL, NULL },
    { "SEND TELEMETRY", LAP_OPT_CYCLE, OPT_TELE, TELE_NAMES, 4, 0, 0, 0, NULL, NULL },
    { "SEND TELEMETRY NOW", LAP_OPT_ACTION, OPT_TELENOW, NULL, 0, 0, 0, 0, NULL, NULL },
    { "RELAY OTHERS", LAP_OPT_TOGGLE, OPT_RELAY, NULL, 0, 0, 0, 0, NULL, NULL },
    { "KEEP RUNNING", LAP_OPT_CYCLE, OPT_KEEP, KEEP_NAMES, 3, 0, 0, 0, NULL, NULL },
    { "SAY HELLO (NODE INFO)", LAP_OPT_ACTION, OPT_HELLO, NULL, 0, 0, 0, 0, NULL, NULL },
};
#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const lap_opt_list_t RADIO_LIST = { "RADIO", radio_rows, ROWS(RADIO_ROWS), NULL, NULL };
static const lap_opt_list_t CHAN_LIST = { "CHANNELS", chan_rows, ROWS(chan_rows), NULL, NULL };
static const lap_opt_list_t POS_LIST = { "POSITION", POS_ROWS, ROWS(POS_ROWS), NULL, NULL };
static const lap_opt_list_t NODE_LIST = { "THIS NODE", NODE_ROWS, ROWS(NODE_ROWS), NULL, NULL };
static const lap_opt_t TOP_ROWS[] = {
    { "TRANSMIT", LAP_OPT_TOGGLE, OPT_TX, NULL, 0, 0, 0, 0, NULL, NULL },
    { "RADIO", LAP_OPT_MENU, 0, NULL, 0, 0, 0, 0, NULL, &RADIO_LIST },
    { "CHANNELS + KEYS", LAP_OPT_MENU, 0, NULL, 0, 0, 0, 0, NULL, &CHAN_LIST },
    { "POSITION", LAP_OPT_MENU, 0, NULL, 0, 0, 0, 0, NULL, &POS_LIST },
    { "THIS NODE", LAP_OPT_MENU, 0, NULL, 0, 0, 0, 0, NULL, &NODE_LIST },
    { "CLEAR LISTS", LAP_OPT_ACTION, OPT_CLEAR, NULL, 0, 0, 0, 0, NULL, NULL },
};
static const lap_opt_list_t OPTIONS = { "MESHTASTIC", TOP_ROWS, ROWS(TOP_ROWS), opt_get, opt_set };

static void open_options(void)
{
    memcpy(radio_rows, RADIO_ROWS, sizeof(radio_rows));
    radio_rows[0].n = MT_REGION_COUNT;
    radio_rows[1].n = MT_PRESET_COUNT;
    char k[48];
    snprintf(lbl[0], sizeof(lbl[0]), "NAME %s", cfg.channel[0] ? cfg.channel : "(preset)");
    key_text(0, k, sizeof(k));
    snprintf(lbl[1], sizeof(lbl[1]), "KEY %.19s", k);
    for (int c = 1; c < NCH; c++)
        snprintf(lbl[1 + c], sizeof(lbl[1 + c]), "CH%d %s", c + 1, ch_on(c) ? chans[c].name : "off");
    snprintf(lbl[5], sizeof(lbl[5]), "NAME %.18s", cfg.long_name);
    if (!lap_options_open(&OPTIONS)) say("OPTIONS could not open");
}

/* ---- lifecycle --------------------------------------------------------- */
static void enter(void)
{
    primary = read_primary();
    lap_keep_awake(true);
    unread = 0;
    if (loaded) {                     /* back from the background: all still here */
        service_update();
        return;
    }
    cfg_load();
    chans_load();
    pki_load();
    if (cfg.pki) pki_make();
    for (int i = 0; i < MT_REGION_COUNT && i < 8; i++) region_names[i] = MT_REGIONS[i].name;
    for (int i = 0; i < MT_PRESET_COUNT && i < 8; i++) preset_names[i] = MT_PRESETS[i].name;
    radio = -1; rxh = txh = 0; tx_slot = -1; dialog = -1; gps_lease = 0;
    memset(txq, 0, sizeof(txq));
    memset(shared, 0, sizeof(shared));
    ch_epoch = lap_millis() / 10000u; tx_epoch = lap_millis() / 600000u;
    nodes_load();
    nodes_saved_ms = lap_millis();
    find_radio();
    apply_channel();
    rx_open();
    if (cfg.tx_on) nodeinfo_due = true;
    loaded = true;
    service_update();
}

static void leave(void)
{
    nodes_dirty();
    save_pump();
    if (lap_service_running()) return;     /* keeps the radio for the tick */
    if (gps_lease > 0) lap_gps_release(gps_lease);
    gps_lease = 0;
    rx_close();
    if (txh > 0) lap_tx_cancel(txh);
    txh = 0;
    loaded = false;
}

/* ---- drawing ----------------------------------------------------------- */
static void ago(uint32_t ms, char *out, size_t n)
{
    if (!ms) { snprintf(out, n, "saved"); return; }
    const uint32_t s = (lap_millis() - ms) / 1000u;
    if (s < 60) snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
    else snprintf(out, n, "%uh", (unsigned)(s / 3600));
}

static float km_bearing(double lat, double lon, const lap_gps_t *g, float *bearing)
{
    const float d2r = 3.14159265f / 180.0f;
    const float la1 = (float)g->lat * d2r, la2 = (float)lat * d2r, dn = (float)(lon - g->lon) * d2r;
    const float dl = la2 - la1;
    const float a = sinf(dl / 2) * sinf(dl / 2) + cosf(la1) * cosf(la2) * sinf(dn / 2) * sinf(dn / 2);
    float b = atan2f(sinf(dn) * cosf(la2), cosf(la1) * sinf(la2) - sinf(la1) * cosf(la2) * cosf(dn)) / d2r;
    if (b < 0) b += 360;
    if (bearing) *bearing = b;
    return 2 * 6371.0f * atan2f(sqrtf(a), sqrtf(1 - a));
}

static const char *compass(float b)
{
    static const char *const P[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return P[((int)((b + 22.5f) / 45.0f)) & 7];
}

static const char *const ART[] = {
    "   /\\  ((o))",
    "  /  \\  /\\  ",
    " / /\\ \\/  \\ ",
    "/_/  \\_/\\__\\",
};

static void draw_header(int w)
{
    const uint8_t peak = LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), wave = LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK);
    for (int i = 0; i < 4; i++) lap_put_str(1, i, ART[i], peak);
    /* The waves light for a moment when a packet arrives, and only then. */
    const bool fresh = last_rx_ms && lap_millis() - last_rx_ms < 1500;
    lap_put_str(8, 0, fresh ? "((o))" : " (o) ", fresh ? wave : LAP_UI_FAINT);
    const int x = 15;
    lap_put_str(x, 0, "MESHTASTIC", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
    lap_ui_chip(w - 11, 0, cfg.tx_on ? "TX ARMED" : "RX ONLY", cfg.tx_on, cfg.tx_on ? LAP_RED : LAP_GREEN);
    print(x, 1, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%s %s %u.%03u", MT_REGIONS[cfg.region].name, preset_name(),
          (unsigned)(freq_hz / 1000000u), (unsigned)(freq_hz / 1000u % 1000u));
    char k[48];
    key_text(0, k, sizeof(k));
    const int more = ch_count() - 1;
    print(x, 2, LAP_UI_DIM, "ch %s%s  slot %d/%d  key %.8s", channel_name(), more ? (more == 1 ? " +1" : more == 2 ? " +2" : " +3") : "",
          slot, slots, k);
    print(x, 3, LAP_UI_DIM, "!%08x %-5s", (unsigned)cfg.node, cfg.short_name);
    print(x + 16, 3, LAP_ATTR(LAP_UI_LIVE, LAP_BLACK), "rx %u ok %u tx %u", (unsigned)heard,
          (unsigned)decoded, (unsigned)sent);
    /* tags: what this node is doing right now */
    lap_gps_t g;
    const bool gps = lap_gps(&g), fix = gps && g.fix;
    char t[24];
    int cx = 1;
    const struct { const char *text; uint8_t hue; } T[] = {
        { primary ? "PRIMARY MESH" : "MESHCORE IS PRIMARY", primary ? (LAP_CYAN | LAP_BRIGHT) : LAP_WHITE },
        { lap_service_running() ? "BACKGROUND" : "OPEN ONLY", lap_service_running() ? (LAP_BLUE | LAP_BRIGHT) : LAP_WHITE },
        { fix ? (cfg.pos_mode ? "GPS SHARED" : "GPS FIX") : cfg.fixed ? (cfg.pos_mode ? "FIXED SHARED" : "FIXED POS")
              : !gps ? "NO GPS" : "GPS SEARCH", fix || cfg.fixed ? (LAP_GREEN | LAP_BRIGHT) : (LAP_YELLOW | LAP_BRIGHT) },
        { cfg.pki && pki_ready ? "PKI" : NULL, LAP_YELLOW | LAP_BRIGHT },
        { cfg.relay ? "RELAY" : NULL, LAP_MAGENTA | LAP_BRIGHT },
    };
    for (int i = 0; i < 5; i++) {
        if (!T[i].text) continue;
        snprintf(t, sizeof(t), "[%s]", T[i].text);
        if (cx + (int)strlen(t) > w) break;
        lap_put_str(cx, 4, t, LAP_ATTR(T[i].hue, LAP_BLACK));
        cx += (int)strlen(t) + 1;
    }
}

static void draw_activity(int y, int w)
{
    lap_put_str(1, y, "AIR", LAP_UI_DIM);
    int lv[HISTORY];
    for (int i = 0; i < hist_n; i++) {          /* -130..-40 dBm into 0..8 */
        int v = (rssi_hist[i] / 10 + 130) * 8 / 90;
        lv[i] = v < 0 ? 0 : v > 8 ? 8 : v;
    }
    const int sw = w - 18 < HISTORY ? w - 18 : HISTORY;
    if (hist_n) lap_ui_sparkline(5, y, sw, 1, lv + (hist_n > sw ? hist_n - sw : 0), hist_n > sw ? sw : hist_n, 8, -1);
    else lap_put_str(5, y, "waiting for a packet", LAP_UI_FAINT);
    print(w - 12, y, LAP_UI_DIM, "ch%3.0f%% tx%3.1f%%", (double)ch_util(), (double)air_util());
}

static int tab_x[TABS + 1];
static void draw_tabs(int y)
{
    static const char *const T[TABS] = { "CHAT", "NODES", "AIR" };
    int x = 1;
    for (int i = 0; i < TABS; i++) {
        tab_x[i] = x;
        x += lap_ui_chip(x, y, T[i], tab == i, LAP_CYAN) + 1;
    }
    tab_x[TABS] = x;
    if (unread) print(x + 1, y, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), "%u new", (unsigned)unread);
}

static int wrap(const char *s, int w, int (*out)(void *, int, const char *, int), void *ctx)
{
    int rows = 0, len = (int)strlen(s);
    while (len > 0) {
        int take = len > w ? w : len;
        if (take < len) { int b = take; while (b > 0 && s[b] != ' ') b--; if (b > w / 2) take = b; }
        if (out) out(ctx, rows, s, take);
        rows++;
        s += take; len -= take;
        while (*s == ' ') { s++; len--; }
    }
    return rows ? rows : 1;
}

typedef struct { int x, y; uint8_t attr; } line_ctx_t;
static int put_line(void *c, int row, const char *s, int n)
{
    line_ctx_t *l = c;
    char t[128];
    if (n > 127) n = 127;
    memcpy(t, s, (size_t)n); t[n] = 0;
    lap_put_str(l->x, l->y + row, t, l->attr);
    return 0;
}

static const char *state_word(const msg_t *m)
{
    switch (m->state) {
    case ST_QUEUED: return "sending";
    case ST_SENT: return m->to == MT_BROADCAST ? "sent" : "waiting for ack";
    case ST_RELAYED: return "sent, repeated";
    case ST_DELIVERED: return "delivered";
    case ST_FAILED: return m->to == MT_BROADCAST ? "not sent" : "no ack";
    default: return "";
    }
}

static void draw_chat(int y0, int h, int w)
{
    if (!msg_n) {
        lap_put_str(2, y0 + 1, "No messages yet.", LAP_UI_DIM);
        print(2, y0 + 3, LAP_UI_FAINT, "Listening on %s / %s%s.", MT_REGIONS[cfg.region].name, channel_name(),
              ch_count() > 1 ? " and more" : "");
        lap_put_str(2, y0 + 4, cfg.tx_on ? "S: to everyone.  C: channel.  NODES, ENTER: to one node." : "Turn TRANSMIT on in OPTIONS to send.",
                    LAP_UI_FAINT);
        return;
    }
    int y = y0 + h;                       /* newest at the bottom */
    for (int k = scroll; k < msg_n && y > y0; k++) {
        const msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        const int rows = wrap(m->text, w - 4, NULL, NULL);
        y -= rows + 1;
        if (y < y0) break;
        char t[12], line[72], chn[16] = "";
        ago(m->at_ms, t, sizeof(t));
        const bool dm = m->to != MT_BROADCAST;
        if (!dm && m->ch) snprintf(chn, sizeof(chn), "[%s] ", ch_name(m->ch));
        if (m->mine) snprintf(line, sizeof(line), "%s%s%s%s%s  %s", chn, dm ? (m->pki ? "to* " : "to ") : "", dm ? who(m->to) : "",
                              dm ? "" : t, "", state_word(m));
        else snprintf(line, sizeof(line), "%s%s%s  %d dBm  %d.%d dB  %d hop%s", chn, dm ? (m->pki ? "DM* " : "DM  ") : "", t,
                      m->rssi / 10, m->snr / 10, m->snr < 0 ? -m->snr % 10 : m->snr % 10, m->hops, m->hops == 1 ? "" : "s");
        const uint8_t hue = hue_of(m->mine && dm ? m->to : m->from);
        lap_put_str(1, y, m->mine ? ">" : "<", LAP_ATTR(hue, LAP_BLACK));
        lap_put_str(3, y, who(m->from), LAP_ATTR(hue_of(m->from), LAP_BLACK));
        const uint8_t sattr = m->state == ST_DELIVERED || m->state == ST_RELAYED ? LAP_ATTR(LAP_UI_LIVE, LAP_BLACK)
                            : m->state == ST_FAILED ? LAP_ATTR(LAP_UI_ALARM, LAP_BLACK)
                            : dm ? LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK)
                            : m->ch ? LAP_ATTR(LAP_YELLOW, LAP_BLACK) : LAP_UI_FAINT;
        lap_put_str(10, y, line, sattr);
        line_ctx_t l = { 3, y + 1, dm ? LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK) : LAP_UI_TEXT };
        wrap(m->text, w - 4, put_line, &l);
    }
}

static int node_order[MAX_NODES];
static void sort_nodes(void)
{
    for (int i = 0; i < node_n; i++) node_order[i] = i;
    for (int i = 1; i < node_n; i++)         /* newest first */
        for (int j = i; j > 0 && nodes[node_order[j]].last_ms > nodes[node_order[j - 1]].last_ms; j--) {
            int t = node_order[j]; node_order[j] = node_order[j - 1]; node_order[j - 1] = t;
        }
}

static void draw_nodes(int y0, int h, int w)
{
    if (!node_n) { lap_put_str(2, y0 + 1, "No nodes heard yet.", LAP_UI_DIM); return; }
    sort_nodes();
    if (sel >= node_n) sel = node_n - 1;
    if (sel < 0) sel = 0;
    lap_gps_t g;
    const bool have = lap_gps(&g) && g.fix;
    const int detail = 7, rows = h - 1 - detail;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + rows) scroll = sel - rows + 1;
    print(1, y0, LAP_UI_FAINT, "%-5s %-15s %4s %5s %3s %4s %4s", "ID", "NAME", "DBM", "SNR", "HOP", "BAT", "SEEN");
    for (int r = 0; r + scroll < node_n && r < rows; r++) {
        const node_t *n = &nodes[node_order[r + scroll]];
        const bool on = r + scroll == sel;
        char t[8], bat[6], line[96];
        ago(n->last_ms, t, sizeof(t));
        if (!n->has_dev) snprintf(bat, sizeof(bat), " -- ");
        else if (n->battery > 100) snprintf(bat, sizeof(bat), " PWR");
        else snprintf(bat, sizeof(bat), "%3u%%", n->battery);
        snprintf(line, sizeof(line), "%-15.15s %4d %3d.%d %3d %4s %4s", n->long_name[0] ? n->long_name : "(no name yet)",
                 n->rssi / 10, n->snr / 10, n->snr < 0 ? -n->snr % 10 : n->snr % 10, n->hops, bat, t);
        if (on) lap_fill(0, y0 + 1 + r, w, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLUE));
        lap_put_str(1, y0 + 1 + r, n->short_name, LAP_ATTR(hue_of(n->num), on ? LAP_BLUE : LAP_BLACK));
        lap_put_str(7, y0 + 1 + r, line, on ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE) : LAP_UI_TEXT);
    }
    /* The chosen node in full. */
    const node_t *n = &nodes[node_order[sel]];
    const int dy = y0 + h - detail;
    lap_hline(0, dy, w, '.', LAP_UI_FAINT);
    print(1, dy + 1, LAP_ATTR(hue_of(n->num), LAP_BLACK), "%s  %s", n->short_name, n->long_name[0] ? n->long_name : "");
    print(1, dy + 2, LAP_UI_DIM, "!%08x  %u packets  last %s  %s", (unsigned)n->num, (unsigned)n->packets,
          n->last[0] ? n->last : "-", n->has_key ? "key: PKI" : "no DM key");
    if (n->has_dev)
        print(1, dy + 3, LAP_UI_DIM, "battery %s%u%%  %.2f V  channel %.0f%%  air %.1f%%", n->battery > 100 ? ">" : "",
              n->battery > 100 ? 100 : n->battery, (double)n->voltage, (double)n->ch_util, (double)n->air_util);
    else lap_put_str(1, dy + 3, "no telemetry yet", LAP_UI_FAINT);
    if (n->has_pos) {
        if (have) {
            float b = 0;
            const float km = km_bearing(n->lat, n->lon, &g, &b);
            print(1, dy + 4, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%.5f %.5f  %.1f km %s (%.0f)", n->lat, n->lon,
                  (double)km, compass(b), (double)b);
        } else print(1, dy + 4, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK), "%.5f %.5f  (no GPS fix here)", n->lat, n->lon);
    } else lap_put_str(1, dy + 4, "no position shared (on MAP once it does)", LAP_UI_FAINT);
    if (trace_to == n->num && trace_fwd[0]) {
        print(1, dy + 5, LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK), "%.*s", w - 2, trace_fwd);
        if (trace_back[0]) print(1, dy + 6, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%.*s", w - 2, trace_back);
    } else {
        lap_put_str(1, dy + 5, "R: traceroute (the path there and back)", LAP_UI_FAINT);
    }
    if (!(trace_to == n->num && trace_back[0]))
        lap_put_str(1, dy + 6, cfg.tx_on ? "ENTER / D: message this node" : "TRANSMIT off: listen only", LAP_UI_FAINT);
}

static void draw_air(int y0, int h)
{
    if (!air_n) { lap_put_str(2, y0 + 1, "Nothing on the air yet.", LAP_UI_DIM); return; }
    print(1, y0, LAP_UI_FAINT, "%-5s %-5s %-10s %3s %4s %4s %4s", "FROM", "TO", "WHAT", "CH", "LEN", "DBM", "AGO");
    for (int r = 0; r + scroll < air_n && r < h - 1; r++) {
        const air_t *a = &air[(air_head - 1 - r - scroll + MAX_AIR * 2) % MAX_AIR];
        char t[8], to[6];
        ago(a->at_ms, t, sizeof(t));
        if (a->to == MT_BROADCAST) snprintf(to, sizeof(to), "all");
        else snprintf(to, sizeof(to), "%s", who(a->to));
        const bool ok = strcmp(a->what, "OTHER CH") && strcmp(a->what, "PRIVATE") && strcmp(a->what, "UNREADABLE") &&
                        strcmp(a->what, "PKI FAIL") && strcmp(a->what, "PKI NO KEY");
        lap_put_str(1, y0 + 1 + r, who(a->from), LAP_ATTR(hue_of(a->from), LAP_BLACK));
        print(7, y0 + 1 + r, LAP_UI_DIM, "%-5s", to);
        print(13, y0 + 1 + r, LAP_ATTR(ok ? LAP_UI_LIVE : LAP_UI_WARN, LAP_BLACK), "%-10s", a->what);
        print(24, y0 + 1 + r, LAP_UI_DIM, "%02x  %4u %4d %4s", a->channel, a->len, a->rssi / 10, t);
    }
}

static lap_btn_t keys[5];

/* "lat,lon" in degrees, to 1e-7: whole and fraction parts read separately
   (strtof alone would lose metres). */
static bool parse_part(const char **p, double *out)
{
    const char *s = *p;
    while (*s == ' ') s++;
    bool neg = false;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if (*s < '0' || *s > '9') return false;
    char *end = NULL;
    double v = (double)strtol(s, &end, 10);
    s = end;
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * scale; scale /= 10; s++; }
    }
    while (*s == ' ') s++;
    *out = neg ? -v : v;
    *p = s;
    return true;
}
static bool parse_latlon(const char *t, double *la, double *lo)
{
    const char *p = t;
    if (!parse_part(&p, la) || *p != ',') return false;
    p++;
    if (!parse_part(&p, lo) || *p) return false;
    return *la >= -90 && *la <= 90 && *lo >= -180 && *lo <= 180;
}

/* A key as typed: base64 of 0, 1, 16 or 32 bytes. Empty = no encryption. */
static bool parse_key(const char *t, chan_cfg_t *c)
{
    while (*t == ' ') t++;
    if (!*t) { c->psk_len = 1; c->psk[0] = 0; return true; }
    uint8_t k[40];
    const int n = mt_b64_decode(t, k, sizeof(k));
    if (n != 0 && n != 1 && n != 16 && n != 32) return false;
    if (n == 0) { c->psk_len = 1; c->psk[0] = 0; return true; }
    memcpy(c->psk, k, (size_t)n);
    c->psk_len = (uint8_t)n;
    return true;
}

static void handle_dialog(void)
{
    if (dialog < 0) return;
    char text[TEXT_MAX + 1];
    int v = 0;
    const int r = lap_dialog_poll(dialog, text, sizeof(text), &v);
    if (r == 1) {
        if (dialog_kind == 1) send_text(MT_BROADCAST, text);
        else if (dialog_kind == 4) send_text(dm_to, text);
        else if (dialog_kind == 2 && text[0]) {
            snprintf(cfg.long_name, sizeof(cfg.long_name), "%s", text);
            int j = 0;   /* short name: the first letters of the words */
            for (const char *p = text; *p && j < 4; p++)
                if ((p == text || p[-1] == ' ') && *p != ' ') cfg.short_name[j++] = *p;
            if (j < 2) snprintf(cfg.short_name, sizeof(cfg.short_name), "%.4s", text);
            else cfg.short_name[j] = 0;
            dirty |= SV_CFG;
            if (cfg.tx_on) send_nodeinfo(false);
        } else if (dialog_kind == 5) {
            double la = 0, lo = 0;
            const bool ok = parse_latlon(text, &la, &lo);
            if (!text[0]) { cfg.fixed = 0; say("fixed position cleared"); }
            else if (ok) {
                cfg.fixed = 1; cfg.fixed_lat = (int32_t)(la * 1e7); cfg.fixed_lon = (int32_t)(lo * 1e7);
                say("fixed position set");
            } else say("want lat,lon in degrees, e.g. -34.6037,-58.3816");
            dirty |= SV_CFG;
        } else if (dialog_kind == 3) {
            snprintf(cfg.channel, sizeof(cfg.channel), "%s", text);
            dirty |= SV_CFG;
            retune();
        } else if (dialog_kind == 6) {
            if (parse_key(text, &chans[0])) { dirty |= SV_CHANS; retune(); say("primary key set"); }
            else say("key: base64 of 1, 16 or 32 bytes (AQ== is the default)");
        } else if (dialog_kind >= 7 && dialog_kind <= 9) {
            const int c = dialog_kind - 6;
            chan_cfg_t nc;
            memset(&nc, 0, sizeof(nc));
            char *sp = strchr(text, ' ');
            if (sp) *sp = 0;
            if (!text[0]) { memset(&chans[c], 0, sizeof(chans[c])); say("channel %d off", c + 1); }
            else if (parse_key(sp ? sp + 1 : "AQ==", &nc)) {
                snprintf(nc.name, sizeof(nc.name), "%s", text);
                nc.on = 1;
                chans[c] = nc;
                say("channel %d: %s", c + 1, nc.name);
            } else say("want: name key, e.g. friends 1PG7OiApB1nwvP+rz05pAQ==");
            dirty |= SV_CHANS;
            apply_channel();
        }
    }
    if (r != 0) dialog = -1;
}

static void draw(void)
{
    const int w = lap_cols(), h = lap_rows();
    if (radio < 0 && !(lap_frame() % 25)) { find_radio(); rx_open(); }
    if (!lap_service_running()) poll();   /* the tick does it while a service runs */
    /* A refused start (an old copy still stopping, say) is tried again. */
    static uint32_t svc_try;
    if (want_background() && !lap_service_running() && lap_millis() - svc_try > 3000) {
        svc_try = lap_millis();
        primary = read_primary();
        service_update();
    }
    handle_dialog();
    if (tab == TAB_CHAT) unread = 0;

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    draw_header(w);
    draw_activity(5, w);
    draw_tabs(6);
    const bool can = cfg.tx_on && radio >= 0;
    static char send_sub[10];
    snprintf(send_sub, sizeof(send_sub), "%.8s", send_ch ? ch_name(send_ch) : "ALL");
    keys[0] = (lap_btn_t){ "SEND", send_sub, 's', false, !can, false };
    keys[1] = (lap_btn_t){ "DM", tab == TAB_NODES && node_n ? nodes[node_order[sel < node_n ? sel : 0]].short_name : NULL, 'd', false,
                           !can || tab != TAB_NODES || !node_n, false };
    keys[2] = (lap_btn_t){ "VIEW", tab == TAB_CHAT ? "CHAT" : tab == TAB_NODES ? "NODES" : "AIR", 't', false, false, unread > 0 };
    if (tab == TAB_NODES) keys[3] = (lap_btn_t){ "TRACE", NULL, 'r', false, !can || !node_n, false };
    else keys[3] = (lap_btn_t){ "CHANNEL", ch_count() > 1 ? "NEXT" : "ONE", 'c', false, ch_count() < 2, false };
    keys[4] = (lap_btn_t)LAP_BTN_OPTIONS;
    const int kh = lap_ui_keys_rows(5);
    const int body = 8, status = h - kh - 1, bh = status - body;
    if (radio < 0) lap_put_str(2, body + 1, "No LoRa radio on this board.", LAP_ATTR(LAP_UI_ALARM, LAP_BLACK));
    else if (tab == TAB_CHAT) draw_chat(body, bh, w);
    else if (tab == TAB_NODES) draw_nodes(body, bh, w);
    else draw_air(body, bh);
    lap_hline(0, status, w, '-', LAP_UI_FAINT);
    if (status_line[0] && lap_millis() - status_ms < 6000) lap_put_str(1, status, status_line, LAP_ATTR(LAP_UI_VALUE, LAP_BLACK));
    else if (rx_error) print(1, status, LAP_ATTR(LAP_UI_WARN, LAP_BLACK), "radio: %d (busy? another app)", (int)rx_error);
    else print(1, status, LAP_UI_FAINT, " %u nodes  %u other  %u relayed  %u pki  %u gaps", (unsigned)node_n, (unsigned)foreign,
               (unsigned)relayed, (unsigned)pki_in, (unsigned)gaps);
    lap_ui_keys(0, h - kh, w, kh, keys, 5, -1);
}

static void act(char c)
{
    if (c == 's') {
        if (!cfg.tx_on) { say("TRANSMIT is off: OPTIONS > TRANSMIT"); return; }
        char prompt[48];
        snprintf(prompt, sizeof(prompt), "MESSAGE TO EVERYONE ON %s", ch_name(send_ch));
        if (dialog < 0) { dialog = lap_text_input(prompt, "", TEXT_MAX, LAP_TEXT_LOWER); dialog_kind = 1; }
    } else if (c == 'd' || c == 'r') {
        if (!cfg.tx_on) { say("TRANSMIT is off: OPTIONS > TRANSMIT"); return; }
        if (tab != TAB_NODES || !node_n) { say("pick a node in NODES first"); return; }
        sort_nodes();
        const node_t *n = &nodes[node_order[sel]];
        if (c == 'r') { send_trace(n->num); return; }
        dm_to = n->num;
        char prompt[48];
        snprintf(prompt, sizeof(prompt), "DIRECT MESSAGE TO %s%s", n->short_name, cfg.pki && n->has_key ? " (PRIVATE)" : "");
        if (dialog < 0) { dialog = lap_text_input(prompt, "", TEXT_MAX, LAP_TEXT_LOWER); dialog_kind = 4; }
    } else if (c == 'c') {
        for (int i = 1; i <= NCH; i++) {
            const int nc = (send_ch + i) % NCH;
            if (ch_on(nc)) { send_ch = nc; break; }
        }
        say("sending on %s", ch_name(send_ch));
    } else if (c == 't') { tab = (tab + 1) % TABS; scroll = 0; }
    else if (c == 'n') send_nodeinfo(true);
    else if (c == 'o') open_options();
}

static bool key(lap_key_t k, char ch)
{
    if (k == LAP_KEY_CHAR) {
        if (ch == 's' || ch == 'd' || ch == 't' || ch == 'n' || ch == 'o' || ch == 'c' || ch == 'r') { act(ch); return true; }
        return false;
    }
    if (k == LAP_KEY_UP) { if (tab == TAB_NODES) { if (sel) sel--; } else scroll++; return true; }
    if (k == LAP_KEY_DOWN) { if (tab == TAB_NODES) { if (sel < node_n - 1) sel++; } else if (scroll) scroll--; return true; }
    if (k == LAP_KEY_LEFT) { tab = (tab + TABS - 1) % TABS; scroll = 0; return true; }
    if (k == LAP_KEY_RIGHT || k == LAP_KEY_TAB) { tab = (tab + 1) % TABS; scroll = 0; return true; }
    if (k == LAP_KEY_ENTER) { act(tab == TAB_NODES ? 'd' : 's'); return true; }
    return false;
}

static bool touch(int col, int row)
{
    const int hit = lap_ui_key_hit(col, row);
    if (hit >= 0) { act(keys[hit].key); return true; }
    if (row == 6) {
        for (int t = 0; t < TABS; t++)
            if (col >= tab_x[t] && col < tab_x[t + 1]) { tab = t; scroll = 0; return true; }
    }
    if (tab == TAB_NODES && row >= 9 && node_n) {          /* tap a node to choose it */
        const int i = scroll + row - 9;
        if (i >= 0 && i < node_n) { sel = i; return true; }
    }
    return false;
}

LAP_APP(.id = "meshtastic", .name = "MESHTASTIC", .sub = "be a Meshtastic node", .version = "0.3",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { "   /\\     ", "  /  \\ ((o", " / /\\ \\/  ", "/_/  \\_/\\_", "  ~ ~ ~ ~ ", "          " },
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX | LAP_CAP_SERVICE,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
