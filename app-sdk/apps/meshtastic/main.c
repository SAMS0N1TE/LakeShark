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
#include "lap_scroll.h"
#include "mt_pki.h"
#include "mt_proto.h"
#include "mt_xlate.h"
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
#define TR_MAX     400       /* a translation can run two or three times its original */
#define SIG_N      24        /* a node's last signal readings */
#define PER_MIN_N  30        /* packets a minute, the last half hour */
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

/* The screen and the translator: a slot of their own, so cfg2 keeps its size. */
typedef struct {
    uint8_t magic;          /* UI_MAGIC */
    uint8_t xl_auto;        /* translate Spanish as it arrives */
    uint8_t xl_show;        /* 0 both, 1 English only, 2 as sent */
    uint8_t quick_es;       /* quick replies go out in Spanish */
    uint8_t sort;           /* NODES: 0 heard, 1 signal, 2 distance, 3 name */
    uint8_t zoom;           /* RADAR: 0 fit, then ZOOM_KM[] */
    uint8_t labels;         /* RADAR: names beside the blips */
    uint8_t anim;           /* 0 full motion, 1 calm */
    uint8_t speak;          /* read new messages aloud: 0 no, 1 translations, 2 all */
    uint8_t reserved[7];
} ui_cfg_t;
#define UI_MAGIC 0x5Au
static ui_cfg_t ui;
static const float ZOOM_KM[] = { 0, 1, 5, 20, 100, 500 };
#define ZOOMS ((int)(sizeof(ZOOM_KM) / sizeof(ZOOM_KM[0])))

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
enum { SV_CFG = 1, SV_CHANS = 2, SV_PKI = 4, SV_NODES = 8, SV_UI = 16, SV_HIST = 32 };
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
static void ui_load(void)
{
    if (lap_load("ui", &ui, sizeof(ui)) == (int)sizeof(ui) && ui.magic == UI_MAGIC && ui.xl_show < 3 &&
        ui.sort < 4 && ui.zoom < ZOOMS && ui.speak < 3) return;
    memset(&ui, 0, sizeof(ui));
    ui.magic = UI_MAGIC; ui.xl_auto = 1; ui.quick_es = 1; ui.labels = 1;
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
    uint8_t lang;           /* mt_lang_t, as detected */
    uint8_t view;           /* 0 the screen's choice, 1 as sent, 2 both */
    char text[TEXT_MAX + 1];
    char tr[TR_MAX + 1];    /* English, when it came in Spanish */
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
    int8_t sig[SIG_N];      /* dBm of the last packets, oldest first (not saved) */
    uint8_t sig_n;
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
static uint16_t per_min[PER_MIN_N];
static uint32_t per_min_epoch;
static uint32_t last_tx_ms, rx_from;    /* for the header's animation */
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
enum { TAB_CHAT, TAB_NODES, TAB_RADAR, TAB_AIR, TAB_LANG, TABS };
static int tab, scroll, sel;
static bool chat_new;               /* a message came: CHAT follows it down */
static bool node_open;              /* NODES: one node's page */
static uint32_t open_num;
static int menu_h, pending_quick = -1, pending_phrase = -1;
/* The translator keeps static buffers: it runs only inside poll(), on
   whichever task polls (the screen's, or the service tick's), and the
   screen asks for its work through these. */
static volatile int xl_msg_req = -1;
static volatile bool xl_try_req, xl_fill_req;
static char try_es[TEXT_MAX + 1], try_en[TR_MAX + 1];
static void xl_backfill(void);

/* TRANSLATE's history: each TRY IT and each Spanish line heard, newest
   first, kept on the card so it outlives the app. */
#define MAX_HIST   16
#define HIST_MAGIC 0x31545348u          /* "HST1" */
typedef struct {
    uint32_t from;                      /* who said it; 0 typed into TRY IT */
    uint32_t unix;                      /* when, if the clock was set */
    char es[TEXT_MAX + 1];
    char en[TR_MAX + 1];                /* empty until translated */
} hist_t;
static struct { uint32_t magic, n; hist_t e[MAX_HIST]; } hist;
#define HIST_HEAD (sizeof(hist) - sizeof(hist.e))
static int xl_hist_req = -1;
static uint32_t hist_clear_ms;          /* CLEAR armed; a second tap within 3 s empties it */
static uint32_t hist_new_ms;            /* the top line arrived: lit a moment */
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
    else if (dirty & SV_UI) { what = SV_UI; id = lap_save("ui", &ui, sizeof(ui)); }
    else if (dirty & SV_HIST) { what = SV_HIST; id = lap_save("hist", &hist, HIST_HEAD + sizeof(hist_t) * hist.n); }
    if (id >= 0) { dirty &= ~what; save_job = id; save_what = what; }
}

static void hist_load(void)
{
    const int got = lap_load("hist", &hist, sizeof(hist));
    if (got < (int)HIST_HEAD || hist.magic != HIST_MAGIC || hist.n > MAX_HIST ||
        got != (int)(HIST_HEAD + sizeof(hist_t) * hist.n)) {
        memset(&hist, 0, sizeof(hist));
        hist.magic = HIST_MAGIC;
        return;
    }
    for (uint32_t i = 0; i < hist.n; i++) { hist.e[i].es[TEXT_MAX] = 0; hist.e[i].en[TR_MAX] = 0; }
}
/* On top; a line said again moves up rather than filling a second slot. */
static void hist_add(uint32_t from, const char *es, const char *en)
{
    if (!es || !es[0]) return;
    char keep[TR_MAX + 1] = "";
    uint32_t at = hist.n < MAX_HIST ? hist.n : MAX_HIST - 1;   /* the slot that goes */
    for (uint32_t i = 0; i < hist.n; i++)
        if (!strcmp(hist.e[i].es, es)) { at = i; memcpy(keep, hist.e[i].en, sizeof(keep)); break; }
    const bool grows = at == hist.n;
    memmove(&hist.e[1], &hist.e[0], sizeof(hist_t) * at);
    hist_t *h = &hist.e[0];
    memset(h, 0, sizeof(*h));
    h->from = from;
    lap_time_t now;
    lap_time(&now);
    h->unix = now.synced ? (uint32_t)now.unix : 0;
    snprintf(h->es, sizeof(h->es), "%s", es);
    snprintf(h->en, sizeof(h->en), "%s", en && en[0] ? en : keep);
    if (grows) hist.n++;
    hist_new_ms = lap_millis();
    dirty |= SV_HIST;
}
/* A line translated later (tapped in CHAT, or AUTO turned on) brings its
   English to the history too. */
static void hist_fill(const char *es, const char *en)
{
    if (!en || !en[0]) return;
    for (uint32_t i = 0; i < hist.n; i++)
        if (!hist.e[i].en[0] && !strcmp(hist.e[i].es, es)) {
            snprintf(hist.e[i].en, sizeof(hist.e[i].en), "%s", en);
            dirty |= SV_HIST;
        }
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
    /* UTF-8 is kept as sent (the screen folds accents for its font, and the
       translator needs them); control bytes go, and a letter cut off at the
       end is dropped rather than left half there. */
    size_t j = 0;
    for (size_t i = 0; i < len && j < TEXT_MAX; i++) {
        const uint8_t c = (uint8_t)text[i];
        if (c == '\n' || c == '\t') m->text[j++] = ' ';
        else if (c >= 32) m->text[j++] = (char)c;
    }
    size_t lead = j;                       /* the last letter: whole, or dropped */
    while (lead > 0 && j - lead < 4 && ((uint8_t)m->text[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead > 0 && (uint8_t)m->text[lead - 1] >= 0xC0) {
        const uint8_t b = (uint8_t)m->text[lead - 1];
        const size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
        if (j - (lead - 1) < need) j = lead - 1;
    }
    m->text[j] = 0;
    if (!mine) {
        int conf = 0;
        mt_lang_t l = mt_xlate_detect(m->text, &conf);
        if (l == MT_LANG_ES && conf < MT_XLATE_MIN_CONF) l = MT_LANG_UNKNOWN;
        m->lang = (uint8_t)l;
        if (l == MT_LANG_ES && ui.xl_auto) mt_xlate_es_en(m->text, m->tr, sizeof(m->tr));
        if (l == MT_LANG_ES) hist_add(m->from, m->text, m->tr);
    }
    chat_new = true;
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
    {
        const uint32_t e = lap_millis() / 60000u;   /* packets a minute */
        if (e - per_min_epoch >= PER_MIN_N) { memset(per_min, 0, sizeof(per_min)); per_min_epoch = e; }
        while (per_min_epoch < e) per_min[++per_min_epoch % PER_MIN_N] = 0;
        if (per_min[e % PER_MIN_N] < 0xFFFF) per_min[e % PER_MIN_N]++;
    }
    const bool dup = seen_before(h.from, h.id);
    if (dup) cancel_relay(h.from, h.id);           /* someone else repeated it first */
    node_t *nd = node_get(h.from, true);
    nd->last_ms = lap_millis(); nd->packets++;
    rx_from = h.from;
    if (rssi != INT16_MIN) {
        const int dbm = rssi / 10 < -127 ? -127 : rssi / 10 > 0 ? 0 : rssi / 10;
        if (nd->sig_n == SIG_N) { memmove(nd->sig, nd->sig + 1, SIG_N - 1); nd->sig_n--; }
        nd->sig[nd->sig_n++] = (int8_t)dbm;
    }
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
        /* Spanish comes in English: in the banner, and aloud when asked */
        const bool en = m->tr[0] != 0;
        if (ui.speak == 2 || (ui.speak == 1 && en)) {
            char t[160];
            snprintf(t, sizeof(t), "%s%s says: %.120s", to_me ? "Direct. " : "", nd->long_name[0] ? nd->long_name : who(h.from),
                     en ? m->tr : m->text);
            lap_say(t);
        }
        if (!lap_screen_open() || tab != TAB_CHAT) {
            char t[96];
            snprintf(t, sizeof(t), "%s%s%s: %.70s", to_me ? "DM " : "", en ? "(ES>EN) " : "", who(h.from), en ? m->tr : m->text);
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
                if (!tx_error) { if (q->relay) relayed++; else sent++; util_add(q->len, true); last_tx_ms = lap_millis(); }
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
static void xl_service(void)
{
    if (xl_fill_req) { xl_fill_req = false; xl_backfill(); }
    const int i = xl_msg_req;
    if (i >= 0 && i < MAX_MSGS) {
        xl_msg_req = -1;
        if (!msgs[i].tr[0]) mt_xlate_es_en(msgs[i].text, msgs[i].tr, sizeof(msgs[i].tr));
        hist_fill(msgs[i].text, msgs[i].tr);
    }
    const int hi = xl_hist_req;
    if (hi >= 0 && hi < (int)hist.n) {
        xl_hist_req = -1;
        if (!hist.e[hi].en[0]) {
            mt_xlate_es_en(hist.e[hi].es, hist.e[hi].en, sizeof(hist.e[hi].en));
            dirty |= SV_HIST;
        }
    }
    if (xl_try_req) {
        xl_try_req = false;
        try_en[0] = 0;
        if (try_es[0]) mt_xlate_es_en(try_es, try_en, sizeof(try_en));
        if (try_en[0]) hist_add(0, try_es, try_en);
    }
}

static void poll(void)
{
    static uint32_t gps_ms;
    xl_service();
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
       OPT_CH2, OPT_CH3, OPT_CH4, OPT_XL_AUTO, OPT_XL_SHOW, OPT_QUICK, OPT_ANIM, OPT_LABELS, OPT_SPEAK };
static const char *region_names[8], *preset_names[8];
static const char *const POS_NAMES[] = { "OFF", "EXACT", "APPROX ~1KM" };
static const char *const EVERY_NAMES[] = { "15 MIN", "30 MIN", "1 HOUR", "2 HOURS" };
static const char *const TELE_NAMES[] = { "1 HOUR", "30 MIN", "3 HOURS", "OFF" };
static const char *const KEEP_NAMES[] = { "WHEN PRIMARY", "ALWAYS", "NEVER" };
static const char *const SHOW_NAMES[] = { "BOTH", "ENGLISH ONLY", "AS SENT" };
static const char *const QUICK_NAMES[] = { "ENGLISH", "SPANISH" };
static const char *const ANIM_NAMES[] = { "FULL", "CALM" };
static const char *const SPEAK_NAMES[] = { "OFF", "TRANSLATIONS", "EVERY MESSAGE" };

/* Spanish already on the screen gets its English when AUTO is turned on. */
static void xl_backfill(void)
{
    for (int k = 0; k < msg_n; k++) {
        msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (!m->mine && m->lang == MT_LANG_ES && !m->tr[0]) mt_xlate_es_en(m->text, m->tr, sizeof(m->tr));
        if (!m->mine && m->lang == MT_LANG_ES) hist_fill(m->text, m->tr);
    }
    for (uint32_t i = 0; i < hist.n; i++)
        if (!hist.e[i].en[0]) { mt_xlate_es_en(hist.e[i].es, hist.e[i].en, sizeof(hist.e[i].en)); dirty |= SV_HIST; }
}
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
    case OPT_XL_AUTO: return ui.xl_auto;
    case OPT_XL_SHOW: return ui.xl_show % 3;
    case OPT_QUICK: return ui.quick_es;
    case OPT_ANIM: return ui.anim;
    case OPT_LABELS: return ui.labels;
    case OPT_SPEAK: return ui.speak % 3;
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
    case OPT_XL_AUTO: ui.xl_auto = v != 0; if (ui.xl_auto) xl_fill_req = true; dirty |= SV_UI; return;
    case OPT_XL_SHOW: ui.xl_show = (uint8_t)v; dirty |= SV_UI; return;
    case OPT_QUICK: ui.quick_es = v != 0; dirty |= SV_UI; return;
    case OPT_ANIM: ui.anim = v != 0; dirty |= SV_UI; return;
    case OPT_LABELS: ui.labels = v != 0; dirty |= SV_UI; return;
    case OPT_SPEAK: ui.speak = (uint8_t)v; dirty |= SV_UI; if (ui.speak) lap_say("Messages will be read aloud."); return;
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
static const lap_opt_t XL_ROWS[] = {
    { "AUTO TRANSLATE", LAP_OPT_TOGGLE, OPT_XL_AUTO, NULL, 0, 0, 0, 0, NULL, NULL },
    { "SHOW", LAP_OPT_CYCLE, OPT_XL_SHOW, SHOW_NAMES, 3, 0, 0, 0, NULL, NULL },
    { "QUICK REPLIES IN", LAP_OPT_CYCLE, OPT_QUICK, QUICK_NAMES, 2, 0, 0, 0, NULL, NULL },
    { "SPEAK MESSAGES", LAP_OPT_CYCLE, OPT_SPEAK, SPEAK_NAMES, 3, 0, 0, 0, NULL, NULL },
};
static const lap_opt_t SCREEN_ROWS[] = {
    { "MOTION", LAP_OPT_CYCLE, OPT_ANIM, ANIM_NAMES, 2, 0, 0, 0, NULL, NULL },
    { "MAP NAMES", LAP_OPT_TOGGLE, OPT_LABELS, NULL, 0, 0, 0, 0, NULL, NULL },
};
#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const lap_opt_list_t XL_LIST = { "TRANSLATE", XL_ROWS, ROWS(XL_ROWS), NULL, NULL };
static const lap_opt_list_t SCREEN_LIST = { "SCREEN", SCREEN_ROWS, ROWS(SCREEN_ROWS), NULL, NULL };
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
    { "TRANSLATE", LAP_OPT_MENU, 0, NULL, 0, 0, 0, 0, NULL, &XL_LIST },
    { "SCREEN", LAP_OPT_MENU, 0, NULL, 0, 0, 0, 0, NULL, &SCREEN_LIST },
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
    ui_load();
    hist_load();
    pki_load();
    if (cfg.pki) pki_make();
    for (int i = 0; i < MT_REGION_COUNT && i < 8; i++) region_names[i] = MT_REGIONS[i].name;
    for (int i = 0; i < MT_PRESET_COUNT && i < 8; i++) preset_names[i] = MT_PRESETS[i].name;
    radio = -1; rxh = txh = 0; tx_slot = -1; dialog = -1; gps_lease = 0;
    menu_h = 0; pending_quick = pending_phrase = -1; node_open = false;
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
/* The look is the board's: its keys, bezels, chips and VFD meters, a radar
   of the mesh in braille, ASCII bubbles for the chat. What moves is driven
   by lap_millis() and holds still under OPTIONS > SCREEN > MOTION CALM.

   Portrait: title, the header (radar beside the frequency), the < TAB >
   strip, the panel in its bezel, four keys. Landscape: the header's parts
   stacked down the left, the strip and panel on the right. */

static int W, H;
static bool wide;                       /* landscape */
static int kr, tr, ty, sx, sw;          /* key rows; the strip's rows, y, x, width */
static int hx, hy, hw, hh;              /* the header block */
static int px, py, pw, ph;              /* inside the panel's bezel */
static int clip0, clip1, smax;          /* the rows a list may use; how far it scrolls */
static lap_scroll_t drag;
static bool dragging;
static lap_btn_t keys[4];
static uint32_t now_ms;
static double me_lat, me_lon;
static bool me_ok;
static bool kbd_used;                   /* the arrows moved the NODES choice */

static bool calm(void) { return ui.anim != 0; }

/* What a tap on the panel lands on, noted while the panel is drawn. */
enum { HIT_MSG = 1, HIT_NODE, HIT_PHRASE, HIT_BLIP, HIT_XL_AUTO, HIT_XL_SHOW, HIT_TRY, HIT_AIR, HIT_HIST, HIT_HIST_CLEAR };
typedef struct { int16_t x0, y0, x1, y1; uint8_t kind; int16_t idx; } hit_t;
#define HITS 96
static hit_t hits[HITS];
static int hit_n;
static void hit(int x0, int y0, int x1, int y1, int kind, int idx)
{
    if (y0 < clip0) y0 = clip0;
    if (y1 > clip1) y1 = clip1;
    if (y1 <= y0 || hit_n >= HITS) return;
    hits[hit_n++] = (hit_t){ (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (uint8_t)kind, (int16_t)idx };
}

/* ---- text: UTF-8 in, one cell a letter ---- */
static int u8_cells(const char *s, int n)
{
    int c = 0;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) if (((uint8_t)s[i] & 0xC0) != 0x80) c++;
    return c;
}
/* The bytes of s that fill at most `cells` cells. */
static int u8_bytes(const char *s, int cells)
{
    int i = 0, c = 0;
    for (; s[i]; i++) {
        if (((uint8_t)s[i] & 0xC0) != 0x80) {
            if (c == cells) break;
            c++;
        }
    }
    return i;
}
/* Text inside the clip rows, cut to w cells. Returns the cells drawn. */
static int put(int x, int y, int w, uint8_t attr, const char *fmt, ...)
{
    if (w <= 0 || y < clip0 || y >= clip1) return 0;
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    /* the font has no inverted marks (they fold to '?'): they go */
    int o = 0;
    for (int i = 0; t[i]; i++) {
        if ((uint8_t)t[i] == 0xC2 && ((uint8_t)t[i + 1] == 0xBF || (uint8_t)t[i + 1] == 0xA1)) { i++; continue; }
        t[o++] = t[i];
    }
    t[o] = 0;
    t[u8_bytes(t, w)] = 0;
    lap_put_str(x, y, t, attr);
    return u8_cells(t, -1);
}
static void cell(int x, int y, int16_t ch, uint8_t attr)
{
    if (y >= clip0 && y < clip1) lap_put_cell(x, y, ch, attr);
}
static void hfill(int x, int y, int w, int16_t ch, uint8_t attr)
{
    if (y >= clip0 && y < clip1 && w > 0) lap_hline(x, y, w, ch, attr);
}

typedef struct { int16_t at, n; } span_t;
/* Lines of at most w cells, broken at spaces where it can. */
static int wrap_spans(const char *s, int w, span_t *out, int max)
{
    int rows = 0, i = 0;
    const int len = (int)strlen(s);
    if (w < 4) w = 4;
    while (i < len && rows < max) {
        while (s[i] == ' ') i++;
        if (i >= len) break;
        int take = u8_bytes(s + i, w);
        if (i + take < len && s[i + take] != ' ') {
            int b = take;
            while (b > 0 && s[i + b] != ' ') b--;
            if (b > 0 && u8_cells(s + i, b) > w / 3) take = b;
        }
        out[rows].at = (int16_t)i; out[rows].n = (int16_t)take;
        rows++;
        i += take;
    }
    return rows;
}
static void put_span(int x, int y, const char *s, span_t sp, int show, uint8_t attr)
{
    char t[TR_MAX + 1];
    int n = sp.n < TR_MAX ? sp.n : TR_MAX;
    memcpy(t, s + sp.at, (size_t)n);
    t[n] = 0;
    if (show >= 0) t[u8_bytes(t, show)] = 0;      /* still typing in */
    put(x, y, 255, attr, "%s", t);
}

static uint8_t A(uint8_t fg) { return LAP_ATTR(fg, LAP_BLACK); }

/* lap_ui_bezel draws wherever it is told, even outside the list it belongs
   to: scrolled, TRY IT's frame was painted over the header. Partly out of
   the list, the same frame is drawn here a cell at a time, clipped. */
static void bezel(int x, int y, int w, int h, const char *title)
{
    if (y >= clip0 && y + h <= clip1) { lap_ui_bezel(x, y, w, h, title); return; }
    const uint8_t edge = A(LAP_CYAN);
    for (int k = 0; k < w; k++) { cell(x + k, y, '=', edge); cell(x + k, y + h - 1, '=', edge); }
    for (int r = 1; r < h - 1; r++) { cell(x, y + r, '|', edge); cell(x + w - 1, y + r, '|', edge); }
    if (title && w > (int)strlen(title) + 8) put(x + 3, y, (int)strlen(title), A(LAP_CYAN | LAP_BRIGHT), "%s", title);
}
static uint8_t dim(uint8_t hue) { return (uint8_t)(hue & 7); }

static void ago(uint32_t ms, char *out, size_t n)
{
    if (!ms) { snprintf(out, n, "saved"); return; }
    const uint32_t s = (now_ms - ms) / 1000u;
    if (s < 60) snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
    else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

static float km_bearing(double la1d, double lo1d, double la2d, double lo2d, float *bearing)
{
    const float d2r = 3.14159265f / 180.0f;
    const float la1 = (float)la1d * d2r, la2 = (float)la2d * d2r, dn = (float)(lo2d - lo1d) * d2r;
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
static char arrow(float b)
{
    static const char P[] = { '^', '/', '>', '\\', 'v', '/', '<', '\\' };
    return P[((int)((b + 22.5f) / 45.0f)) & 7];
}
static void dist_text(float km, char *out, size_t n)
{
    if (km < 1.0f) snprintf(out, n, "%d m", (int)(km * 1000.0f + 0.5f));
    else if (km < 10.0f) snprintf(out, n, "%.1f km", (double)km);
    else snprintf(out, n, "%d km", (int)(km + 0.5f));
}

/* 0..4 bars, by SNR when the radio gave one (LoRa reads below the noise). */
static int sig_level(int16_t rssi, int16_t snr)
{
    if (snr != INT16_MIN && snr != 0) {
        const int s = snr / 10;
        return s > 5 ? 4 : s > 0 ? 3 : s > -7 ? 2 : s > -13 ? 1 : 0;
    }
    const int r = rssi / 10;
    return r > -80 ? 4 : r > -95 ? 3 : r > -108 ? 2 : r > -118 ? 1 : 0;
}
static uint8_t level_hue(int l) { return l >= 3 ? (LAP_GREEN | LAP_BRIGHT) : l == 2 ? (LAP_YELLOW | LAP_BRIGHT) : (LAP_RED | LAP_BRIGHT); }
/* Four rising bars in braille, unlit ones grey. */
static void sig_bars(int x, int y, int level)
{
    static const uint8_t L[4] = { 0x01, 0x02, 0x04, 0x40 }, R[4] = { 0x08, 0x10, 0x20, 0x80 };
    for (int b = 0; b < 4; b++) {
        uint8_t d = 0;
        for (int r = 3; r >= 3 - b; r--) d |= L[r] | R[r];
        cell(x + b, y, LAP_BRAILLE(d), b < level ? A(level_hue(level)) : LAP_UI_FAINT);
    }
}
/* [######--] 78%, or the plug when on power. Returns cells used. */
static int battery(int x, int y, int cells, const node_t *n)
{
    if (!n->has_dev) return put(x, y, cells + 7, LAP_UI_FAINT, "[%*s] --", cells, "");
    if (n->battery > 100) return put(x, y, cells + 7, A(LAP_CYAN | LAP_BRIGHT), "=[%.*s]- PWR", cells - 2 > 0 ? cells - 2 : 0, "########");
    const int pct = n->battery, lit = (pct * cells + 50) / 100;
    const uint8_t hue = pct > 50 ? (LAP_GREEN | LAP_BRIGHT) : pct > 20 ? (LAP_YELLOW | LAP_BRIGHT) : (LAP_RED | LAP_BRIGHT);
    cell(x, y, '[', LAP_UI_DIM);
    for (int i = 0; i < cells; i++) cell(x + 1 + i, y, i < lit ? LAP_BLOCK : LAP_SHADE_25, i < lit ? A(hue) : LAP_UI_FAINT);
    cell(x + 1 + cells, y, ']', LAP_UI_DIM);
    return 2 + cells + put(x + 2 + cells, y, 5, A(hue), " %d%%", pct);
}

/* ---- a braille canvas, bigger than lap_art's, with true circles ---- */
#define CV_W 76
#define CV_H 44
typedef struct {
    int x, y, w, h;
    float ax;                           /* a dot's height over its width: rings stay round */
    uint8_t dot[CV_H][CV_W], at[CV_H][CV_W];
} canvas_t;
static canvas_t cv;
static void cv_begin(int x, int y, int w, int h)
{
    memset(cv.dot, 0, sizeof(cv.dot));
    memset(cv.at, 0, sizeof(cv.at));
    cv.x = x; cv.y = y;
    cv.w = w < 0 ? 0 : w > CV_W ? CV_W : w;
    cv.h = h < 0 ? 0 : h > CV_H ? CV_H : h;
    int cw = 0, ch = 0;
    lap_cell_px(&cw, &ch);
    cv.ax = cw > 0 && ch > 0 ? ((float)ch / 4.0f) / ((float)cw / 2.0f) : 1.0f;
}
static void cv_dot(int x, int y, uint8_t attr)
{
    static const uint8_t bit[2][4] = { { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 } };
    if (x < 0 || y < 0 || x >= cv.w * 2 || y >= cv.h * 4) return;
    cv.dot[y / 4][x / 2] |= bit[x & 1][y & 3];
    if (attr) cv.at[y / 4][x / 2] = attr;
}
static int rnd(float f) { return (int)(f < 0 ? f - 0.5f : f + 0.5f); }
static void cv_line(int x0, int y0, int x1, int y1, uint8_t attr)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int stx = x0 < x1 ? 1 : -1, sty = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (int i = 0; i < 512; i++) {
        cv_dot(x0, y0, attr);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += stx; }
        if (e2 <= dx) { e += dx; y0 += sty; }
    }
}
/* A ring r dots across (taller or shorter in dots so it looks round);
   every gap-th point left out. */
static void cv_ring(float cx, float cy, float r, int gap, uint8_t attr)
{
    const int n = (int)(r * 7.0f) + 10;
    for (int i = 0; i < n; i++) {
        if (gap && i % gap == 0) continue;
        const float t = 6.2831853f * (float)i / (float)n;
        cv_dot(rnd(cx + r * cosf(t)), rnd(cy + r * sinf(t) / cv.ax), attr);
    }
}
static void cv_flush(void)
{
    for (int r = 0; r < cv.h; r++)
        for (int c = 0; c < cv.w; c++)
            if (cv.dot[r][c]) cell(cv.x + c, cv.y + r, LAP_BRAILLE(cv.dot[r][c]), cv.at[r][c] ? cv.at[r][c] : A(LAP_CYAN));
}

/* ---- the mesh web ---- */
/* The mesh as a network, not a radar: this node in the middle, every node
   heard as a point, a line from each to the node it most likely came
   through (direct ones straight to the middle, solid; relayed ones dashed,
   to the nearest node one hop closer). A packet heard runs along its path
   to the middle as a bright pulse; one sent runs out along every line.
   With a position here and there, points sit by bearing and distance
   (north up, on a faint map grid); without, by hops at an angle of their
   own. */
typedef struct { float fx, fy; uint8_t hue; bool placed; int node, parent; } blip_t;
static blip_t blips[MAX_NODES];
static int blip_n;
static float radar_km;                  /* the edge, when nodes sit by distance */

static float hash_ang(uint32_t num) { return 6.2831853f * (float)((num * 2654435761u) >> 20 & 1023u) / 1024.0f; }
static float nice_km(float km)
{
    static const float S[] = { 0.3f, 0.6f, 1.5f, 3, 6, 15, 30, 45, 60, 90, 150, 300, 600, 1500, 3000, 6000 };
    for (int i = 0; i < (int)(sizeof(S) / sizeof(S[0])); i++) if (km <= S[i]) return S[i];
    return 20000;
}
static void make_blips(void)
{
    blip_n = 0;
    float far = 0;
    for (int i = 0; i < node_n; i++)
        if (me_ok && nodes[i].has_pos) {
            const float d = km_bearing(me_lat, me_lon, nodes[i].lat, nodes[i].lon, NULL);
            if (d > far) far = d;
        }
    radar_km = ui.zoom ? ZOOM_KM[ui.zoom] : nice_km(far > 0 ? far * 1.08f : 1);
    for (int i = 0; i < node_n && blip_n < MAX_NODES; i++) {
        const node_t *n = &nodes[i];
        blip_t *b = &blips[blip_n++];
        b->node = i; b->parent = -1;
        b->hue = hue_of(n->num);
        float ang, r;
        if (me_ok && n->has_pos) {
            float brg = 0;
            const float d = km_bearing(me_lat, me_lon, n->lat, n->lon, &brg);
            ang = (brg - 90.0f) * 3.14159265f / 180.0f;            /* north up */
            r = d / radar_km;
            if (r > 1.0f) r = 1.04f;                                 /* off the scale: at the edge */
            if (r < 0.05f) r = 0.05f;
            b->placed = true;
        } else {
            ang = hash_ang(n->num);
            r = 0.32f + 0.22f * (float)(n->hops > 3 ? 3 : n->hops);
            b->placed = false;
        }
        b->fx = r * cosf(ang); b->fy = r * sinf(ang);
    }
    /* who each relayed node most likely came through: the nearest point one hop closer */
    for (int i = 0; i < blip_n; i++) {
        const int hops = nodes[blips[i].node].hops;
        if (!hops) continue;
        float best = 1e9f;
        for (int j = 0; j < blip_n; j++) {
            if (nodes[blips[j].node].hops != hops - 1) continue;
            const float dx = blips[i].fx - blips[j].fx, dy = blips[i].fy - blips[j].fy, d = dx * dx + dy * dy;
            if (d < best) { best = d; blips[i].parent = j; }
        }
    }
}
static void web_xy(const blip_t *b, float cx, float cy, float R, int *px, int *py)
{
    *px = rnd(cx + b->fx * R);
    *py = rnd(cy + b->fy * R / cv.ax);
}
static void cv_dashed(int x0, int y0, int x1, int y1, uint8_t attr)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int stx = x0 < x1 ? 1 : -1, sty = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (int i = 0; i < 512; i++) {
        if (i % 4 < 2) cv_dot(x0, y0, attr);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += stx; }
        if (e2 <= dx) { e += dx; y0 += sty; }
    }
}
/* A bright pulse f (0..1) of the way from (x0,y0) to (x1,y1), with a short tail. */
static void pulse(int x0, int y0, int x1, int y1, float f, uint8_t head, uint8_t tail)
{
    for (int k = 2; k >= 0; k--) {
        float g = f - 0.06f * (float)k;
        if (g < 0) continue;
        const int px2 = rnd((float)x0 + (float)(x1 - x0) * g), py2 = rnd((float)y0 + (float)(y1 - y0) * g);
        const uint8_t at = k ? tail : head;
        cv_dot(px2, py2, at);
        if (!k) { cv_dot(px2 + 1, py2, at); cv_dot(px2, py2 + 1, at); cv_dot(px2 + 1, py2 + 1, at); }
    }
}
/* The path from blip i to the middle: up to four legs. Returns legs. */
static int web_path(int i, int lx[5], int ly[5], float cx, float cy, float R)
{
    int n = 0;
    for (int b = i; b >= 0 && n < 4; b = blips[b].parent) { web_xy(&blips[b], cx, cy, R, &lx[n], &ly[n]); n++; }
    lx[n] = rnd(cx); ly[n] = rnd(cy);
    return n;
}
static void mesh_web(int x, int y, int w, int h, bool names, bool big)
{
    cv_begin(x, y, w, h);
    const float cx = (float)cv.w - 0.5f, cy = (float)cv.h * 2.0f - 0.5f;
    float R = cx - 2.0f;
    if ((cy - 2.0f) * cv.ax < R) R = (cy - 2.0f) * cv.ax;
    if (R < 4) return;
    const bool moving = !calm();
    /* the ground: a sparse map grid when points sit on the map */
    if (big && me_ok) {
        const int step = 8;
        for (int gy = (int)cy % step; gy < cv.h * 4; gy += step)
            for (int gx = (int)cx % step; gx < cv.w * 2; gx += step) cv_dot(gx, gy, LAP_UI_FAINT);
    }
    /* the lines, then traffic on them */
    for (int i = 0; i < blip_n; i++) {
        const blip_t *b = &blips[i];
        int bx, by, qx, qy;
        web_xy(b, cx, cy, R, &bx, &by);
        if (b->parent >= 0) web_xy(&blips[b->parent], cx, cy, R, &qx, &qy);
        else { qx = rnd(cx); qy = rnd(cy); }
        const node_t *n = &nodes[b->node];
        const bool stale = !n->last_ms || now_ms - n->last_ms > 1800000u;
        if (!n->hops && b->parent < 0) cv_line(qx, qy, bx, by, stale ? LAP_UI_FAINT : A(dim(b->hue)));
        else cv_dashed(qx, qy, bx, by, stale ? LAP_UI_FAINT : A(dim(b->hue)));
        /* idle: a faint dot drifts in along each live direct line */
        if (moving && !stale && !n->hops) {
            const uint32_t period = 2600u + (n->num % 7u) * 300u;
            const float f = (float)((now_ms + n->num * 97u) % period) / (float)period;
            cv_dot(rnd((float)bx + (float)(qx - bx) * f), rnd((float)by + (float)(qy - by) * f), A(dim(b->hue)));
        }
    }
    for (int i = 0; i < blip_n && moving; i++) {
        const node_t *n = &nodes[blips[i].node];
        if (!n->last_ms || now_ms - n->last_ms > 1500u) continue;
        int lx[5], ly[5];
        const int legs = web_path(i, lx, ly, cx, cy, R);
        const float f = (float)(now_ms - n->last_ms) / 1500.0f * (float)legs;
        const int leg = (int)f < legs ? (int)f : legs - 1;
        pulse(lx[leg], ly[leg], lx[leg + 1], ly[leg + 1], f - (float)leg, A(LAP_WHITE | LAP_BRIGHT), A(blips[i].hue));
    }
    if (moving && last_tx_ms && now_ms - last_tx_ms < 1500u) {       /* ours, going out */
        const float f = (float)(now_ms - last_tx_ms) / 1500.0f;
        for (int i = 0; i < blip_n; i++) {
            if (blips[i].parent >= 0) continue;
            int bx, by;
            web_xy(&blips[i], cx, cy, R, &bx, &by);
            pulse(rnd(cx), rnd(cy), bx, by, f, A(LAP_YELLOW | LAP_BRIGHT), A(LAP_YELLOW));
        }
    }
    /* the points */
    for (int i = 0; i < blip_n; i++) {
        const blip_t *b = &blips[i];
        const node_t *n = &nodes[b->node];
        int bx, by;
        web_xy(b, cx, cy, R, &bx, &by);
        const bool fresh = n->last_ms && now_ms - n->last_ms < 1500u;
        const bool stale = !n->last_ms || now_ms - n->last_ms > 1800000u;
        const uint8_t at = fresh && moving && (now_ms / 150u) % 2u ? A(LAP_WHITE | LAP_BRIGHT) : stale ? A(LAP_BLUE | LAP_BRIGHT)
                         : A(b->placed ? b->hue : dim(b->hue));
        cv_dot(bx, by, at); cv_dot(bx + 1, by, at); cv_dot(bx, by + 1, at); cv_dot(bx + 1, by + 1, at);
        if (big) { cv_dot(bx - 1, by, at); cv_dot(bx + 2, by + 1, at); cv_dot(bx, by - 1, at); cv_dot(bx + 1, by + 2, at); }
    }
    /* this node: a diamond that beats while it listens */
    const int mx = rnd(cx), my = rnd(cy);
    const int s = big ? 3 : 2, beat = moving && rxh > 0 && (now_ms / 700u) % 2u ? 1 : 0;
    const uint8_t me_at = A(cfg.tx_on ? (LAP_YELLOW | LAP_BRIGHT) : (LAP_WHITE | LAP_BRIGHT));
    for (int k = -(s + beat); k <= s + beat; k++) {
        const int a = s + beat - (k < 0 ? -k : k);
        cv_dot(mx + k, my - a, me_at); cv_dot(mx + k, my + a, me_at);
    }
    cv_flush();
    if (big && me_ok) {                                      /* north */
        put(x, y, 3, A(LAP_YELLOW | LAP_BRIGHT), "N");
        put(x, y + 1, 3, A(LAP_YELLOW), "^");
    }
    if (!names) return;
    /* names, where they fit */
    static uint8_t used[CV_H][CV_W / 8 + 1];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < blip_n; i++) {
        const blip_t *b = &blips[i];
        const node_t *n = &nodes[b->node];
        int bx, by;
        web_xy(b, cx, cy, R, &bx, &by);
        const int ccol = bx / 2, crow = by / 4;
        hit(x + ccol - 1, y + crow - 1, x + ccol + 6, y + crow + 2, HIT_BLIP, b->node);
        const int len = u8_cells(n->short_name, -1);
        int lx = ccol + 2;
        if (lx + len > cv.w) lx = ccol - 1 - len;
        if (lx < 0 || crow < 0 || crow >= cv.h) continue;
        bool free = true;
        for (int k = lx - 1; k <= lx + len && free; k++)
            if (k >= 0 && k < CV_W && (used[crow][k / 8] >> (k % 8) & 1)) free = false;
        if (!free) continue;
        for (int k = lx; k < lx + len && k < CV_W; k++) used[crow][k / 8] |= (uint8_t)(1u << (k % 8));
        put(x + lx, y + crow, len, A(b->placed ? b->hue : dim(b->hue)), "%s", n->short_name);
    }
}

/* ---- the frame: title, header, strip ---- */
static void title_bar(void)
{
    const uint8_t tb = LAP_ATTR(LAP_BLACK, LAP_CYAN);
    lap_fill(0, 0, W, 1, ' ', tb);
    put(1, 0, 11, tb, "MESHTASTIC");
    /* a light that blinks with each packet heard, steady while it listens */
    const bool blink = last_rx_ms && now_ms - last_rx_ms < 300u;
    put(12, 0, 3, blink ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_GREEN) : tb, "%s", rxh > 0 ? "(*)" : "( )");
    char st[32];
    uint8_t sa;
    if (radio < 0) { snprintf(st, sizeof(st), " NO LORA RADIO "); sa = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED); }
    else if (txh > 0) { snprintf(st, sizeof(st), " SENDING "); sa = LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT); }
    else if (rx_error) { snprintf(st, sizeof(st), " RADIO BUSY "); sa = LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT); }
    else if (rxh > 0) { snprintf(st, sizeof(st), " LISTENING "); sa = LAP_ATTR(LAP_BLACK, LAP_GREEN | LAP_BRIGHT); }
    else { snprintf(st, sizeof(st), " STARTING "); sa = LAP_ATTR(LAP_BLACK, LAP_WHITE); }
    const int sl = (int)strlen(st);
    put(16, 0, W - 17 - sl, tb, "!%08x  %u heard", (unsigned)cfg.node, (unsigned)heard);
    put(W - sl, 0, sl, sa, "%s", st);
    if (status_line[0] && now_ms - status_ms < 5000u) {
        const uint8_t a = LAP_ATTR(LAP_BLACK, LAP_WHITE | LAP_BRIGHT);
        lap_fill(0, 0, W, 1, ' ', a);
        put(1, 0, W - 2, a, "%s", status_line);
    }
}

static void chips_row(int x, int y, int w)
{
    lap_gps_t g;
    const bool gps = lap_gps(&g), fix = gps && g.fix;
    const struct { const char *t; bool on; uint8_t hue; } C[] = {
        { "PRIMARY", primary, LAP_CYAN },
        { "BKGND", lap_service_running(), LAP_BLUE },
        { fix ? "GPS" : cfg.fixed ? "FIXED" : gps ? "GPS..." : "NO GPS", fix || cfg.fixed, LAP_GREEN },
        { "PKI", cfg.pki && pki_ready, LAP_YELLOW },
        { "RELAY", cfg.relay != 0, LAP_MAGENTA },
        { "ES>EN", ui.xl_auto != 0, LAP_CYAN },
    };
    int cx = x;
    for (int i = 0; i < (int)(sizeof(C) / sizeof(C[0])); i++) {
        const int need = (int)strlen(C[i].t) + 2;
        if (cx + need > x + w) break;
        if (y >= clip0 && y < clip1) cx = lap_ui_chip(cx, y, C[i].t, C[i].on, C[i].hue) + 1;
    }
}

/* 906.875 in 3x5 block figures, MHz beside; the last figure flickers
   while a packet comes in. */
static void big_freq(int x, int y, int w)
{
    char f[16];
    snprintf(f, sizeof(f), "%u.%03u", (unsigned)(freq_hz / 1000000u), (unsigned)(freq_hz / 1000u % 1000u));
    const uint8_t on = A(LAP_YELLOW | LAP_BRIGHT);
    const bool hot = last_rx_ms && now_ms - last_rx_ms < 400u && !calm();
    int cx = x;
    for (const char *c = f; *c && cx + 3 <= x + w; c++) {
        if (*c == '.') { cell(cx, y + 4, LAP_BLOCK, on); cx += 2; continue; }
        lap_big_char(cx, y, *c, hot && !c[1] ? A(LAP_WHITE | LAP_BRIGHT) : on);
        cx += 4;
    }
    if (cx + 3 <= x + w) put(cx, y + 4, 3, A(LAP_CYAN | LAP_BRIGHT), "MHz");
}

static void header(void)
{
    const int x = hx, y = hy;
    /* the radar: portrait beside the figures, landscape under them */
    int rx, ry, rw, rh, ix, iy, iw;
    if (!wide) { rw = 17; rh = hh; rx = x; ry = y; ix = x + rw + 1; iy = y; iw = hw - rw - 1; }
    else { ix = x; iy = y; iw = hw; rx = x + 1; ry = y + 10; rw = hw - 2; rh = hh - 10; }
    if (rh > 2) mesh_web(rx, ry, rw, rh, false, false);
    /* who this is, and whether it may transmit */
    const int cw = lap_ui_chip(ix, iy, cfg.short_name, true, LAP_CYAN) - ix;
    put(ix + cw + 1, iy, iw - cw - 12, LAP_UI_TEXT, "%s", cfg.long_name);
    lap_ui_chip(ix + iw - (cfg.tx_on ? 10 : 9), iy, cfg.tx_on ? "TX ARMED" : "RX ONLY", cfg.tx_on, cfg.tx_on ? LAP_RED : LAP_GREEN);
    /* the frequency, big: block figures, as the board's glass has them */
    big_freq(ix, iy + 1, iw);
    const int fy = iy + 6;
    char k[48];
    key_text(0, k, sizeof(k));
    const int more = ch_count() - 1;
    put(ix, fy, iw, A(LAP_UI_VALUE), "%s %s  %s%s  slot %d", MT_REGIONS[cfg.region].name, preset_name(), channel_name(),
        more ? (more == 1 ? " +1" : more == 2 ? " +2" : " +3") : "", slot);
    /* the channel: how busy, and this node's share */
    const float cu = ch_util(), au = air_util();
    put(ix, fy + 1, 3, LAP_UI_DIM, "CH");
    const int bw = iw - 15 > 4 ? iw - 15 : 4;
    lap_ui_bargraph(ix + 3, fy + 1, bw, 1, cu / 40.0f > 1 ? 1 : cu / 40.0f, -1);
    put(ix + 4 + bw, fy + 1, 11, A(cu > 25 ? LAP_UI_WARN : LAP_UI_LIVE), "%3.0f%% tx%.1f", (double)cu, (double)au);
    /* signal of the last packets */
    put(ix, fy + 2, 3, LAP_UI_DIM, "SIG");
    if (hist_n > 1) {
        float v[HISTORY];
        for (int i = 0; i < hist_n; i++) v[i] = (float)rssi_hist[i] / 10.0f;
        const int sw2 = iw - 9 > HISTORY / 2 ? HISTORY / 2 : iw - 9;
        lap_ui_history(ix + 4, fy + 2, sw2, 1, v, hist_n, -130, -50, LAP_GREEN | LAP_BRIGHT);
        put(ix + iw - 4, fy + 2, 4, A(LAP_UI_LIVE), "%4d", rssi_hist[hist_n - 1] / 10);
    } else {
        static const char *const dots[] = { ".   ", "..  ", "... ", "...." };
        put(ix + 4, fy + 2, iw - 4, LAP_UI_FAINT, "waiting for a packet%s", calm() ? "" : dots[(now_ms / 400u) % 4u]);
    }
    chips_row(ix, fy + 3, iw);
}

/* Three keys, < name >, drawn in the look of the board's key bar (see
   lap_art.h: one lap_ui_keys a frame). */
static void strip(int x, int y, int w, int h, const char *label, const char *value, bool badge)
{
    const uint8_t fr = A(LAP_CYAN | LAP_BRIGHT), fill = A(LAP_CYAN), face = A(LAP_WHITE | LAP_BRIGHT), val = A(LAP_YELLOW | LAP_BRIGHT);
    if (h < 3 || w < 12) return;
    for (int i = 0; i < 3; i++) {
        const int x0 = x + i * w / 3, x1 = x + (i + 1) * w / 3, bx = x0 + (i ? 1 : 0), bwid = x1 - x0 - (i ? 1 : 0) - (i < 2 ? 1 : 0);
        lap_box(bx, y, bwid, h, NULL, fr);
        lap_fill(bx + 1, y + 1, bwid - 2, h - 2, LAP_SHADE_25, fill);
        char t[40];
        if (i != 1) {
            snprintf(t, sizeof(t), " %s ", i ? ">" : "<");
            lap_put_str(bx + (bwid - 3) / 2, y + h / 2, t, face);
            continue;
        }
        const int ly = y + (h - (value ? 2 : 1)) / 2;
        int n = snprintf(t, sizeof(t), " %.*s ", bwid - 4 > 0 ? bwid - 4 : 0, label);
        lap_put_str(bx + (bwid - n) / 2, ly, t, face);
        if (value) {
            n = snprintf(t, sizeof(t), " %.*s ", bwid - 4 > 0 ? bwid - 4 : 0, value);
            lap_put_str(bx + (bwid - n) / 2, ly + 1, t, val);
        }
        if (badge && ((now_ms / 400u) % 2u || calm())) lap_put_cell(bx + bwid - 2, y + 1, '*', A(LAP_RED | LAP_BRIGHT));
    }
}

/* ---- CHAT ---- */
static const char *state_word(const msg_t *m)
{
    switch (m->state) {
    case ST_QUEUED: return "sending";
    case ST_SENT: return m->to == MT_BROADCAST ? "sent" : "waiting for ack";
    case ST_RELAYED: return "sent, heard repeated";
    case ST_DELIVERED: return "delivered";
    case ST_FAILED: return m->to == MT_BROADCAST ? "not sent" : "no ack";
    default: return "";
    }
}
/* 0 as sent, 1 both, 2 English only */
static int msg_mode(const msg_t *m)
{
    if (!m->tr[0]) return 0;
    if (m->view) return m->view - 1;
    return ui.xl_show == 0 ? 1 : ui.xl_show == 1 ? 2 : 0;
}
#define WRAP_MAX 24
static int bubble_rows(const msg_t *m, int tw, span_t *a, int *na, span_t *b, int *nb)
{
    const int mode = msg_mode(m);
    *na = mode == 2 ? 0 : wrap_spans(m->text, tw, a, WRAP_MAX);
    *nb = mode ? wrap_spans(m->tr, tw, b, WRAP_MAX) : 0;
    if (!*na && !*nb) *na = wrap_spans(m->text, tw, a, WRAP_MAX);
    return 2 + (*na ? *na : 1) + (mode == 1 ? 1 + *nb : *nb);
}
static void frame_row(int x, int y, int w, char l, char r, uint8_t at)
{
    cell(x, y, l, at);
    hfill(x + 1, y, w - 2, '-', at);
    cell(x + w - 1, y, r, at);
}
/* One message as a bubble at row y (may be partly outside the clip). */
static void bubble(const msg_t *m, int idx, int x, int y, int bw)
{
    span_t a[WRAP_MAX], b[WRAP_MAX];
    int na, nb;
    const int tw = bw - 4;
    const int rows = bubble_rows(m, tw, a, &na, b, &nb);
    const bool dm = m->to != MT_BROADCAST;
    const uint32_t age = now_ms - m->at_ms;
    const bool fresh = !m->mine && age < 1500u && !calm();
    uint8_t hue = m->mine ? (LAP_CYAN | LAP_BRIGHT) : hue_of(m->from);
    if (dm) hue = LAP_MAGENTA | LAP_BRIGHT;
    /* what this board sent sits on a filled background: blue, magenta for a DM */
    const uint8_t bg = m->mine ? (dm ? LAP_MAGENTA : LAP_BLUE) : LAP_BLACK;
#define AB(fg) LAP_ATTR((fg), bg)
    if (m->mine)
        for (int r = 0; r < rows; r++) hfill(x, y + r, bw, ' ', AB(LAP_WHITE));
    const uint8_t fr = fresh ? AB(LAP_WHITE | LAP_BRIGHT) : m->mine ? AB(LAP_CYAN | LAP_BRIGHT) : A(dim(hue));
    hit(x, y, x + bw, y + rows, HIT_MSG, idx);
    /* top: the sender, its long name, what kind, how long ago */
    frame_row(x, y, bw, '.', '.', fr);
    char t[12];
    ago(m->at_ms, t, sizeof(t));
    const char *who_s = who(m->mine && dm ? m->to : m->from);
    int cx = x + 2;
    if (y >= clip0 && y < clip1) {
        if (m->mine) {
            const int tl = (int)strlen(t);
            put(x + 2, y, tl + 2, AB(LAP_WHITE), " %s ", t);
            char lab[24];
            snprintf(lab, sizeof(lab), "%s%s", dm ? "DM " : "", dm ? who_s : cfg.short_name);
            const int ll = (int)strlen(lab) + 2;
            lap_ui_chip(x + bw - 2 - ll, y, lab, true, dm ? LAP_MAGENTA : LAP_CYAN);
        } else {
            cx = lap_ui_chip(cx, y, who_s, true, dim(hue)) + 1;
            const node_t *n = node_get(m->from, false);
            if (dm) cx = lap_ui_chip(cx, y, m->pki ? "DM PRIVATE" : "DM", true, LAP_MAGENTA) + 1;
            else if (m->ch) cx = lap_ui_chip(cx, y, ch_name(m->ch), true, LAP_YELLOW) + 1;
            const int tl = (int)strlen(t) + 2;
            if (n && n->long_name[0] && x + bw - 3 - tl - cx > 4) put(cx, y, x + bw - 3 - tl - cx, LAP_UI_DIM, " %s ", n->long_name);
            put(x + bw - 2 - tl, y, tl, LAP_UI_DIM, " %s ", t);
        }
    }
    /* the words, typing in for a moment when new */
    int shown = fresh ? (int)(age / 12u) : -1;
    int yy = y + 1;
    const uint8_t text_at = m->mine ? AB(LAP_WHITE | LAP_BRIGHT) : dm ? A(LAP_MAGENTA | LAP_BRIGHT) : LAP_UI_TEXT;
    for (int i = 0; i < na; i++, yy++) {
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        int s = -1;
        if (shown >= 0) { s = shown; shown -= a[i].n; if (shown < 0) shown = 0; }
        put_span(x + 2, yy, m->text, a[i], s, text_at);
    }
    const int mode = msg_mode(m);
    if (mode == 1) {                                     /* both: a rule, then the English */
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        for (int k = x + 2; k < x + bw - 10; k += 2) cell(k, yy, '-', LAP_UI_FAINT);
        put(x + bw - 9, yy, 7, A(LAP_CYAN), "ES > EN");
        yy++;
    }
    for (int i = 0; i < nb; i++, yy++) {
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        put_span(x + 2, yy, m->tr, b[i], -1, A(LAP_CYAN | LAP_BRIGHT));
    }
    if (mode == 2) put(x + bw - 9, y, 7, A(LAP_CYAN), "ES > EN");
    /* bottom: the signal it came in on, or what became of ours */
    frame_row(x, yy, bw, '\'', '\'', fr);
    if (yy < clip0 || yy >= clip1) return;
    if (m->mine) {
        const char *w = state_word(m);
        static const char spin[] = "|/-\\";
        char st[32];
        if (m->state == ST_QUEUED && !calm()) snprintf(st, sizeof(st), " %c %s ", spin[(now_ms / 120u) % 4u], w);
        else if (m->state == ST_DELIVERED) snprintf(st, sizeof(st), " ** %s ", w);
        else snprintf(st, sizeof(st), " %s ", w);
        const uint8_t sa = m->state == ST_DELIVERED || m->state == ST_RELAYED ? LAP_ATTR(LAP_BLACK, LAP_GREEN | LAP_BRIGHT)
                         : m->state == ST_FAILED ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : AB(LAP_YELLOW | LAP_BRIGHT);
        put(x + bw - 2 - (int)strlen(st), yy, (int)strlen(st), sa, "%s", st);
    } else {
        const int l = sig_level(m->rssi, m->snr);
        sig_bars(x + 2, yy, l);
        put(x + 7, yy, bw - 9, LAP_UI_DIM, " %d dBm  %d.%d dB  %s ", m->rssi / 10, m->snr / 10,
            m->snr < 0 ? -m->snr % 10 : m->snr % 10, m->hops ? (m->hops == 1 ? "1 hop" : m->hops == 2 ? "2 hops" : "3+ hops") : "direct");
    }
}
#undef AB

/* The empty chat: a tower on a hill, sending rings into the sky. */
static void tower(int x, int y, int w, int h)
{
    static const char *const T[] = {
        "     |     ",
        "    /|\\    ",
        "   / | \\   ",
        "  /--+--\\  ",
        "  \\  |  /  ",
        "   \\ | /   ",
        "  --\\|/--  ",
        "  /  |  \\  ",
        " /---+---\\ ",
    };
    const int n = (int)(sizeof(T) / sizeof(T[0]));
    const int tx = x + (w - 11) / 2, top = y + 4;
    if (top + n + 4 > y + h) return;
    for (int i = 0; i < n; i++) put(tx, top + i, 11, A(LAP_WHITE), "%s", T[i]);
    cell(tx + 5, top - 1, 'o', A(rxh > 0 ? (LAP_RED | LAP_BRIGHT) : LAP_RED));
    /* rings: each born at the tip, opening and fading */
    for (int r = 0; r < 3 && !calm(); r++) {
        const int ph = (int)((now_ms / 260u + (uint32_t)(r * 4)) % 12u);
        const int dx = 1 + ph, dy = ph / 3;
        const uint8_t at = ph < 4 ? A(LAP_GREEN | LAP_BRIGHT) : ph < 8 ? A(LAP_GREEN) : LAP_UI_FAINT;
        if (tx + 5 - dx - 1 >= x) { cell(tx + 5 - dx, top - 1 - dy, '(', at); cell(tx + 5 + dx, top - 1 - dy, ')', at); }
    }
    /* the hill */
    for (int c = x + 1; c < x + w - 1; c++) {
        const int d = c - (tx + 5) < 0 ? (tx + 5) - c : c - (tx + 5);
        const int hy2 = top + n + (d > 14 ? 2 : d > 7 ? 1 : 0);
        cell(c, hy2, (c + (int)(now_ms / 700u)) % 7 == 0 && !calm() ? '~' : '_', A(LAP_GREEN));
    }
    int ty2 = top + n + 3;
    char f[24];
    snprintf(f, sizeof(f), "%u.%03u MHz", (unsigned)(freq_hz / 1000000u), (unsigned)(freq_hz / 1000u % 1000u));
    put(x + (w - 30) / 2, ty2++, w, LAP_UI_TEXT, "Listening on %s %s", f, channel_name());
    ty2++;
    put(x + 2, ty2++, w - 4, LAP_UI_DIM, cfg.tx_on ? "SEND talks to everyone on the channel." : "Turn TRANSMIT on (OPTIONS) to talk.");
    put(x + 2, ty2++, w - 4, LAP_UI_DIM, "QUICK sends a ready phrase, in Spanish.");
    put(x + 2, ty2++, w - 4, LAP_UI_DIM, "Spanish that comes in shows in English too.");
}

static void draw_chat(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    if (!msg_n) { smax = 0; tower(x, y, w, h); return; }
    const int bw = w - 4 < 24 ? w : w - 4;
    span_t a[WRAP_MAX], b[WRAP_MAX];
    int na, nb, total = 0;
    for (int k = msg_n - 1; k >= 0; k--) total += bubble_rows(&msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS], bw - 4, a, &na, b, &nb);
    const bool at_end = scroll >= smax;
    smax = total > h ? total - h : 0;
    if ((chat_new || at_end) && !dragging) scroll = smax;
    chat_new = false;
    if (scroll > smax) scroll = smax;
    int yy = y - scroll;
    if (total < h) yy = y + h - total;                   /* few: they sit at the bottom */
    for (int k = msg_n - 1; k >= 0; k--) {
        const int i = (msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS;
        const msg_t *m = &msgs[i];
        const int rows = bubble_rows(m, bw - 4, a, &na, b, &nb);
        if (yy + rows > y && yy < y + h) bubble(m, i, m->mine ? x + w - bw : x, yy, bw);
        yy += rows;
    }
}

/* ---- NODES ---- */
static int node_order[MAX_NODES];
static float node_km(const node_t *n, float *brg)
{
    if (!me_ok || !n->has_pos) return -1;
    return km_bearing(me_lat, me_lon, n->lat, n->lon, brg);
}
static bool node_before(const node_t *a, const node_t *b)
{
    switch (ui.sort) {
    case 1: if (!a->last_ms != !b->last_ms) return a->last_ms != 0; return a->rssi > b->rssi;
    case 2: {
        const float da = node_km(a, NULL), db = node_km(b, NULL);
        if ((da < 0) != (db < 0)) return da >= 0;
        return da < db;
    }
    case 3: return strcmp(a->long_name[0] ? a->long_name : a->short_name, b->long_name[0] ? b->long_name : b->short_name) < 0;
    default: return a->last_ms > b->last_ms;
    }
}
static void sort_nodes(void)
{
    for (int i = 0; i < node_n; i++) node_order[i] = i;
    for (int i = 1; i < node_n; i++)
        for (int j = i; j > 0 && node_before(&nodes[node_order[j]], &nodes[node_order[j - 1]]); j--) {
            const int t = node_order[j]; node_order[j] = node_order[j - 1]; node_order[j - 1] = t;
        }
}
static const char *const SORT_NAMES[] = { "HEARD", "SIGNAL", "NEAREST", "NAME" };
#define CARD 4

static void card(const node_t *n, int idx, int x, int y, int w, bool on)
{
    hit(x, y, x + w, y + CARD - 1, HIT_NODE, idx);
    const uint8_t hue = hue_of(n->num);
    const bool fresh = now_ms - n->last_ms < 1500u && !calm();
    if (on) for (int r = 0; r < CARD - 1; r++) cell(x, y + r, '>', A(LAP_YELLOW | LAP_BRIGHT));
    char t[12];
    ago(n->last_ms, t, sizeof(t));
    if (y >= clip0 && y < clip1) {
        const int cx = lap_ui_chip(x + 1, y, n->short_name, true, fresh && (now_ms / 200u) % 2u ? LAP_WHITE : dim(hue)) + 1;
        put(cx, y, w - (cx - x) - 6, A(hue), "%s", n->long_name[0] ? n->long_name : "(no name yet)");
        put(x + w - 5, y, 5, fresh ? A(LAP_UI_LIVE) : LAP_UI_DIM, "%5s", t);
    }
    /* the link (a node from the saved list has none until it is heard) */
    int cx;
    if (n->last_ms) {
        const int l = sig_level(n->rssi, n->snr);
        sig_bars(x + 2, y + 1, l);
        cx = x + 8 + put(x + 7, y + 1, 18, A(level_hue(l)), "%d dBm %d.%d dB", n->rssi / 10, n->snr / 10, n->snr < 0 ? -n->snr % 10 : n->snr % 10);
    } else cx = x + 3 + put(x + 2, y + 1, 22, LAP_UI_FAINT, "not heard since start");
    /* hops as a chain: me o-o-o node */
    if (cx + 9 < x + w - 15) {
        const int hops = n->hops > 3 ? 3 : n->hops;
        if (!hops) put(cx, y + 1, 8, A(LAP_GREEN | LAP_BRIGHT), "direct");
        else for (int k = 0; k <= hops; k++) {
            cell(cx + k * 2, y + 1, 'o', A(k == hops ? hue : LAP_WHITE));
            if (k < hops) cell(cx + k * 2 + 1, y + 1, '-', LAP_UI_FAINT);
        }
    }
    battery(x + w - 14, y + 1, 6, n);
    /* where, and what it is */
    float brg = 0;
    const float km = node_km(n, &brg);
    cx = x + 2;
    if (km >= 0) {
        char d[16];
        dist_text(km, d, sizeof(d));
        cell(cx, y + 2, arrow(brg), A(LAP_YELLOW | LAP_BRIGHT));
        cx += 2 + put(cx + 2, y + 2, 16, A(LAP_UI_VALUE), "%s %s", d, compass(brg));
    } else if (n->has_pos) cx += put(cx, y + 2, 24, LAP_UI_DIM, "has position, no fix here");
    else cx += put(cx, y + 2, 16, LAP_UI_FAINT, "no position");
    cx += 2;
    if (n->has_key) cx += 1 + put(cx, y + 2, 4, A(LAP_YELLOW), "KEY");
    if (n->last[0]) put(cx, y + 2, x + w - cx - 1, LAP_UI_FAINT, "last %s", n->last);
    for (int k = x + 2; k < x + w - 1; k += 3) cell(k, y + 3, '.', LAP_UI_FAINT);
}

static int find_node(uint32_t num)
{
    for (int i = 0; i < node_n; i++) if (nodes[i].num == num) return i;
    return -1;
}

/* A compass rose with a needle that swings to the bearing. */
static void compass_rose(int x, int y, int w, int h, float brg, bool have)
{
    static float shown = 0;
    float d = brg - shown;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    shown = calm() ? brg : shown + d * 0.15f;
    cv_begin(x, y, w, h);
    const float cx = (float)cv.w - 0.5f, cy = (float)cv.h * 2.0f - 0.5f;
    float R = cx - 1.0f;
    if ((cy - 1.0f) * cv.ax < R) R = (cy - 1.0f) * cv.ax;
    cv_ring(cx, cy, R, 0, A(LAP_CYAN));
    for (int k = 0; k < 8; k++) {
        const float t = (float)k * 0.785398f;
        cv_line(rnd(cx + (R - 2) * cosf(t)), rnd(cy + (R - 2) * sinf(t) / cv.ax), rnd(cx + R * cosf(t)), rnd(cy + R * sinf(t) / cv.ax), LAP_UI_FAINT);
    }
    if (have) {
        const float t = (shown - 90.0f) * 3.14159265f / 180.0f;
        cv_line(rnd(cx), rnd(cy), rnd(cx + (R - 1) * cosf(t)), rnd(cy + (R - 1) * sinf(t) / cv.ax), A(LAP_YELLOW | LAP_BRIGHT));
        cv_line(rnd(cx), rnd(cy), rnd(cx - (R * 0.4f) * cosf(t)), rnd(cy - (R * 0.4f) * sinf(t) / cv.ax), A(LAP_RED));
    }
    cv_dot(rnd(cx), rnd(cy), A(LAP_WHITE | LAP_BRIGHT));
    cv_flush();
    cell(x + w / 2, y, 'N', A(LAP_WHITE | LAP_BRIGHT));
}

static void draw_node_page(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    const int i = find_node(open_num);
    if (i < 0) { node_open = false; return; }
    const node_t *n = &nodes[i];
    const uint8_t hue = hue_of(n->num);
    int yy = y - scroll;
    /* the name, big */
    int bx = x + 1;
    for (const char *c = n->short_name; *c && bx + 3 < x + w; c++, bx += 4) {
        const char ch = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
        if (yy >= clip0 && yy + 5 <= clip1) lap_big_char(bx, yy, ch, A(hue));
    }
    const int tx = bx + 1, tw = x + w - tx - 1;
    char t[12];
    ago(n->last_ms, t, sizeof(t));
    if (tw > 12) {
        put(tx, yy, tw, A(hue), "%s", n->long_name[0] ? n->long_name : "(no name yet)");
        put(tx, yy + 1, tw, LAP_UI_DIM, "!%08x", (unsigned)n->num);
        if (n->last_ms) put(tx, yy + 2, tw, LAP_UI_DIM, "heard %s ago", t);
        else put(tx, yy + 2, tw, LAP_UI_FAINT, "not heard since start");
        put(tx, yy + 3, tw, LAP_UI_DIM, "%u packets  ch %s", (unsigned)n->packets, ch_name(n->ch));
        put(tx, yy + 4, tw, n->has_key ? A(LAP_YELLOW) : LAP_UI_FAINT, n->has_key ? "private DMs: key known" : "no DM key yet");
    }
    yy += 6;
    /* SIGNAL */
    bezel(x, yy, w, 6, " SIGNAL ");
    const int l = sig_level(n->rssi, n->snr);
    const float frac = ((float)n->rssi / 10.0f + 130.0f) / 90.0f;
    if (yy + 1 >= clip0 && yy + 3 <= clip1) lap_ui_bargraph(x + 2, yy + 1, w - 4, 2, frac < 0 ? 0 : frac > 1 ? 1 : frac, -1);
    sig_bars(x + 2, yy + 3, l);
    put(x + 7, yy + 3, w - 9, A(level_hue(l)), "%d dBm   SNR %d.%d dB   %s", n->rssi / 10, n->snr / 10, n->snr < 0 ? -n->snr % 10 : n->snr % 10,
        n->hops ? (n->hops == 1 ? "1 hop" : n->hops == 2 ? "2 hops" : "3+ hops") : "direct");
    if (n->sig_n > 1 && yy + 4 >= clip0 && yy + 4 < clip1) {
        float v[SIG_N];
        for (int k = 0; k < n->sig_n; k++) v[k] = (float)n->sig[k];
        lap_ui_history(x + 2, yy + 4, w - 4 < SIG_N / 2 ? w - 4 : SIG_N / 2, 1, v, n->sig_n, -130, -50, hue);
        put(x + 3 + SIG_N / 2, yy + 4, w - 6 - SIG_N / 2, LAP_UI_FAINT, "last %u packets", (unsigned)n->sig_n);
    } else put(x + 2, yy + 4, w - 4, LAP_UI_FAINT, "history fills as it is heard");
    yy += 7;
    /* WHERE and POWER: side by side when there is room */
    const bool side = w >= 60;
    const int ww = side ? w / 2 - 1 : w, wh = 9;
    bezel(x, yy, ww, wh, " WHERE ");
    float brg = 0;
    const float km = node_km(n, &brg);
    compass_rose(x + 2, yy + 1, 13, 7, brg, km >= 0);
    const int qx = x + 17, qw = ww - 19;
    if (km >= 0) {
        char d[16];
        dist_text(km, d, sizeof(d));
        put(qx, yy + 2, qw, A(LAP_UI_VALUE), "%s", d);
        put(qx, yy + 3, qw, A(LAP_UI_VALUE), "%s  %.0f deg", compass(brg), (double)brg);
    } else put(qx, yy + 2, qw, LAP_UI_FAINT, n->has_pos ? "no fix here" : "no position");
    if (n->has_pos) {
        put(qx, yy + 5, qw, LAP_UI_DIM, "%.5f", n->lat);
        put(qx, yy + 6, qw, LAP_UI_DIM, "%.5f", n->lon);
        put(qx, yy + 7, qw, LAP_UI_FAINT, "on the MAP");
    } else put(qx, yy + 5, qw, LAP_UI_FAINT, "shared ones go on MAP");
    const int ox = side ? x + ww + 2 : x, oy = side ? yy : yy + wh + 1, ow = side ? w - ww - 2 : w;
    bezel(ox, oy, ow, wh, " POWER ");
    if (n->has_dev) {
        const float bf = n->battery > 100 ? 1.0f : (float)n->battery / 100.0f;
        if (oy + 1 >= clip0 && oy + 3 <= clip1) lap_ui_bargraph(ox + 2, oy + 1, ow - 4, 2, bf, -1);
        put(ox + 2, oy + 3, ow - 4, A(n->battery > 100 ? (LAP_CYAN | LAP_BRIGHT) : n->battery > 20 ? LAP_UI_LIVE : LAP_UI_ALARM),
            n->battery > 100 ? "on power" : "battery %u%%", (unsigned)n->battery);
        put(ox + 2, oy + 4, ow - 4, LAP_UI_DIM, "%.2f V", (double)n->voltage);
        put(ox + 2, oy + 5, ow - 4, LAP_UI_DIM, "channel %.1f%%", (double)n->ch_util);
        put(ox + 2, oy + 6, ow - 4, LAP_UI_DIM, "its air %.1f%%", (double)n->air_util);
    } else {
        put(ox + 2, oy + 2, ow - 4, LAP_UI_FAINT, "no telemetry yet");
        put(ox + 2, oy + 4, ow - 4, LAP_UI_FAINT, "nodes send it hourly");
    }
    yy = (side ? yy : oy) + wh + 1;
    /* ROUTE */
    bezel(x, yy, w, 6, " ROUTE ");
    if (trace_to == n->num && (trace_fwd[0] || trace_id)) {
        if (trace_id && !trace_fwd[0]) {
            static const char *const S[] = { ">   ", ">>  ", ">>> ", ">>>>" };
            put(x + 2, yy + 2, w - 4, A(LAP_CYAN | LAP_BRIGHT), "tracing %s", calm() ? "..." : S[(now_ms / 250u) % 4u]);
        } else {
            span_t sp[4];
            int r = 1, k = wrap_spans(trace_fwd, w - 4, sp, 2);
            for (int q = 0; q < k; q++) put_span(x + 2, yy + r++, trace_fwd, sp[q], -1, A(LAP_CYAN | LAP_BRIGHT));
            if (trace_back[0]) {
                k = wrap_spans(trace_back, w - 4, sp, 2);
                for (int q = 0; q < k; q++) put_span(x + 2, yy + r++, trace_back, sp[q], -1, A(LAP_CYAN));
            }
        }
    } else {
        put(x + 2, yy + 2, w - 4, LAP_UI_FAINT, "TRACE: the path there and back,");
        put(x + 2, yy + 3, w - 4, LAP_UI_FAINT, "every hop with its SNR");
    }
    yy += 6;
    const int total = yy + scroll - y;
    smax = total > h ? total - h : 0;
}

static void draw_nodes(int x, int y, int w, int h)
{
    if (node_open) { draw_node_page(x, y, w, h); return; }
    clip0 = y; clip1 = y + h;
    if (!node_n) {
        smax = 0;
        mesh_web(x + (w - 30) / 2, y + 2, 30, 12, false, true);
        put(x + (w - 19) / 2, y + 15, w, LAP_UI_TEXT, "No nodes heard yet.");
        put(x + 2, y + 17, w - 4, LAP_UI_DIM, "A node shows up the first time it is heard.");
        put(x + 2, y + 18, w - 4, LAP_UI_DIM, "Most send node info every few hours; HELLO");
        put(x + 2, y + 19, w - 4, LAP_UI_DIM, "asks everyone in range to answer.");
        return;
    }
    sort_nodes();
    if (sel >= node_n) sel = node_n - 1;
    if (sel < 0) sel = 0;
    const int total = node_n * CARD;
    smax = total > h ? total - h : 0;
    if (scroll > smax) scroll = smax;
    for (int r = 0; r < node_n; r++) {
        const int yy = y - scroll + r * CARD;
        if (yy + CARD <= y || yy >= y + h) continue;
        card(&nodes[node_order[r]], node_order[r], x, yy, w, r == sel && kbd_used);
    }
}

/* ---- RADAR ---- */
static void draw_radar(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    smax = 0;
    const int rh = h - 3;
    int rw = w;
    if (rw > CV_W) rw = CV_W;
    mesh_web(x + (w - rw) / 2, y, rw, rh, ui.labels != 0, true);
    int placed = 0, hopped = 0;
    for (int i = 0; i < blip_n; i++) { if (blips[i].placed) placed++; else hopped++; }
    char sc[16];
    dist_text(radar_km, sc, sizeof(sc));
    if (me_ok) {
        char ring[16];
        dist_text(radar_km * 8.0f / (float)(rw * 2 > 8 ? rw : 8), ring, sizeof(ring));
        put(x + 1, y + h - 3, w - 2, A(LAP_UI_VALUE), "edge %s  grid dot every %s  %s", sc, ring, ui.zoom ? "(ZOOM)" : "(FIT)");
        put(x + 1, y + h - 2, w - 2, LAP_UI_DIM, "%d on the map, %d placed by hops", placed, hopped);
    } else {
        put(x + 1, y + h - 3, w - 2, A(LAP_UI_WARN), "No position here: placed by hops");
        put(x + 1, y + h - 2, w - 2, LAP_UI_DIM, "Needs GPS or OPTIONS > POSITION > FIXED");
    }
    put(x + 1, y + h - 1, w - 2, LAP_UI_FAINT, "Dashed = relayed. Tap a node to open it.");
}

/* ---- AIR ---- */
static uint8_t air_hue(const air_t *a)
{
    if (!strcmp(a->what, "MY ECHO")) return LAP_CYAN | LAP_BRIGHT;
    if (!strcmp(a->what, "REPEAT")) return LAP_BLUE | LAP_BRIGHT;
    if (!strcmp(a->what, "OTHER CH") || !strcmp(a->what, "PRIVATE") || !strncmp(a->what, "PKI ", 4)) return LAP_YELLOW | LAP_BRIGHT;
    return LAP_GREEN | LAP_BRIGHT;
}
static const char *air_glyph(const air_t *a)
{
    static const struct { const char *what, *g; } G[] = {
        { "TEXT", "[T]" }, { "POSITION", "(+)" }, { "NODEINFO", "[i]" }, { "TELEMETRY", "[%]" }, { "ROUTING", "[a]" },
        { "TRACEROUTE", ">>>" }, { "NEIGHBORINFO", "[n]" }, { "PKI DM", "[#]" }, { "PRIVATE", "[?]" }, { "OTHER CH", "[?]" },
        { "REPEAT", "[r]" }, { "MY ECHO", "[e]" },
    };
    for (int i = 0; i < (int)(sizeof(G) / sizeof(G[0])); i++) if (!strncmp(a->what, G[i].what, strlen(G[i].what))) return G[i].g;
    return "[ ]";
}
/* The last minute of the air as LoRa chirps: each packet a run of rising
   sweeps as long as it was on the air, newest at the right. */
static void chirps(int x, int y, int w, int h)
{
    cv_begin(x, y, w, h);
    const int dw = cv.w * 2, dh = cv.h * 4;
    const float ms_per_dot = 60000.0f / (float)dw;
    for (int k = 0; k < dw; k += 4) cv_dot(k, dh - 1, LAP_UI_FAINT);
    for (int r = 0; r < air_n; r++) {
        const air_t *a = &air[(air_head - 1 - r + MAX_AIR * 2) % MAX_AIR];
        const uint32_t age = now_ms - a->at_ms;
        if (age > 60000u) break;
        const uint32_t on = airtime_ms(a->len);
        int span = (int)((float)on / ms_per_dot);
        if (span < 6) span = 6;                          /* a packet stays visible however short */
        const int x1 = dw - 1 - (int)((float)age / ms_per_dot), x0 = x1 - span;
        /* rising sweeps, each falling back at the end: LoRa's sawtooth */
        const int sym = 4, top2 = 1, bot = dh - 2;
        const uint8_t at = A(age < 1500u && !calm() ? (LAP_WHITE | LAP_BRIGHT) : air_hue(a));
        int lx = x0, ly = bot;
        for (int px2 = x0; px2 <= x1; px2++) {
            const int ph2 = (px2 - x0) % sym;
            const int yy = bot - ph2 * (bot - top2) / (sym - 1);
            if (ph2 == 0 && px2 != x0) cv_line(lx, ly, px2, top2, at);   /* the wrap */
            cv_line(ph2 == 0 ? px2 : lx, ph2 == 0 ? bot : ly, px2, yy, at);
            lx = px2; ly = yy;
        }
    }
    cv_flush();
}

static void draw_air(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    int yy = y - scroll;
    /* packets a minute, the last half hour */
    int lv[PER_MIN_N], top = 4, now = 0;
    const uint32_t e = now_ms / 60000u;
    for (int i = 0; i < PER_MIN_N; i++) {
        const int back = PER_MIN_N - 1 - i;              /* minutes ago */
        const bool kept = (uint32_t)back <= e && e - (uint32_t)back <= per_min_epoch && per_min_epoch - (e - (uint32_t)back) < PER_MIN_N;
        lv[i] = kept ? per_min[(e - (uint32_t)back) % PER_MIN_N] : 0;
        if (lv[i] > top) top = lv[i];
    }
    now = lv[PER_MIN_N - 1];
    put(x + 1, yy, 10, LAP_UI_DIM, "PER MINUTE");
    if (yy >= clip0 && yy + 2 <= clip1) lap_ui_sparkline(x + 12, yy, w - 24 < PER_MIN_N ? w - 24 : PER_MIN_N, 2, lv + (w - 24 < PER_MIN_N ? PER_MIN_N - (w - 24) : 0), w - 24 < PER_MIN_N ? w - 24 : PER_MIN_N, top, -1);
    put(x + w - 11, yy, 10, A(LAP_UI_LIVE), "%4d now", now);
    put(x + w - 11, yy + 1, 10, LAP_UI_FAINT, "30 min");
    yy += 3;
    put(x + 1, yy, 10, LAP_UI_DIM, "LAST MIN");
    put(x + 1, yy + 1, 10, LAP_UI_FAINT, "as chirps");
    if (yy >= clip0 && yy + 4 <= clip1) chirps(x + 12, yy, w - 13, 4);
    yy += 5;
    const float cu = ch_util();
    put(x + 1, yy, 10, LAP_UI_DIM, "CHANNEL");
    if (yy >= clip0 && yy < clip1) lap_ui_bargraph(x + 12, yy, w - 24, 1, cu / 40.0f > 1 ? 1 : cu / 40.0f, -1);
    put(x + w - 11, yy, 10, A(cu > 25 ? LAP_UI_WARN : LAP_UI_LIVE), "%4.1f%%", (double)cu);
    yy++;
    put(x + 1, yy, 10, LAP_UI_DIM, "MY AIR");
    if (yy >= clip0 && yy < clip1) lap_ui_meter(x + 12, yy, w - 24, air_util() / 10.0f > 1 ? 1 : air_util() / 10.0f, LAP_CYAN | LAP_BRIGHT);
    put(x + w - 11, yy, 10, A(LAP_CYAN | LAP_BRIGHT), "%4.1f%%/h", (double)air_util());
    yy += 2;
    /* totals as chips */
    if (yy >= clip0 && yy < clip1) {
        char t[24];
        int cx = x + 1;
        const struct { const char *l; uint32_t v; uint8_t hue; } C[] = {
            { "HEARD", heard, LAP_GREEN }, { "READ", decoded, LAP_GREEN }, { "OTHER", foreign, LAP_YELLOW },
            { "SENT", sent, LAP_CYAN }, { "RELAYED", relayed, LAP_MAGENTA }, { "PKI", pki_in, LAP_YELLOW }, { "LOST", gaps, LAP_RED },
        };
        for (int i = 0; i < (int)(sizeof(C) / sizeof(C[0])); i++) {
            snprintf(t, sizeof(t), "%s %u", C[i].l, (unsigned)C[i].v);
            if (cx + (int)strlen(t) + 2 > x + w) break;
            cx = lap_ui_chip(cx, yy, t, C[i].v > 0, C[i].hue) + 1;
        }
    }
    yy += 2;
    put(x + 1, yy, w - 2, LAP_UI_FAINT, "    FROM   TO     WHAT        dBm   AGO");
    yy++;
    if (!air_n) {
        put(x + 2, yy + 1, w - 4, LAP_UI_DIM, "Nothing on the air yet.");
        put(x + 2, yy + 2, w - 4, LAP_UI_FAINT, "Every packet shows here, even unreadable ones.");
    }
    for (int r = 0; r < air_n; r++, yy++) {
        const air_t *a = &air[(air_head - 1 - r + MAX_AIR * 2) % MAX_AIR];
        if (yy < clip0 || yy >= clip1) continue;
        const bool fresh = now_ms - a->at_ms < 1500u && !calm();
        char t[8], to[6];
        ago(a->at_ms, t, sizeof(t));
        if (a->to == MT_BROADCAST) snprintf(to, sizeof(to), "all");
        else snprintf(to, sizeof(to), "%s", who(a->to));
        if (fresh) lap_fill(x, yy, w, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLUE));
        const uint8_t bg = fresh ? LAP_BLUE : LAP_BLACK;
        put(x + 1, yy, 3, LAP_ATTR(air_hue(a), bg), "%s", air_glyph(a));
        put(x + 5, yy, 6, LAP_ATTR(hue_of(a->from), bg), "%s", who(a->from));
        put(x + 12, yy, 6, LAP_ATTR(LAP_WHITE, bg), "%s", to);
        put(x + 19, yy, 11, LAP_ATTR(air_hue(a), bg), "%s", a->what);
        put(x + 31, yy, 5, LAP_ATTR(LAP_WHITE, bg), "%4d", a->rssi / 10);
        put(x + 37, yy, 5, LAP_ATTR(LAP_WHITE, bg), "%4s", t);
        hit(x, yy, x + w, yy + 1, HIT_AIR, r);
    }
    const int total = yy + scroll - y;
    smax = total > h ? total - h : 0;
}

/* ---- TRANSLATE ---- */
static void lang_art(int x, int y, int w)
{
    const int bx = x + (w - 30) / 2;
    static const char *const B[] = { ".------.", "|  ES  |", "'---.--'" };
    static const char *const C[] = { ".------.", "|  EN  |", "'--.---'" };
    for (int i = 0; i < 3; i++) {
        put(bx, y + i, 8, A(LAP_YELLOW | LAP_BRIGHT), "%s", B[i]);
        put(bx + 22, y + i, 8, A(LAP_CYAN | LAP_BRIGHT), "%s", C[i]);
    }
    /* the words travel across */
    const int n = 12, ph = calm() ? -1 : (int)((now_ms / 90u) % (uint32_t)(n + 4));
    for (int k = 0; k < n; k++) {
        const bool lit = ph >= 0 && (k == ph || k == ph - 1 || k == ph - 2);
        cell(bx + 9 + k, y + 1, k == n - 1 ? '>' : '~', lit ? A(LAP_WHITE | LAP_BRIGHT) : A(ui.xl_auto ? LAP_GREEN : LAP_BLACK | LAP_BRIGHT));
    }
}
static void draw_lang(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    int yy = y - scroll;
    lang_art(x, yy, w);
    yy += 4;
    /* the two switches, tappable */
    if (yy >= clip0 && yy < clip1) {
        int cx = x + 2;
        cx = lap_ui_chip(cx, yy, ui.xl_auto ? "AUTO ON" : "AUTO OFF", ui.xl_auto != 0, LAP_GREEN) + 1;
        hit(x + 2, yy, cx, yy + 1, HIT_XL_AUTO, 0);
        const int c0 = cx;
        cx = lap_ui_chip(cx, yy, ui.xl_show == 0 ? "SHOW BOTH" : ui.xl_show == 1 ? "ENGLISH ONLY" : "AS SENT", true, LAP_CYAN) + 1;
        hit(c0, yy, cx, yy + 1, HIT_XL_SHOW, 0);
        put(cx + 1, yy, x + w - cx - 2, LAP_UI_FAINT, "offline, on the board");
    }
    yy += 2;
    /* TRY IT */
    span_t sp[8];
    const char *es = try_es[0] ? try_es : "Tap here and write some Spanish.";
    const int ne = wrap_spans(es, w - 4, sp, 8);
    span_t sq[8];
    const int nt = try_en[0] ? wrap_spans(try_en, w - 4, sq, 8) : 0;
    const int bh2 = 2 + ne + (nt ? nt + 1 : 0);
    bezel(x, yy, w, bh2, " TRY IT ");
    hit(x, yy, x + w, yy + bh2, HIT_TRY, 0);
    int r = yy + 1;
    for (int i = 0; i < ne; i++) put_span(x + 2, r++, es, sp[i], -1, try_es[0] ? LAP_UI_TEXT : LAP_UI_FAINT);
    if (nt) {
        for (int k = x + 2; k < x + w - 2; k += 2) cell(k, r, '-', LAP_UI_FAINT);
        r++;
        for (int i = 0; i < nt; i++) put_span(x + 2, r++, try_en, sq[i], -1, A(LAP_CYAN | LAP_BRIGHT));
    }
    yy += bh2 + 1;
    /* HISTORY: each TRY IT and each Spanish line heard, kept on the card */
    if (yy >= clip0 && yy < clip1) {
        put(x + 1, yy, 8, A(LAP_UI_VALUE), "HISTORY");
        if (hist.n) {
            const bool armed = hist_clear_ms && now_ms - hist_clear_ms < 3000u;
            const char *cl = armed ? "SURE? TAP" : "CLEAR";
            const int c0 = x + w - (int)strlen(cl) - 3;
            put(x + 9, yy, c0 - x - 10, LAP_UI_FAINT, "%u kept on the card", (unsigned)hist.n);
            const int c1 = lap_ui_chip(c0, yy, cl, armed, LAP_RED);
            hit(c0, yy, c1, yy + 1, HIT_HIST_CLEAR, 0);
        } else put(x + 9, yy, w - 10, LAP_UI_FAINT, "kept on the card");
    }
    yy++;
    lap_time_t tnow;
    lap_time(&tnow);
    for (uint32_t k = 0; k < hist.n; k++) {
        const hist_t *e = &hist.e[k];
        const int y0 = yy;
        const bool fresh = k == 0 && hist_new_ms && now_ms - hist_new_ms < 1500u && !calm();
        char t[8] = "";
        if (e->unix && tnow.synced && (uint32_t)tnow.unix >= e->unix) {
            const uint32_t s = (uint32_t)tnow.unix - e->unix;
            if (s < 60) snprintf(t, sizeof(t), "now");
            else if (s < 3600) snprintf(t, sizeof(t), "%um", (unsigned)(s / 60));
            else if (s < 86400) snprintf(t, sizeof(t), "%uh", (unsigned)(s / 3600));
            else snprintf(t, sizeof(t), "%ud", (unsigned)(s / 86400));
        }
        const char *name = e->from ? who(e->from) : "YOU";
        const int cw2 = (int)strlen(name) + 1;
        cell(x + 1, yy, fresh ? '>' : ' ', A(LAP_WHITE | LAP_BRIGHT));
        put(x + 2, yy, cw2, e->from ? A(hue_of(e->from)) : A(LAP_YELLOW | LAP_BRIGHT), "%s", name);
        put(x + 2 + cw2, yy, w - 8 - cw2, fresh ? A(LAP_WHITE | LAP_BRIGHT) : LAP_UI_TEXT, "%s", e->es);
        put(x + w - 5, yy++, 4, LAP_UI_FAINT, "%4s", t);
        put(x + 4, yy++, w - 6, e->en[0] ? A(LAP_CYAN | LAP_BRIGHT) : LAP_UI_FAINT, "%s",
            e->en[0] ? e->en : "(not translated yet: tap)");
        hit(x, y0, x + w, yy, HIT_HIST, (int)k);
    }
    if (!hist.n) {
        put(x + 2, yy++, w - 4, LAP_UI_FAINT, "Nothing yet. Each TRY IT and each Spanish");
        put(x + 2, yy++, w - 4, LAP_UI_FAINT, "line heard on the mesh is kept here.");
    }
    yy++;
    /* the phrasebook */
    put(x + 1, yy, w - 2, A(LAP_UI_VALUE), "PHRASEBOOK");
    put(x + 12, yy++, w - 13, LAP_UI_FAINT, ui.quick_es ? "tap one to send it in Spanish" : "tap one to send it in English");
    const int pn = mt_phrase_count();
    const char *cat = NULL;
    for (int i = 0; i < pn; i++) {
        const mt_phrase_t p = mt_phrase(i);
        if (!p.en[0]) continue;
        if (!cat || strcmp(cat, p.category)) {
            cat = p.category;
            char up[16];
            int k = 0;
            for (; p.category[k] && k < 15; k++) up[k] = (char)(p.category[k] >= 'a' && p.category[k] <= 'z' ? p.category[k] - 32 : p.category[k]);
            up[k] = 0;
            put(x + 2, yy++, w - 4, LAP_UI_FAINT, "- %s -", up);
        }
        /* side by side when both fit, else the Spanish under the English */
        const int half = (w - 4) / 2;
        if (u8_cells(p.en, -1) < half && u8_cells(p.es, -1) <= w - 4 - half) {
            put(x + 2, yy, half - 1, LAP_UI_TEXT, "%s", p.en);
            put(x + 2 + half, yy, w - 4 - half, A(LAP_YELLOW | LAP_BRIGHT), "%s", p.es);
            hit(x, yy, x + w, yy + 1, HIT_PHRASE, i);
            yy++;
        } else {
            put(x + 2, yy, w - 4, LAP_UI_TEXT, "%s", p.en);
            put(x + 4, yy + 1, w - 6, A(LAP_YELLOW | LAP_BRIGHT), "%s", p.es);
            hit(x, yy, x + w, yy + 2, HIT_PHRASE, i);
            yy += 2;
        }
    }
    const int total = yy + scroll - y;
    smax = total > h ? total - h : 0;
}

/* ---- layout and the whole frame ---- */
static void layout(void)
{
    W = lap_cols(); H = lap_rows();
    wide = W >= 90 && W > H * 2;
    kr = lap_ui_keys_rows(4);
    tr = lap_ui_keys_rows(3);
    if (tr > 5) tr = 5;                 /* finger-sized, without eating the chat */
    if (tr < 3) tr = 3;
    if (!wide) {
        hx = 0; hy = 1; hw = W; hh = 10;
        sx = 0; sw = W; ty = hy + hh + 1;
        py = ty + tr; px = 0; pw = W; ph = H - kr - py;
    } else {
        hw = 40; hx = 1; hy = 1; hh = H - kr - 1;
        sx = hw + 2; sw = W - sx; ty = 1;
        py = ty + tr; px = sx; pw = sw; ph = H - kr - py;
    }
}

static void set_keys(void)
{
    const bool can = cfg.tx_on && radio >= 0;
    static char v0[12], v2[12];
    switch (tab) {
    case TAB_CHAT:
        snprintf(v0, sizeof(v0), "%.8s", ch_name(send_ch));
        keys[0] = (lap_btn_t){ "SEND", v0, 's', false, !can, false };
        keys[1] = (lap_btn_t){ "QUICK", ui.quick_es ? "ES" : "EN", 'q', false, !can, false };
        snprintf(v2, sizeof(v2), "%d OF %d", send_ch + 1, ch_count());
        keys[2] = (lap_btn_t){ "CHANNEL", v2, 'c', false, ch_count() < 2, false };
        break;
    case TAB_NODES:
        if (node_open) {
            const int i = find_node(open_num);
            keys[0] = (lap_btn_t){ "DM", i >= 0 ? nodes[i].short_name : NULL, 'd', false, !can, false };
            keys[1] = (lap_btn_t){ "TRACE", NULL, 'r', trace_id && trace_to == open_num, !can, false };
            keys[2] = (lap_btn_t){ "BACK", NULL, 'b', false, false, false };
        } else {
            keys[0] = (lap_btn_t){ "SORT", SORT_NAMES[ui.sort % 4], 'g', false, !node_n, false };
            keys[1] = (lap_btn_t){ "HELLO", NULL, 'n', false, !can, false };
            keys[2] = (lap_btn_t){ "MAP", NULL, 'v', false, false, false };
        }
        break;
    case TAB_RADAR:
        snprintf(v0, sizeof(v0), "%s", ui.zoom ? "" : "FIT");
        if (ui.zoom) dist_text(ZOOM_KM[ui.zoom], v0, sizeof(v0));
        keys[0] = (lap_btn_t){ "ZOOM", v0, 'z', false, false, false };
        keys[1] = (lap_btn_t){ "NAMES", ui.labels ? "ON" : "OFF", 'l', ui.labels != 0, false, false };
        keys[2] = (lap_btn_t){ "NODES", NULL, 'u', false, false, false };
        break;
    case TAB_AIR:
        keys[0] = (lap_btn_t){ "SEND", NULL, 's', false, !can, false };
        keys[1] = (lap_btn_t){ "HELLO", NULL, 'n', false, !can, false };
        keys[2] = (lap_btn_t){ "CLEAR", NULL, 'k', false, !air_n, false };
        break;
    default:
        keys[0] = (lap_btn_t){ "TYPE", "ES > EN", 'y', false, false, false };
        keys[1] = (lap_btn_t){ "AUTO", ui.xl_auto ? "ON" : "OFF", 'a', ui.xl_auto != 0, false, false };
        keys[2] = (lap_btn_t){ "SHOW", ui.xl_show == 0 ? "BOTH" : ui.xl_show == 1 ? "ENGLISH" : "AS SENT", 'w', false, false, false };
        break;
    }
    keys[3] = (lap_btn_t){ "MORE", NULL, 'm', false, false, false };
}

/* ---- dialogs ---- */
static void open_send(const char *init)
{
    if (dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "MESSAGE TO EVERYONE ON %s", ch_name(send_ch));
    dialog = lap_text_input(prompt, init ? init : "", TEXT_MAX, LAP_TEXT_LOWER);
    dialog_kind = 1;
}
static void open_dm(uint32_t num)
{
    if (dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    const node_t *n = node_get(num, false);
    if (!n) return;
    dm_to = num;
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "DIRECT MESSAGE TO %s%s", n->short_name, cfg.pki && n->has_key ? " (PRIVATE)" : "");
    dialog = lap_text_input(prompt, "", TEXT_MAX, LAP_TEXT_LOWER);
    dialog_kind = 4;
}
static void open_try(void)
{
    if (dialog >= 0) return;
    dialog = lap_text_input("SPANISH TO TRANSLATE", "", TEXT_MAX, LAP_TEXT_LOWER);
    dialog_kind = 10;
}
static void quick_done(int index) { pending_quick = index; }
static void open_quick(void)
{
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    if (!lap_picker_open(ui.quick_es ? "QUICK REPLY (SENT IN SPANISH)" : "QUICK REPLY", quick_done)) return;
    const int n = mt_phrase_count();
    for (int i = 0; i < n && i < 48; i++) {
        const mt_phrase_t p = mt_phrase(i);
        if (p.en[0]) lap_picker_add(ui.quick_es ? p.es : p.en, ui.quick_es ? p.en : p.es);
    }
}
static void phrase_send(int i)
{
    const mt_phrase_t p = mt_phrase(i);
    if (p.en[0]) open_send(ui.quick_es ? p.es : p.en);
}

enum { M_OPTIONS, M_HELLO, M_QUICK, M_XL, M_CLEAR, M_MOTION, M_CLOSE, M_N };
static void more(void)
{
    if (menu_h > 0 || dialog >= 0) return;
    static char xl[32], mo[32];
    snprintf(xl, sizeof(xl), "AUTO TRANSLATE: %s", ui.xl_auto ? "ON" : "OFF");
    snprintf(mo, sizeof(mo), "MOTION: %s", ui.anim ? "CALM" : "FULL");
    static const char *items[M_N];
    items[M_OPTIONS] = "OPTIONS"; items[M_HELLO] = "SAY HELLO (NODE INFO)"; items[M_QUICK] = "QUICK REPLY";
    items[M_XL] = xl; items[M_CLEAR] = "CLEAR CHAT"; items[M_MOTION] = mo; items[M_CLOSE] = "CLOSE MESHTASTIC";
    menu_h = lap_choose(items, M_N);
    if (menu_h < 0) menu_h = 0;
}
static void menu_poll(void)
{
    if (menu_h <= 0) return;
    int v = -1;
    char none[4];
    const int r = lap_dialog_poll(menu_h, none, sizeof(none), &v);
    if (!r) return;
    menu_h = 0;
    if (r != 1) return;
    switch (v) {
    case M_OPTIONS: open_options(); break;
    case M_HELLO: if (cfg.tx_on) { send_nodeinfo(true); say("hello sent: nodes in range answer"); } else say("TRANSMIT is off"); break;
    case M_QUICK: open_quick(); break;
    case M_XL: ui.xl_auto = !ui.xl_auto; if (ui.xl_auto) xl_fill_req = true; dirty |= SV_UI; break;
    case M_CLEAR: msg_n = msg_head = 0; scroll = 0; say("chat cleared"); break;
    case M_MOTION: ui.anim = !ui.anim; dirty |= SV_UI; break;
    case M_CLOSE: lap_exit(); break;
    }
}

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
        } else if (dialog_kind == 10) {
            snprintf(try_es, sizeof(try_es), "%s", text);
            try_en[0] = 0;
            xl_try_req = true;                           /* translated on the next poll */
        }
    }
    if (r != 0) dialog = -1;
}


static void to_tab(int t)
{
    tab = (t + TABS) % TABS;
    scroll = 0;
    lap_scroll_stop(&drag);
    if (tab == TAB_CHAT) chat_new = true;               /* open at the newest */
}
static void open_node(int i)
{
    if (i < 0 || i >= node_n) return;
    open_num = nodes[i].num;
    node_open = true;
    tab = TAB_NODES;
    scroll = 0;
    lap_scroll_stop(&drag);
}

static void draw(void)
{
    now_ms = lap_millis();
    if (radio < 0 && !(lap_frame() % 25)) { find_radio(); rx_open(); }
    if (!lap_service_running()) poll();   /* the tick does it while a service runs */
    /* A refused start (an old copy still stopping, say) is tried again. */
    static uint32_t svc_try;
    if (want_background() && !lap_service_running() && now_ms - svc_try > 3000) {
        svc_try = now_ms;
        primary = read_primary();
        service_update();
    }
    handle_dialog();
    menu_poll();
    if (pending_quick >= 0 && dialog < 0) { phrase_send(pending_quick); pending_quick = -1; }
    if (pending_phrase >= 0 && dialog < 0) { phrase_send(pending_phrase); pending_phrase = -1; }
    if (tab == TAB_CHAT) unread = 0;
    {
        int32_t alt; uint32_t t;
        me_ok = my_position(&me_lat, &me_lon, &alt, &t);
    }
    layout();
    make_blips();
    const int sw2 = lap_swipe();
    if (sw2 == LAP_SWIPE_LEFT) to_tab(tab + 1);
    else if (sw2 == LAP_SWIPE_RIGHT) to_tab(tab - 1);
    /* the panel's list follows the finger (the bezel's rule rows excluded) */
    clip0 = py + 1; clip1 = py + ph - 1;
    dragging = lap_scroll_follow(&drag, &scroll, smax, clip0, clip1);
    if (dragging && tab == TAB_NODES) kbd_used = false;
    hit_n = 0;

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    clip0 = 0; clip1 = H;
    title_bar();
    header();
    if (wide) lap_vline(hw + 1, 1, H - kr - 1, ':', LAP_UI_FAINT);
    /* the strip: < name >, with the count of what is new */
    char sv[24];
    if (unread && tab != TAB_CHAT) snprintf(sv, sizeof(sv), "%d/%d  %u NEW", tab + 1, TABS, (unsigned)unread);
    else snprintf(sv, sizeof(sv), "%d/%d", tab + 1, TABS);
    static const char *const TAB_NAME[TABS] = { "CHAT", "NODES", "MESH MAP", "AIR", "TRANSLATE" };
    strip(sx, ty, sw, tr, node_open && tab == TAB_NODES ? "NODE" : TAB_NAME[tab], sv, unread && tab != TAB_CHAT);
    /* the panel */
    char bt[48];
    switch (tab) {
    case TAB_CHAT: snprintf(bt, sizeof(bt), " CHAT  %s  %d ", ch_name(send_ch), msg_n); break;
    case TAB_NODES:
        if (node_open) { const int i = find_node(open_num); snprintf(bt, sizeof(bt), " %s ", i >= 0 ? nodes[i].short_name : "NODE"); }
        else snprintf(bt, sizeof(bt), " NODES %d  by %s ", node_n, SORT_NAMES[ui.sort % 4]);
        break;
    case TAB_RADAR: snprintf(bt, sizeof(bt), " MESH MAP  %d nodes ", node_n); break;
    case TAB_AIR: snprintf(bt, sizeof(bt), " AIR  %u heard ", (unsigned)heard); break;
    default: snprintf(bt, sizeof(bt), " SPANISH > ENGLISH "); break;
    }
    lap_ui_bezel(px, py, pw, ph, bt);
    const int ix = px + 1, iy = py + 1, iw = pw - 2, ih = ph - 2;
    if (radio < 0) {
        clip0 = iy; clip1 = iy + ih;
        put(ix + 1, iy + 1, iw - 2, A(LAP_UI_ALARM), "No LoRa radio on this board.");
        put(ix + 1, iy + 2, iw - 2, LAP_UI_DIM, "MESHTASTIC needs the LR2021 or the SX1262.");
    } else {
        switch (tab) {
        case TAB_CHAT: draw_chat(ix, iy, iw, ih); break;
        case TAB_NODES: draw_nodes(ix, iy, iw, ih); break;
        case TAB_RADAR: draw_radar(ix, iy, iw, ih); break;
        case TAB_AIR: draw_air(ix, iy, iw, ih); break;
        default: draw_lang(ix, iy, iw, ih); break;
        }
    }
    clip0 = 0; clip1 = H;
    if (smax > 0) {                                      /* where the panel is in its list */
        const int at = ih > 1 ? iy + (int)((long)scroll * (ih - 1) / smax) : iy;
        lap_put_cell(px + pw - 1, at, LAP_BLOCK, A(LAP_CYAN | LAP_BRIGHT));
    }
    set_keys();
    lap_ui_keys(0, H - kr, W, kr, keys, 4, -1);
}

static void act(char c)
{
    switch (c) {
    case 's': open_send(NULL); break;
    case 'q': open_quick(); break;
    case 'c':
        for (int i = 1; i <= NCH; i++) {
            const int nc = (send_ch + i) % NCH;
            if (ch_on(nc)) { send_ch = nc; break; }
        }
        say("sending on %s", ch_name(send_ch));
        break;
    case 'd':
        if (tab == TAB_NODES && node_open) open_dm(open_num);
        else if (tab == TAB_NODES && node_n) { sort_nodes(); open_dm(nodes[node_order[sel]].num); }
        else say("open a node in NODES first");
        break;
    case 'r':
        if (!cfg.tx_on) { say("TRANSMIT is off"); break; }
        if (tab == TAB_NODES && node_open) send_trace(open_num);
        else if (tab == TAB_NODES && node_n) { sort_nodes(); send_trace(nodes[node_order[sel]].num); }
        break;
    case 'b': node_open = false; scroll = 0; lap_scroll_stop(&drag); break;
    case 'g': ui.sort = (uint8_t)((ui.sort + 1) % 4); dirty |= SV_UI; scroll = 0; say("nodes by %s", SORT_NAMES[ui.sort]); break;
    case 'n':
        if (!cfg.tx_on) { say("TRANSMIT is off"); break; }
        if (tab == TAB_NODES && node_open) { send_nodeinfo_to(open_num, true); say("asked %s who it is", who(open_num)); }
        else { send_nodeinfo(true); say("hello sent: nodes in range answer"); }
        break;
    case 'v': to_tab(TAB_RADAR); break;
    case 'u': node_open = false; to_tab(TAB_NODES); break;
    case 'z': ui.zoom = (uint8_t)((ui.zoom + 1) % ZOOMS); dirty |= SV_UI; break;
    case 'l': ui.labels = !ui.labels; dirty |= SV_UI; break;
    case 'k': air_n = air_head = 0; scroll = 0; break;
    case 'y': open_try(); break;
    case 'a': ui.xl_auto = !ui.xl_auto; if (ui.xl_auto) xl_fill_req = true; dirty |= SV_UI; say("auto translate %s", ui.xl_auto ? "on" : "off"); break;
    case 'w': ui.xl_show = (uint8_t)((ui.xl_show + 1) % 3); dirty |= SV_UI; break;
    case 'm': more(); break;
    case 'o': open_options(); break;
    case 't': to_tab(tab + 1); break;
    default: break;
    }
}

static void step(int by)
{
    scroll += by;
    if (scroll > smax) scroll = smax;
    if (scroll < 0) scroll = 0;
}

static bool key(lap_key_t k, char ch)
{
    const int page = ph > 6 ? ph - 4 : 2;
    switch (k) {
    case LAP_KEY_LEFT: to_tab(tab - 1); return true;
    case LAP_KEY_RIGHT: case LAP_KEY_TAB: to_tab(tab + 1); return true;
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP:
        if (tab == TAB_NODES && !node_open) { kbd_used = true; if (sel) sel--; if (sel * CARD < scroll) scroll = sel * CARD; }
        else step(-2);
        return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN:
        if (tab == TAB_NODES && !node_open) {
            kbd_used = true;
            if (sel < node_n - 1) sel++;
            if ((sel + 1) * CARD > scroll + ph - 2) scroll = (sel + 1) * CARD - (ph - 2);
        } else step(2);
        return true;
    case LAP_KEY_PGUP: step(-page); return true;
    case LAP_KEY_PGDN: step(page); return true;
    case LAP_KEY_ENTER:
        if (tab == TAB_NODES && !node_open && node_n) { sort_nodes(); open_node(node_order[sel]); }
        else if (tab == TAB_NODES && node_open) act('d');
        else if (tab == TAB_LANG) act('y');
        else act('s');
        return true;
    case LAP_KEY_BACK:
        if (tab == TAB_NODES && node_open) { act('b'); return true; }
        return false;
    case LAP_KEY_MENU: more(); return true;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    if (ch >= '1' && ch <= '0' + TABS) { to_tab(ch - '1'); return true; }
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    if (strchr("sqcdrbgnvuzlkyawmot", ch)) { act(ch); return true; }
    return false;
}

static bool touch(int col, int row)
{
    if (row >= H - kr) {
        if (dragging) return true;
        const int b = lap_ui_key_hit(col, row);
        if (b >= 0 && !keys[b].dim) act(keys[b].key);
        return true;
    }
    if (row >= ty && row < ty + tr && col >= sx && col < sx + sw) {
        const int t = (col - sx) * 3 / (sw > 0 ? sw : 1);
        if (t == 0) to_tab(tab - 1);
        else if (t == 2) to_tab(tab + 1);
        else if (tab == TAB_NODES && node_open) act('b');
        return true;
    }
    if (row == 0) {
        if (status_line[0] && now_ms - status_ms < 5000u) status_ms = 0;
        else more();
        return true;
    }
    if (col >= hx && col < hx + hw && row >= hy && row < hy + hh && !(wide && row >= py)) {
        if (!wide || row < hy + hh) { to_tab(TAB_RADAR); return true; }
    }
    if (dragging) return true;
    /* The panel. A tap here is used even when it lands on nothing: unused,
       the board would turn it into UP, ENTER or DOWN by where it fell. */
    for (int i = hit_n - 1; i >= 0; i--) {
        const hit_t *h = &hits[i];
        if (col < h->x0 || col >= h->x1 || row < h->y0 || row >= h->y1) continue;
        switch (h->kind) {
        case HIT_MSG: {
            msg_t *m = &msgs[h->idx];
            if (m->tr[0]) {                              /* both > as sent > English > both */
                const int cur = msg_mode(m), next = cur == 1 ? 0 : cur == 0 ? 2 : 1;
                m->view = (uint8_t)(next + 1);
            } else if (!m->mine && m->lang != MT_LANG_EN) {
                xl_msg_req = h->idx;                     /* translated on the next poll */
                m->view = 2;
            } else if (!m->mine) {
                const int ni = find_node(m->from);
                if (ni >= 0) open_node(ni);
            }
            return true;
        }
        case HIT_NODE: kbd_used = false; open_node(h->idx); return true;
        case HIT_BLIP: open_node(h->idx); return true;
        case HIT_AIR: {
            const air_t *a = &air[(air_head - 1 - h->idx + MAX_AIR * 2) % MAX_AIR];
            const int ni = find_node(a->from);
            if (ni >= 0) open_node(ni);
            return true;
        }
        case HIT_PHRASE: pending_phrase = h->idx; return true;
        case HIT_XL_AUTO: act('a'); return true;
        case HIT_XL_SHOW: act('w'); return true;
        case HIT_TRY: open_try(); return true;
        case HIT_HIST: {
            if (h->idx < 0 || h->idx >= (int)hist.n) return true;
            const hist_t *e = &hist.e[h->idx];
            if (!e->en[0]) { xl_hist_req = h->idx; return true; }
            /* back into TRY IT, whole, to read or change */
            snprintf(try_es, sizeof(try_es), "%s", e->es);
            snprintf(try_en, sizeof(try_en), "%s", e->en);
            scroll = 0;
            return true;
        }
        case HIT_HIST_CLEAR:
            if (hist_clear_ms && now_ms - hist_clear_ms < 3000u) {
                hist.n = 0;
                hist_clear_ms = 0;
                dirty |= SV_HIST;
                say("history cleared");
            } else hist_clear_ms = now_ms ? now_ms : 1;
            return true;
        }
    }
    return true;
}

LAP_APP(.id = "meshtastic", .name = "MESHTASTIC", .sub = "be a Meshtastic node", .version = "1.1",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { "   /\\     ", "  /  \\ ((o", " / /\\ \\/  ", "/_/  \\_/\\_", "  ~ ~ ~ ~ ", "          " },
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX | LAP_CAP_SERVICE,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
