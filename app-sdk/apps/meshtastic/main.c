/* MESHTASTIC: be a Meshtastic node on the board's LoRa radio.
   - hears every packet on the chosen region / preset (or custom LoRa) and
     reads up to eight channels, each with its own name, key and role
   - CHAT (conversations by channel and by node, replies and reactions,
     delivery reports), NODES (names, hardware, role, signal, battery,
     position, favourites, ignore, traceroute, requests), MESH MAP (with the
     links nodes report and waypoints), AIR (and the range test), TRANSLATE
   - sends text, direct messages with acks (public-key DMs when PRIVATE DMS
     is on and the other node has published a key), node info, position,
     device telemetry, waypoints and neighbour info when TRANSMIT is on;
     rebroadcasts by its role, adding itself to traceroutes it carries
   - the channel set as a meshtastic.org link and QR code; settings backup
   - nodes and waypoints with a position go on the board's MAP
   - keeps running in the background when Meshtastic is the board's primary
     mesh (MESH > OPTIONS > AT BOOT > PRIMARY MESH) or KEEP RUNNING = ALWAYS

   Source-derived in part: roles, rebroadcast modes, the slot rule, module
   behaviour (range test, neighbour info, store and forward) and message
   layouts follow Meshtastic's firmware and protobufs (Copyright (C)
   Meshtastic LLC and contributors, GPL-3.0). This file is GPL-3.0. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "mt_pki.h"
#include "mt_proto.h"
#include "mt_scan.h"
#include "mt_xlate.h"
#include "qrcodegen.h"
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
#define TEXT_MAX   MT_DATA_PAYLOAD   /* a text packet's most bytes */
#define HIST_TEXT  200       /* the translator's history keeps the size it saved */
#define TR_MAX     400       /* a translation can run two or three times its original */
#define SIG_N      24        /* a node's last signal readings */
#define PER_MIN_N  30        /* packets a minute, the last half hour */
#define TXQ        6
#define NCH        8
#define MAX_WPT    16
#define MAX_REACT  32
#define NBR_N      8         /* the links kept for each node */
#define ACK_WAIT_MS 20000u
#define ACK_TRIES   3
#define TRACE_WAIT_MS 60000u
#define URL_MAX    1000

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
    uint8_t xl_auto;        /* translate as it arrives */
    uint8_t xl_show;        /* 0 both, 1 English only, 2 as sent */
    uint8_t quick;          /* quick replies go out in: 0 English, 1 Spanish (as 1.3 saved it), else the mt_lang_t */
    uint8_t sort;           /* NODES: 0 heard, 1 signal, 2 distance, 3 name */
    uint8_t zoom;           /* RADAR: 0 fit, then ZOOM_KM[] */
    uint8_t labels;         /* RADAR: names beside the blips */
    uint8_t anim;           /* 0 full motion, 1 calm */
    uint8_t speak;          /* read new messages aloud: 0 no, 1 translations, 2 all */
    uint8_t region_set;     /* 1 once a region was chosen by the user or by GPS */
    uint8_t xl_from;        /* languages read: MT_LANG_BIT()s and XL_SET; 0 (before 1.4) is Spanish only */
    uint8_t xl_from_hi;     /* the MT_LANG_BIT()s from bit 8 on: Polish, Russian, Ukrainian */
    uint8_t reserved[4];
} ui_cfg_t;
#define XL_SET 1u
#define UI_MAGIC 0x5Au
static ui_cfg_t ui;
static const float ZOOM_KM[] = { 0, 1, 5, 20, 100, 500 };
#define ZOOMS ((int)(sizeof(ZOOM_KM) / sizeof(ZOOM_KM[0])))

/* 1.3's settings, a slot of their own: device, LoRa, position, modules. */
typedef struct {
    uint32_t magic;          /* EXT_MAGIC */
    uint8_t role;            /* Config.DeviceConfig.Role */
    uint8_t rebroadcast;     /* RebroadcastMode */
    uint8_t ni_every;        /* node info: index into NI_MIN */
    uint8_t hop_limit;       /* 1..7 */
    uint8_t custom, bw, sf, cr;      /* use_preset off: bandwidth (MT_BW_*), SF, CR */
    uint16_t slot;           /* channel_num: 0 = from the name */
    uint16_t smart_m;        /* smart position: metres moved */
    uint16_t smart_s;        /* smart position: seconds at least between */
    uint8_t smart, duty_override;
    int32_t offset_hz;       /* frequency_offset */
    uint8_t licensed;        /* ham mode: no encryption, may pass the power cap */
    uint8_t notify;          /* 0 every message, 1 direct only, 2 off */
    uint8_t range;           /* range test: 0 off, 1 log, 2.. send every RANGE_S */
    uint8_t nbr_every;       /* neighbour info: index into NBR_H (0 off) */
    uint8_t reserved[20];
} ext_t;
#define EXT_MAGIC 0x31545845u   /* "EXT1" */
static ext_t ext;
static const uint16_t NI_MIN[] = { 60, 180, 360, 720, 1440 };
static const uint16_t RANGE_S[] = { 0, 0, 15, 30, 60, 300 };
static const uint8_t NBR_H[] = { 0, 4, 6, 12, 24 };

/* Channels as Meshtastic keeps them: [0] the primary, [1..7] secondary or
   off, each with its name, key, mute and the position precision sent on it.
   v1.2 kept four (chan_old_t, "chans"); they are read once into these. */
enum { CH_OFF, CH_PRIMARY, CH_SECONDARY };
typedef struct {
    char name[12];
    uint8_t psk_len, on;
    uint8_t psk[32];
    uint8_t pad[2];
} chan_old_t;
typedef struct {
    char name[12];
    uint8_t psk_len, role, mute, precision;
    uint8_t psk[32];
} chan_cfg_t;
static chan_cfg_t chans[NCH];
static struct { uint8_t key[32]; int key_len; uint8_t hash; } crt[NCH];

typedef struct { uint8_t priv[32], pub[32]; uint32_t magic; } pki_t;
#define PKI_MAGIC 0x4B504D31u
static pki_t pki;
static bool pki_ready;

/* Saves go one at a time (the board refuses a second while one is pending),
   each at most 4 KB: what changed is marked and written in turn. */
enum { SV_CFG = 1, SV_CHANS = 2, SV_PKI = 4, SV_NODES = 8, SV_UI = 16, SV_HIST = 32, SV_EXT = 64, SV_WPT = 128,
       SV_BACKUP = 256, SV_RESTORE = 512, SV_RLOG = 1024 };
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
static bool fresh_install;     /* no saved settings: the region may follow the GPS */
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
    else fresh_install = true;
    dirty |= SV_CFG;
}
static void ext_defaults(void)
{
    memset(&ext, 0, sizeof(ext));
    ext.magic = EXT_MAGIC;
    ext.role = MT_ROLE_CLIENT;
    ext.ni_every = 1;                /* 3 hours, as the firmware */
    ext.hop_limit = 3;
    ext.bw = MT_BW_250; ext.sf = 11; ext.cr = 5;
    ext.smart_m = 100; ext.smart_s = 30;
}
static void ext_load(void)
{
    if (lap_load("ext", &ext, sizeof(ext)) == (int)sizeof(ext) && ext.magic == EXT_MAGIC && ext.hop_limit >= 1 &&
        ext.hop_limit <= 7 && ext.bw < MT_BW_COUNT && ext.notify < 3 && ext.range < 6 && ext.nbr_every < 5 &&
        ext.ni_every < 5) return;
    ext_defaults();
    /* from 1.2: RELAY OTHERS off was a CLIENT_MUTE */
    if (!fresh_install && !cfg.relay) ext.role = MT_ROLE_CLIENT_MUTE;
    dirty |= SV_EXT;
}
static void chans_load(void)
{
    if (lap_load("chans8", chans, sizeof(chans)) == (int)sizeof(chans) && chans[0].psk_len <= 32) {
        chans[0].role = CH_PRIMARY;
        snprintf(chans[0].name, sizeof(chans[0].name), "%s", cfg.channel);
        return;
    }
    memset(chans, 0, sizeof(chans));
    chan_old_t old[4];
    if (lap_load("chans", old, sizeof(old)) == (int)sizeof(old) && old[0].psk_len <= 32) {
        for (int c = 0; c < 4; c++) {
            chans[c].psk_len = old[c].psk_len;
            memcpy(chans[c].psk, old[c].psk, sizeof(chans[c].psk));
            memcpy(chans[c].name, old[c].name, sizeof(chans[c].name));
            chans[c].role = c == 0 ? CH_PRIMARY : old[c].on && old[c].name[0] ? CH_SECONDARY : CH_OFF;
        }
    } else {
        chans[0].psk_len = 1;
        chans[0].psk[0] = cfg.psk;      /* 1 = AQ==, 0 = none */
        /* First run after v0.2: private DMs on, as for a new node (a 2.7 node
           refuses to send a DM to one that has published no key). */
        cfg.pki = 1;
        dirty |= SV_CFG;
    }
    chans[0].role = CH_PRIMARY;
    snprintf(chans[0].name, sizeof(chans[0].name), "%s", cfg.channel);
    /* SHARE POSITION's old approximate setting is now the primary's precision */
    chans[0].precision = cfg.pos_mode == 2 ? 15 : 32;
    dirty |= SV_CHANS;
}
static void ui_load(void)
{
    if (lap_load("ui", &ui, sizeof(ui)) == (int)sizeof(ui) && ui.magic == UI_MAGIC && ui.xl_show < 3 &&
        ui.sort < 4 && ui.zoom < ZOOMS && ui.speak < 3) {
        if (ui.quick >= MT_LANG_COUNT) ui.quick = 1;
        return;
    }
    memset(&ui, 0, sizeof(ui));
    ui.magic = UI_MAGIC; ui.xl_auto = 1; ui.quick = 1; ui.labels = 1;
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
    uint32_t reply_id;      /* the message this one answers */
    int16_t rssi, snr;      /* tenths */
    uint8_t hops, mine, state, tries, ch, pki;
    uint8_t lang;           /* mt_lang_t, as detected */
    uint8_t view;           /* 0 the screen's choice, 1 as sent, 2 both */
    uint8_t via_sf;         /* came back from a store-and-forward router */
    char text[TEXT_MAX + 1];
    char tr[TR_MAX + 1];    /* English, when it came in Spanish */
} msg_t;
typedef struct {
    uint32_t num, last_ms, packets, asked_ms;
    uint32_t heard_unix;    /* when it was last heard, if the clock was set */
    int16_t rssi, snr;
    uint8_t hops, battery, ch, hw, role, unread;
    bool has_pos, named, has_dev, has_key, fav, ignored, licensed, sf_router;
    float voltage, ch_util, air_util;
    double lat, lon;
    char short_name[6], long_name[32];
    char last[10];          /* what it sent last */
    uint8_t pubkey[32];
    int8_t sig[SIG_N];      /* dBm of the last packets, oldest first (not saved) */
    uint8_t sig_n;
    /* the nodes it says it hears directly (NeighborInfo), SNR in quarter dB */
    uint32_t nbr[NBR_N];
    int8_t nbr_snr[NBR_N];
    uint8_t nbr_n;
    uint32_t nbr_ms;
} node_t;
/* A reaction (tapback): who put which emoji on which message. */
typedef struct { uint32_t target, from; char glyph[8]; } react_t;
/* A waypoint: from the mesh or dropped here. */
typedef struct {
    uint32_t id, from, expire, icon;
    int32_t lat, lon;       /* degrees * 1e7 */
    char name[32], desc[64];
} wpt_t;
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
    bool used, relay, keep;     /* keep: a router's repeat, not dropped when another is heard */
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
static react_t reacts[MAX_REACT];
static int react_head;
static wpt_t wpts[MAX_WPT];
static int wpt_n;
static uint16_t unread_ch[NCH];         /* unread channel messages, by channel */
/* range test: what this node sent, and what it heard */
static uint32_t range_seq, range_ms, range_start_ms, range_got, range_lost, range_last_seq;
typedef struct { uint32_t from, seq, at_ms; int16_t rssi, snr; float km; } range_rx_t;
static range_rx_t range_rx[6];
static int range_rx_n;
static char range_file[64];
/* store and forward: the last router asked, and what it said */
static uint32_t sf_router, sf_ms, sf_expect, sf_got;
static uint32_t last_nbr_ms, last_lost_ms;
static double smart_lat, smart_lon;     /* where the last position went out from */
static bool smart_have;

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
/* SCAN (mt_scan.c): which presets are in use nearby. While it runs the radio
   sits on the preset being scanned, so the node neither sends nor hears its
   own mesh; STOP (or leaving the app) puts it back. */
static mt_scan_t scan;
static bool scan_on;
static int scan_sel;                    /* the row UP/DOWN chose; PIN pins it */
static uint8_t scan_dwell = 1;          /* SCAN_DWELL_S index */
static const uint16_t SCAN_DWELL_S[] = { 5, 10, 20, 30, 60 };
static const char *const SCAN_DWELL_NAMES[] = { "5 S", "10 S", "20 S", "30 S", "60 S" };

/* ---- screen ------------------------------------------------------------ */
enum { TAB_CHAT, TAB_NODES, TAB_RADAR, TAB_AIR, TAB_SCAN, TAB_LANG, TABS };
static int tab, scroll, sel;
static bool chat_new;               /* a message came: CHAT follows it down */
static bool node_open;              /* NODES: one node's page */
static uint32_t open_num;
/* CHAT: which conversation (CONV_ALL, a channel, or one node's DMs), the
   list of them, and the message chosen to reply to or react on */
enum { CONV_ALL, CONV_CH, CONV_DM };
static int conv_kind, conv_ch;
static uint32_t conv_node;
static bool conv_list;
static uint32_t pick_id, pick_from;     /* chosen message: packet id and sender */
static bool share_open, share_all = true, wpt_list;  /* the SHARE page; MESH MAP's waypoint list */
static int menu_h, pending_quick = -1, pending_phrase = -1;
/* The translator keeps static buffers: it runs only inside poll(), on
   whichever task polls (the screen's, or the service tick's), and the
   screen asks for its work through these. */
static volatile int xl_msg_req = -1;
static volatile bool xl_try_req, xl_fill_req;
static char try_es[TEXT_MAX + 1], try_en[TR_MAX + 1];
static uint8_t try_lang;                /* the language TRY IT read it as */
static void xl_backfill(void);

/* The languages read (TRANSLATE > FROM). Spanish is built in; the others
   read their data from the card into PSRAM while they are on: the pack the
   board's PACKS installs (lakeshark/packs/lang-<code>.bin) first, then a
   copy by hand, meshtastic/lang/<code>.bin or meshtastic/lang-<code>.bin. */
static const char *const LANG_UP[MT_LANG_COUNT] = { "", "EN", "ES", "PT", "FR", "DE", "IT", "NL", "PL", "RU", "UK" };
static const char *const LANG_AT[3] = {
    "/sdcard/lakeshark/packs/lang-",            /* <code>.bin, installed from PACKS */
    "/sdcard/meshtastic/lang/",                 /* <code>.bin, as copied with a card reader */
    "/sdcard/meshtastic/lang-",                 /* <code>.bin, one folder deep, as the console's "sd put" writes */
};
enum { LS_OFF, LS_SIZE, LS_READ, LS_READY, LS_MISSING, LS_BAD };
typedef struct { uint8_t state, alt; int job; uint32_t size, got; uint8_t *buf; } lang_card_t;   /* alt: LANG_AT */
static lang_card_t lang_st[MT_LANG_COUNT];
static int lang_busy = -1;              /* the language with a card job out */
static unsigned xl_langs(void)
{
    return (ui.xl_from & XL_SET) ? ((ui.xl_from & ~XL_SET) | (unsigned)ui.xl_from_hi << 8) : MT_LANG_BIT(MT_LANG_ES);
}
static bool xl_on(int l) { return l > MT_LANG_EN && l < MT_LANG_COUNT && (xl_langs() & MT_LANG_BIT(l)); }
static bool xl_ready(int l) { return xl_on(l) && mt_xlate_lang_ready((mt_lang_t)l); }
static int xl_count(void) { int n = 0; for (int l = MT_LANG_ES; l < MT_LANG_COUNT; l++) n += xl_on(l); return n; }
static int xl_first(void) { for (int l = MT_LANG_ES; l < MT_LANG_COUNT; l++) if (xl_on(l)) return l; return MT_LANG_ES; }
static void say(const char *fmt, ...);
static void xl_toggle(int l)
{
    unsigned m = xl_langs() ^ MT_LANG_BIT(l);
    if (!m) { say("one language at least: turn another on first"); return; }
    ui.xl_from = (uint8_t)((m & 0xFFu) | XL_SET);
    ui.xl_from_hi = (uint8_t)(m >> 8);
    dirty |= SV_UI;
    if (xl_on(l)) xl_fill_req = true;
}
static mt_lang_t quick_lang(void)
{
    return ui.quick == 0 ? MT_LANG_EN : ui.quick < MT_LANG_FIRST_CARD ? MT_LANG_ES : (mt_lang_t)ui.quick;
}
/* "Spanish", "Portuguese" */
static const char *lang_word(int l)
{
    static const char *const W[MT_LANG_COUNT] = { "", "English", "Spanish", "Portuguese", "French", "German", "Italian",
                                                  "Dutch", "Polish", "Russian", "Ukrainian" };
    return l > 0 && l < MT_LANG_COUNT ? W[l] : "";
}
/* What the screen draws of a line: Cyrillic as Latin letters, Polish letters
   plain (the line itself is what is sent and kept). Four at a time. */
static const char *scr(const char *s, int lang)
{
    static char b[4][TEXT_MAX * 2 + 8];
    static int k;
    if (!s || !mt_xlate_needs_screen(s)) return s;
    k = (k + 1) & 3;
    mt_xlate_screen(s, (mt_lang_t)lang, b[k], sizeof(b[k]));
    return b[k];
}
/* The phrasebook's English for a language: "I speak little Spanish" names the one it goes out in. */
static const char *phrase_en(int i, mt_lang_t l, char *b, size_t n)
{
    const char *en = mt_phrase(i).en, *at = strstr(en, "Spanish");
    if (!at || l <= MT_LANG_ES || l >= MT_LANG_COUNT) return en;
    snprintf(b, n, "%.*s%s%s", (int)(at - en), en, lang_word(l), at + 7);
    return b;
}
static const char *lf_where(void);
/* A line into English: in its own language when that one is read, else the
   language read that recognises it, else the first one read (or Spanish).
   *lang says which was used. */
static int xl_line(const char *in, uint8_t *lang, char *out, size_t cap)
{
    int l = *lang;
    if (!xl_ready(l)) {
        int conf = 0;
        l = mt_xlate_detect_in(in, xl_langs(), &conf);
        if (l >= MT_LANG_FIRST_CARD && conf >= MT_XLATE_MIN_CONF && !xl_ready(l)) {
            /* German, say, while German is off or its file is not on the card: not a Spanish guess */
            *lang = (uint8_t)l;
            snprintf(out, cap, "(%s: %s)", mt_lang_name((mt_lang_t)l), xl_on(l) ? lf_where() : "turn it on in FROM");
            return -1;
        }
        if (!xl_ready(l)) l = xl_ready(xl_first()) ? xl_first() : MT_LANG_ES;
    }
    *lang = (uint8_t)l;
    return mt_xlate_to_en((mt_lang_t)l, in, out, cap);
}

/* Card data: the size, then 64 KB reads into one PSRAM buffer, then attach.
   One job at a time, from poll(); off languages give their memory back. */
static void lang_pump(void)
{
    char path[48];
    for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++)
        if (!xl_on(l) && lang_st[l].state != LS_OFF && l != lang_busy) {
            mt_xlate_lang_detach((mt_lang_t)l);
            free(lang_st[l].buf);
            memset(&lang_st[l], 0, sizeof(lang_st[l]));
        }
    if (lang_busy >= 0) {
        const int l = lang_busy;
        lang_card_t *s = &lang_st[l];
        const int r = lap_sd_poll(s->job);
        if (r == LAP_SD_PENDING) return;
        lang_busy = -1;
        snprintf(path, sizeof(path), "%s%s.bin", LANG_AT[s->alt], mt_lang_code((mt_lang_t)l));
        if (s->state == LS_SIZE) {
            if (r == LAP_SD_ENOENT && s->alt < 2) { s->alt++; s->state = LS_OFF; return; }   /* the next place, next */
            if (r < 0) {
                s->state = r == LAP_SD_ENOENT ? LS_MISSING : LS_BAD;
                lap_log("translator: no %s (%d)", path, r);
                return;
            }
            if (r < 128 || r > 1024 * 1024 || !(s->buf = malloc((size_t)r))) { s->state = LS_BAD; return; }
            s->size = (uint32_t)r;
            s->got = 0;
            s->state = LS_READ;
        } else if (s->state == LS_READ) {
            if (r <= 0) { free(s->buf); s->buf = NULL; s->state = LS_BAD; return; }
            s->got += (uint32_t)r;
            if (s->got >= s->size) {
                if (mt_xlate_lang_attach((mt_lang_t)l, s->buf, s->size) == 0) {
                    s->state = LS_READY;
                    xl_fill_req = true;                 /* what came in meanwhile */
                    lap_log("translator: %s ready (%u bytes, %s)", mt_lang_code((mt_lang_t)l), (unsigned)s->size, path);
                } else { free(s->buf); s->buf = NULL; s->state = LS_BAD; }
                return;
            }
        }
        if (!xl_on(l)) return;                          /* turned off meanwhile: freed next time */
        {
            const uint32_t n = s->size - s->got > 65536u ? 65536u : s->size - s->got;
            const int j = lap_sd_read(path, s->got, s->buf + s->got, n);
            if (j >= 0) { s->job = j; lang_busy = l; }
            else { free(s->buf); s->buf = NULL; s->state = LS_OFF; }      /* tried again */
        }
        return;
    }
    for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++)
        if (xl_on(l) && lang_st[l].state == LS_OFF) {
            snprintf(path, sizeof(path), "%s%s.bin", LANG_AT[lang_st[l].alt], mt_lang_code((mt_lang_t)l));
            const int j = lap_sd_size(path);
            if (j < 0) return;                          /* busy: next poll */
            lang_st[l].job = j;
            lang_st[l].state = LS_SIZE;
            lang_busy = l;
            return;
        }
}
/* A card copied since: look again for what was missing. */
static void lang_retry(void)
{
    for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++)
        if (lang_st[l].state == LS_MISSING || lang_st[l].state == LS_BAD) { lang_st[l].state = LS_OFF; lang_st[l].alt = 0; }
}

/* ---- the language files on the card ------------------------------------ */
/* MESH LANGUAGES, a store app, downloads them into meshtastic/lang/; PACKS >
   LANGUAGES puts them in lakeshark/packs/ where the firmware has it. This
   app needs no network: MORE LANGUAGES sends people to the store, FROM lists
   what meshtastic/lang/ holds, and a file there can be removed again. */
#define LF_DIR "/sdcard/meshtastic/lang"
static uint16_t lf_have;                /* the languages in meshtastic/lang/, by MT_LANG_BIT */
static uint16_t lf_kb[MT_LANG_COUNT];   /* their sizes, KB */
static int lf_list_job = -1;
static bool lf_list_due = true;
static lap_sd_entry_t lf_ents[16];
static int lf_loose[3] = { -1, -1, -1 };    /* card jobs whose answer does not matter, polled to free them */
static volatile int lf_del = -1;        /* REMOVE chosen: done in poll() */
static int lf_new_fw = -1;              /* the board has store.open (and PACKS > LANGUAGES): 1, 0, -1 not looked */
static bool langs_open;                 /* the MORE LANGUAGES panel, on TRANSLATE */

/* Firmware after 2.23.2 has PACKS > LANGUAGES and the store.open action:
   one look at the action table says which. A pack on the card says so too. */
static bool lf_packs(void)
{
    if (lf_new_fw < 0) {
        char nm[24], sig[8];
        lf_new_fw = 0;
        for (int i = 0; i < lap_action_count(); i++)
            if (lap_action_info(i, nm, sizeof(nm), sig, sizeof(sig)) && !strcmp(nm, "store.open")) { lf_new_fw = 1; break; }
    }
    if (lf_new_fw) return true;
    for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++)
        if (lang_st[l].state == LS_READY && lang_st[l].alt == 0) return true;
    return false;
}
/* Where a missing language comes from, in a few words. */
static const char *lf_where(void)
{
    return lf_packs() ? "get it from STORE > MESH LANGUAGES or PACKS" : "get it from STORE > MESH LANGUAGES";
}
static void xl_set(int l, bool on)
{
    unsigned m = on ? xl_langs() | MT_LANG_BIT(l) : xl_langs() & ~MT_LANG_BIT(l);
    if (!m) m = MT_LANG_BIT(MT_LANG_ES);            /* one at least */
    ui.xl_from = (uint8_t)((m & 0xFFu) | XL_SET);
    ui.xl_from_hi = (uint8_t)(m >> 8);
    dirty |= SV_UI;
    if (on) xl_fill_req = true;
}
static unsigned lf_kbs(uint32_t bytes) { return (unsigned)((bytes + 1023u) / 1024u); }
static void lf_forget(int j)
{
    for (int i = 0; j >= 0 && i < 3; i++) if (lf_loose[i] < 0) { lf_loose[i] = j; return; }
}
/* REMOVE: the file goes; a language read from it goes off. */
static void lf_remove_now(int l)
{
    char path[48];
    if (l < MT_LANG_FIRST_CARD || l >= MT_LANG_COUNT) return;
    snprintf(path, sizeof(path), LF_DIR "/%s.bin", mt_lang_code((mt_lang_t)l));
    const int j = lap_sd_remove(path);
    if (j < 0) { say("the card is busy, try again"); return; }
    lf_forget(j);
    lang_card_t *s = &lang_st[l];
    if (s->state == LS_READY && s->alt == 1 && lang_busy != l) {
        mt_xlate_lang_detach((mt_lang_t)l);
        free(s->buf);
        memset(s, 0, sizeof(*s));
        if (xl_on(l)) xl_set(l, false);
    }
    lf_have &= (uint16_t)~MT_LANG_BIT(l);
    say("%s removed from the card, %u KB free again", lang_word(l), (unsigned)lf_kb[l]);
}
/* poll(): what meshtastic/lang/ holds (FROM's REMOVE rows), and REMOVE. */
static void lf_pump(void)
{
    for (int i = 0; i < 3; i++)
        if (lf_loose[i] >= 0 && lap_sd_poll(lf_loose[i]) != LAP_SD_PENDING) lf_loose[i] = -1;
    if (lf_del >= 0) { const int l = lf_del; lf_del = -1; lf_remove_now(l); }
    if (lf_list_job >= 0) {
        const int r = lap_sd_poll(lf_list_job);
        if (r == LAP_SD_PENDING) return;
        lf_list_job = -1;
        uint16_t have = 0;
        for (int i = 0; i < r && i < 16; i++) {
            const char *nm = lf_ents[i].name;
            char c[3] = { (char)(nm[0] | 0x20), (char)(nm[1] | 0x20), 0 };
            if (lf_ents[i].dir || strlen(nm) != 6 || nm[2] != '.' || (nm[3] | 0x20) != 'b' || (nm[4] | 0x20) != 'i' ||
                (nm[5] | 0x20) != 'n') continue;
            const int l = mt_lang_from_code(c);
            if (l < MT_LANG_FIRST_CARD || l >= MT_LANG_COUNT) continue;
            have |= (uint16_t)MT_LANG_BIT(l);
            lf_kb[l] = (uint16_t)lf_kbs(lf_ents[i].size);
        }
        lf_have = have;
        return;
    }
    if (lf_list_due) {
        const int j = lap_sd_list(LF_DIR, lf_ents, 16);
        if (j >= 0) { lf_list_job = j; lf_list_due = false; }
    }
}

/* TRANSLATE's history: each TRY IT and each Spanish line heard, newest
   first, kept on the card so it outlives the app. */
#define MAX_HIST   16
#define HIST_MAGIC 0x31545348u          /* "HST1" */
typedef struct {
    uint32_t from;                      /* who said it; 0 typed into TRY IT */
    uint32_t unix;                      /* when, if the clock was set */
    char es[HIST_TEXT + 1];
    char en[TR_MAX + 1];                /* empty until translated */
} hist_t;
static struct { uint32_t magic, n; hist_t e[MAX_HIST]; } hist;
#define HIST_HEAD (sizeof(hist) - sizeof(hist.e))
static int xl_hist_req = -1;
static uint32_t hist_clear_ms;          /* CLEAR armed; a second tap within 3 s empties it */
static uint32_t hist_new_ms;            /* the top line arrived: lit a moment */
/* What the open dialog is for (DLG_*), and where its answer goes. */
enum { DLG_NONE, DLG_TEXT, DLG_LONG, DLG_SHORT, DLG_DM, DLG_FIXED, DLG_TRY, DLG_REPLY, DLG_CH_NAME, DLG_CH_KEY,
       DLG_IMPORT, DLG_IMPORT_OK, DLG_REGION, DLG_ROLE, DLG_REACT, DLG_WPT, DLG_CALLSIGN, DLG_RESTORE_OK,
       DLG_CH_PRIMARY, DLG_FROM, DLG_QUICK, DLG_PHRASE, DLG_DEL };
static int dialog = -1, dialog_kind;
static uint32_t dm_to;
static int opt_from = -2, opt_cur = -2; /* the OPTIONS level a dialog came from (-1 the root, -2 none) */
static int ch_sel;                      /* OPTIONS > CHANNELS: the channel being set */
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

static mt_lora_t lora_now(void)
{
    return (mt_lora_t){ cfg.region, cfg.preset, ext.custom, ext.bw, ext.sf, ext.cr, ext.slot, ext.offset_hz };
}
/* the preset's name, or "Custom": an unnamed channel goes by it */
static const char *preset_name(void) { const mt_lora_t l = lora_now(); return mt_lora_name(&l); }
static const char *ch_name(int c) { return chans[c].name[0] ? chans[c].name : preset_name(); }
static const char *channel_name(void) { return ch_name(0); }
static bool ch_on(int c) { return c == 0 || chans[c].role == CH_SECONDARY; }
static int ch_count(void) { int n = 0; for (int c = 0; c < NCH; c++) n += ch_on(c); return n; }
/* The default channel: the default key under the preset's own name (upstream
   keeps neighbour info and S&F history off it). */
static bool ch_default(int c)
{
    return !ext.custom && chans[c].psk_len == 1 && chans[c].psk[0] == 1 && !chans[c].name[0] && !ext.slot;
}
/* Roles that keep quiet: no routine node info, position or telemetry. */
static bool role_quiet(void)
{
    return ext.role == MT_ROLE_CLIENT_HIDDEN || ext.role == MT_ROLE_REPEATER || ext.role == MT_ROLE_TAK ||
           ext.role == MT_ROLE_TAK_TRACKER;
}

/* The region's limits, as Meshtastic defines them: whether this board's radio
   can reach the band, the power cap, and the duty-cycle budget (EU regions). */
static int region_state;        /* 0 usable, 1 the radio cannot reach the band, 2 preset wider than the band */
static bool duty_wait;          /* a send is waiting for duty-cycle budget */
static char lbl[8][24];
static void region_check(void)
{
    const mt_region_t *r = &MT_REGIONS[cfg.region];
    region_state = freq_hz ? 0 : 2;
    if (!region_state && radio >= 0) {
        /* the LR2021 reports its ranges (sub-GHz and 2.4 GHz); the SX1262 reports none: 150-960 MHz */
        lap_radio_info2_t in;
        memset(&in, 0, sizeof(in));
        const uint64_t lo = (uint64_t)r->lo_khz * 1000u, hi = (uint64_t)r->hi_khz * 1000u;
        bool ok = false;
        if (lap_radio_info2(radio, &in, sizeof(in)) == 0 && in.frequency_count > 0) {
            for (int i = 0; i < in.frequency_count && i < LAP_RADIO_RANGES_MAX; i++)
                if (lo >= in.frequencies[i].min_hz && hi <= in.frequencies[i].max_hz) ok = true;
        } else ok = lo >= 150000000u && hi <= 960000000u;
        if (!ok) region_state = 1;
    }
}
/* The region's cap, passed only by a licensed (ham) node, as the firmware does. */
static int eff_power(void)
{
    const int lim = MT_REGIONS[cfg.region].power_dbm;
    return cfg.power_dbm < lim || ext.licensed ? cfg.power_dbm : lim;
}
static void apply_channel(void)
{
    snprintf(cfg.channel, sizeof(cfg.channel), "%s", chans[0].name);   /* 1.2 reads the primary's name here */
    for (int c = 0; c < NCH; c++) {
        /* licensed (ham) operation sends in the clear: every key is "none" */
        crt[c].key_len = ext.licensed ? 0 : mt_channel_key(chans[c].psk, chans[c].psk_len, crt[c].key);
        crt[c].hash = mt_channel_hash(ch_name(c), crt[c].key, crt[c].key_len);
    }
    if (!ch_on(send_ch)) send_ch = 0;
    if (conv_kind == CONV_CH && !ch_on(conv_ch)) conv_kind = CONV_ALL;
    const mt_lora_t l = lora_now();
    freq_hz = mt_lora_frequency(&l, chans[0].name, &slot, &slots);
    region_check();
}

static void key_text(int c, char *out, int cap)
{
    if (chans[c].psk_len == 0 || (chans[c].psk_len == 1 && chans[c].psk[0] == 0)) snprintf(out, (size_t)cap, "none");
    else mt_b64_encode(chans[c].psk, chans[c].psk_len, out, cap);
}

static lap_packet_config_t lora_cfg(void)
{
    const mt_lora_t l = lora_now();
    uint8_t sf, cr;
    mt_lora_sf_cr(&l, &sf, &cr);
    return (lap_packet_config_t){ .size = sizeof(lap_packet_config_t), .version = 1,
        .frequency_hz = freq_hz, .bandwidth_hz = mt_lora_bw_hz(&l), .sync_word = MT_SYNC_WORD,
        .preamble = MT_PREAMBLE, .modulation = LAP_MOD_LORA, .sf = sf, .coding_rate = cr,
        .crc = 1, .payload_bytes = 0, .power_dbm = eff_power() };
}

/* Time on air of one packet (explicit header, CRC on). */
static uint32_t airtime_ms(int len)
{
    const mt_lora_t l = lora_now();
    uint8_t sf, crd;
    mt_lora_sf_cr(&l, &sf, &crd);
    const float tsym = (float)(1u << sf) * 1000.0f / (float)mt_lora_bw_hz(&l);
    const int de = tsym > 16.0f, cr = crd > 4 ? crd - 4 : 1;
    int n = (int)ceilf((8.0f * len - 4.0f * sf + 28 + 16) / (4.0f * (sf - 2 * de)));
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

/* Duty cycle (EU_433, EU_868 10 %, UA_868 1 %): own airtime of the last hour
   plus this packet must stay under the region's share. */
static bool duty_ok(int len)
{
    const int d = MT_REGIONS[cfg.region].duty;
    if (d >= 100 || ext.duty_override) return true;
    util_roll();
    uint32_t sum = 0;
    for (int i = 0; i < 6; i++) sum += tx_busy[i];
    return (uint64_t)(sum + airtime_ms(len)) * 100u <= (uint64_t)d * 3600000u;
}

/* SCAN: the preset being scanned, receive only. */
static lap_packet_config_t scan_cfg(void)
{
    const mt_scan_entry_t *e = &scan.e[scan.cur];
    return (lap_packet_config_t){ .size = sizeof(lap_packet_config_t), .version = 1,
        .frequency_hz = e->freq_hz, .bandwidth_hz = e->bw_hz, .sync_word = MT_SYNC_WORD,
        .preamble = e->preamble, .modulation = LAP_MOD_LORA, .sf = e->sf, .coding_rate = e->cr,
        .crc = 1, .payload_bytes = 0, .power_dbm = 0 };
}
static bool scanning(void) { return scan_on && scan.cur >= 0 && scan.cur < scan.n; }

static void rx_close(void) { if (rxh > 0) lap_packet_close(rxh); rxh = 0; }
static void rx_open(void)
{
    if (radio < 0 || rxh > 0 || txh > 0) return;
    if (scanning() ? region_state == 1 : region_state != 0) return;
    lap_packet_config_t c = scanning() ? scan_cfg() : lora_cfg();
    rxh = lap_packet_open(radio, &c);
    static int last_err;
    if (rxh < 0) {
        if (rxh != last_err) lap_log("receive refused (%d) on radio %d", rxh, radio);
        last_err = rx_error = rxh; rxh = 0;
    } else { rx_error = last_err = 0; }
}
static void retune(void) { apply_channel(); rx_close(); rx_open(); }

/* SCAN: the plan for the node's region (made again when the region changes). */
static void scan_plan(void)
{
    if (scan.n && scan.region == cfg.region) return;
    const bool was = scan_on;
    mt_scan_init(&scan, cfg.region, SCAN_DWELL_S[scan_dwell % 5] * 1000u);
    scan_sel = 0;
    if (was && mt_scan_start(&scan, lap_millis()) < 0) scan_on = false;
}
static void scan_go(bool on)
{
    if (on == scan_on) return;
    const uint32_t now = lap_millis();
    if (on) {
        if (radio < 0 || region_state == 1) { say("no LoRa radio for %s", MT_REGIONS[cfg.region].name); return; }
        scan_plan();
        if (mt_scan_start(&scan, now) < 0) { say("no preset fits %s", MT_REGIONS[cfg.region].name); return; }
        scan_on = true;
        say("scanning %d presets on %s", scan.n, MT_REGIONS[cfg.region].name);
    } else {
        mt_scan_stop(&scan, now);
        scan_on = false;
        say("scan stopped, back on %s", preset_name());
    }
    if (txh <= 0) { rx_close(); rx_open(); }   /* a send in flight reopens when it ends */
}
static void scan_pin(int entry)
{
    scan_plan();
    if (entry >= 0 && entry < scan.n && scan.pin == entry) entry = -1;   /* again: cycle */
    const int t = mt_scan_pin(&scan, entry, lap_millis());
    if (t >= 0 && txh <= 0) { rx_close(); rx_open(); }
    if (entry >= 0) say("listening on %s only", MT_SCAN_PRESETS[scan.e[entry].preset].name);
    else say("cycling all presets again");
}
/* poll(): the dwell clock, and a new preset when it is time. */
static void scan_service(void)
{
    if (scan.region != cfg.region) {            /* LORA > REGION changed under the scan */
        scan_plan();
        if (txh <= 0) { rx_close(); rx_open(); }
        return;
    }
    if (mt_scan_tick(&scan, lap_millis()) >= 0 && txh <= 0) { rx_close(); rx_open(); }
}

static void find_radio(void)
{
    lap_caps_t caps;
    if (lap_caps(&caps, sizeof(caps)) < 0 || !(caps.hardware & LAP_HW_PACKET)) return;
    for (int i = 0; i < caps.radio_count; i++) {
        lap_radio_modes_t m;
        if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA)) { radio = i; region_check(); return; }
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

/* The node list as saved: 84 bytes a node, 48 nodes inside one 4 KB save
   ("nodes3"; 1.2 saved 76 bytes a node as "nodes2", read once). */
typedef struct {
    uint32_t num, heard;            /* heard: unix time, 0 unknown */
    int32_t lat, lon;
    uint8_t flags, battery, hops, ch;
    uint8_t hw, role, flags2, pad;
    char short_name[5], long_name[23];
    uint8_t pubkey[32];
} rec_t;
typedef struct {
    uint32_t num;
    int32_t lat, lon;
    uint8_t flags, battery, hops, ch;
    char short_name[5], long_name[23];
    uint8_t pubkey[32];
} rec2_t;
enum { RF_POS = 1, RF_NAMED = 2, RF_KEY = 4, RF_DEV = 8, RF_FAV = 16, RF_IGNORE = 32, RF_LICENSED = 64, RF_SF = 128 };
static union { rec_t r3[MAX_NODES]; rec2_t r2[MAX_NODES]; } recs;

static int nodes_write(void)
{
    for (int i = 0; i < node_n; i++) {
        const node_t *n = &nodes[i];
        rec_t *r = &recs.r3[i];
        memset(r, 0, sizeof(*r));
        r->num = n->num; r->heard = n->heard_unix; r->lat = (int32_t)(n->lat * 1e7); r->lon = (int32_t)(n->lon * 1e7);
        r->flags = (uint8_t)((n->has_pos ? RF_POS : 0) | (n->named ? RF_NAMED : 0) | (n->has_key ? RF_KEY : 0) |
                             (n->has_dev ? RF_DEV : 0) | (n->fav ? RF_FAV : 0) | (n->ignored ? RF_IGNORE : 0) |
                             (n->licensed ? RF_LICENSED : 0) | (n->sf_router ? RF_SF : 0));
        r->battery = n->battery; r->hops = n->hops; r->ch = n->ch; r->hw = n->hw; r->role = n->role;
        snprintf(r->short_name, sizeof(r->short_name), "%s", n->short_name);
        snprintf(r->long_name, sizeof(r->long_name), "%s", n->long_name);
        if (n->has_key) memcpy(r->pubkey, n->pubkey, 32);
    }
    return lap_save("nodes3", recs.r3, sizeof(rec_t) * (size_t)node_n);
}
static void node_from(node_t *n, uint32_t num, int32_t lat, int32_t lon, uint8_t flags, uint8_t battery, uint8_t hops,
                      uint8_t ch, const char *sn, const char *ln, const uint8_t *key)
{
    memset(n, 0, sizeof(*n));
    n->num = num; n->lat = lat * 1e-7; n->lon = lon * 1e-7;
    n->has_pos = flags & RF_POS; n->named = flags & RF_NAMED; n->has_key = flags & RF_KEY;
    n->has_dev = flags & RF_DEV; n->fav = flags & RF_FAV; n->ignored = flags & RF_IGNORE;
    n->licensed = flags & RF_LICENSED; n->sf_router = flags & RF_SF;
    n->battery = battery; n->hops = hops; n->ch = ch < NCH ? ch : 0;
    memcpy(n->short_name, sn, 5); n->short_name[5] = 0;
    memcpy(n->long_name, ln, 23); n->long_name[23] = 0;
    if (n->has_key) memcpy(n->pubkey, key, 32);
}
static void nodes_load(void)
{
    node_n = 0;
    int got = lap_load("nodes3", recs.r3, sizeof(recs.r3));
    if (got > 0 && got % (int)sizeof(rec_t) == 0) {
        for (int i = 0; i < got / (int)sizeof(rec_t) && i < MAX_NODES; i++) {
            const rec_t *r = &recs.r3[i];
            node_t *n = &nodes[node_n++];
            node_from(n, r->num, r->lat, r->lon, r->flags, r->battery, r->hops, r->ch, r->short_name, r->long_name, r->pubkey);
            n->heard_unix = r->heard; n->hw = r->hw; n->role = r->role;
            publish(n);
        }
        return;
    }
    got = lap_load("nodes2", recs.r2, sizeof(recs.r2));
    for (int i = 0; got > 0 && i < got / (int)sizeof(rec2_t) && i < MAX_NODES; i++) {
        const rec2_t *r = &recs.r2[i];
        node_t *n = &nodes[node_n++];
        node_from(n, r->num, r->lat, r->lon, r->flags & 15, r->battery, r->hops, r->ch, r->short_name, r->long_name, r->pubkey);
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
/* BACKUP TO CARD writes every setting (and the node's keys) in one file;
   RESTORE FROM CARD reads it back. */
#define BACKUP_PATH "/sdcard/meshtastic/settings.bin"
#define BACKUP_MAGIC 0x4B42544Du    /* "MTBK" */
typedef struct {
    uint32_t magic, version;
    cfg_t cfg; ext_t ext; chan_cfg_t chans[NCH]; ui_cfg_t ui; pki_t pki;
} backup_t;
static backup_t backup_buf;
static void restore_apply(void);
static char rlog[1536];
static int rlog_n;
static bool card_dir;               /* /sdcard/meshtastic asked for this run */
static void save_pump(void)
{
    static uint32_t last;
    if (save_job >= 0) {
        const int r = lap_sd_poll(save_job);
        if (r == LAP_SD_PENDING) return;
        if (save_what == SV_RESTORE) {
            if (r == (int)sizeof(backup_buf)) restore_apply();
            else say(r == LAP_SD_ENOENT ? "no backup on the card yet" : "could not read the backup (%d)", r);
        } else if (save_what == SV_BACKUP) {
            say(r >= 0 ? "settings saved to the card: meshtastic/settings.bin" : "backup failed (%d)", r);
        } else if (r < 0 && save_what != SV_RLOG) dirty |= save_what;
        save_job = -1;
    }
    if (!dirty || lap_millis() - last < 300) return;
    last = lap_millis();
    unsigned what = 0;
    int id = LAP_SD_EBUSY;
    if ((dirty & (SV_BACKUP | SV_RLOG)) && !card_dir) { card_dir = true; what = 0; id = lap_sd_mkdir("/sdcard/meshtastic"); }
    else if (dirty & SV_CFG) { what = SV_CFG; id = lap_save("cfg2", &cfg, sizeof(cfg)); }
    else if (dirty & SV_EXT) { what = SV_EXT; id = lap_save("ext", &ext, sizeof(ext)); }
    else if (dirty & SV_CHANS) { what = SV_CHANS; id = lap_save("chans8", chans, sizeof(chans)); }
    else if (dirty & SV_PKI) { what = SV_PKI; id = lap_save("pki", &pki, sizeof(pki)); }
    else if (dirty & SV_NODES) { what = SV_NODES; id = nodes_write(); }
    else if (dirty & SV_UI) { what = SV_UI; id = lap_save("ui", &ui, sizeof(ui)); }
    else if (dirty & SV_WPT) { what = SV_WPT; id = lap_save("wpts", wpts, sizeof(wpt_t) * (size_t)wpt_n); }
    else if (dirty & SV_HIST) { what = SV_HIST; id = lap_save("hist", &hist, HIST_HEAD + sizeof(hist_t) * hist.n); }
    else if (dirty & SV_BACKUP) {
        what = SV_BACKUP;
        memset(&backup_buf, 0, sizeof(backup_buf));
        backup_buf.magic = BACKUP_MAGIC; backup_buf.version = 1;
        backup_buf.cfg = cfg; backup_buf.ext = ext; memcpy(backup_buf.chans, chans, sizeof(chans));
        backup_buf.ui = ui; backup_buf.pki = pki;
        id = lap_sd_write(BACKUP_PATH, 0, &backup_buf, sizeof(backup_buf));
    } else if (dirty & SV_RESTORE) { what = SV_RESTORE; id = lap_sd_read(BACKUP_PATH, 0, &backup_buf, sizeof(backup_buf)); }
    else if (dirty & SV_RLOG) {
        what = SV_RLOG;
        id = rlog_n ? lap_sd_write(range_file, LAP_SD_APPEND, rlog, (uint32_t)rlog_n) : LAP_SD_EINVAL;
        if (id >= 0 || !rlog_n) rlog_n = 0;
        if (id < 0) { dirty &= ~SV_RLOG; return; }
    }
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
        mt_lang_t l = mt_xlate_detect_in(m->text, xl_langs(), &conf);
        if (l > MT_LANG_EN && conf < MT_XLATE_MIN_CONF) l = MT_LANG_UNKNOWN;
        m->lang = (uint8_t)l;
        if (xl_ready(l) && ui.xl_auto) mt_xlate_to_en(l, m->text, m->tr, sizeof(m->tr));
        if (xl_on(l)) hist_add(m->from, m->text, m->tr);
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
static uint32_t reply_delay, send_reply, send_emoji;
static int send_on, send_hops = -1;
static bool send_plain, last_pki;

/* Public-key DMs carry text and the like; node info, position, acks and
   traceroutes always go on the channel, as the protocol has them. A
   licensed (ham) node sends nothing encrypted. */
static bool pki_on(void) { return cfg.pki && pki_ready && !ext.licensed; }
static bool pki_for(uint32_t dest, uint32_t port)
{
    if (!pki_on() || send_plain || dest == MT_BROADCAST) return false;
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
    const uint32_t delay = reply_delay, rid = send_reply, emo = send_emoji;
    const uint8_t hops = (uint8_t)(send_hops >= 0 ? send_hops : ext.hop_limit);
    reply_delay = 0; send_on = 0; send_plain = false; last_pki = false; send_reply = send_emoji = 0; send_hops = -1;
    if (!cfg.tx_on) { say("TRANSMIT is off: OPTIONS > TRANSMIT"); return 0; }
    if (radio < 0) { say("no LoRa radio"); return 0; }
    if (region_state) { say(region_state == 1 ? "%s is not a band of this radio" : "preset too wide for %s", MT_REGIONS[cfg.region].name); return 0; }
    txq_t *q = txq_free();
    if (!q) { say("send queue full"); return 0; }
    mt_header_t h = { dest, cfg.node, reuse_id ? reuse_id : lap_random(), hops, hops, want_ack, 0,
                      pk ? 0 : crt[c].hash, 0, (uint8_t)(cfg.node & 0xFF) };
    if (!h.id) h.id = 1;
    mt_header_pack(&h, q->frame);
    mt_data_t d = { port, payload, len, want_response, request_id, rid, emo };
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
    q->from = h.from; q->id = h.id; q->relay = false; q->keep = false; q->used = true;
    if (!reuse_id) seen_before(h.from, h.id);
    return h.id;
}

/* How the role repeats: 0 as a client (far first, near ones stand down when
   another is heard), 1 a router (at once, always), 2 late (after the rest,
   always: ROUTER_LATE, and CLIENT_BASE for its favourites). */
static int relay_style(const mt_header_t *h)
{
    switch (ext.role) {
    case MT_ROLE_ROUTER: case MT_ROLE_REPEATER: return 1;
    case MT_ROLE_ROUTER_LATE: return 2;
    case MT_ROLE_CLIENT_BASE: {
        const node_t *a = node_get(h->from, false), *b = node_get(h->dest, false);
        return (a && a->fav) || (b && b->fav) ? 2 : 0;
    }
    default: return 0;
    }
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
    const int style = relay_style(h);
    const uint32_t wait = style == 1 ? 60u + lap_random() % 200u
                        : 150u + (uint32_t)(s + 20) * 45u + (lap_random() % 300u) + (style == 2 ? 2200u : 0u);
    q->due_ms = lap_millis() + wait;
    q->from = h->from; q->id = h->id; q->relay = true; q->keep = style != 0; q->used = true;
}

static void cancel_relay(uint32_t from, uint32_t id)
{
    for (int i = 0; i < TXQ; i++)
        if (txq[i].used && txq[i].relay && !txq[i].keep && txq[i].from == from && txq[i].id == id && i != tx_slot)
            txq[i].used = false;
}

/* Whether this node repeats a packet, by role and rebroadcast mode.
   decoded: it was on one of this node's channels; port: what it carried. */
static bool may_relay(const node_t *from, bool decoded, uint32_t port)
{
    if (ext.role == MT_ROLE_CLIENT_MUTE || ext.rebroadcast == MT_REBC_NONE) return false;
    const int mode = ext.role == MT_ROLE_CLIENT_HIDDEN ? MT_REBC_LOCAL_ONLY : ext.rebroadcast;
    switch (mode) {
    case MT_REBC_LOCAL_ONLY: return decoded;
    case MT_REBC_KNOWN_ONLY: return decoded && from && from->named;
    case MT_REBC_CORE_PORTNUMS_ONLY:
        return decoded && (port == MT_PORT_TEXT || port == MT_PORT_POSITION || port == MT_PORT_NODEINFO ||
                           port == MT_PORT_ROUTING || port == MT_PORT_TELEMETRY || port == MT_PORT_TRACEROUTE);
    default: return true;
    }
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
    u.role = ext.role;
    u.licensed = ext.licensed != 0;
    if (pki_on()) { memcpy(u.public_key, pki.pub, 32); u.has_key = true; }
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

/* On the channel it goes out on, at that channel's precision (none: not sent). */
static bool send_position_ask(uint32_t dest, uint32_t request_id, bool ask)
{
    double lat, lon; int32_t alt; uint32_t t;
    const int c = ch_on(send_on) ? send_on : 0;
    const int prec = chans[c].precision;
    uint8_t buf[48];
    int n = 0;
    if (cfg.pos_mode && prec && my_position(&lat, &lon, &alt, &t))
        n = mt_position_encode(lat, lon, alt, t, prec, buf, sizeof(buf));
    else if (!ask) { send_on = 0; return false; }      /* a request may go empty */
    if (n < 0) { send_on = 0; return false; }
    if (!send_packet(dest, MT_PORT_POSITION, buf, (size_t)n, false, ask, request_id, 0)) return false;
    if (n > 0 && !ask) { last_pos_ms = lap_millis(); smart_lat = lat; smart_lon = lon; smart_have = true; }
    return true;
}
static bool send_position(uint32_t dest, uint32_t request_id) { return send_position_ask(dest, request_id, false); }

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

/* Text to a node (a DM, acked) or everyone on channel ch; reply_id names
   the message it answers. */
static void send_text_on(uint32_t dest, int ch, const char *text, uint32_t reply_id)
{
    const size_t len = strlen(text);
    if (!len) return;
    if (!last_nodeinfo_ms && !role_quiet()) send_nodeinfo(true);   /* introduce ourselves first */
    const bool dm = dest != MT_BROADCAST;
    const node_t *to = dm ? node_get(dest, false) : NULL;
    const int c = dm ? (to ? to->ch : 0) : ch;
    send_on = c;
    send_reply = reply_id;
    const uint32_t id = send_packet(dest, MT_PORT_TEXT, (const uint8_t *)text, len, dm, false, 0, 0);
    if (!id) return;
    msg_t *m = msg_add(cfg.node, dest, id, text, len, INT16_MIN, INT16_MIN, 0, true);
    m->state = ST_QUEUED;
    m->tries = 1;
    m->ch = (uint8_t)c;
    m->pki = last_pki;
    m->reply_id = reply_id;
    say(dm ? (last_pki ? "sending to %s (private)" : "sending to %s") : "sending", who(dest));
}

static void resend(msg_t *m, bool plain)
{
    /* A public-key DM that got no ack, or a refusal, goes once more on the
       channel key: under a new id, since the other side may have marked the
       old one as seen. */
    if (plain) send_plain = true;
    send_on = m->ch;
    send_reply = m->reply_id;
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

/* ---- reactions, waypoints, neighbours, range test, store and forward ---- */
static uint32_t unix_now(void)
{
    lap_time_t t;
    lap_time(&t);
    return t.synced ? (uint32_t)t.unix : 0;
}

/* A tapback's emoji as the board's font can show it. */
static const char *react_label(const char *g)
{
    static const struct { const char *u, *a; } T[] = {
        { "\xf0\x9f\x91\x8d", "+1" }, { "\xf0\x9f\x91\x8e", "-1" }, { "\xe2\x9d\xa4", "<3" },
        { "\xf0\x9f\x98\x82", "LOL" }, { "\xf0\x9f\xa4\xa3", "LOL" }, { "\xe2\x80\xbc", "!!" }, { "\xe2\x9d\x93", "??" },
        { "\xf0\x9f\x92\xa9", "POO" }, { "\xf0\x9f\x98\xae", "WOW" }, { "\xf0\x9f\x98\xa2", "SAD" },
        { "\xf0\x9f\x8e\x89", "YAY" }, { "\xf0\x9f\x94\xa5", "HOT" }, { "\xe2\x9c\x85", "OK" }, { "\xf0\x9f\x91\x80", "EYES" },
        { "\xf0\x9f\x99\x8f", "TY" }, { "\xf0\x9f\x98\x80", ":D" }, { "\xf0\x9f\x98\x8a", ":)" }, { "\xf0\x9f\x91\x8b", "HI" },
    };
    for (int i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++)
        if (!strncmp(g, T[i].u, strlen(T[i].u))) return T[i].a;
    return ((uint8_t)g[0] < 0x80 && g[0]) ? g : "*";
}
static void react_add(uint32_t target, uint32_t from, const uint8_t *g, size_t n)
{
    for (int i = 0; i < MAX_REACT; i++)            /* one reaction a node a message: the newest */
        if (reacts[i].target == target && reacts[i].from == from) { reacts[i].target = 0; break; }
    react_t *r = &reacts[react_head];
    react_head = (react_head + 1) % MAX_REACT;
    r->target = target; r->from = from;
    const size_t k = n < sizeof(r->glyph) - 1 ? n : sizeof(r->glyph) - 1;
    memcpy(r->glyph, g, k);
    r->glyph[k] = 0;
    chat_new = true;
}

static void wpt_publish(const wpt_t *w, bool show)
{
    lap_map_node_t m;
    memset(&m, 0, sizeof(m));
    m.size = sizeof(m); m.version = 1; m.id = w->id;
    snprintf(m.name, sizeof(m.name), "WP %s", w->name);
    snprintf(m.short_name, sizeof(m.short_name), "WP");
    m.has_pos = show; m.lat = w->lat * 1e-7; m.lon = w->lon * 1e-7;
    m.rssi_tenths = INT16_MIN; m.snr_tenths = INT16_MIN;
    lap_map_node(&m);
}
static void wpt_drop(int i)
{
    wpt_publish(&wpts[i], false);
    memmove(&wpts[i], &wpts[i + 1], sizeof(wpt_t) * (size_t)(wpt_n - i - 1));
    wpt_n--;
    dirty |= SV_WPT;
}
/* A waypoint heard (or sent): an expiry in the past removes it, as upstream deletes one. */
static void wpt_add(const mt_waypoint_t *w, uint32_t from)
{
    const uint32_t now = unix_now();
    int at = -1;
    for (int i = 0; i < wpt_n; i++) if (wpts[i].id == w->id) at = i;
    if (w->expire && now && w->expire <= now) { if (at >= 0) wpt_drop(at); return; }
    if (!w->has_pos) return;
    if (at < 0) {
        if (wpt_n == MAX_WPT) wpt_drop(0);          /* full: the oldest goes */
        at = wpt_n++;
    }
    wpt_t *p = &wpts[at];
    memset(p, 0, sizeof(*p));
    p->id = w->id; p->from = from; p->expire = w->expire; p->icon = w->icon;
    p->lat = (int32_t)(w->lat * 1e7); p->lon = (int32_t)(w->lon * 1e7);
    snprintf(p->name, sizeof(p->name), "%s", w->name[0] ? w->name : "waypoint");
    snprintf(p->desc, sizeof(p->desc), "%s", w->desc);
    wpt_publish(p, true);
    dirty |= SV_WPT;
}
static void wpt_expire(void)
{
    const uint32_t now = unix_now();
    for (int i = wpt_n - 1; now && i >= 0; i--) if (wpts[i].expire && wpts[i].expire <= now) wpt_drop(i);
}
static void wpt_load(void)
{
    const int got = lap_load("wpts", wpts, sizeof(wpts));
    wpt_n = got > 0 && got % (int)sizeof(wpt_t) == 0 ? got / (int)sizeof(wpt_t) : 0;
    for (int i = 0; i < wpt_n; i++) { wpts[i].name[31] = 0; wpts[i].desc[63] = 0; wpt_publish(&wpts[i], true); }
}

/* What a node says it hears directly. */
static void nbr_store(const mt_neighbors_t *nb)
{
    node_t *o = node_get(nb->node, true);
    o->nbr_n = 0;
    for (int i = 0; i < nb->n && o->nbr_n < NBR_N; i++) {
        if (!nb->id[i] || nb->id[i] == nb->node) continue;
        const float q = nb->snr[i] * 4.0f;
        o->nbr[o->nbr_n] = nb->id[i];
        o->nbr_snr[o->nbr_n++] = (int8_t)(q < -127 ? -127 : q > 127 ? 127 : q);
    }
    o->nbr_ms = lap_millis() | 1;
}
/* Ours: the nodes heard straight (no hops) lately, best SNR first. */
static int my_neighbors(mt_neighbors_t *nb)
{
    memset(nb, 0, sizeof(*nb));
    nb->node = nb->last_sent_by = cfg.node;
    nb->interval_s = NBR_H[ext.nbr_every] * 3600u;
    const uint32_t window = (NBR_H[ext.nbr_every] ? NBR_H[ext.nbr_every] : 6) * 2u * 3600000u;
    for (int i = 0; i < node_n && nb->n < MT_NBR_MAX; i++) {
        const node_t *n = &nodes[i];
        if (!n->last_ms || n->hops || lap_millis() - n->last_ms > window || n->snr == INT16_MIN) continue;
        nb->id[nb->n] = n->num; nb->snr[nb->n] = (float)n->snr / 10.0f; nb->n++;
    }
    return nb->n;
}
static bool send_neighbors(uint32_t dest, uint32_t request_id)
{
    mt_neighbors_t nb;
    if (!my_neighbors(&nb)) return false;
    uint8_t buf[200];
    const int n = mt_neighbors_encode(&nb, buf, sizeof(buf));
    if (n <= 0 || !send_packet(dest, MT_PORT_NEIGHBORINFO, buf, (size_t)n, false, false, request_id, 0)) return false;
    last_nbr_ms = lap_millis();
    return true;
}

/* Range test, as upstream's module: "seq N" from the sender; the receiver
   writes a line a packet to the card, in upstream's columns. */
static void range_csv(const char *fmt, ...)
{
    if (rlog_n > (int)sizeof(rlog) - 200) return;      /* the card is behind: drop rather than block */
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(rlog + rlog_n, sizeof(rlog) - (size_t)rlog_n, fmt, ap);
    va_end(ap);
    if (w > 0) rlog_n += w < (int)sizeof(rlog) - rlog_n ? w : (int)sizeof(rlog) - rlog_n - 1;
    dirty |= SV_RLOG;
}
static bool my_position(double *lat, double *lon, int32_t *alt, uint32_t *t);
static float km_between(double la1, double lo1, double la2, double lo2);
static void range_heard(const mt_header_t *h, const node_t *nd, const mt_data_t *d, int16_t rssi, int16_t snr)
{
    char pl[40];
    const size_t k = d->payload_len < sizeof(pl) - 1 ? d->payload_len : sizeof(pl) - 1;
    memcpy(pl, d->payload, k);
    pl[k] = 0;
    for (char *c = pl; *c; c++) if (*c == '"' || *c == ',' || (uint8_t)*c < 32) *c = ' ';
    const uint32_t seq = !strncmp(pl, "seq ", 4) ? (uint32_t)strtoul(pl + 4, NULL, 10) : 0;
    double la = 0, lo = 0; int32_t alt = 0; uint32_t t = 0;
    const bool here = my_position(&la, &lo, &alt, &t);
    const float km = here && nd->has_pos ? km_between(la, lo, nd->lat, nd->lon) : -1.0f;
    if (range_rx_n == (int)(sizeof(range_rx) / sizeof(range_rx[0]))) memmove(range_rx, range_rx + 1, sizeof(range_rx) - sizeof(range_rx[0]));
    else range_rx_n++;
    range_rx[range_rx_n - 1] = (range_rx_t){ h->from, seq, lap_millis(), rssi, snr, km };
    if (seq && range_last_seq && seq > range_last_seq + 1 && seq - range_last_seq < 1000) range_lost += seq - range_last_seq - 1;
    if (seq) range_last_seq = seq;
    range_got++;
    if (!ext.range) return;
    if (!range_file[0]) {
        lap_time_t tm;
        lap_time(&tm);
        if (tm.synced) snprintf(range_file, sizeof(range_file), "/sdcard/meshtastic/rangetest-%04d%02d%02d-%02d%02d.csv",
                                tm.year, tm.month, tm.day, tm.hour, tm.minute);
        else snprintf(range_file, sizeof(range_file), "/sdcard/meshtastic/rangetest-%08x.csv", (unsigned)lap_random());
        range_csv("time,from,sender name,sender lat,sender long,rx lat,rx long,rx elevation,rx snr,distance,hop limit,payload,rx rssi\n");
    }
    lap_time_t tm;
    lap_time(&tm);
    char name[32];
    snprintf(name, sizeof(name), "%s", nd->long_name);
    for (char *c = name; *c; c++) if (*c == ',' || *c == '"') *c = ' ';
    range_csv("%02d:%02d:%02d,%u,%s,%.6f,%.6f,%.6f,%.6f,%d,%.2f,%.0f,%d,\"%s\",%.1f\n", tm.hour, tm.minute, tm.second,
              (unsigned)h->from, name, nd->has_pos ? nd->lat : 0.0, nd->has_pos ? nd->lon : 0.0, here ? la : 0.0,
              here ? lo : 0.0, (int)alt, snr == INT16_MIN ? 0.0 : snr / 10.0, km >= 0 ? km * 1000.0 : 0.0,
              (int)h->hop_limit, pl, rssi == INT16_MIN ? 0.0 : rssi / 10.0);
}
static void range_send(void)
{
    char t[24];
    const int n = snprintf(t, sizeof(t), "seq %u", (unsigned)++range_seq);
    send_hops = 0;                                   /* upstream: hop limit 0, the sender's own range */
    if (send_packet(MT_BROADCAST, MT_PORT_RANGE_TEST, (const uint8_t *)t, (size_t)n, false, false, 0, 0)) range_ms = lap_millis();
    else range_seq--;
}

/* Store and forward: ask a router for what it kept. Routers refuse on the
   default channel, so the ask goes on the channel the router was heard on. */
static void sf_ask(uint32_t router)
{
    const node_t *n = node_get(router, false);
    const int c = n ? n->ch : send_ch;
    if (ch_default(c)) { say("S&F routers answer only on a private channel"); return; }
    uint8_t buf[12];
    const int len = mt_sf_history_request(120, buf, sizeof(buf));
    send_on = c;
    if (len > 0 && send_packet(router, MT_PORT_STORE_FORWARD, buf, (size_t)len, false, false, 0, 0)) {
        sf_router = router; sf_ms = lap_millis(); sf_expect = sf_got = 0;
        say("asked %s for the last 2 hours", who(router));
    }
}
static msg_t *msg_add(uint32_t from, uint32_t to, uint32_t id, const char *text, size_t len,
                      int16_t rssi, int16_t snr, uint8_t hops, bool mine);
static void notify_text(msg_t *m, node_t *nd, bool dm, bool quiet);
static void sf_packet(const mt_header_t *h, const mt_sf_t *sf, node_t *nd, int ch, int16_t rssi, int16_t snr, uint8_t hops)
{
    switch (sf->rr) {
    case MT_SF_ROUTER_HEARTBEAT: case MT_SF_ROUTER_PONG:
        if (!nd->sf_router) { nd->sf_router = true; nodes_dirty(); }
        break;
    case MT_SF_ROUTER_PING:
        if (h->dest == cfg.node && cfg.tx_on) {
            const uint8_t pong[2] = { 0x08, MT_SF_CLIENT_PONG };
            reply_delay = 700u + lap_random() % 500u; send_on = ch;
            send_packet(h->from, MT_PORT_STORE_FORWARD, pong, 2, false, false, 0, 0);
        }
        break;
    case MT_SF_ROUTER_HISTORY:
        nd->sf_router = true;
        sf_expect = sf->history_messages;
        say("%s sends %u stored message%s", who(h->from), (unsigned)sf_expect, sf_expect == 1 ? "" : "s");
        break;
    case MT_SF_ROUTER_BUSY: case MT_SF_ROUTER_ERROR:
        say("S&F router %s is busy: try again soon", who(h->from));
        break;
    case MT_SF_ROUTER_TEXT_DIRECT: case MT_SF_ROUTER_TEXT_BROADCAST: {
        if (!sf->text || !sf->text_len) break;
        /* the header names the original sender and packet; skip what is already here */
        for (int k = 0; k < msg_n; k++) {
            const msg_t *o = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
            if (o->from == h->from && o->id == h->id) return;
        }
        msg_t *m = msg_add(h->from, sf->rr == MT_SF_ROUTER_TEXT_DIRECT ? cfg.node : MT_BROADCAST, h->id,
                           (const char *)sf->text, sf->text_len, rssi, snr, hops, false);
        m->ch = (uint8_t)ch;
        m->via_sf = 1;
        sf_got++;
        notify_text(m, nd, sf->rr == MT_SF_ROUTER_TEXT_DIRECT, true);
        break;
    }
    default: break;
    }
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

/* Whether CHAT is showing this message's conversation right now. */
static bool conv_shows(const msg_t *m);
static int unread_total(void)
{
    int n = 0;
    for (int c = 0; c < NCH; c++) n += unread_ch[c];
    for (int i = 0; i < node_n; i++) n += nodes[i].unread;
    return n;
}
/* A new message: counted unread unless it is on screen, and an alert by
   NOTIFY (every message, direct only, off; a muted channel never alerts):
   the board's chirp and tick, the banner, and speech when asked. */
static void notify_text(msg_t *m, node_t *nd, bool dm, bool quiet)
{
    const bool seen = lap_screen_open() && tab == TAB_CHAT && !conv_list && !share_open && conv_shows(m);
    if (!seen) {
        if (dm) { if (nd->unread < 250) nd->unread++; }
        else if (unread_ch[m->ch % NCH] < 999) unread_ch[m->ch % NCH]++;
        unread = (uint32_t)unread_total();
    }
    const bool alert = ext.notify == 0 ? (dm || !chans[m->ch % NCH].mute) : ext.notify == 1 ? dm : false;
    const bool en = m->tr[0] != 0;
    if ((ui.speak == 2 || (ui.speak == 1 && en)) && !quiet && (dm || !chans[m->ch % NCH].mute)) {
        char t[160];
        snprintf(t, sizeof(t), "%s%s says: %.120s", dm ? "Direct. " : "", nd->long_name[0] ? nd->long_name : who(m->from),
                 en ? m->tr : scr(m->text, m->lang));
        lap_say(t);
    }
    if (!alert || quiet) return;
    lap_sound(LAP_SOUND_CHIRP);
    lap_haptic(dm ? LAP_HAPTIC_CONFIRM : LAP_HAPTIC_TICK);
    if (!seen) {
        char t[96];
        char tag[12] = "";
        if (en && m->lang < MT_LANG_COUNT) snprintf(tag, sizeof(tag), "(%s>EN) ", LANG_UP[m->lang]);
        snprintf(t, sizeof(t), "%s%s%s: %.70s", dm ? "DM " : "", tag, who(m->from), en ? m->tr : scr(m->text, m->lang));
        lap_notify("MESHTASTIC", t);
    }
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
    { const uint32_t u = unix_now(); if (u) nd->heard_unix = u; }
    rx_from = h.from;
    if (rssi != INT16_MIN) {
        const int dbm = rssi / 10 < -127 ? -127 : rssi / 10 > 0 ? 0 : rssi / 10;
        if (nd->sig_n == SIG_N) { memmove(nd->sig, nd->sig + 1, SIG_N - 1); nd->sig_n--; }
        nd->sig[nd->sig_n++] = (int8_t)dbm;
    }
    if (!dup || rssi > nd->rssi) { nd->rssi = rssi; nd->snr = snr; nd->hops = hops; }
    const bool to_me = h.dest == cfg.node;
    if (nd->ignored) {                              /* as upstream: dropped, not repeated */
        air_add(&h, meta, "IGNORED");
        return;
    }
    if (dup) {
        /* A retry addressed to us: our ack was lost, send it again. */
        if (h.want_ack && to_me && cfg.tx_on) ack_to(h.from, h.id, nd->ch);
        air_add(&h, meta, "REPEAT");
        return;
    }
    const bool can_relay = cfg.tx_on && h.hop_limit > 0 && !to_me;
    static uint8_t raw[MT_HEADER_LEN + MT_MAX_PAYLOAD + 16];
    const int rn = n > (int)sizeof(raw) ? (int)sizeof(raw) : n;
    memcpy(raw, buf, (size_t)rn);

    uint8_t *pl = buf + MT_HEADER_LEN;
    size_t pn = (size_t)n - MT_HEADER_LEN;
    mt_data_t d;
    int ch = -1;
    bool via_pki = false;
    static uint8_t plain[256];
    if (to_me && h.channel == 0 && pn > MT_PKI_OVERHEAD && pki_on()) {
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
        if (can_relay && may_relay(nd, false, 0)) queue_relay(raw, rn, &h, snr);   /* the still encrypted bytes */
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
    if (can_relay && may_relay(nd, true, d.portnum)) {
        mt_route_t r;
        if (d.portnum == MT_PORT_TRACEROUTE && !via_pki && mt_route_decode(d.payload, d.payload_len, &r)) {
            route_update(&r, &h, snr, d.request_id == 0, false);
            uint8_t rb[96], frame[MT_HEADER_LEN + MT_MAX_PAYLOAD];
            const int rl = mt_route_encode(&r, rb, sizeof(rb));
            mt_data_t nd2 = { d.portnum, rb, (size_t)(rl < 0 ? 0 : rl), d.want_response, d.request_id, 0, 0 };
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
        if (h.want_ack && to_me && cfg.tx_on) ack_to(h.from, h.id, ch);
        if (d.emoji && d.reply_id) {                    /* a tapback on a message */
            react_add(d.reply_id, h.from, d.payload, d.payload_len);
        } else {
            msg_t *m = msg_add(h.from, h.dest, h.id, (const char *)d.payload, d.payload_len, rssi, snr, hops, false);
            m->ch = (uint8_t)ch;
            m->pki = via_pki;
            m->reply_id = d.reply_id;
            notify_text(m, nd, to_me, false);
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
            nd->hw = (uint8_t)(u.hw_model > 255 ? 0 : u.hw_model);
            nd->role = (uint8_t)(u.role > 255 ? 0 : u.role);
            nd->licensed = u.licensed;
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
    } else if (d.portnum == MT_PORT_WAYPOINT && (h.dest == MT_BROADCAST || to_me)) {
        mt_waypoint_t w;
        if (mt_waypoint_decode(d.payload, d.payload_len, &w)) {
            const bool fresh = !(w.expire && unix_now() && w.expire <= unix_now());
            wpt_add(&w, h.from);
            if (fresh && w.has_pos && ext.notify == 0) {
                char t[64];
                snprintf(t, sizeof(t), "waypoint %.30s from %s", w.name, who(h.from));
                lap_notify("MESHTASTIC", t);
            }
        }
    } else if (d.portnum == MT_PORT_NEIGHBORINFO) {
        mt_neighbors_t nb;
        if (mt_neighbors_decode(d.payload, d.payload_len, &nb) && !(nb.n == 1 && nb.id[0] == 0 && nb.snr[0] == 0.0f))
            nbr_store(&nb);
        if (d.want_response && to_me && cfg.tx_on && lap_millis() - last_nbr_ms > 180000u) {
            reply_delay = 800u + lap_random() % 600u; send_on = ch; send_neighbors(h.from, h.id);
        }
    } else if (d.portnum == MT_PORT_RANGE_TEST && h.from != cfg.node) {
        range_heard(&h, nd, &d, rssi, snr);
    } else if (d.portnum == MT_PORT_STORE_FORWARD) {
        mt_sf_t sf;
        if (mt_sf_decode(d.payload, d.payload_len, &sf)) sf_packet(&h, &sf, nd, ch, rssi, snr, hops);
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
    if (scan_on) return;                        /* receive only while SCAN runs: sends wait */
    const uint32_t now = lap_millis();
    const bool quiet = role_quiet();
    if (nodeinfo_due) { nodeinfo_due = false; reply_delay = 900u + lap_random() % 1200u; send_nodeinfo(false); }
    if (cfg.tx_on && !quiet && last_nodeinfo_ms && now - last_nodeinfo_ms > NI_MIN[ext.ni_every % 5] * 60000u)
        send_nodeinfo(false);
    /* position: on its interval (a TRACKER twice as often), and with SMART
       when it has moved far enough, not sooner than the smart time */
    uint32_t pe = POS_MIN[cfg.pos_every % 4] * 60000u;
    if (ext.role == MT_ROLE_TRACKER || ext.role == MT_ROLE_TAK_TRACKER) pe /= 2;
    if (cfg.tx_on && cfg.pos_mode && !quiet && now - last_pos_ms > pe) {
        if (!send_position(MT_BROADCAST, 0)) last_pos_ms = now - pe + 60000u;
    } else if (cfg.tx_on && cfg.pos_mode && !quiet && (ext.smart || ext.role == MT_ROLE_TRACKER) && smart_have &&
               now - last_pos_ms > (uint32_t)ext.smart_s * 1000u) {
        lap_gps_t g;
        if (lap_gps(&g) && g.fix && km_between(smart_lat, smart_lon, g.lat, g.lon) * 1000.0f >= (float)ext.smart_m)
            send_position(MT_BROADCAST, 0);
    }
    /* telemetry: on its interval (a SENSOR at least every 30 minutes) */
    uint32_t te = TELE_MIN[cfg.tele % 4] * 60000u;
    if (ext.role == MT_ROLE_SENSOR && (!te || te > 1800000u)) te = 1800000u;
    if (cfg.tx_on && te && !quiet && last_nodeinfo_ms && now - last_tele_ms > te && now > 120000u) {
        if (!send_telemetry(MT_BROADCAST, 0)) last_tele_ms = now - te + 60000u;
    }
    /* LOST_AND_FOUND: where it is, as a text on the primary, each position interval */
    if (cfg.tx_on && ext.role == MT_ROLE_LOST_AND_FOUND && now - last_lost_ms > (pe < 900000u ? pe : 900000u) && now > 60000u) {
        double la, lo; int32_t alt; uint32_t t;
        last_lost_ms = now;
        if (my_position(&la, &lo, &alt, &t)) {
            char m[80];
            snprintf(m, sizeof(m), "I'm lost! Position: %.5f, %.5f", la, lo);
            send_text_on(MT_BROADCAST, 0, m, 0);
        }
    }
    /* neighbour info, never on the default channel and slot (as upstream) */
    const uint32_t ne = NBR_H[ext.nbr_every % 5] * 3600000u;
    if (cfg.tx_on && ne && !quiet && !ch_default(0) && now - last_nbr_ms > ne && now > 300000u) {
        if (!send_neighbors(MT_BROADCAST, 0)) last_nbr_ms = now - ne + 600000u;
    }
    /* range test sender: "seq N" every RANGE_S seconds, off after 8 hours */
    if (ext.range >= 2 && cfg.tx_on) {
        if (!range_start_ms) range_start_ms = now ? now : 1;
        if (now - range_start_ms > 8u * 3600000u) {
            ext.range = 1; dirty |= SV_EXT; range_start_ms = 0;
            say("range test: 8 hours, sender off");
        } else if (now - range_ms > RANGE_S[ext.range] * 1000u) range_send();
    } else range_start_ms = 0;
    {
        static uint32_t wpt_ms;
        if (now - wpt_ms > 30000u) { wpt_ms = now; wpt_expire(); }
    }
    if (trace_id && now - trace_ms > TRACE_WAIT_MS) {
        trace_id = 0;
        snprintf(trace_fwd, sizeof(trace_fwd), "no answer from %s", who(trace_to));
    }
    ack_service();
    int pick = -1;
    for (int i = 0; i < TXQ; i++)
        if (txq[i].used && (int32_t)(now - txq[i].due_ms) >= 0 && (pick < 0 || txq[i].due_ms < txq[pick].due_ms)) pick = i;
    if (pick < 0 || radio < 0 || region_state) { duty_wait = false; return; }
    if (!duty_ok(txq[pick].len)) {          /* over the region's duty cycle: wait, a relay is dropped */
        duty_wait = true;
        if (txq[pick].relay) txq[pick].used = false;
        return;
    }
    duty_wait = false;
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
    if (fresh_install && !ui.region_set) {   /* a new install takes the region of where it is */
        lap_gps_t g;
        if (lap_gps(&g) && g.fix) {
            const int r = mt_region_guess(g.lat, g.lon);
            ui.region_set = 1; dirty |= SV_UI;
            if (r != cfg.region) { cfg.region = (uint8_t)r; dirty |= SV_CFG; retune(); say("region %s from GPS", MT_REGIONS[r].name); }
        }
    }
    if (want && gps_lease <= 0) gps_lease = lap_gps_acquire(0);
    else if (!want && gps_lease > 0) { lap_gps_release(gps_lease); gps_lease = 0; }
}

/* Radio work: from draw while the screen is open, from the service tick
   while it is not. */
static void xl_service(void)
{
    lang_pump();
    lf_pump();
    if (xl_fill_req) { xl_fill_req = false; xl_backfill(); }
    const int i = xl_msg_req;
    if (i >= 0 && i < MAX_MSGS) {
        xl_msg_req = -1;
        if (!msgs[i].tr[0] && xl_line(msgs[i].text, &msgs[i].lang, msgs[i].tr, sizeof(msgs[i].tr)) < 0) {
            say("%s", msgs[i].tr);
            msgs[i].tr[0] = 0;
        }
        hist_fill(msgs[i].text, msgs[i].tr);
    }
    const int hi = xl_hist_req;
    if (hi >= 0 && hi < (int)hist.n) {
        xl_hist_req = -1;
        if (!hist.e[hi].en[0]) {
            uint8_t l = MT_LANG_UNKNOWN;
            if (xl_line(hist.e[hi].es, &l, hist.e[hi].en, sizeof(hist.e[hi].en)) < 0) {
                say("%s", hist.e[hi].en);
                hist.e[hi].en[0] = 0;
            }
            dirty |= SV_HIST;
        }
    }
    if (xl_try_req) {
        xl_try_req = false;
        try_en[0] = 0;
        try_lang = MT_LANG_UNKNOWN;
        /* the notice (a language off or not on the card) shows in TRY IT but is not kept */
        if (try_es[0] && xl_line(try_es, &try_lang, try_en, sizeof(try_en)) > 0) hist_add(0, try_es, try_en);
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
            if (!scan_on) on_packet(buf, n, &meta);
            else if (mt_scan_heard(&scan, buf, n, meta.rssi_tenths_dbm, meta.snr_tenths_db, lap_millis()))
                last_rx_ms = lap_millis();          /* the header's figures flicker for it too */
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
                if (!queued_ms) queued_ms = lap_millis() ? lap_millis() : 1;
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
    if (scan_on) scan_service();
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
/* One tree, laid out as the official apps have it: NODE (user and device),
   LORA, POSITION, CHANNELS, MODULES, and the card backup. The board keeps
   at most six lists and 48 rows, so every list is a level under the root.
   Rows that show words the board copies when the list opens (a name, a
   role) are brought back fresh after they change: the list reopens with
   that level as its root and a "< ALL SETTINGS" row on top. Row ids are
   grouped by list (100s NODE ... 500s MODULES) so a dialog knows where it
   came from. */
enum {
    OPT_TX = 1, OPT_NODE_M, OPT_LORA_M, OPT_POS_M, OPT_CH_M, OPT_MOD_M, OPT_BACKUP, OPT_RESTORE, OPT_BACK,
    OPT_LONG = 100, OPT_SHORT, OPT_HAM, OPT_PKI, OPT_ROLE, OPT_REBC, OPT_NIEVERY, OPT_KEEP,
    OPT_REGION = 200, OPT_PRESET, OPT_CUSTOM, OPT_BW, OPT_SF, OPT_CR, OPT_HOPS, OPT_POWER, OPT_SLOT, OPT_OFFSET, OPT_DUTY,
    OPT_POS = 300, OPT_POSEVERY, OPT_SMART, OPT_SMART_M, OPT_SMART_S, OPT_FIXED,
    OPT_CH = 400, OPT_CH_NAME, OPT_CH_ROLE, OPT_CH_KEY, OPT_CH_MUTE, OPT_CH_PREC, OPT_SHARE, OPT_IMPORT,
    OPT_NOTIFY = 500, OPT_TELE, OPT_RANGE, OPT_NBR, OPT_XL_AUTO, OPT_QUICK, OPT_SPEAK,
    OPT_SCAN_ON = 600, OPT_SCAN_DWELL, OPT_SCAN_PIN, OPT_SCAN_CLEAR, OPT_SCAN_PRESETS, OPT_SCAN_P0 = 620,
};
#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))
enum { SUB_NODE, SUB_LORA, SUB_POS, SUB_CH, SUB_MOD, SUBS };
static const char *const SUB_NAME[SUBS] = { "NODE", "LORA", "POSITION", "CHANNELS", "MODULES" };
static const char *preset_names[8];
static const char *const EVERY_NAMES[] = { "15 MIN", "30 MIN", "1 HOUR", "2 HOURS" };
static const char *const TELE_NAMES[] = { "1 HOUR", "30 MIN", "3 HOURS", "OFF" };
static const char *const KEEP_NAMES[] = { "WHEN PRIMARY", "ALWAYS", "NEVER" };
static const char *const QUICK_NAMES[] = { "ENGLISH", "SPANISH", "PORTUGUESE", "FRENCH", "GERMAN", "ITALIAN", "DUTCH",
                                            "POLISH", "RUSSIAN", "UKRAINIAN" };
#define QUICKS ((int)(sizeof(QUICK_NAMES) / sizeof(QUICK_NAMES[0])))
static const char *const SPEAK_NAMES[] = { "OFF", "TRANSLATIONS", "EVERY MESSAGE" };
static const char *const REBC_NAMES[] = { "ALL", "ALL NO DECODE", "LOCAL ONLY", "KNOWN ONLY", "NONE", "CORE ONLY" };
static const char *const NI_NAMES[] = { "1 HOUR", "3 HOURS", "6 HOURS", "12 HOURS", "24 HOURS" };
static const char *const BW_NAMES[] = { "31.25 KHZ", "62.5 KHZ", "125 KHZ", "250 KHZ", "500 KHZ" };
static const char *const CHROLE_NAMES[] = { "PRIMARY", "SECONDARY", "OFF" };
static const char *const KEY_NAMES[] = { "DEFAULT AQ==", "NONE", "AES-256", "AES-128", "TYPE A KEY" };
static const char *const PREC_NAMES[] = { "OFF", "PRECISE", "45 M", "182 M", "729 M", "2.9 KM", "11.7 KM", "23.3 KM" };
static const uint8_t PREC_BITS[] = { 0, 32, 19, 17, 15, 13, 11, 10 };
static const char *const NOTIFY_NAMES[] = { "EVERY MESSAGE", "DIRECT ONLY", "OFF" };
static const char *const RANGE_NAMES[] = { "OFF", "LOG ONLY", "SEND 15 S", "SEND 30 S", "SEND 1 MIN", "SEND 5 MIN" };
static const char *const NBR_NAMES[] = { "OFF", "4 HOURS", "6 HOURS", "12 HOURS", "24 HOURS" };
/* The roles a user picks (upstream's list less the deprecated ROUTER_CLIENT). */
static const uint8_t ROLE_PICK[] = { MT_ROLE_CLIENT, MT_ROLE_CLIENT_MUTE, MT_ROLE_CLIENT_HIDDEN, MT_ROLE_CLIENT_BASE,
                                     MT_ROLE_TRACKER, MT_ROLE_SENSOR, MT_ROLE_ROUTER, MT_ROLE_ROUTER_LATE,
                                     MT_ROLE_REPEATER, MT_ROLE_LOST_AND_FOUND, MT_ROLE_TAK, MT_ROLE_TAK_TRACKER };
#define ROLE_PICKS ((int)(sizeof(ROLE_PICK) / sizeof(ROLE_PICK[0])))

/* Lines already on the screen get their English when AUTO is turned on, a
   language is turned on, or its card data arrives. */
static void xl_backfill(void)
{
    for (int k = 0; k < msg_n; k++) {
        msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (m->mine || !xl_ready(m->lang)) continue;
        if (!m->tr[0] && ui.xl_auto) mt_xlate_to_en((mt_lang_t)m->lang, m->text, m->tr, sizeof(m->tr));
        hist_fill(m->text, m->tr);
    }
    for (uint32_t i = 0; i < hist.n; i++)
        if (!hist.e[i].en[0]) {
            int conf = 0;
            const mt_lang_t l = mt_xlate_detect_in(hist.e[i].es, xl_langs(), &conf);
            if (xl_ready(l)) { mt_xlate_to_en(l, hist.e[i].es, hist.e[i].en, sizeof(hist.e[i].en)); dirty |= SV_HIST; }
        }
}

static int key_kind(int c)
{
    const chan_cfg_t *k = &chans[c];
    if (k->psk_len == 1 && k->psk[0] == 1) return 0;
    if (k->psk_len == 0 || (k->psk_len == 1 && k->psk[0] == 0)) return 1;
    return k->psk_len == 32 ? 2 : k->psk_len == 16 ? 3 : 4;
}
static int prec_index(int bits)
{
    int best = 1, bd = 99;
    for (int i = 0; i < 8; i++) {
        const int d = PREC_BITS[i] > bits ? PREC_BITS[i] - bits : bits - PREC_BITS[i];
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int max_power(void)
{
    lap_radio_modes_t m;
    if (radio >= 0 && !lap_radio_modes(radio, &m) && m.max_power_dbm > 0) return m.max_power_dbm;
    return 22;
}

static double opt_get(int id)
{
    switch (id) {
    case OPT_TX: return cfg.tx_on;
    case OPT_HAM: return ext.licensed;
    case OPT_PKI: return cfg.pki;
    case OPT_REBC: return ext.rebroadcast % 6;
    case OPT_NIEVERY: return ext.ni_every % 5;
    case OPT_KEEP: return cfg.keep % 3;
    case OPT_PRESET: return cfg.preset;
    case OPT_CUSTOM: return ext.custom;
    case OPT_BW: return ext.bw % MT_BW_COUNT;
    case OPT_SF: return ext.custom ? ext.sf : MT_PRESETS[cfg.preset].sf;
    case OPT_CR: return ext.custom ? ext.cr : MT_PRESETS[cfg.preset].cr;
    case OPT_HOPS: return ext.hop_limit;
    case OPT_POWER: return cfg.power_dbm;
    case OPT_SLOT: return ext.slot;
    case OPT_OFFSET: return ext.offset_hz / 1000.0;
    case OPT_DUTY: return ext.duty_override;
    case OPT_POS: return cfg.pos_mode != 0;
    case OPT_POSEVERY: return cfg.pos_every % 4;
    case OPT_SMART: return ext.smart;
    case OPT_SMART_M: return ext.smart_m;
    case OPT_SMART_S: return ext.smart_s;
    case OPT_CH: return ch_sel + 1;
    case OPT_CH_NAME: return ch_sel;
    case OPT_CH_ROLE: return ch_sel == 0 ? 0 : chans[ch_sel].role == CH_SECONDARY ? 1 : 2;
    case OPT_CH_KEY: return key_kind(ch_sel);
    case OPT_CH_MUTE: return chans[ch_sel].mute;
    case OPT_CH_PREC: return prec_index(chans[ch_sel].precision);
    case OPT_NOTIFY: return ext.notify % 3;
    case OPT_TELE: return cfg.tele % 4;
    case OPT_RANGE: return ext.range % 6;
    case OPT_NBR: return ext.nbr_every % 5;
    case OPT_XL_AUTO: return ui.xl_auto;
    case OPT_QUICK: return quick_lang() == MT_LANG_EN ? 0 : quick_lang() - MT_LANG_ES + 1;
    case OPT_SPEAK: return ui.speak % 3;
    case OPT_SCAN_ON: return scan_on;
    case OPT_SCAN_DWELL: return scan_dwell % 5;
    case OPT_SCAN_PIN: return scan.pin + 1;
    default:
        if (id >= OPT_SCAN_P0 && id < OPT_SCAN_P0 + scan.n) return scan.e[id - OPT_SCAN_P0].on;
        return 0;
    }
}

static int opt_want = -2;               /* reopen OPTIONS: -1 the root, else a level as the root */
static void open_text(int kind, const char *prompt, const char *init, int max)
{
    if (dialog >= 0) return;
    dialog = lap_text_input(prompt, init, max, LAP_TEXT_LOWER);
    dialog_kind = kind;
    if (dialog >= 0) opt_from = opt_cur;
}
static void open_choose(int kind, const char *const *items, int n)
{
    if (dialog >= 0) return;
    dialog = lap_choose(items, n);
    dialog_kind = kind;
    if (dialog >= 0) opt_from = opt_cur;
}
static void open_confirm(int kind, const char *msg)
{
    if (dialog >= 0) return;
    dialog = lap_confirm(msg);
    dialog_kind = kind;
    if (dialog >= 0) opt_from = opt_cur;
}
static void lora_changed(void)
{
    retune();
    dirty |= SV_EXT | SV_CFG;
}
static void ham_set(bool on);

static void opt_set_row(int id, double v);
static void opt_set(int id, double v)
{
    opt_cur = id >= 100 ? id / 100 - 1 : -1;
    opt_set_row(id, v);
    opt_cur = -2;
}
static void opt_set_row(int id, double v)
{
    char t[64];
    const int iv = (int)v;
    if (id >= OPT_SCAN_P0 && id < OPT_SCAN_P0 + scan.n) {        /* SCAN > PRESETS: in the cycle or out */
        if (!mt_scan_enable(&scan, id - OPT_SCAN_P0, v != 0)) say("one preset stays in the scan");
        return;
    }
    switch (id) {
    case OPT_SCAN_ON: scan_go(v != 0); return;
    case OPT_SCAN_DWELL:
        scan_dwell = (uint8_t)(iv < 0 ? 0 : iv % 5);
        mt_scan_set_dwell(&scan, SCAN_DWELL_S[scan_dwell] * 1000u);
        return;
    case OPT_SCAN_PIN: if (iv - 1 != scan.pin) scan_pin(iv - 1); return;
    case OPT_SCAN_CLEAR: mt_scan_clear(&scan); say("scan table cleared"); return;
    case OPT_TX: cfg.tx_on = v != 0; if (cfg.tx_on) send_nodeinfo(true); break;
    case OPT_BACK: opt_want = -1; return;
    case OPT_BACKUP: dirty |= SV_BACKUP; say("saving settings to the card"); return;
    case OPT_RESTORE: open_confirm(DLG_RESTORE_OK, "RESTORE ALL SETTINGS FROM meshtastic/settings.bin ON THE CARD?"); return;
    /* NODE */
    case OPT_LONG: open_text(DLG_LONG, ext.licensed ? "CALLSIGN (YOUR LONG NAME)" : "LONG NAME ON THE MESH", cfg.long_name, 31); return;
    case OPT_SHORT: open_text(DLG_SHORT, "SHORT NAME (UP TO 4)", cfg.short_name, 4); return;
    case OPT_HAM: ham_set(v != 0); return;
    case OPT_PKI:
        cfg.pki = v != 0;
        if (cfg.pki) pki_make();
        if (cfg.tx_on) send_nodeinfo(false);            /* publish or withdraw the key */
        break;
    case OPT_ROLE: {
        static char items[ROLE_PICKS][32];
        static const char *ptr[ROLE_PICKS];
        for (int i = 0; i < ROLE_PICKS; i++) {
            snprintf(items[i], sizeof(items[i]), "%s%s", ROLE_PICK[i] == ext.role ? "* " : "", mt_role_name(ROLE_PICK[i]));
            ptr[i] = items[i];
        }
        open_choose(DLG_ROLE, ptr, ROLE_PICKS);
        return;
    }
    case OPT_REBC: ext.rebroadcast = (uint8_t)iv; dirty |= SV_EXT; return;
    case OPT_NIEVERY: ext.ni_every = (uint8_t)iv; dirty |= SV_EXT; return;
    case OPT_KEEP: cfg.keep = (uint8_t)iv; service_update(); break;
    /* LORA */
    case OPT_REGION: {
        static char items[32][24];
        static const char *ptr[32];
        for (int i = 0; i < MT_REGION_COUNT && i < 32; i++) {
            snprintf(items[i], sizeof(items[i]), "%s%s", i == cfg.region ? "* " : "", MT_REGIONS[i].name);
            ptr[i] = items[i];
        }
        open_choose(DLG_REGION, ptr, MT_REGION_COUNT < 32 ? MT_REGION_COUNT : 32);
        return;
    }
    case OPT_PRESET: cfg.preset = (uint8_t)iv; ext.custom = 0; lora_changed(); return;
    case OPT_CUSTOM:
        if (v != 0 && !ext.custom) {        /* start from the preset in use */
            const mt_preset_t *p = &MT_PRESETS[cfg.preset];
            ext.bw = p->bw_hz <= 125000 ? MT_BW_125 : p->bw_hz <= 250000 ? MT_BW_250 : MT_BW_500;
            ext.sf = p->sf; ext.cr = p->cr;
        }
        ext.custom = v != 0; lora_changed(); return;
    case OPT_BW: ext.bw = (uint8_t)iv; ext.custom = 1; lora_changed(); return;
    case OPT_SF: ext.sf = (uint8_t)iv; if (!ext.custom) { ext.bw = MT_PRESETS[cfg.preset].bw_hz <= 125000 ? MT_BW_125 : MT_PRESETS[cfg.preset].bw_hz <= 250000 ? MT_BW_250 : MT_BW_500; ext.cr = MT_PRESETS[cfg.preset].cr; } ext.custom = 1; lora_changed(); return;
    case OPT_CR: ext.cr = (uint8_t)iv; if (!ext.custom) { ext.bw = MT_PRESETS[cfg.preset].bw_hz <= 125000 ? MT_BW_125 : MT_PRESETS[cfg.preset].bw_hz <= 250000 ? MT_BW_250 : MT_BW_500; ext.sf = MT_PRESETS[cfg.preset].sf; } ext.custom = 1; lora_changed(); return;
    case OPT_HOPS: ext.hop_limit = (uint8_t)(iv < 1 ? 1 : iv > 7 ? 7 : iv); dirty |= SV_EXT; return;
    case OPT_POWER: cfg.power_dbm = (int8_t)iv; break;
    case OPT_SLOT: ext.slot = (uint16_t)(iv < 0 ? 0 : iv > slots ? slots : iv); lora_changed(); return;
    case OPT_OFFSET: ext.offset_hz = (int32_t)(v * 1000.0); lora_changed(); return;
    case OPT_DUTY: ext.duty_override = v != 0; dirty |= SV_EXT; return;
    /* POSITION */
    case OPT_POS: cfg.pos_mode = v != 0; last_pos_ms = 0; break;
    case OPT_POSEVERY: cfg.pos_every = (uint8_t)iv; break;
    case OPT_SMART: ext.smart = v != 0; dirty |= SV_EXT; return;
    case OPT_SMART_M: ext.smart_m = (uint16_t)iv; dirty |= SV_EXT; return;
    case OPT_SMART_S: ext.smart_s = (uint16_t)iv; dirty |= SV_EXT; return;
    case OPT_FIXED:
        t[0] = 0;
        if (cfg.fixed) snprintf(t, sizeof(t), "%.5f,%.5f", cfg.fixed_lat * 1e-7, cfg.fixed_lon * 1e-7);
        open_text(DLG_FIXED, "FIXED POSITION lat,lon (empty = GPS only)", t, 30);
        return;
    /* CHANNELS */
    case OPT_CH: ch_sel = iv < 1 ? 0 : iv > NCH ? NCH - 1 : iv - 1; return;
    case OPT_CH_NAME: {
        char prompt[48];
        snprintf(prompt, sizeof(prompt), "CHANNEL %d NAME (EMPTY = %s)", ch_sel + 1, preset_name());
        open_text(DLG_CH_NAME, prompt, chans[ch_sel].name, 11);
        return;
    }
    case OPT_CH_ROLE:
        if (ch_sel == 0) { say("channel 1 is the primary: make another one primary from its ROLE"); return; }
        if (iv == 0) {
            snprintf(t, sizeof(t), "MAKE CHANNEL %d THE PRIMARY? THE FREQUENCY FOLLOWS ITS NAME.", ch_sel + 1);
            open_confirm(DLG_CH_PRIMARY, t);
            return;
        }
        chans[ch_sel].role = iv == 1 ? CH_SECONDARY : CH_OFF;
        dirty |= SV_CHANS; apply_channel();
        return;
    case OPT_CH_KEY: {
        chan_cfg_t *c = &chans[ch_sel];
        if (iv == 4) { key_text(ch_sel, t, sizeof(t)); open_text(DLG_CH_KEY, "KEY, base64 of 1, 16 or 32 bytes", t, 48); return; }
        memset(c->psk, 0, sizeof(c->psk));
        if (iv == 0) { c->psk_len = 1; c->psk[0] = 1; }
        else if (iv == 1) { c->psk_len = 0; }
        else {
            c->psk_len = iv == 2 ? 32 : 16;
            for (int i = 0; i < c->psk_len; i += 4) { const uint32_t r = lap_random(); memcpy(c->psk + i, &r, 4); }
        }
        dirty |= SV_CHANS; apply_channel();
        return;
    }
    case OPT_CH_MUTE: chans[ch_sel].mute = v != 0; dirty |= SV_CHANS; return;
    case OPT_CH_PREC: chans[ch_sel].precision = PREC_BITS[iv & 7]; dirty |= SV_CHANS; return;
    case OPT_SHARE: share_open = true; scroll = 0; lap_options_close(); return;
    case OPT_IMPORT: open_text(DLG_IMPORT, "CHANNEL LINK (meshtastic.org/e/#...)", "", 767); return;
    /* MODULES */
    case OPT_NOTIFY: ext.notify = (uint8_t)iv; dirty |= SV_EXT; return;
    case OPT_TELE: cfg.tele = (uint8_t)iv; break;
    case OPT_RANGE:
        ext.range = (uint8_t)iv; range_seq = 0; range_ms = 0; range_file[0] = 0; dirty |= SV_EXT;
        if (iv >= 2 && ch_default(0)) say("range test on the public channel: a private one is kinder");
        return;
    case OPT_NBR: ext.nbr_every = (uint8_t)iv; last_nbr_ms = 0; dirty |= SV_EXT;
        if (iv && ch_default(0)) say("neighbour info is never sent on the default channel");
        return;
    case OPT_XL_AUTO: ui.xl_auto = v != 0; if (ui.xl_auto) xl_fill_req = true; dirty |= SV_UI; return;
    case OPT_QUICK: {
        /* more languages than a row steps through: a list */
        static char items[QUICKS][32];
        static const char *ptr[QUICKS];
        const int cur = quick_lang() == MT_LANG_EN ? 0 : quick_lang() - MT_LANG_ES + 1;
        for (int i = 0; i < QUICKS; i++) {
            snprintf(items[i], sizeof(items[i]), "%s%s", i == cur ? "* " : "", QUICK_NAMES[i]);
            ptr[i] = items[i];
        }
        open_choose(DLG_QUICK, ptr, QUICKS);
        return;
    }
    case OPT_SPEAK: ui.speak = (uint8_t)iv; dirty |= SV_UI; if (ui.speak) lap_say("Messages will be read aloud."); return;
    default: return;
    }
    dirty |= SV_CFG;
}

/* The rows, built when the list opens (labels carry the values the board
   copies). sub_rows[k][0] is the "< ALL SETTINGS" row of a level opened as
   the root; the root's MENU rows point past it. */
static lap_opt_t sub_rows[SUBS][12];
static int sub_n[SUBS];
static lap_opt_list_t sub_list[SUBS], root_list;
static lap_opt_t main_rows[8];
static char ch_names[NCH][14], slot_unit[24];
static const char *ch_name_ptr[NCH];
static lap_opt_t R(const char *l, int kind, int id, const char *const *names, int n, double lo, double hi, double step,
                   const char *unit)
{
    return (lap_opt_t){ l, (uint8_t)kind, id, names, n, lo, hi, step, unit, NULL };
}
/* Rows go straight into their list (no big tables on the screen task's stack). */
static lap_opt_t back_row;
static int rows_k = -1;
static void rows_at(int k) { rows_k = k; if (k >= 0) sub_n[k] = 0; }
static void row(lap_opt_t r)
{
    if (rows_k < 0) back_row = r;
    else if (sub_n[rows_k] < 11) sub_rows[rows_k][1 + sub_n[rows_k]++] = r;
}
static void opt_build(void)
{
    for (int i = 0; i < MT_PRESET_COUNT && i < 8; i++) preset_names[i] = MT_PRESETS[i].name;
    snprintf(lbl[0], sizeof(lbl[0]), "NAME %.18s", cfg.long_name);
    snprintf(lbl[1], sizeof(lbl[1]), "SHORT NAME %s", cfg.short_name);
    snprintf(lbl[2], sizeof(lbl[2]), "ROLE %.18s", mt_role_name(ext.role));
    snprintf(lbl[3], sizeof(lbl[3]), "REGION %s", MT_REGIONS[cfg.region].name);
    if (cfg.fixed) snprintf(lbl[4], sizeof(lbl[4]), "FIXED %.3f,%.3f", cfg.fixed_lat * 1e-7, cfg.fixed_lon * 1e-7);
    else snprintf(lbl[4], sizeof(lbl[4]), "FIXED POSITION off");
    snprintf(slot_unit, sizeof(slot_unit), "of %d, 0 by name", slots);
    for (int c = 0; c < NCH; c++) {
        if (c && chans[c].role != CH_SECONDARY && !chans[c].name[0]) snprintf(ch_names[c], sizeof(ch_names[c]), "(unused)");
        else if (chans[c].name[0]) snprintf(ch_names[c], sizeof(ch_names[c]), "%s", chans[c].name);
        else snprintf(ch_names[c], sizeof(ch_names[c]), "(%.11s)", preset_name());
        ch_name_ptr[c] = ch_names[c];
    }
    rows_at(-1);
    row(R("< ALL SETTINGS", LAP_OPT_ACTION, OPT_BACK, NULL, 0, 0, 0, 0, NULL));
    rows_at(SUB_NODE);
    row(R(lbl[0], LAP_OPT_ACTION, OPT_LONG, NULL, 0, 0, 0, 0, NULL));
    row(R(lbl[1], LAP_OPT_ACTION, OPT_SHORT, NULL, 0, 0, 0, 0, NULL));
    row(R("LICENSED (HAM)", LAP_OPT_TOGGLE, OPT_HAM, NULL, 0, 0, 0, 0, NULL));
    row(R("PRIVATE DMS (PKI)", LAP_OPT_TOGGLE, OPT_PKI, NULL, 0, 0, 0, 0, NULL));
    row(R(lbl[2], LAP_OPT_ACTION, OPT_ROLE, NULL, 0, 0, 0, 0, NULL));
    row(R("REBROADCAST", LAP_OPT_CYCLE, OPT_REBC, REBC_NAMES, 6, 0, 0, 0, NULL));
    row(R("NODE INFO EVERY", LAP_OPT_CYCLE, OPT_NIEVERY, NI_NAMES, 5, 0, 0, 0, NULL));
    row(R("KEEP RUNNING", LAP_OPT_CYCLE, OPT_KEEP, KEEP_NAMES, 3, 0, 0, 0, NULL));
    rows_at(SUB_LORA);
    row(R(lbl[3], LAP_OPT_ACTION, OPT_REGION, NULL, 0, 0, 0, 0, NULL));
    row(R("PRESET", LAP_OPT_CYCLE, OPT_PRESET, preset_names, MT_PRESET_COUNT < 8 ? MT_PRESET_COUNT : 8, 0, 0, 0, NULL));
    row(R("CUSTOM (NO PRESET)", LAP_OPT_TOGGLE, OPT_CUSTOM, NULL, 0, 0, 0, 0, NULL));
    row(R("BANDWIDTH", LAP_OPT_CYCLE, OPT_BW, BW_NAMES, MT_BW_COUNT, 0, 0, 0, NULL));
    row(R("SPREAD FACTOR", LAP_OPT_LEVEL, OPT_SF, NULL, 0, 5, 12, 1, "SF"));
    row(R("CODING RATE", LAP_OPT_LEVEL, OPT_CR, NULL, 0, 5, 8, 1, "4/n"));
    row(R("HOP LIMIT", LAP_OPT_LEVEL, OPT_HOPS, NULL, 0, 1, 7, 1, "hops"));
    row(R("TX POWER", LAP_OPT_LEVEL, OPT_POWER, NULL, 0, 1, max_power(), 1, "dBm"));
    row(R("FREQUENCY SLOT", LAP_OPT_LEVEL, OPT_SLOT, NULL, 0, 0, slots, 1, slot_unit));
    row(R("FREQUENCY OFFSET", LAP_OPT_NUMBER, OPT_OFFSET, NULL, 0, -500, 500, 0, "kHz"));
    row(R("OVERRIDE DUTY CYCLE", LAP_OPT_TOGGLE, OPT_DUTY, NULL, 0, 0, 0, 0, NULL));
    rows_at(SUB_POS);
    row(R("SHARE POSITION", LAP_OPT_TOGGLE, OPT_POS, NULL, 0, 0, 0, 0, NULL));
    row(R("POSITION EVERY", LAP_OPT_CYCLE, OPT_POSEVERY, EVERY_NAMES, 4, 0, 0, 0, NULL));
    row(R("SMART POSITION", LAP_OPT_TOGGLE, OPT_SMART, NULL, 0, 0, 0, 0, NULL));
    row(R("SMART: MOVED", LAP_OPT_LEVEL, OPT_SMART_M, NULL, 0, 10, 5000, 10, "m"));
    row(R("SMART: AT MOST EVERY", LAP_OPT_LEVEL, OPT_SMART_S, NULL, 0, 5, 3600, 5, "s"));
    row(R(lbl[4], LAP_OPT_ACTION, OPT_FIXED, NULL, 0, 0, 0, 0, NULL));
    rows_at(SUB_CH);
    row(R("CHANNEL", LAP_OPT_LEVEL, OPT_CH, NULL, 0, 1, NCH, 1, "of 8"));
    row(R("NAME", LAP_OPT_CYCLE, OPT_CH_NAME, ch_name_ptr, NCH, 0, 0, 0, NULL));
    row(R("ROLE", LAP_OPT_CYCLE, OPT_CH_ROLE, CHROLE_NAMES, 3, 0, 0, 0, NULL));
    row(R("KEY", LAP_OPT_CYCLE, OPT_CH_KEY, KEY_NAMES, 5, 0, 0, 0, NULL));
    row(R("MUTE", LAP_OPT_TOGGLE, OPT_CH_MUTE, NULL, 0, 0, 0, 0, NULL));
    row(R("POSITION ON IT", LAP_OPT_CYCLE, OPT_CH_PREC, PREC_NAMES, 8, 0, 0, 0, NULL));
    row(R("SHARE: LINK + QR", LAP_OPT_ACTION, OPT_SHARE, NULL, 0, 0, 0, 0, NULL));
    row(R("ADD FROM A LINK", LAP_OPT_ACTION, OPT_IMPORT, NULL, 0, 0, 0, 0, NULL));
    rows_at(SUB_MOD);
    row(R("ALERTS", LAP_OPT_CYCLE, OPT_NOTIFY, NOTIFY_NAMES, 3, 0, 0, 0, NULL));
    row(R("SEND TELEMETRY", LAP_OPT_CYCLE, OPT_TELE, TELE_NAMES, 4, 0, 0, 0, NULL));
    row(R("RANGE TEST", LAP_OPT_CYCLE, OPT_RANGE, RANGE_NAMES, 6, 0, 0, 0, NULL));
    row(R("NEIGHBOR INFO", LAP_OPT_CYCLE, OPT_NBR, NBR_NAMES, 5, 0, 0, 0, NULL));
    row(R("AUTO TRANSLATE", LAP_OPT_TOGGLE, OPT_XL_AUTO, NULL, 0, 0, 0, 0, NULL));
    snprintf(lbl[5], sizeof(lbl[5]), "QUICK IN %s", quick_lang() == MT_LANG_EN ? "ENGLISH" : mt_lang_name(quick_lang()));
    row(R(lbl[5], LAP_OPT_ACTION, OPT_QUICK, NULL, 0, 0, 0, 0, NULL));
    row(R("SPEAK MESSAGES", LAP_OPT_CYCLE, OPT_SPEAK, SPEAK_NAMES, 3, 0, 0, 0, NULL));
    rows_at(-1);
    for (int k = 0; k < SUBS; k++) {
        sub_rows[k][0] = back_row;
        sub_list[k] = (lap_opt_list_t){ SUB_NAME[k], &sub_rows[k][1], sub_n[k], NULL, NULL };
    }
    main_rows[0] = R("TRANSMIT", LAP_OPT_TOGGLE, OPT_TX, NULL, 0, 0, 0, 0, NULL);
    for (int k = 0; k < SUBS; k++) {
        main_rows[1 + k] = R(SUB_NAME[k], LAP_OPT_MENU, OPT_NODE_M + k, NULL, 0, 0, 0, 0, NULL);
        main_rows[1 + k].sub = &sub_list[k];
    }
    main_rows[6] = R("BACKUP TO CARD", LAP_OPT_ACTION, OPT_BACKUP, NULL, 0, 0, 0, 0, NULL);
    main_rows[7] = R("RESTORE FROM CARD", LAP_OPT_ACTION, OPT_RESTORE, NULL, 0, 0, 0, 0, NULL);
}

/* at -1: the whole tree; else that level as the root, fresh. */
static void open_options_at(int at)
{
    opt_build();
    if (at < 0 || at >= SUBS) root_list = (lap_opt_list_t){ "MESHTASTIC", main_rows, ROWS(main_rows), opt_get, opt_set };
    else root_list = (lap_opt_list_t){ SUB_NAME[at], sub_rows[at], sub_n[at] + 1, opt_get, opt_set };
    if (!lap_options_open(&root_list)) say("OPTIONS could not open");
}
/* On the SCAN tab OPTIONS opens SCAN's own level (the whole tree is full),
   with "< ALL SETTINGS" on top: SCAN, DWELL, PIN, PRESETS (each in the cycle
   or out), CLEAR TABLE. */
static lap_opt_t scan_rows[6], scan_preset_rows[MT_SCAN_MAX];
static lap_opt_list_t scan_list, scan_preset_list;
static char scan_pin_unit[24];
static void open_scan_options(void)
{
    scan_plan();
    opt_build();                                         /* back_row: "< ALL SETTINGS" */
    snprintf(scan_pin_unit, sizeof(scan_pin_unit), "of %d, 0 cycles", scan.n);   /* the board shows 8 names at most */
    for (int i = 0; i < scan.n; i++) {
        scan_preset_rows[i] = R(MT_SCAN_PRESETS[scan.e[i].preset].name, LAP_OPT_TOGGLE, OPT_SCAN_P0 + i, NULL, 0, 0, 0, 0, NULL);
    }
    scan_preset_list = (lap_opt_list_t){ "PRESETS", scan_preset_rows, scan.n, NULL, NULL };
    int k = 0;
    scan_rows[k++] = back_row;
    scan_rows[k++] = R("SCAN", LAP_OPT_TOGGLE, OPT_SCAN_ON, NULL, 0, 0, 0, 0, NULL);
    scan_rows[k++] = R("DWELL", LAP_OPT_CYCLE, OPT_SCAN_DWELL, SCAN_DWELL_NAMES, 5, 0, 0, 0, NULL);
    scan_rows[k++] = R("PIN (ROW)", LAP_OPT_LEVEL, OPT_SCAN_PIN, NULL, 0, 0, scan.n, 1, scan_pin_unit);
    scan_rows[k] = R("PRESETS", LAP_OPT_MENU, OPT_SCAN_PRESETS, NULL, 0, 0, 0, 0, NULL);
    scan_rows[k++].sub = &scan_preset_list;
    scan_rows[k++] = R("CLEAR TABLE", LAP_OPT_ACTION, OPT_SCAN_CLEAR, NULL, 0, 0, 0, 0, NULL);
    root_list = (lap_opt_list_t){ "SCAN", scan_rows, k, opt_get, opt_set };
    if (!lap_options_open(&root_list)) say("OPTIONS could not open");
}
static void open_options(void)
{
    if (tab == TAB_SCAN && !share_open && !langs_open) open_scan_options();
    else open_options_at(-1);
}

/* Licensed (ham) operation: no encryption on any channel and no PKI, the
   long name a callsign; turning it on asks for one. */
static void ham_set(bool on)
{
    ext.licensed = on;
    dirty |= SV_EXT;
    retune();
    if (on) open_text(DLG_CALLSIGN, "CALLSIGN (YOUR LONG NAME ON THE MESH)", cfg.long_name, 31);
    else if (cfg.tx_on) send_nodeinfo(false);
}

/* ---- lifecycle --------------------------------------------------------- */
static void enter(void)
{
    primary = read_primary();
    lap_keep_awake(true);
    unread = 0;
    lf_list_due = true;               /* what meshtastic/lang/ holds, for REMOVE */
    lang_retry();                     /* back from STORE or PACKS with a language: it loads */
    if (loaded) {                     /* back from the background: all still here */
        service_update();
        return;
    }
    cfg_load();
    ext_load();
    chans_load();
    ui_load();
    hist_load();
    pki_load();
    if (cfg.pki) pki_make();
    radio = -1; rxh = txh = 0; tx_slot = -1; dialog = -1; gps_lease = 0;
    menu_h = 0; pending_quick = pending_phrase = -1; node_open = false; opt_want = -2; opt_from = -2;
    scan_on = false; scan.n = 0;
    memset(txq, 0, sizeof(txq));
    memset(shared, 0, sizeof(shared));
    memset(reacts, 0, sizeof(reacts));
    memset(unread_ch, 0, sizeof(unread_ch));
    ch_epoch = lap_millis() / 10000u; tx_epoch = lap_millis() / 600000u;
    range_seq = 0; range_file[0] = 0; rlog_n = 0; card_dir = false;
    nodes_load();
    wpt_load();
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
    scan_go(false);                     /* the node gets its own preset back */
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
static bool compact;                    /* a short window (a desk's): the header in four rows */
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

/* The TFT's camera hole in the app's cells (cut_w 0: none). Firmware that
   knows it says so as "ui.cutout"; the panel's text then stops short of it
   and the header's web moves off it, as the board's own screens do. */
static int cut_x, cut_y, cut_w, cut_h;
static void read_cutout(void)
{
    char t[32];
    cut_w = 0;
    if (!lap_value_text("ui.cutout", t, sizeof(t))) return;
    char *p = t;
    const int x = (int)strtol(p, &p, 10); if (*p == ',') p++;
    const int y = (int)strtol(p, &p, 10); if (*p == ',') p++;
    const int w = (int)strtol(p, &p, 10); if (*p == ',') p++;
    const int h = (int)strtol(p, &p, 10);
    if (w > 0 && h > 0) { cut_x = x; cut_y = y; cut_w = w; cut_h = h; }
}
static bool cut_rows(int y0, int y1) { return cut_w && cut_y < y1 && cut_y + cut_h > y0; }

static bool calm(void) { return ui.anim != 0; }

/* What a tap on the panel lands on, noted while the panel is drawn. */
enum { HIT_MSG = 1, HIT_NODE, HIT_PHRASE, HIT_BLIP, HIT_XL_AUTO, HIT_XL_SHOW, HIT_TRY, HIT_AIR, HIT_HIST, HIT_HIST_CLEAR,
       HIT_CONV, HIT_WPT, HIT_SHARE_ALL, HIT_XL_LANG, HIT_QUICK_LANG, HIT_XL_MORE, HIT_SCAN };
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
static float km_between(double la1, double lo1, double la2, double lo2) { return km_bearing(la1, lo1, la2, lo2, NULL); }
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
typedef struct { float fx, fy; uint8_t hue; bool placed, real; int node, parent; } blip_t;
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
/* Whether node a reported hearing b directly. */
static bool node_links(const node_t *a, uint32_t b)
{
    for (int k = 0; k < a->nbr_n; k++) if (a->nbr[k] == b) return true;
    return false;
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
        b->node = i; b->parent = -1; b->real = false;
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
    /* who each relayed node came through: a node one hop closer that either
       says it hears it (neighbour info), else the nearest point one hop closer */
    for (int i = 0; i < blip_n; i++) {
        const node_t *ni = &nodes[blips[i].node];
        const int hops = ni->hops;
        if (!hops) continue;
        float best = 1e9f;
        for (int j = 0; j < blip_n; j++) {
            const node_t *nj = &nodes[blips[j].node];
            if (nj->hops != hops - 1) continue;
            const bool link = node_links(ni, nj->num) || node_links(nj, ni->num);
            const float dx = blips[i].fx - blips[j].fx, dy = blips[i].fy - blips[j].fy, d = dx * dx + dy * dy - (link ? 100.0f : 0.0f);
            if (d < best) { best = d; blips[i].parent = j; blips[i].real = link; }
        }
    }
}
static int blip_of(uint32_t num)
{
    for (int i = 0; i < blip_n; i++) if (nodes[blips[i].node].num == num) return i;
    return -1;
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
    /* links the nodes report (neighbour info), as faint cyan threads */
    for (int i = 0; i < blip_n; i++) {
        const node_t *n = &nodes[blips[i].node];
        int bx, by, qx, qy;
        web_xy(&blips[i], cx, cy, R, &bx, &by);
        for (int k = 0; k < n->nbr_n; k++) {
            if (n->nbr[k] == cfg.node) { cv_line(rnd(cx), rnd(cy), bx, by, A(LAP_CYAN)); continue; }
            const int j = blip_of(n->nbr[k]);
            if (j < 0 || j == blips[i].parent) continue;
            web_xy(&blips[j], cx, cy, R, &qx, &qy);
            cv_line(bx, by, qx, qy, A(LAP_CYAN));
        }
    }
    /* waypoints, by bearing and distance, as small crosses */
    if (me_ok && radar_km > 0)
        for (int i = 0; i < wpt_n; i++) {
            float brg = 0;
            const float d = km_bearing(me_lat, me_lon, wpts[i].lat * 1e-7, wpts[i].lon * 1e-7, &brg);
            float r = d / radar_km;
            if (r > 1.04f) continue;
            const float ang = (brg - 90.0f) * 3.14159265f / 180.0f;
            const int wx = rnd(cx + r * cosf(ang) * R), wy = rnd(cy + r * sinf(ang) * R / cv.ax);
            const uint8_t at = A(LAP_YELLOW | LAP_BRIGHT);
            for (int k = -2; k <= 2; k++) { cv_dot(wx + k, wy + k, at); cv_dot(wx + k, wy - k, at); }
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
        if ((!n->hops && b->parent < 0) || b->real) cv_line(qx, qy, bx, by, stale ? LAP_UI_FAINT : A(dim(b->hue)));
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
    if (me_ok && radar_km > 0)
        for (int i = 0; i < wpt_n; i++) {
            float brg = 0;
            const float d = km_bearing(me_lat, me_lon, wpts[i].lat * 1e-7, wpts[i].lon * 1e-7, &brg);
            const float r = d / radar_km;
            if (r > 1.04f) continue;
            const float ang = (brg - 90.0f) * 3.14159265f / 180.0f;
            const int ccol = rnd(cx + r * cosf(ang) * R) / 2 + 2, crow = rnd(cy + r * sinf(ang) * R / cv.ax) / 4;
            const int len = u8_cells(wpts[i].name, -1) < 10 ? u8_cells(wpts[i].name, -1) : 10;
            if (crow < 0 || crow >= cv.h || ccol + len > cv.w) continue;
            for (int k = ccol; k < ccol + len && k < CV_W; k++) used[crow][k / 8] |= (uint8_t)(1u << (k % 8));
            put(x + ccol, y + crow, len, A(LAP_YELLOW), "%s", wpts[i].name);
        }
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

/* The role in a word for the header: RELAY or MUTE for a client. */
static const char *role_chip(void)
{
    switch (ext.role) {
    case MT_ROLE_CLIENT: return ext.rebroadcast == MT_REBC_NONE ? "MUTE" : "RELAY";
    case MT_ROLE_CLIENT_MUTE: return "MUTE";
    case MT_ROLE_CLIENT_HIDDEN: return "HIDDEN";
    case MT_ROLE_CLIENT_BASE: return "BASE";
    case MT_ROLE_ROUTER: return "ROUTER";
    case MT_ROLE_ROUTER_LATE: return "LATE";
    case MT_ROLE_REPEATER: return "REPEAT";
    case MT_ROLE_TRACKER: case MT_ROLE_TAK_TRACKER: return "TRACKER";
    case MT_ROLE_SENSOR: return "SENSOR";
    case MT_ROLE_LOST_AND_FOUND: return "LOST";
    case MT_ROLE_TAK: return "TAK";
    default: return "ROLE";
    }
}
/* "ES>EN", "ES+2>EN": what is read */
static const char *xl_chip(void)
{
    static char b[12];
    const int n = xl_count();
    if (n <= 1) snprintf(b, sizeof(b), "%s>EN", LANG_UP[xl_first()]);
    else snprintf(b, sizeof(b), "%s+%c>EN", LANG_UP[xl_first()], (char)('0' + (n - 1) % 10));
    return b;
}
static void chips_row(int x, int y, int w)
{
    lap_gps_t g;
    const bool gps = lap_gps(&g), fix = gps && g.fix;
    const struct { const char *t; bool on; uint8_t hue; } C[] = {
        { "PRIMARY", primary, LAP_CYAN },
        { "BKGND", lap_service_running(), LAP_BLUE },
        { fix ? "GPS" : cfg.fixed ? "FIXED" : gps ? "GPS..." : "NO GPS", fix || cfg.fixed, LAP_GREEN },
        { ext.licensed ? "HAM" : "PKI", ext.licensed || pki_on(), ext.licensed ? LAP_RED : LAP_YELLOW },
        { role_chip(), ext.role != MT_ROLE_CLIENT_MUTE && ext.rebroadcast != MT_REBC_NONE, LAP_MAGENTA },
        { xl_chip(), ui.xl_auto != 0, LAP_CYAN },
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
/* What the radio is on: the scanned preset while SCAN runs. */
static uint64_t shown_hz(void) { return scanning() ? scan.e[scan.cur].freq_hz : freq_hz; }
static void settings_line(int x, int y, int w, const char *more)
{
    if (scanning()) {
        const mt_scan_entry_t *e = &scan.e[scan.cur];
        put(x, y, w, A(LAP_YELLOW | LAP_BRIGHT), "%s SCAN %s  SF%u  s%d/%d", MT_REGIONS[cfg.region].name,
            MT_SCAN_PRESETS[e->preset].name, e->sf, e->slot, e->slots);
        return;
    }
    put(x, y, w, A(LAP_UI_VALUE), "%s %s  %s%s  s%d/%d", MT_REGIONS[cfg.region].name, preset_name(), channel_name(), more,
        slot, slots);
}
static void tx_chip(int x, int y, int w)
{
    if (scan_on) { lap_ui_chip(x + w - 9, y, "SCAN RX", true, LAP_YELLOW); return; }
    lap_ui_chip(x + w - (cfg.tx_on ? (duty_wait ? 11 : 10) : 9), y, cfg.tx_on ? (duty_wait ? "DUTY WAIT" : "TX ARMED") : "RX ONLY",
                cfg.tx_on, cfg.tx_on ? (duty_wait ? LAP_YELLOW : LAP_RED) : LAP_GREEN);
}

static void big_freq(int x, int y, int w)
{
    char f[16];
    const uint64_t hz = shown_hz();
    snprintf(f, sizeof(f), "%u.%03u", (unsigned)(hz / 1000000u), (unsigned)(hz / 1000u % 1000u));
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
    if (compact) {                      /* who, the frequency and settings, the channel, the chips */
        const int cw = lap_ui_chip(x, y, cfg.short_name, true, LAP_CYAN) - x;
        put(x + cw + 1, y, hw - cw - 12, LAP_UI_TEXT, "%s", cfg.long_name);
        tx_chip(x, y, hw);
        const int fw = put(x, y + 1, 12, A(LAP_YELLOW | LAP_BRIGHT), "%u.%03u MHz", (unsigned)(shown_hz() / 1000000u),
                           (unsigned)(shown_hz() / 1000u % 1000u));
        settings_line(x + fw + 1, y + 1, hw - fw - 1, "");
        const float cu = ch_util();
        put(x, y + 2, 3, LAP_UI_DIM, "CH");
        const int bw = hw - 15 > 4 ? hw - 15 : 4;
        lap_ui_bargraph(x + 3, y + 2, bw, 1, cu / 40.0f > 1 ? 1 : cu / 40.0f, -1);
        put(x + 4 + bw, y + 2, 11, A(cu > 25 ? LAP_UI_WARN : LAP_UI_LIVE), "%3.0f%% tx%.1f", (double)cu, (double)air_util());
        chips_row(x, y + 3, hw);
        if (region_state) put(x, y + 2, hw, A(LAP_UI_WARN), "%s", region_state == 1 ? "BAND NOT ON THIS RADIO" : "PRESET TOO WIDE FOR BAND");
        return;
    }
    /* the radar: portrait beside the figures, landscape under them */
    int rx, ry, rw, rh, ix, iy, iw;
    if (!wide) { rw = 17; rh = hh; rx = x; ry = y; ix = x + rw + 1; iy = y; iw = hw - rw - 1; }
    else { ix = x; iy = y; iw = hw; rx = x + 1; ry = y + 10; rw = hw - 2; rh = hh - 10; }
    if (cut_rows(ry, ry + rh) && cut_x <= rx + 1 && cut_x + cut_w > rx) {   /* a hole at the left edge */
        const int nx = cut_x + cut_w + 1;
        rw -= nx - rx; rx = nx;
    }
    if (rh > 2) mesh_web(rx, ry, rw, rh, false, false);
    /* who this is, and whether it may transmit */
    const int cw = lap_ui_chip(ix, iy, cfg.short_name, true, LAP_CYAN) - ix;
    put(ix + cw + 1, iy, iw - cw - 12, LAP_UI_TEXT, "%s", cfg.long_name);
    tx_chip(ix, iy, iw);
    /* the frequency, big: block figures, as the board's glass has them */
    big_freq(ix, iy + 1, iw);
    const int fy = iy + 6;
    char k[48];
    key_text(0, k, sizeof(k));
    const int more = ch_count() - 1;
    char mc[8] = "";
    if (more) snprintf(mc, sizeof(mc), " +%d", more);
    settings_line(ix, fy, iw, mc);
    /* the channel: how busy, and this node's share */
    const float cu = ch_util(), au = air_util();
    put(ix, fy + 1, 3, LAP_UI_DIM, "CH");
    const int bw = iw - 15 > 4 ? iw - 15 : 4;
    lap_ui_bargraph(ix + 3, fy + 1, bw, 1, cu / 40.0f > 1 ? 1 : cu / 40.0f, -1);
    const int duty = MT_REGIONS[cfg.region].duty;
    if (duty < 100) put(ix + 4 + bw, fy + 1, 11, A(au > duty * 0.8f ? LAP_UI_WARN : LAP_UI_LIVE), "%3.0f%% %.1f/%d", (double)cu, (double)au, duty);
    else put(ix + 4 + bw, fy + 1, 11, A(cu > 25 ? LAP_UI_WARN : LAP_UI_LIVE), "%3.0f%% tx%.1f", (double)cu, (double)au);
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
    if (region_state) put(ix, fy + 2, iw, A(LAP_UI_WARN), "%s", region_state == 1 ? "BAND NOT ON THIS RADIO" : "PRESET TOO WIDE FOR BAND");
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
/* The reactions on a message, gathered: up to six labels with counts. */
typedef struct { const char *label[6]; int count[6], n; } tally_t;
static void react_tally(const msg_t *m, tally_t *t)
{
    t->n = 0;
    for (int i = 0; i < MAX_REACT; i++) {
        const react_t *r = &reacts[i];
        if (!r->target || r->target != m->id) continue;
        const char *l = react_label(r->glyph);
        int k = 0;
        while (k < t->n && strcmp(t->label[k], l)) k++;
        if (k == t->n) { if (t->n == 6) continue; t->label[k] = l; t->count[k] = 0; t->n++; }
        t->count[k]++;
    }
}
static const msg_t *msg_find(uint32_t id)
{
    for (int k = 0; k < msg_n; k++) {
        const msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (m->id == id) return m;
    }
    return NULL;
}
static int bubble_rows(const msg_t *m, int tw, span_t *a, int *na, span_t *b, int *nb)
{
    const int mode = msg_mode(m);
    *na = mode == 2 ? 0 : wrap_spans(scr(m->text, m->lang), tw, a, WRAP_MAX);
    *nb = mode ? wrap_spans(scr(m->tr, m->lang), tw, b, WRAP_MAX) : 0;
    if (!*na && !*nb) *na = wrap_spans(scr(m->text, m->lang), tw, a, WRAP_MAX);
    tally_t t;
    react_tally(m, &t);
    return 2 + (*na ? *na : 1) + (mode == 1 ? 1 + *nb : *nb) + (m->reply_id ? 1 : 0) + (t.n ? 1 : 0);
}
static bool picked(const msg_t *m) { return pick_id && m->id == pick_id && m->from == pick_from; }
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
    const bool sel_on = picked(m);
    const uint8_t fr = sel_on ? AB(LAP_YELLOW | LAP_BRIGHT) : fresh ? AB(LAP_WHITE | LAP_BRIGHT) : m->mine ? AB(LAP_CYAN | LAP_BRIGHT) : A(dim(hue));
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
            if (m->via_sf) cx = lap_ui_chip(cx, y, "S&F", true, LAP_BLUE) + 1;
            const int tl = (int)strlen(t) + 2;
            if (n && n->long_name[0] && x + bw - 3 - tl - cx > 4) put(cx, y, x + bw - 3 - tl - cx, LAP_UI_DIM, " %s ", n->long_name);
            put(x + bw - 2 - tl, y, tl, LAP_UI_DIM, " %s ", t);
        }
    }
    /* the message it answers, quoted in a line */
    int yy = y + 1;
    if (m->reply_id) {
        const msg_t *q = msg_find(m->reply_id);
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        cell(x + 2, yy, '>', A(LAP_CYAN | LAP_BRIGHT));
        if (q) {
            const int wl = put(x + 4, yy, 6, A(hue_of(q->from)), "%s", who(q->from));
            put(x + 5 + wl, yy, bw - 7 - wl, m->mine ? AB(LAP_WHITE) : LAP_UI_DIM, "%s", scr(q->text, q->lang));
        } else put(x + 4, yy, bw - 6, m->mine ? AB(LAP_WHITE) : LAP_UI_FAINT, "an earlier message");
        yy++;
    }
    /* the words, typing in for a moment when new */
    int shown = fresh ? (int)(age / 12u) : -1;
    const uint8_t text_at = m->mine ? AB(LAP_WHITE | LAP_BRIGHT) : dm ? A(LAP_MAGENTA | LAP_BRIGHT) : LAP_UI_TEXT;
    for (int i = 0; i < na; i++, yy++) {
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        int s = -1;
        if (shown >= 0) { s = shown; shown -= a[i].n; if (shown < 0) shown = 0; }
        put_span(x + 2, yy, scr(m->text, m->lang), a[i], s, text_at);
    }
    const int mode = msg_mode(m);
    if (mode == 1) {                                     /* both: a rule, then the English */
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        for (int k = x + 2; k < x + bw - 10; k += 2) cell(k, yy, '-', LAP_UI_FAINT);
        put(x + bw - 9, yy, 7, A(LAP_CYAN), "%s > EN", m->lang < MT_LANG_COUNT && m->lang > MT_LANG_EN ? LANG_UP[m->lang] : "??");
        yy++;
    }
    for (int i = 0; i < nb; i++, yy++) {
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        put_span(x + 2, yy, scr(m->tr, m->lang), b[i], -1, A(LAP_CYAN | LAP_BRIGHT));
    }
    if (mode == 2) put(x + bw - 9, y, 7, A(LAP_CYAN), "%s > EN", m->lang < MT_LANG_COUNT && m->lang > MT_LANG_EN ? LANG_UP[m->lang] : "??");
    /* reactions: one chip a kind, with how many */
    tally_t tl;
    react_tally(m, &tl);
    if (tl.n) {
        cell(x, yy, '|', fr); cell(x + bw - 1, yy, '|', fr);
        int cx = x + 2;
        for (int k = 0; k < tl.n && yy >= clip0 && yy < clip1; k++) {
            char c[16];
            snprintf(c, sizeof(c), tl.count[k] > 1 ? "%s %d" : "%s", tl.label[k], tl.count[k]);
            if (cx + (int)strlen(c) + 2 > x + bw - 1) break;
            static const uint8_t RH[] = { LAP_MAGENTA, LAP_YELLOW, LAP_GREEN, LAP_CYAN, LAP_RED, LAP_BLUE };
            cx = lap_ui_chip(cx, yy, c, true, RH[k % 6]) + 1;
        }
        yy++;
    }
    /* bottom: the signal it came in on, or what became of ours */
    frame_row(x, yy, bw, '\'', '\'', fr);
    if (sel_on && yy >= clip0 && yy < clip1) put(x + 2, yy, 14, A(LAP_YELLOW | LAP_BRIGHT), " REPLY  REACT ");
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
    } else if (!sel_on) {
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
    put(x + 2, ty2++, w - 4, LAP_UI_DIM, "QUICK sends a ready phrase, in %s.", quick_lang() == MT_LANG_EN ? "English" : lang_word(quick_lang()));
    put(x + 2, ty2++, w - 4, LAP_UI_DIM, "%s that comes in shows in English too.", xl_count() > 1 ? "What you read in TRANSLATE" : lang_word(xl_first()));
}

static bool conv_match(const msg_t *m, int kind, int c, uint32_t node)
{
    const bool dm = m->to != MT_BROADCAST;
    if (kind == CONV_ALL) return true;
    if (kind == CONV_CH) return !dm && m->ch == c;
    return dm && (m->mine ? m->to == node : m->from == node);
}
static bool conv_shows(const msg_t *m) { return conv_match(m, conv_kind, conv_ch, conv_node); }
static const char *conv_title(void)
{
    static char t[24];
    if (conv_kind == CONV_CH) snprintf(t, sizeof(t), "#%s", ch_name(conv_ch));
    else if (conv_kind == CONV_DM) snprintf(t, sizeof(t), "@%s", who(conv_node));
    else snprintf(t, sizeof(t), "ALL");
    return t;
}
static void conv_read(void)
{
    if (conv_kind == CONV_ALL) { memset(unread_ch, 0, sizeof(unread_ch)); for (int i = 0; i < node_n; i++) nodes[i].unread = 0; }
    else if (conv_kind == CONV_CH) unread_ch[conv_ch] = 0;
    else { node_t *n = node_get(conv_node, false); if (n) n->unread = 0; }
    unread = (uint32_t)unread_total();
}
static void conv_open(int kind, int c, uint32_t node)
{
    conv_kind = kind; conv_ch = c; conv_node = node; conv_list = false;
    if (kind == CONV_CH) send_ch = c;
    pick_id = 0; scroll = 0; chat_new = true;
    lap_scroll_stop(&drag);
}

/* The conversations: everything, each channel, each node a DM went to or
   came from, newest first, with what is unread and the last line. */
typedef struct { int kind, ch; uint32_t node; const msg_t *last; int unread, count; } conv_t;
static int conv_collect(conv_t *out, int max)
{
    int n = 0;
    out[n++] = (conv_t){ CONV_ALL, 0, 0, NULL, unread_total(), msg_n };
    for (int c = 0; c < NCH && n < max; c++) if (ch_on(c)) out[n++] = (conv_t){ CONV_CH, c, 0, NULL, unread_ch[c], 0 };
    for (int k = 0; k < msg_n; k++) {
        const msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (!out[0].last) out[0].last = m;
        if (m->to == MT_BROADCAST) {
            for (int i = 1; i < n; i++)
                if (out[i].kind == CONV_CH && out[i].ch == m->ch) { if (!out[i].last) out[i].last = m; out[i].count++; }
            continue;
        }
        const uint32_t peer = m->mine ? m->to : m->from;
        int i = 1;
        while (i < n && !(out[i].kind == CONV_DM && out[i].node == peer)) i++;
        if (i == n) {
            if (n == max) continue;
            const node_t *nd = node_get(peer, false);
            out[n++] = (conv_t){ CONV_DM, 0, peer, m, nd ? nd->unread : 0, 0 };
        }
        out[i].count++;
    }
    for (int i = 0; i < node_n && n < max; i++) {             /* unread from a node whose lines have scrolled away */
        if (!nodes[i].unread) continue;
        int k = 1;
        while (k < n && !(out[k].kind == CONV_DM && out[k].node == nodes[i].num)) k++;
        if (k == n) out[n++] = (conv_t){ CONV_DM, 0, nodes[i].num, NULL, nodes[i].unread, 0 };
    }
    return n;
}
static int conv_sel;
static void draw_conv_list(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    static conv_t cv_[MAX_NODES + NCH + 1];
    const int n = conv_collect(cv_, (int)(sizeof(cv_) / sizeof(cv_[0])));
    if (conv_sel >= n) conv_sel = n - 1;
    const int rows = 3, total = n * rows + 2;
    smax = total > h ? total - h : 0;
    if (scroll > smax) scroll = smax;
    int yy = y - scroll;
    for (int i = 0; i < n; i++, yy += rows) {
        const conv_t *c = &cv_[i];
        if (yy + rows <= y || yy >= y + h) continue;
        hit(x, yy, x + w, yy + rows - 1, HIT_CONV, i);
        const bool on = i == conv_sel && kbd_used;
        const bool here = c->kind == conv_kind && (c->kind == CONV_ALL || (c->kind == CONV_CH && c->ch == conv_ch) ||
                                                   (c->kind == CONV_DM && c->node == conv_node));
        uint8_t hue = c->kind == CONV_ALL ? (LAP_CYAN | LAP_BRIGHT) : c->kind == CONV_CH ? (LAP_YELLOW | LAP_BRIGHT) : hue_of(c->node);
        if (on) cell(x, yy, '>', A(LAP_YELLOW | LAP_BRIGHT));
        /* the badge: # a channel, @ a node, * everything */
        const char *mark = c->kind == CONV_ALL ? "*" : c->kind == CONV_CH ? "#" : "@";
        put(x + 1, yy, 1, A(hue), "%s", mark);
        int cx = x + 3;
        if (c->kind == CONV_ALL) cx += put(cx, yy, w - 20, A(hue), "ALL MESSAGES");
        else if (c->kind == CONV_CH) {
            cx += put(cx, yy, 14, A(hue), "%s", ch_name(c->ch));
            cx += 2 + put(cx + 1, yy, 10, LAP_UI_FAINT, c->ch ? "ch %d" : "primary", c->ch + 1);
            if (chans[c->ch].mute) cx = lap_ui_chip(cx + 1, yy, "MUTED", true, LAP_BLUE);
            if (ch_default(c->ch)) cx = lap_ui_chip(cx + 1, yy, "PUBLIC", false, LAP_WHITE);
        } else {
            const node_t *nd = node_get(c->node, false);
            cx = lap_ui_chip(cx, yy, who(c->node), true, dim(hue)) + 1;
            cx += put(cx, yy, w - (cx - x) - 14, A(hue), "%s", nd && nd->long_name[0] ? nd->long_name : "");
            if (nd && pki_on() && nd->has_key) lap_ui_chip(x + w - 18, yy, "PRIVATE", true, LAP_YELLOW);
        }
        if (here && !on) put(x + w - 9, yy, 8, LAP_UI_FAINT, "(here)");
        if (c->unread) {
            char u[12];
            snprintf(u, sizeof(u), "%d NEW", c->unread);
            lap_ui_chip(x + w - (int)strlen(u) - 3, yy, u, true, LAP_RED);
        }
        /* the last line */
        if (c->last) {
            char t[12];
            ago(c->last->at_ms, t, sizeof(t));
            const int wl = put(x + 3, yy + 1, 6, A(hue_of(c->last->from)), "%s", c->last->mine ? "you" : who(c->last->from));
            put(x + 4 + wl, yy + 1, w - 12 - wl, c->unread ? LAP_UI_TEXT : LAP_UI_DIM, "%s", c->last->tr[0] ? c->last->tr : scr(c->last->text, c->last->lang));
            put(x + w - 6, yy + 1, 5, LAP_UI_DIM, "%5s", t);
        } else put(x + 3, yy + 1, w - 6, LAP_UI_FAINT, c->kind == CONV_DM ? "no lines left here" : "quiet so far");
        for (int k = x + 3; k < x + w - 1; k += 3) cell(k, yy + 2, '.', LAP_UI_FAINT);
    }
    put(x + 1, yy, w - 2, LAP_UI_FAINT, "A node's page (DM) starts a new conversation.");
    /* below, when there is room: a channel and a node talking through the
       tower; its rings light when a packet is heard */
    const int ay = yy + 3;
    if (ay + 6 <= y + h && w >= 40) {
        static const char *const ART[] = {
            ".---------.               .--------.",
            "| # chan  |~~.         .~~| @ node |",
            "'---------'   \\  (o)  /   '--------'",
            "               '--|--'              ",
            "                 /|\\                ",
            "                /_|_\\               ",
        };
        const int ax = x + (w - 36) / 2;
        const bool hot = last_rx_ms && now_ms - last_rx_ms < 1500u;
        for (int r = 0; r < 6; r++)
            for (int c = 0; ART[r][c]; c++) {
                const char ch = ART[r][c];
                if (ch == ' ') continue;
                uint8_t at = A(LAP_WHITE);
                if (c < 12 && r < 3) at = A(LAP_YELLOW | LAP_BRIGHT);
                else if (c >= 26 && r < 3) at = A(LAP_MAGENTA | LAP_BRIGHT);
                else if (ch == '~' || ch == '\\' || ch == '/' ) at = r < 3 ? A(hot ? LAP_GREEN | LAP_BRIGHT : LAP_GREEN) : A(LAP_WHITE);
                else if (ch == 'o') at = A(rxh > 0 ? LAP_RED | LAP_BRIGHT : LAP_RED);
                cell(ax + c, ay + r, ch, at);
            }
    }
}

static void draw_chat(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    if (conv_list) { draw_conv_list(x, y, w, h); return; }
    conv_read();
    int shown_n = 0;
    for (int k = 0; k < msg_n; k++) shown_n += conv_shows(&msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS]);
    if (!shown_n) {
        smax = 0;
        if (conv_kind == CONV_ALL) { tower(x, y, w, h); return; }
        put(x + 2, y + 2, w - 4, LAP_UI_TEXT, "Nothing in %s yet.", conv_title());
        put(x + 2, y + 4, w - 4, LAP_UI_DIM, conv_kind == CONV_DM ? "SEND writes to this node alone." : "SEND writes to everyone on it.");
        put(x + 2, y + 5, w - 4, LAP_UI_DIM, "CHATS lists every conversation.");
        return;
    }
    const int bw = w - 4 < 24 ? w : w - 4;
    span_t a[WRAP_MAX], b[WRAP_MAX];
    int na, nb, total = 0;
    for (int k = msg_n - 1; k >= 0; k--) {
        const msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (conv_shows(m)) total += bubble_rows(m, bw - 4, a, &na, b, &nb);
    }
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
        if (!conv_shows(m)) continue;
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
    if (a->fav != b->fav) return a->fav;              /* favourites first, ignored last */
    if (a->ignored != b->ignored) return b->ignored;
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
        int cx = lap_ui_chip(x + 1, y, n->short_name, true, fresh && (now_ms / 200u) % 2u ? LAP_WHITE : dim(hue)) + 1;
        if (n->fav) { cell(cx, y, '*', A(LAP_YELLOW | LAP_BRIGHT)); cx += 2; }
        if (n->unread) { char u[8]; snprintf(u, sizeof(u), "%d", n->unread); cx = lap_ui_chip(cx, y, u, true, LAP_RED) + 1; }
        if (n->ignored) cx += 1 + put(cx, y, 8, A(LAP_RED), "IGNORED");
        put(cx, y, w - (cx - x) - 6, n->ignored ? LAP_UI_FAINT : A(hue), "%s", n->long_name[0] ? n->long_name : "(no name yet)");
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
    if (n->sf_router) cx += 1 + put(cx, y + 2, 4, A(LAP_BLUE | LAP_BRIGHT), "S&F");
    if (n->role && n->role != MT_ROLE_CLIENT && cx + 10 < x + w) cx += 1 + put(cx, y + 2, 11, A(LAP_MAGENTA), "%s", mt_role_name(n->role));
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
    /* DEVICE: what it is and how it is set */
    bezel(x, yy, w, 6, " DEVICE ");
    {
        const char *hw = n->named ? mt_hw_name(n->hw) : NULL;
        put(x + 2, yy + 1, 9, LAP_UI_DIM, "hardware");
        put(x + 12, yy + 1, w - 14, hw ? A(LAP_UI_VALUE) : LAP_UI_FAINT, "%s", hw ? hw : "not told yet");
        put(x + 2, yy + 2, 9, LAP_UI_DIM, "role");
        int cx = x + 12 + put(x + 12, yy + 2, 16, n->named ? A(LAP_MAGENTA | LAP_BRIGHT) : LAP_UI_FAINT, "%s",
                              n->named ? mt_role_name(n->role) : "not told yet");
        if (n->licensed) cx = lap_ui_chip(cx + 1, yy + 2, "LICENSED", true, LAP_RED);
        if (n->sf_router) cx = lap_ui_chip(cx + 1, yy + 2, "S&F ROUTER", true, LAP_BLUE);
        put(x + 2, yy + 3, 9, LAP_UI_DIM, "heard");
        if (n->heard_unix) {
            lap_time_t tm;
            lap_time(&tm);
            const uint32_t nu = unix_now(), back = nu > n->heard_unix ? nu - n->heard_unix : 0;
            const int sod = (int)((tm.hour * 3600 + tm.minute * 60 + tm.second - (int)(back % 86400u) + 86400) % 86400);
            char a2[12] = "";
            if (back >= 86400u) snprintf(a2, sizeof(a2), "%ud ago", (unsigned)(back / 86400u));
            else if (n->last_ms) snprintf(a2, sizeof(a2), "%s ago", t);
            put(x + 12, yy + 3, w - 14, A(LAP_UI_LIVE), "%02d:%02d  %s", sod / 3600, sod / 60 % 60, a2);
        } else put(x + 12, yy + 3, w - 14, n->last_ms ? A(LAP_UI_LIVE) : LAP_UI_FAINT, n->last_ms ? "%s ago" : "not since start", t);
        put(x + 2, yy + 4, 9, LAP_UI_DIM, "hops");
        cx = x + 12 + put(x + 12, yy + 4, 12, A(n->hops ? LAP_YELLOW | LAP_BRIGHT : LAP_GREEN | LAP_BRIGHT),
                          n->hops ? "%d away" : "direct", n->hops);
        if (n->fav) cx = lap_ui_chip(cx + 1, yy + 4, "FAVOURITE", true, LAP_YELLOW);
        if (n->ignored) cx = lap_ui_chip(cx + 1, yy + 4, "IGNORED", true, LAP_RED);
        (void)cx;
    }
    yy += 7;
    /* LINKS: who it says it hears (neighbour info), with SNR */
    if (n->nbr_n) {
        const int lr = (n->nbr_n + 1) / 2;
        bezel(x, yy, w, lr + 2, " LINKS IT REPORTS ");
        for (int k = 0; k < n->nbr_n; k++) {
            const int col = k % 2, row = k / 2, lx = x + 2 + col * ((w - 4) / 2);
            const int q = n->nbr_snr[k];
            const int lw = put(lx, yy + 1 + row, 6, A(hue_of(n->nbr[k])), "%s", who(n->nbr[k]));
            put(lx + lw + 1, yy + 1 + row, 10, A(q >= 0 ? LAP_UI_LIVE : LAP_UI_WARN), "%+.1f dB", q / 4.0);
        }
        yy += lr + 3;
    }
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
/* The waypoints: name, who dropped it, how far, when it goes. */
static void draw_wpts(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    const int rows = 3, total = wpt_n * rows + 3;
    smax = total > h ? total - h : 0;
    if (scroll > smax) scroll = smax;
    int yy = y - scroll;
    if (!wpt_n) {
        static const char *const FLAG[] = { "   |>  ", "   |   ", "  _|_  " };
        for (int i = 0; i < 3; i++) put(x + (w - 7) / 2, y + 2 + i, 7, A(i ? LAP_WHITE : LAP_YELLOW | LAP_BRIGHT), "%s", FLAG[i]);
        put(x + 2, y + 6, w - 4, LAP_UI_TEXT, "No waypoints yet.");
        put(x + 2, y + 8, w - 4, LAP_UI_DIM, "Waypoints sent on the mesh land here and on MAP.");
        put(x + 2, y + 9, w - 4, LAP_UI_DIM, "MORE > DROP A WAYPOINT HERE sends one.");
        return;
    }
    const uint32_t nu = unix_now();
    for (int i = wpt_n - 1; i >= 0; i--, yy += rows) {
        const wpt_t *p = &wpts[i];
        if (yy + rows <= y || yy >= y + h) continue;
        hit(x, yy, x + w, yy + rows - 1, HIT_WPT, i);
        cell(x + 1, yy, 'X', A(LAP_YELLOW | LAP_BRIGHT));
        const int nl = put(x + 3, yy, w - 22, A(LAP_YELLOW | LAP_BRIGHT), "%s", p->name);
        put(x + 4 + nl, yy, 12, A(hue_of(p->from)), "%s", p->from == cfg.node ? "you" : who(p->from));
        if (p->expire && nu) {
            const uint32_t left = p->expire > nu ? p->expire - nu : 0;
            put(x + w - 12, yy, 11, LAP_UI_DIM, left > 86400u ? "%ud left" : left > 3600u ? "%uh left" : "%um left",
                (unsigned)(left > 86400u ? left / 86400u : left > 3600u ? left / 3600u : left / 60u));
        } else put(x + w - 12, yy, 11, LAP_UI_FAINT, p->expire ? "expires" : "no expiry");
        float brg = 0;
        if (me_ok) {
            char d[16];
            dist_text(km_bearing(me_lat, me_lon, p->lat * 1e-7, p->lon * 1e-7, &brg), d, sizeof(d));
            cell(x + 3, yy + 1, arrow(brg), A(LAP_YELLOW | LAP_BRIGHT));
            const int dl = put(x + 5, yy + 1, 18, A(LAP_UI_VALUE), "%s %s", d, compass(brg));
            put(x + 6 + dl, yy + 1, w - 8 - dl, LAP_UI_DIM, "%s", p->desc);
        } else put(x + 3, yy + 1, w - 5, LAP_UI_DIM, "%.5f, %.5f  %s", p->lat * 1e-7, p->lon * 1e-7, p->desc);
        for (int k = x + 3; k < x + w - 1; k += 3) cell(k, yy + 2, '.', LAP_UI_FAINT);
    }
    put(x + 1, yy, w - 2, LAP_UI_FAINT, "Tap one to remove it here.");
}

static void draw_radar(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    if (wpt_list) { draw_wpts(x, y, w, h); return; }
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
    int links = 0;
    for (int i = 0; i < node_n; i++) links += nodes[i].nbr_n;
    if (links) put(x + 1, y + h - 1, w - 2, LAP_UI_FAINT, "Cyan: %d links nodes report. Dashed: guessed.", links);
    else put(x + 1, y + h - 1, w - 2, LAP_UI_FAINT, "Dashed = relayed. Tap a node to open it.");
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
        { "TRACEROUTE", ">>>" }, { "NEIGHBORS", "[n]" }, { "PKI DM", "[#]" }, { "PRIVATE", "[?]" }, { "OTHER CH", "[?]" },
        { "REPEAT", "[r]" }, { "MY ECHO", "[e]" }, { "WAYPOINT", "[X]" }, { "RANGE TEST", "[~]" }, { "S&F", "[S]" },
        { "IGNORED", "[-]" },
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
    const int dty = MT_REGIONS[cfg.region].duty < 100 ? MT_REGIONS[cfg.region].duty : 10;
    const float ful = air_util() / (float)dty;
    if (yy >= clip0 && yy < clip1) lap_ui_meter(x + 12, yy, w - 24, ful > 1 ? 1 : ful, LAP_CYAN | LAP_BRIGHT);
    if (MT_REGIONS[cfg.region].duty < 100) put(x + w - 11, yy, 10, A(LAP_CYAN | LAP_BRIGHT), "%.1f/%d%%/h", (double)air_util(), dty);
    else put(x + w - 11, yy, 10, A(LAP_CYAN | LAP_BRIGHT), "%4.1f%%/h", (double)air_util());
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
    /* RANGE TEST: what goes out, and each "seq" heard with its signal */
    if (ext.range || range_got) {
        const int rr = 3 + range_rx_n;
        bezel(x, yy, w, rr + 1, " RANGE TEST ");
        if (ext.range >= 2) {
            const uint32_t next = range_ms ? RANGE_S[ext.range] * 1000u - (now_ms - range_ms < RANGE_S[ext.range] * 1000u ? now_ms - range_ms : RANGE_S[ext.range] * 1000u) : 0;
            put(x + 2, yy + 1, w - 4, A(LAP_CYAN | LAP_BRIGHT), "SENDING seq %u every %u s  next in %u s", (unsigned)range_seq,
                (unsigned)RANGE_S[ext.range], (unsigned)(next / 1000u));
        } else put(x + 2, yy + 1, w - 4, A(LAP_GREEN | LAP_BRIGHT), ext.range ? "LISTENING, every packet to the card" : "OFF (MODULES > RANGE TEST)");
        put(x + 2, yy + 2, w - 4, LAP_UI_DIM, "heard %u  missed %u  %s", (unsigned)range_got, (unsigned)range_lost,
            range_file[0] ? range_file + 10 : "");
        for (int k = range_rx_n - 1, r = 0; k >= 0; k--, r++) {
            const range_rx_t *q = &range_rx[k];
            char t[8], d[16] = "";
            ago(q->at_ms, t, sizeof(t));
            if (q->km >= 0) dist_text(q->km, d, sizeof(d));
            const int l = sig_level(q->rssi, q->snr);
            sig_bars(x + 2, yy + 3 + r, l);
            put(x + 7, yy + 3 + r, w - 9, A(level_hue(l)), "seq %-4u %-5s %5.1f dB %4d dBm %s %s", (unsigned)q->seq, who(q->from),
                q->snr == INT16_MIN ? 0.0 : q->snr / 10.0, q->rssi == INT16_MIN ? 0 : q->rssi / 10, d, t);
        }
        yy += rr + 2;
    }
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

/* ---- SCAN: the presets in use nearby ---- */
/* One colour a preset: long greens and cyans, medium yellows and red, short magentas and blue. */
static uint8_t scan_hue(int preset)
{
    static const uint8_t H[] = { LAP_GREEN | LAP_BRIGHT, LAP_GREEN, LAP_CYAN, LAP_CYAN | LAP_BRIGHT, LAP_YELLOW | LAP_BRIGHT,
                                 LAP_YELLOW, LAP_RED | LAP_BRIGHT, LAP_MAGENTA | LAP_BRIGHT, LAP_MAGENTA, LAP_BLUE | LAP_BRIGHT };
    return H[(unsigned)preset % sizeof(H)];
}
static char scan_digit(int i) { return (char)(i < 9 ? '1' + i : i == 9 ? '0' : '+'); }
/* =====----- in the row's colour; the rest grey. */
static void scan_bar(int x, int y, int w, float f, uint8_t hue, uint8_t bg)
{
    if (w <= 0) return;
    int lit = (int)(f * (float)w + 0.5f);
    if (f > 0 && lit == 0) lit = 1;
    for (int k = 0; k < w; k++) cell(x + k, y, k < lit ? '=' : '-', LAP_ATTR(k < lit ? hue : LAP_BLACK | LAP_BRIGHT, bg));
}
static void draw_scan(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    scan_plan();
    int yy = y - scroll;
    const mt_region_t *rg = &MT_REGIONS[scan.region];
    /* the band as a ruler: each preset's default slot where it sits, by its row number */
    {
        char lo[8], hi[8];
        snprintf(lo, sizeof(lo), "%u", (unsigned)(rg->lo_khz / 1000u));
        snprintf(hi, sizeof(hi), "%u", (unsigned)((rg->hi_khz + 999u) / 1000u));
        const int rx = x + 2 + (int)strlen(lo), rw = w - 4 - (int)strlen(lo) - (int)strlen(hi);
        put(x + 1, yy + 1, 4, LAP_UI_DIM, "%s", lo);
        put(rx + rw + 1, yy + 1, 4, LAP_UI_DIM, "%s", hi);
        for (int k = 0; k < rw; k++) {
            cell(rx + k, yy, k % 4 ? ' ' : '.', LAP_UI_FAINT);
            cell(rx + k, yy + 1, '-', A(LAP_CYAN));
        }
        const uint64_t lo_hz = (uint64_t)rg->lo_khz * 1000u, band = (uint64_t)(rg->hi_khz - rg->lo_khz) * 1000u;
        for (int i = 0; i < scan.n && rw > 2 && band; i++) {
            const mt_scan_entry_t *e = &scan.e[i];
            const int at = (int)((e->freq_hz - lo_hz) * (uint64_t)(rw - 1) / band);
            const bool tuned = scan_on && i == scan.cur;
            const uint8_t hue = scan_hue(e->preset);
            if (tuned) {
                if (yy + 1 >= clip0 && yy + 1 < clip1) lap_put_cell(rx + at, yy + 1, scan_digit(i), LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLUE));
                cell(rx + at, yy, 'v', A(LAP_WHITE | LAP_BRIGHT));
                cell(rx + at, yy + 2, '^', A(LAP_WHITE | LAP_BRIGHT));
            } else cell(rx + at, yy + 1, scan_digit(i), !e->on ? LAP_UI_FAINT : e->packets ? A(hue) : A(dim(hue)));
            if (e->packets && !tuned) cell(rx + at, yy, '\'', A(hue));   /* heard here */
        }
        yy += 3;
    }
    /* where the radio is, and how long it stays */
    if (scan_on && scan.cur >= 0) {
        const mt_scan_entry_t *e = &scan.e[scan.cur];
        const uint32_t el = now_ms - scan.step_at;
        const bool hold = scan.rx_step && now_ms - scan.last_rx_at < e->hang_ms;
        int cx = x + 1;
        if (yy >= clip0 && yy < clip1)
            cx = lap_ui_chip(cx, yy, scan.pin >= 0 ? "PINNED" : hold ? "HOLD" : "LISTEN", true,
                             scan.pin >= 0 ? LAP_YELLOW : hold ? LAP_MAGENTA : LAP_GREEN) + 1;
        cx += put(cx, yy, w - (cx - x), A(scan_hue(e->preset)), "%s ", MT_SCAN_PRESETS[e->preset].name);
        put(cx, yy, w - (cx - x), LAP_UI_TEXT, "%u.%03u SF%u BW%u 4/%u", (unsigned)((e->freq_hz + 500u) / 1000000u),
            (unsigned)((e->freq_hz + 500u) / 1000u % 1000u), e->sf, (unsigned)(e->bw_hz / 1000u), e->cr);
        /* the dwell: how far into it, by the scanner's own clock */
        const int mw = w - 13;
        if (scan.pin >= 0) put(x + 1, yy + 1, w - 2, LAP_UI_DIM, "stays here until UNPIN");
        else if (mw >= 4) {
            const float f = el >= e->dwell_ms ? 1.0f : (float)el / (float)e->dwell_ms;
            cell(x + 1, yy + 1, '[', A(LAP_CYAN));
            scan_bar(x + 2, yy + 1, mw, f, LAP_CYAN | LAP_BRIGHT, LAP_BLACK);
            cell(x + 2 + mw, yy + 1, ']', A(LAP_CYAN));
            put(x + mw + 4, yy + 1, 7, A(LAP_UI_VALUE), "%2u/%us", (unsigned)(el / 1000u), (unsigned)(e->dwell_ms / 1000u));
        }
    } else {
        put(x + 1, yy, w - 2, A(LAP_UI_DIM), "STOPPED. GO steps through %d presets", scan.n);
        put(x + 1, yy + 1, w - 2, A(LAP_UI_DIM), "on %s, each on its default slot.", rg->name);
    }
    yy += 3;
    /* the table */
    /* narrow: 5-letter tags and SNR in whole dB; wide: full names, SNR to 0.1 dB */
    const bool long_names = w >= 64;
    const int nw = long_names ? 11 : 5, sw = long_names ? 6 : 4;
    const int cM = x + 4 + nw + 1, cP = cM + 7, cN = cP + 4, cR = cN + 4, cS = cR + 5, cA = cS + sw - 1,
              cH = cA + 5, cB = cH + 4;
    const int bw = x + w - 1 - cB;
    put(x + 1, yy, 3, LAP_UI_FAINT, " #");
    put(x + 4, yy, nw + 1, LAP_UI_FAINT, "PRESET");
    put(cM, yy, 7, LAP_UI_FAINT, "    MHz");
    put(cP, yy, 4, LAP_UI_FAINT, " PKT");
    put(cN, yy, 4, LAP_UI_FAINT, " NOD");
    put(cR, yy, 4, LAP_UI_FAINT, " dBm");
    put(cS, yy, sw - 1, LAP_UI_FAINT, long_names ? "  SNR" : "SNR");
    put(cA, yy, 4, LAP_UI_FAINT, " AGO");
    put(cH, yy, 3, LAP_UI_FAINT, "CH");
    if (bw >= 4) put(cB, yy, bw, LAP_UI_FAINT, "SHARE");
    yy++;
    if (scan_sel >= scan.n) scan_sel = scan.n ? scan.n - 1 : 0;
    for (int i = 0; i < scan.n; i++, yy++) {
        const mt_scan_entry_t *e = &scan.e[i];
        if (yy < clip0 || yy >= clip1) continue;
        const bool tuned = scan_on && i == scan.cur, chosen = i == scan_sel;
        const uint8_t bg = chosen ? LAP_BLUE : LAP_BLACK, hue = scan_hue(e->preset);
        if (chosen) lap_fill(x, yy, w, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLUE));
        const uint8_t num = e->packets ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, bg) : LAP_ATTR(LAP_BLACK | LAP_BRIGHT, bg);
        if (scan.pin == i) put(x + 1, yy, 1, LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, bg), "*");
        else if (tuned) put(x + 1, yy, 1, LAP_ATTR(LAP_WHITE | LAP_BRIGHT, bg), ">");
        put(x + 2, yy, 1, LAP_ATTR(hue, bg), "%c", scan_digit(i));
        put(x + 4, yy, nw, LAP_ATTR(e->on ? hue : (uint8_t)(LAP_BLACK | LAP_BRIGHT), bg), "%s",
            long_names ? MT_SCAN_PRESETS[e->preset].name : MT_SCAN_PRESETS[e->preset].tag);
        put(cM, yy, 7, LAP_ATTR(e->on ? LAP_YELLOW : (uint8_t)(LAP_BLACK | LAP_BRIGHT), bg), "%3u.%03u",
            (unsigned)((e->freq_hz + 500u) / 1000000u), (unsigned)((e->freq_hz + 500u) / 1000u % 1000u));
        if (!e->on) { put(cP, yy, 12, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, bg), " off"); continue; }
        put(cP, yy, 4, num, "%4u", (unsigned)e->packets);
        put(cN, yy, 4, num, e->node_more ? "%3u+" : "%3u", (unsigned)e->node_n);
        if (e->packets) {
            char t[8];
            ago(e->last_ms, t, sizeof(t));
            const int l = sig_level(e->rssi, e->snr);
            put(cR, yy, 4, LAP_ATTR(level_hue(l), bg), "%4d", e->rssi == INT16_MIN ? 0 : e->rssi / 10);
            const double sn = e->snr == INT16_MIN ? 0.0 : e->snr / 10.0;
            if (long_names) put(cS, yy, 5, LAP_ATTR(level_hue(l), bg), "%5.1f", sn);
            else put(cS, yy, 3, LAP_ATTR(level_hue(l), bg), "%3d", (int)(sn < 0 ? sn - 0.5 : sn + 0.5));
            put(cA, yy, 4, LAP_ATTR(LAP_WHITE, bg), "%4s", t);
            const int th = mt_scan_top_hash(e);
            put(cH, yy, 3, LAP_ATTR(th == e->def_hash ? (LAP_GREEN | LAP_BRIGHT) : (LAP_YELLOW | LAP_BRIGHT), bg), "%02X%s", th & 0xFF,
                e->hash_n > 1 ? "+" : "");
        } else {
            put(cR, yy, 4, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, bg), "   .");
            /* how long it has listened there: seconds, then minutes, then hours, in four cells */
            const unsigned ls = (unsigned)(e->listen_ms / 1000u);
            if (!e->listen_ms) put(cA, yy, 4, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, bg), "   .");
            else put(cA, yy, 4, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, bg), ls < 600u ? "%3us" : ls < 6000u ? "%3um" : "%3uh",
                     ls < 600u ? ls : ls < 6000u ? ls / 60u : (ls / 3600u > 999u ? 999u : ls / 3600u));
        }
        if (bw >= 4) {
            const float f = mt_scan_share(&scan, i);
            const int pw = bw >= 10 ? 4 : 0;
            scan_bar(cB, yy, bw - pw, f, hue, bg);
            if (pw && e->packets) put(cB + bw - pw + 1, yy, 3, LAP_ATTR(LAP_WHITE, bg), "%2d%%", (int)(f * 100.0f + 0.5f));
        }
        hit(x, yy, x + w, yy + 1, HIT_SCAN, i);
    }
    yy++;
    /* totals */
    if (yy >= clip0 && yy < clip1) {
        uint32_t other = 0;
        for (int i = 0; i < scan.n; i++)
            for (int k = 0; k < scan.e[i].hash_n; k++)
                if (scan.e[i].hash[k] != scan.e[i].def_hash) other += scan.e[i].hash_count[k];
        char t[24];
        int cx = x + 1;
        const struct { const char *l; uint32_t v; uint8_t hue; } C[] = {
            { "HEARD", scan.total, LAP_GREEN }, { "NODES", (uint32_t)scan.seen_n, LAP_CYAN }, { "READ", scan.readable, LAP_GREEN },
            { "OTHER CH", other, LAP_YELLOW }, { "SWEEPS", scan.sweeps, LAP_MAGENTA },
        };
        for (int i = 0; i < (int)(sizeof(C) / sizeof(C[0])); i++) {
            snprintf(t, sizeof(t), "%s %u", C[i].l, (unsigned)C[i].v);
            if (cx + (int)strlen(t) + 2 > x + w) break;
            cx = lap_ui_chip(cx, yy, t, C[i].v > 0, C[i].hue) + 1;
        }
    }
    yy += 2;
    /* packets as they come */
    if (!scan.log_n) {
        put(x + 2, yy++, w - 4, LAP_UI_TEXT, "Each preset on its default slot, %u s or more.", (unsigned)SCAN_DWELL_S[scan_dwell % 5]);
        put(x + 2, yy++, w - 4, LAP_UI_DIM, "CH is the channel hash: green the default.");
        put(x + 2, yy++, w - 4, LAP_UI_DIM, "ENTER or a second tap: listen on that row only.");
        put(x + 2, yy++, w - 4, A(LAP_UI_WARN), "While SCAN runs this node sends nothing.");
    } else {
        put(x + 1, yy++, w - 2, LAP_UI_FAINT, "# FROM TO   HOP CH ID       WHAT");
        const int cw = x + w - 9;                        /* dBm and AGO at the right */
        put(cw, yy - 1, 8, LAP_UI_FAINT, " dBm AGO");
        for (int k = 0; k < scan.log_n; k++, yy++) {
            const mt_scan_pkt_t *p = mt_scan_log_at(&scan, k);
            if (!p || yy < clip0 || yy >= clip1) continue;
            const bool fresh = now_ms - p->at_ms < 1500u && !calm();
            const uint8_t bg = fresh ? LAP_BLUE : LAP_BLACK;
            if (fresh) lap_fill(x, yy, w, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLUE));
            char t[8], to[6], what[48];
            ago(p->at_ms, t, sizeof(t));
            snprintf(to, sizeof(to), "%s", p->to == MT_BROADCAST ? "all" : who(p->to));
            if (p->readable && p->portnum == MT_PORT_TEXT && p->text[0]) snprintf(what, sizeof(what), "\"%s\"", p->text);
            else if (p->readable) snprintf(what, sizeof(what), "%s", mt_port_name(p->portnum));
            else snprintf(what, sizeof(what), "%s%s", p->is_default ? "UNREAD" : !p->hash && p->to != MT_BROADCAST ? "PKI DM" : "PRIVATE", p->via_mqtt ? " MQTT" : "");
            const int pr = p->entry < scan.n ? scan.e[p->entry].preset : 0;
            put(x + 1, yy, 1, LAP_ATTR(scan_hue(pr), bg), "%c", scan_digit(p->entry));
            put(x + 3, yy, 4, LAP_ATTR(hue_of(p->from), bg), "%s", who(p->from));
            put(x + 8, yy, 4, LAP_ATTR(LAP_WHITE, bg), "%s", to);
            put(x + 13, yy, 3, LAP_ATTR(LAP_CYAN | LAP_BRIGHT, bg), "%u/%u", p->hop_limit, p->hop_start);
            put(x + 17, yy, 2, LAP_ATTR(p->is_default ? (LAP_GREEN | LAP_BRIGHT) : (LAP_YELLOW | LAP_BRIGHT), bg), "%02X", p->hash);
            put(x + 20, yy, 8, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, bg), "%08x", (unsigned)p->id);
            put(x + 29, yy, cw - x - 29, LAP_ATTR(p->readable ? (LAP_GREEN | LAP_BRIGHT) : (LAP_YELLOW), bg), "%s", what);
            put(cw, yy, 4, LAP_ATTR(level_hue(sig_level(p->rssi, p->snr)), bg), "%4d", p->rssi == INT16_MIN ? 0 : p->rssi / 10);
            put(cw + 5, yy, 3, LAP_ATTR(LAP_WHITE, bg), "%3s", t);
        }
    }
    const int total = yy + scroll - y;
    smax = total > h ? total - h : 0;
}

/* ---- SHARE: the channel set as a meshtastic.org link and a QR code ---- */
static char share_url[URL_MAX];
static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(25)], qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(25)];
static uint16_t qr_px[(117 + 8) * (117 + 8)];
static int qr_side;                     /* modules a side with the quiet zone, 0 = none */
static uint32_t qr_serial;
/* This node's channels (all of them, or the primary alone) and its LoRa
   settings, as the official apps share them. */
static void chset_mine(mt_chset_t *s, bool all)
{
    memset(s, 0, sizeof(*s));
    for (int c = 0; c < NCH && s->n < MT_SET_MAX; c++) {
        if (!ch_on(c) || (!all && c)) continue;
        mt_chset_ch_t *o = &s->ch[s->n++];
        snprintf(o->name, sizeof(o->name), "%s", chans[c].name);
        o->psk_len = chans[c].psk_len; memcpy(o->psk, chans[c].psk, chans[c].psk_len);
        o->precision = chans[c].precision; o->muted = chans[c].mute;
    }
    s->has_lora = true;
    s->lora = lora_now();
    s->region_code = MT_REGIONS[cfg.region].code;
    s->hop_limit = ext.hop_limit;
}
static void share_build(void)
{
    mt_chset_t set;
    chset_mine(&set, share_all);
    qr_side = 0;
    if (mt_chset_url(&set, share_url, sizeof(share_url)) < 0) { snprintf(share_url, sizeof(share_url), "too many channels for one link"); return; }
    if (!qrcodegen_encodeText(share_url, qr_tmp, qr, qrcodegen_Ecc_LOW, 1, 25, qrcodegen_Mask_AUTO, true)) return;
    const int n = qrcodegen_getSize(qr);
    qr_side = n + 8;
    const uint16_t white = 0xFFFF, black = 0x0000;
    for (int y = 0; y < qr_side; y++)
        for (int x = 0; x < qr_side; x++)
            qr_px[y * qr_side + x] = qrcodegen_getModule(qr, x - 4, y - 4) ? black : white;
    qr_serial++;
}
static void share_draw(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    smax = 0;
    /* the channels it carries, as chips */
    int cx = x + 1, yy = y;
    cx = lap_ui_chip(cx, yy, share_all ? "ALL CHANNELS" : "PRIMARY ONLY", true, LAP_CYAN) + 1;
    hit(x, yy, cx, yy + 1, HIT_SHARE_ALL, 0);
    for (int c = 0; c < NCH; c++) {
        if (!ch_on(c) || (!share_all && c)) continue;
        if (cx + (int)strlen(ch_name(c)) + 3 > x + w) break;
        cx = lap_ui_chip(cx, yy, ch_name(c), true, c ? LAP_YELLOW : LAP_GREEN) + 1;
    }
    yy += 2;
    /* the code: as square as the cells allow, with room for the link under it */
    span_t sp[12];
    const int lines = wrap_spans(share_url, w - 2, sp, 12);
    int cw = 0, chh = 0;
    lap_cell_px(&cw, &chh);
    if (cw <= 0 || chh <= 0) { cw = 10; chh = 17; }
    const int avail_h = h - (yy - y) - lines - 2;
    if (qr_side && avail_h > 6) {
        int side_px = w * cw < avail_h * chh ? w * cw : avail_h * chh;
        side_px -= side_px % qr_side;                    /* whole pixels a module */
        const int qw = side_px / cw, qh = side_px / chh;
        lap_image(x + (w - qw) / 2, yy, qw, qh, qr_px, qr_side, qr_side, qr_serial);
        yy += qh + 1;
    } else if (!qr_side) {
        put(x + 2, yy + 1, w - 4, A(LAP_UI_WARN), "The link is too long for a QR code here.");
        yy += 3;
    }
    for (int i = 0; i < lines; i++) put_span(x + 1, yy++, share_url, sp[i], -1, A(i ? LAP_CYAN : LAP_CYAN | LAP_BRIGHT));
    put(x + 1, yy, w - 2, LAP_UI_FAINT, "Scan with the Meshtastic app, or type the link.");
}

/* ---- TRANSLATE ---- */
static void lang_art(int x, int y, int w)
{
    const int bx = x + (w - 30) / 2;
    static const char *const C[] = { ".------.", "|  EN  |", "'--.---'" };
    char mid[12];
    const int nl = xl_count();
    if (nl <= 1) snprintf(mid, sizeof(mid), "|  %s  |", LANG_UP[xl_first()]);
    else snprintf(mid, sizeof(mid), "| %s+%d |", LANG_UP[xl_first()], nl - 1 > 9 ? 9 : nl - 1);
    const char *const B[] = { ".------.", mid, "'---.--'" };
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
/* MORE LANGUAGES where the board cannot open STORE for us (2.23.2): a shelf
   of the eight, lit by what the card holds, and where the rest come from. */
static void langs_draw(int x, int y, int w, int h)
{
    clip0 = y; clip1 = y + h;
    int yy = y + 1 - scroll;
    const int bx = x + (w > 25 ? (w - 25) / 2 : 0);
    put(bx, yy, 25, A(LAP_MAGENTA), ".--.--.--.--.--.--.--.--.");
    for (int k = 0; k < 8; k++) {
        const int l = MT_LANG_FIRST_CARD + k, st = lang_st[l].state;
        const bool pack = st == LS_READY && lang_st[l].alt == 0;
        const bool card = (lf_have & MT_LANG_BIT(l)) || (st == LS_READY && lang_st[l].alt);
        if (yy + 1 >= clip0 && yy + 1 < clip1) cell(bx + 3 * k, yy + 1, '|', A(LAP_MAGENTA));
        put(bx + 3 * k + 1, yy + 1, 2, pack ? A(LAP_CYAN | LAP_BRIGHT) : card ? A(LAP_GREEN | LAP_BRIGHT) : LAP_UI_FAINT, "%s",
            LANG_UP[l]);
    }
    if (yy + 1 >= clip0 && yy + 1 < clip1) cell(bx + 24, yy + 1, '|', A(LAP_MAGENTA));
    put(bx, yy + 2, 25, A(LAP_MAGENTA), "'--'--'--'--'--'--'--'--'");
    put(bx, yy + 3, x + w - bx - 1, LAP_UI_FAINT, " green on the card, cyan a pack");
    yy += 5;
    put(x + 2, yy++, w - 3, LAP_UI_TEXT, "Spanish is built in. The other eight are a");
    put(x + 2, yy++, w - 3, LAP_UI_TEXT, "file each on the card, 155-198 KB.");
    yy++;
    put(x + 2, yy++, w - 3, A(LAP_UI_VALUE), "GET THEM FROM");
    put(x + 4, yy++, w - 5, A(LAP_CYAN | LAP_BRIGHT), "STORE > MESH LANGUAGES");
    put(x + 4, yy++, w - 5, LAP_UI_DIM, "over Wi-Fi, into the folder this app reads");
    if (lf_packs()) {
        put(x + 4, yy++, w - 5, A(LAP_CYAN | LAP_BRIGHT), "or PACKS > LANGUAGES");
    }
    yy++;
    put(x + 2, yy++, w - 3, LAP_UI_TEXT, "Then turn them on in FROM.");
    put(x + 2, yy++, w - 3, LAP_UI_FAINT, "BACK closes this.");
    const int total = yy + scroll - y + 1;
    smax = total > h ? total - h : 0;
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
    /* FROM: the languages read, a chip each; green ready, yellow loading, red not on the card;
       on as many rows as the width needs */
    {
        const int c00 = x + 6;
        int cx = c00;
        put(x + 1, yy, 5, A(LAP_UI_VALUE), "FROM");
        for (int l = MT_LANG_ES; l < MT_LANG_COUNT; l++) {
            const int st = l == MT_LANG_ES ? LS_READY : lang_st[l].state;
            const uint8_t hue = !xl_on(l) ? LAP_BLUE : st == LS_READY ? LAP_GREEN : st == LS_MISSING || st == LS_BAD ? LAP_RED
                              : LAP_YELLOW;
            const int cw = (int)strlen(LANG_UP[l]) + 4;            /* *ES* and a gap */
            if (cx + cw - 1 > x + w && cx > c00) { yy++; cx = c00; }
            if (yy >= clip0 && yy < clip1) {
                lap_ui_chip(cx, yy, LANG_UP[l], xl_on(l), hue);
                hit(cx, yy, cx + cw, yy + 1, HIT_XL_LANG, l);
            }
            cx += cw;
        }
    }
    yy++;
    /* what the card holds of them */
    if (yy >= clip0 && yy < clip1) {
        char miss[24] = "", load[24] = "";
        for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++) {
            if (!xl_on(l)) continue;
            const int st = lang_st[l].state;
            char *d = st == LS_MISSING || st == LS_BAD ? miss : st != LS_READY ? load : NULL;
            if (d && strlen(d) + 4 < sizeof(miss)) snprintf(d + strlen(d), sizeof(miss) - strlen(d), "%s%s", d[0] ? " " : "", LANG_UP[l]);
        }
        if (miss[0]) {
            put(x + 2, yy, w - 3, A(LAP_RED | LAP_BRIGHT), "%s: %s", miss, lf_where());
            hit(x, yy, x + w, yy + 1, HIT_XL_MORE, 0);
        }
        else if (load[0]) put(x + 2, yy, w - 3, A(LAP_YELLOW), "%s: reading from the card", load);
        else put(x + 2, yy, w - 3, LAP_UI_FAINT, xl_count() > 1 ? "tap a language to read it or not"
                                                               : "tap a language to read it too");
    }
    yy++;
    /* MORE LANGUAGES: STORE at MESH LANGUAGES, or where to get them */
    if (yy >= clip0 && yy < clip1) {
        const int cx = lap_ui_chip(x + 2, yy, "+ MORE LANGUAGES", true, LAP_MAGENTA);
        hit(x + 2, yy, cx + 1, yy + 1, HIT_XL_MORE, 0);
        put(cx + 2, yy, x + w - cx - 3, LAP_UI_FAINT, "STORE > MESH LANGUAGES");
    }
    yy += 2;
    /* TRY IT */
    span_t sp[8];
    const char *es = try_es[0] ? scr(try_es, try_lang) : "Tap here and write a line in any of them.";
    const int ne = wrap_spans(es, w - 4, sp, 8);
    span_t sq[8];
    const int nt = try_en[0] ? wrap_spans(try_en, w - 4, sq, 8) : 0;
    const int bh2 = 2 + ne + (nt ? nt + 1 : 0);
    bezel(x, yy, w, bh2, " TRY IT ");
    hit(x, yy, x + w, yy + bh2, HIT_TRY, 0);
    int r = yy + 1;
    for (int i = 0; i < ne; i++) put_span(x + 2, r++, es, sp[i], -1, try_es[0] ? LAP_UI_TEXT : LAP_UI_FAINT);
    if (nt) {
        for (int k = x + 2; k < x + w - 10; k += 2) cell(k, r, '-', LAP_UI_FAINT);
        if (try_lang > MT_LANG_EN && try_lang < MT_LANG_COUNT) put(x + w - 9, r, 7, A(LAP_CYAN), "%s > EN", LANG_UP[try_lang]);
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
        put(x + 2 + cw2, yy, w - 8 - cw2, fresh ? A(LAP_WHITE | LAP_BRIGHT) : LAP_UI_TEXT, "%s", scr(e->es, MT_LANG_UNKNOWN));
        put(x + w - 5, yy++, 4, LAP_UI_FAINT, "%4s", t);
        put(x + 4, yy++, w - 6, e->en[0] ? A(LAP_CYAN | LAP_BRIGHT) : LAP_UI_FAINT, "%s",
            e->en[0] ? e->en : "(not translated yet: tap)");
        hit(x, y0, x + w, yy, HIT_HIST, (int)k);
    }
    if (!hist.n) {
        put(x + 2, yy++, w - 4, LAP_UI_FAINT, "Nothing yet. Each TRY IT and each line heard");
        put(x + 2, yy++, w - 4, LAP_UI_FAINT, "in a language you read is kept here.");
    }
    yy++;
    /* the phrasebook, sent in the language chosen (the chip changes it) */
    const mt_lang_t ql = quick_lang();
    const mt_lang_t shown = ql == MT_LANG_EN ? (mt_lang_t)xl_first() : ql;     /* beside the English */
    if (yy >= clip0 && yy < clip1) {
        put(x + 1, yy, 11, A(LAP_UI_VALUE), "PHRASEBOOK");
        char lab[24];
        snprintf(lab, sizeof(lab), "SEND IN %s", ql == MT_LANG_EN ? "ENGLISH" : mt_lang_name(ql));
        const int c0 = x + 12, c1 = lap_ui_chip(c0, yy, lab, true, LAP_YELLOW);
        hit(c0, yy, c1, yy + 1, HIT_QUICK_LANG, 0);
        put(c1 + 1, yy, x + w - c1 - 2, LAP_UI_FAINT, "tap a line to send it");
    }
    yy++;
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
        /* side by side when both fit, else the other language under the English; the one sent is bright */
        const char *other = scr(mt_phrase_in(i, shown), shown);
        char enb[64];
        const char *pen = phrase_en(i, shown, enb, sizeof(enb));
        const uint8_t en_at = ql == MT_LANG_EN ? A(LAP_YELLOW | LAP_BRIGHT) : LAP_UI_TEXT;
        const uint8_t ot_at = ql == MT_LANG_EN ? LAP_UI_DIM : A(LAP_YELLOW | LAP_BRIGHT);
        const int half = (w - 4) / 2;
        if (u8_cells(pen, -1) < half && u8_cells(other, -1) <= w - 4 - half) {
            put(x + 2, yy, half - 1, en_at, "%s", pen);
            put(x + 2 + half, yy, w - 4 - half, ot_at, "%s", other);
            hit(x, yy, x + w, yy + 1, HIT_PHRASE, i);
            yy++;
        } else {
            put(x + 2, yy, w - 4, en_at, "%s", pen);
            put(x + 4, yy + 1, w - 6, ot_at, "%s", other);
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
    read_cutout();
    W = lap_cols(); H = lap_rows();
    wide = W >= 90 && W > H * 2;
    kr = lap_ui_keys_rows(4);
    tr = lap_ui_keys_rows(3);
    if (tr > 5) tr = 5;                 /* finger-sized, without eating the chat */
    if (tr < 3) tr = 3;
    compact = !wide && H < 40;
    if (compact) tr = 3;
    if (!wide) {
        hx = 0; hy = 1; hw = W; hh = compact ? 4 : 10;
        sx = 0; sw = W; ty = hy + hh + (compact ? 0 : 1);
        py = ty + tr; px = 0; pw = W; ph = H - kr - py;
    } else {
        hw = 40; hx = 1; hy = 1; hh = H - kr - 1;
        sx = hw + 2; sw = W - sx; ty = 1;
        py = ty + tr; px = sx; pw = sw; ph = H - kr - py;
    }
}

static void set_keys(void)
{
    const bool can = cfg.tx_on && radio >= 0 && !scan_on;
    static char v0[16], v2[16];
    if (share_open) {
        keys[0] = (lap_btn_t){ "BACK", NULL, 'b', false, false, false };
        keys[1] = (lap_btn_t){ share_all ? "PRIMARY" : "ALL", "CHANNELS", 'h', false, ch_count() < 2, false };
        keys[2] = (lap_btn_t){ "ADD LINK", NULL, 'i', false, false, false };
        keys[3] = (lap_btn_t){ "MORE", NULL, 'm', false, false, false };
        return;
    }
    if (langs_open) {
        keys[0] = (lap_btn_t){ "BACK", NULL, 'b', false, false, false };
        keys[1] = (lap_btn_t){ "FROM", xl_chip(), 'f', false, false, false };
        keys[2] = (lap_btn_t){ "TRY IT", NULL, 'y', false, false, false };
        keys[3] = (lap_btn_t){ "MORE", NULL, 'm', false, false, false };
        return;
    }
    switch (tab) {
    case TAB_CHAT:
        if (conv_list) {
            keys[0] = (lap_btn_t){ "CLOSE", NULL, 'c', false, false, false };
            keys[1] = (lap_btn_t){ "NEW DM", "NODES", 'u', false, !node_n, false };
            keys[2] = (lap_btn_t){ "READ ALL", NULL, 'k', false, !unread_total(), false };
        } else if (pick_id) {
            keys[0] = (lap_btn_t){ "REPLY", NULL, 'r', false, !can, false };
            keys[1] = (lap_btn_t){ "REACT", NULL, 'e', false, !can, false };
            keys[2] = (lap_btn_t){ "BACK", NULL, 'b', false, false, false };
        } else {
            snprintf(v0, sizeof(v0), "%.9s", conv_kind == CONV_ALL ? ch_name(send_ch) : conv_title());
            keys[0] = (lap_btn_t){ "SEND", v0, 's', false, !can, false };
            keys[1] = (lap_btn_t){ "QUICK", LANG_UP[quick_lang()], 'q', false, !can, false };
            const int u = unread_total();
            if (u) snprintf(v2, sizeof(v2), "%d NEW", u);
            else snprintf(v2, sizeof(v2), "%s", conv_title());
            keys[2] = (lap_btn_t){ "CHATS", v2, 'c', u > 0, false, false };
        }
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
        keys[0] = (lap_btn_t){ "ZOOM", v0, 'z', false, wpt_list, false };
        keys[1] = (lap_btn_t){ "NAMES", ui.labels ? "ON" : "OFF", 'l', ui.labels != 0, wpt_list, false };
        snprintf(v2, sizeof(v2), "%d", wpt_n);
        keys[2] = (lap_btn_t){ wpt_list ? "MAP" : "WAYPTS", wpt_list ? NULL : v2, 'j', wpt_list, false, false };
        break;
    case TAB_AIR:
        keys[0] = (lap_btn_t){ "SEND", NULL, 's', false, !can, false };
        keys[1] = (lap_btn_t){ "HELLO", NULL, 'n', false, !can, false };
        keys[2] = (lap_btn_t){ "CLEAR", NULL, 'k', false, !air_n, false };
        break;
    case TAB_SCAN:
        snprintf(v0, sizeof(v0), "%d PRESETS", scan.n);
        keys[0] = (lap_btn_t){ scan_on ? "STOP" : "GO", scan_on ? NULL : v0, 'g', scan_on, radio < 0, false };
        if (scan.n && scan_sel < scan.n) snprintf(v2, sizeof(v2), "%s", MT_SCAN_PRESETS[scan.e[scan_sel].preset].tag);
        else v2[0] = 0;
        keys[1] = (lap_btn_t){ scan.pin >= 0 ? "UNPIN" : "PIN", scan.pin >= 0 ? NULL : v2, 'p', scan.pin >= 0, !scan.n, false };
        keys[2] = (lap_btn_t){ "DWELL", SCAN_DWELL_NAMES[scan_dwell % 5], 'w', false, false, false };
        break;
    default:
        keys[0] = (lap_btn_t){ "TYPE", xl_chip(), 'y', false, false, false };
        keys[1] = (lap_btn_t){ "AUTO", ui.xl_auto ? "ON" : "OFF", 'a', ui.xl_auto != 0, false, false };
        keys[2] = (lap_btn_t){ "SHOW", ui.xl_show == 0 ? "BOTH" : ui.xl_show == 1 ? "ENGLISH" : "AS SENT", 'w', false, false, false };
        break;
    }
    keys[3] = (lap_btn_t){ "MORE", NULL, 'm', false, false, false };
}

/* ---- dialogs ---- */
static int dlg_ch;                       /* the channel a channel message goes on */
static uint32_t reply_to, reply_id;      /* a reply: to whom (or everyone), and the message */
static int reply_ch;
static char phrase_out[200];             /* a phrase in Cyrillic or Polish letters waiting for its yes */
static uint32_t phrase_to;
static int phrase_ch;
static int room_for(uint32_t dest, bool reply)
{
    return mt_text_room(reply, false, dest != MT_BROADCAST && pki_for(dest, MT_PORT_TEXT) ? MT_PKI_OVERHEAD : 0);
}
static void open_dm_text(uint32_t num, const char *init)
{
    if (dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    const node_t *n = node_get(num, false);
    if (!n) return;
    dm_to = num;
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "DIRECT MESSAGE TO %s%s", n->short_name, pki_on() && n->has_key ? " (PRIVATE)" : "");
    dialog = lap_text_input(prompt, init ? init : "", room_for(num, false), LAP_TEXT_LOWER);
    dialog_kind = DLG_DM;
}
static void open_send(const char *init)
{
    if (dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    if (conv_kind == CONV_DM) { open_dm_text(conv_node, init); return; }
    dlg_ch = conv_kind == CONV_CH ? conv_ch : send_ch;
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "MESSAGE TO EVERYONE ON %s", ch_name(dlg_ch));
    dialog = lap_text_input(prompt, init ? init : "", room_for(MT_BROADCAST, false), LAP_TEXT_LOWER);
    dialog_kind = DLG_TEXT;
}
static void open_dm(uint32_t num) { open_dm_text(num, NULL); }
static const msg_t *picked_msg(void)
{
    for (int k = 0; k < msg_n; k++) {
        const msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
        if (picked(m)) return m;
    }
    return NULL;
}
/* Where an answer to a message goes: its sender (a DM), or its channel. */
static void answer_to(const msg_t *m, uint32_t *dest, int *ch)
{
    if (m->to != MT_BROADCAST) { *dest = m->mine ? m->to : m->from; *ch = m->ch; }
    else { *dest = MT_BROADCAST; *ch = m->ch; }
}
static void open_reply(void)
{
    const msg_t *m = picked_msg();
    if (!m || dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    answer_to(m, &reply_to, &reply_ch);
    reply_id = m->id;
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "REPLY TO %s: %.40s", m->mine ? "YOU" : who(m->from), scr(m->text, m->lang));
    dialog = lap_text_input(prompt, "", room_for(reply_to, true), LAP_TEXT_LOWER);
    dialog_kind = DLG_REPLY;
}
/* Tapbacks the official apps offer, and the emoji each sends. */
static const char *const REACT_ITEMS[] = { "+1   thumbs up", "-1   thumbs down", "<3   heart", "LOL  laughing", "!!   wow",
                                           "??   question", "OK   check", "YAY  party", "HOT  fire", "TY   thanks" };
static const char *const REACT_SEND[] = { "\xf0\x9f\x91\x8d", "\xf0\x9f\x91\x8e", "\xe2\x9d\xa4\xef\xb8\x8f",
                                          "\xf0\x9f\x98\x82", "\xe2\x80\xbc\xef\xb8\x8f", "\xe2\x9d\x93", "\xe2\x9c\x85",
                                          "\xf0\x9f\x8e\x89", "\xf0\x9f\x94\xa5", "\xf0\x9f\x99\x8f" };
static void open_react(void)
{
    const msg_t *m = picked_msg();
    if (!m || dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    answer_to(m, &reply_to, &reply_ch);
    reply_id = m->id;
    open_choose(DLG_REACT, REACT_ITEMS, (int)(sizeof(REACT_ITEMS) / sizeof(REACT_ITEMS[0])));
}
static void send_react(int i)
{
    if (i < 0 || i >= (int)(sizeof(REACT_SEND) / sizeof(REACT_SEND[0]))) return;
    const char *e = REACT_SEND[i];
    const bool dm = reply_to != MT_BROADCAST;
    send_on = reply_ch; send_reply = reply_id; send_emoji = 1;
    if (send_packet(reply_to, MT_PORT_TEXT, (const uint8_t *)e, strlen(e), dm, false, 0, 0)) {
        react_add(reply_id, cfg.node, (const uint8_t *)e, strlen(e));
        say("reaction sent");
    }
    pick_id = 0;
}
static void open_try(void)
{
    if (dialog >= 0) return;
    char prompt[48];
    if (xl_count() > 1) snprintf(prompt, sizeof(prompt), "A LINE TO TRANSLATE (%s)", xl_chip());
    else snprintf(prompt, sizeof(prompt), "%s TO TRANSLATE", mt_lang_name((mt_lang_t)xl_first()));
    dialog = lap_text_input(prompt, "", TEXT_MAX, LAP_TEXT_LOWER);
    dialog_kind = DLG_TRY;
}
static void quick_done(int index) { pending_quick = index; }
static void open_quick(void)
{
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    const mt_lang_t ql = quick_lang();
    char title[40];
    if (ql == MT_LANG_EN) snprintf(title, sizeof(title), "QUICK REPLY");
    else snprintf(title, sizeof(title), "QUICK REPLY (SENT IN %s)", mt_lang_name(ql));
    if (!lap_picker_open(title, quick_done)) return;
    const int n = mt_phrase_count();
    for (int i = 0; i < n && i < 48; i++) {
        const mt_phrase_t p = mt_phrase(i);
        if (p.en[0]) lap_picker_add(ql == MT_LANG_EN ? p.en : scr(mt_phrase_in(i, ql), ql), ql == MT_LANG_EN ? scr(mt_phrase_in(i, (mt_lang_t)xl_first()), xl_first()) : p.en);
    }
}
static void phrase_send(int i)
{
    const mt_phrase_t p = mt_phrase(i);
    const mt_lang_t ql = quick_lang();
    if (!p.en[0]) return;
    const char *s = mt_phrase_in(i, ql);
    if (!mt_xlate_needs_screen(s)) { open_send(s); return; }
    /* Cyrillic or Polish letters: the keyboard can neither show nor edit them, so the line goes as
       written (UTF-8, two bytes a letter), after a yes on its Latin spelling */
    if (dialog >= 0) return;
    if (!cfg.tx_on) { say("TRANSMIT is off: MORE > OPTIONS > TRANSMIT"); return; }
    phrase_to = conv_kind == CONV_DM ? conv_node : MT_BROADCAST;
    phrase_ch = conv_kind == CONV_CH ? conv_ch : send_ch;
    snprintf(phrase_out, sizeof(phrase_out), "%s", s);
    char msg[160];
    snprintf(msg, sizeof(msg), "SEND IN %s TO %s? %.90s (%u BYTES)", mt_lang_name(ql),
             phrase_to == MT_BROADCAST ? ch_name(phrase_ch) : who(phrase_to), scr(s, ql), (unsigned)strlen(s));
    open_confirm(DLG_PHRASE, msg);
}
/* the phrasebook's language, round: English, Spanish ... Ukrainian */
static void quick_next(void)
{
    const int v = quick_lang() == MT_LANG_EN ? 0 : quick_lang() - MT_LANG_ES + 1, nv = (v + 1) % QUICKS;
    ui.quick = (uint8_t)(nv == 0 ? 0 : nv == 1 ? 1 : MT_LANG_ES + nv - 1);
    dirty |= SV_UI;
    say("quick replies in %s", nv == 0 ? "English" : lang_word(quick_lang()));
}
/* TRANSLATE FROM: one list, a pick turns that language on or off and the list comes back */
static bool from_again;
static uint32_t from_ms;
static uint8_t from_rm[MT_LANG_COUNT], from_rm_n;       /* the REMOVE rows' languages */
static int del_lang = -1;                               /* the one REMOVE asks about */
static void to_tab(int t);
static void open_from(void)
{
    static char lab[2 * MT_LANG_COUNT + 2][32];         /* the board takes 31 bytes a row */
    static const char *items[2 * MT_LANG_COUNT + 2];
    int n = 0;
    if (dialog >= 0) return;
    for (int l = MT_LANG_ES; l < MT_LANG_COUNT; l++, n++) {
        const int st = l == MT_LANG_ES ? LS_READY : lang_st[l].state;
        snprintf(lab[n], sizeof(lab[n]), "[%c] %s%s", xl_on(l) ? 'x' : ' ', mt_lang_name((mt_lang_t)l),
                 !xl_on(l) ? "" : l == MT_LANG_ES ? " (BUILT IN)" : st == LS_READY ? (lang_st[l].alt ? " (CARD)" : " (PACK)") :
                 st == LS_MISSING || st == LS_BAD ? " (NOT ON CARD)" : " (LOADING)");
        items[n] = lab[n];
    }
    /* what MESH LANGUAGES (or a hand) put in meshtastic/lang/, to give the card space back */
    from_rm_n = 0;
    for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++)
        if ((lf_have & MT_LANG_BIT(l)) && l != lf_del) {
            snprintf(lab[n], sizeof(lab[n]), "REMOVE %s (%u KB)", mt_lang_name((mt_lang_t)l), (unsigned)lf_kb[l]);
            items[n] = lab[n];
            from_rm[from_rm_n++] = (uint8_t)l;
            n++;
        }
    snprintf(lab[n], sizeof(lab[n]), "+ MORE LANGUAGES");
    items[n] = lab[n];
    n++;
    snprintf(lab[n], sizeof(lab[n]), "DONE");
    items[n] = lab[n];
    n++;
    open_choose(DLG_FROM, items, n);
}
/* FROM again once the card has said what it holds (a moment), so the list
   shows NOT ON CARD and not LOADING. */
static void from_open(void)
{
    lang_retry();
    lf_list_due = true;
    from_again = true;
    from_ms = lap_millis();
}
static bool from_settled(void)
{
    if (lf_list_job >= 0 || lf_list_due) return false;
    for (int l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++)
        if (xl_on(l) && (lang_st[l].state == LS_OFF || lang_st[l].state == LS_SIZE)) return false;
    return true;
}
/* A language chosen in FROM or tapped on TRANSLATE: on or off, looked for on the card again. */
static void from_pick(int l, bool from_list)
{
    if (l < MT_LANG_ES || l >= MT_LANG_COUNT) return;
    xl_toggle(l);
    lang_retry();
    if (from_list) { from_again = true; from_ms = lap_millis(); }
}
/* MORE LANGUAGES: STORE at MESH LANGUAGES' card, on a board that can open
   it for an app (store.open); on one that cannot (2.23.2), a panel that
   says where they come from. */
static void more_langs(void)
{
    char out[16];
    if (lap_action("store.open", "meshlang", out, sizeof(out)) == LAP_ACT_OK) return;   /* STORE next frame */
    if (tab != TAB_LANG) to_tab(TAB_LANG);
    langs_open = true;
    scroll = 0;
}

/* A waypoint where this node is, for a day (or for good without a clock). */
static void send_waypoint(const char *name)
{
    double la, lo; int32_t alt; uint32_t t;
    if (!my_position(&la, &lo, &alt, &t)) { say("no GPS fix and no fixed position"); return; }
    mt_waypoint_t w;
    memset(&w, 0, sizeof(w));
    do w.id = lap_random(); while (!w.id);
    w.lat = la; w.lon = lo; w.has_pos = true;
    const uint32_t nu = unix_now();
    w.expire = nu ? nu + 86400u : 0;
    w.icon = 0x1F4CD;                                 /* a round pin */
    snprintf(w.name, sizeof(w.name), "%s", name);
    uint8_t buf[128];
    const int n = mt_waypoint_encode(&w, buf, sizeof(buf));
    send_on = send_ch;
    if (n > 0 && send_packet(MT_BROADCAST, MT_PORT_WAYPOINT, buf, (size_t)n, false, false, 0, 0)) {
        wpt_add(&w, cfg.node);
        say("waypoint %s sent on %s", w.name, ch_name(send_ch));
    }
}

/* A channel link read: replace the set, or with ?add=true add its channels. */
static mt_chset_t pending_set;
static bool pending_add;
static void import_apply(void)
{
    const mt_chset_t *s = &pending_set;
    if (!pending_add) {
        memset(chans, 0, sizeof(chans));
        for (int i = 0; i < s->n && i < NCH; i++) {
            chan_cfg_t *c = &chans[i];
            snprintf(c->name, sizeof(c->name), "%s", s->ch[i].name);
            c->psk_len = s->ch[i].psk_len; memcpy(c->psk, s->ch[i].psk, s->ch[i].psk_len);
            c->role = i == 0 ? CH_PRIMARY : CH_SECONDARY;
            c->mute = s->ch[i].muted;
            c->precision = s->ch[i].has_module ? s->ch[i].precision : (i == 0 ? 32 : 0);
        }
        /* the LoRa settings come too (never TRANSMIT or power): preset or custom, slot, hops, region */
        if (s->has_lora) {
            cfg.preset = s->lora.preset; ext.custom = s->lora.custom;
            if (s->lora.custom) { ext.bw = s->lora.bw_code; ext.sf = s->lora.sf; ext.cr = s->lora.cr; }
            ext.slot = s->lora.slot; ext.offset_hz = s->lora.offset_hz;
            if (s->hop_limit) ext.hop_limit = (uint8_t)s->hop_limit;
            if (s->region_code) { cfg.region = s->lora.region; ui.region_set = 1; dirty |= SV_UI; }
        }
        say("%d channel%s from the link", s->n, s->n == 1 ? "" : "s");
    } else {
        int added = 0;
        for (int i = 0; i < s->n; i++) {
            int c = 1;
            while (c < NCH && ch_on(c)) c++;
            if (c >= NCH) break;
            chan_cfg_t *o = &chans[c];
            memset(o, 0, sizeof(*o));
            snprintf(o->name, sizeof(o->name), "%s", s->ch[i].name);
            o->psk_len = s->ch[i].psk_len; memcpy(o->psk, s->ch[i].psk, s->ch[i].psk_len);
            o->role = CH_SECONDARY; o->mute = s->ch[i].muted; o->precision = s->ch[i].precision;
            added++;
        }
        say(added ? "%d channel%s added" : "no free channel to add to", added, added == 1 ? "" : "s");
    }
    dirty |= SV_CHANS | SV_CFG | SV_EXT;
    retune();
    if (share_open) share_build();
}

static void restore_apply(void)
{
    const backup_t *b = &backup_buf;
    if (b->magic != BACKUP_MAGIC || b->version != 1 || b->ext.magic != EXT_MAGIC || b->cfg.region >= MT_REGION_COUNT ||
        b->cfg.preset >= MT_PRESET_COUNT || b->cfg.node < 16) { say("that is not a settings backup"); return; }
    cfg = b->cfg; ext = b->ext; memcpy(chans, b->chans, sizeof(chans)); ui = b->ui;
    if (b->pki.magic == PKI_MAGIC) { pki = b->pki; pki_ready = true; memset(shared, 0, sizeof(shared)); }
    chans[0].role = CH_PRIMARY;
    dirty |= SV_CFG | SV_EXT | SV_CHANS | SV_UI | SV_PKI;
    retune();
    service_update();
    if (cfg.tx_on) send_nodeinfo(false);
    say("settings restored from the card");
}

/* MORE: what can be done from here. On a node's page, that node's actions. */
enum { MA_OPTIONS, MA_SHARE, MA_IMPORT, MA_WPT, MA_POS, MA_HELLO, MA_QUICK, MA_XL, MA_XL_FROM, MA_XL_MORE, MA_MOTION, MA_CLEAR, MA_CLEAR_NODES,
       MA_CLOSE, MA_FAV, MA_IGNORE, MA_ASK_POS, MA_ASK_INFO, MA_ASK_TELE, MA_SF, MA_TRACE, MA_REMOVE, MA_DM };
static int menu_act[20], menu_cnt;
static void more(void)
{
    if (menu_h > 0 || dialog >= 0) return;
    static char lab[20][32];
    static const char *items[20];
    menu_cnt = 0;
#define ITEM(a, ...) do { snprintf(lab[menu_cnt], sizeof(lab[0]), __VA_ARGS__); items[menu_cnt] = lab[menu_cnt]; menu_act[menu_cnt++] = (a); } while (0)
    const int ni = tab == TAB_NODES && node_open ? find_node(open_num) : -1;
    if (ni >= 0) {
        const node_t *n = &nodes[ni];
        ITEM(MA_DM, "DIRECT MESSAGE %s", n->short_name);
        ITEM(MA_FAV, n->fav ? "NOT A FAVOURITE" : "FAVOURITE");
        ITEM(MA_ASK_POS, "ASK FOR ITS POSITION");
        ITEM(MA_ASK_INFO, "ASK FOR ITS NODE INFO");
        ITEM(MA_ASK_TELE, "ASK FOR ITS TELEMETRY");
        ITEM(MA_TRACE, "TRACEROUTE");
        ITEM(MA_SF, n->sf_router ? "STORED MESSAGES (S&F)" : "ASK FOR STORED (S&F)");
        ITEM(MA_IGNORE, n->ignored ? "STOP IGNORING" : "IGNORE THIS NODE");
        ITEM(MA_REMOVE, "REMOVE FROM THE LIST");
        ITEM(MA_OPTIONS, "OPTIONS");
    } else {
        ITEM(MA_OPTIONS, "OPTIONS");
        ITEM(MA_SHARE, "SHARE CHANNELS (QR)");
        ITEM(MA_IMPORT, "ADD CHANNELS FROM A LINK");
        ITEM(MA_WPT, "DROP A WAYPOINT HERE");
        ITEM(MA_POS, "SEND POSITION NOW");
        ITEM(MA_HELLO, "SAY HELLO (NODE INFO)");
        ITEM(MA_QUICK, "QUICK REPLY");
        ITEM(MA_XL, "AUTO TRANSLATE: %s", ui.xl_auto ? "ON" : "OFF");
        ITEM(MA_XL_FROM, "TRANSLATE FROM: %s", xl_chip());
        ITEM(MA_XL_MORE, "MORE LANGUAGES");
        ITEM(MA_MOTION, "MOTION: %s", ui.anim ? "CALM" : "FULL");
        ITEM(MA_CLEAR, "CLEAR CHAT");
        ITEM(MA_CLEAR_NODES, "CLEAR NODE LIST");
    }
    ITEM(MA_CLOSE, "CLOSE MESHTASTIC");
#undef ITEM
    menu_h = lap_choose(items, menu_cnt);
    if (menu_h < 0) menu_h = 0;
}
static void remove_node(uint32_t num)
{
    const int i = find_node(num);
    if (i < 0) return;
    node_t *n = &nodes[i];
    n->has_pos = false;
    publish(n);                                        /* off the board's MAP */
    shared_forget(num);
    memmove(&nodes[i], &nodes[i + 1], sizeof(node_t) * (size_t)(node_n - i - 1));
    node_n--;
    node_open = false;
    nodes_dirty();
}
static void menu_poll(void)
{
    if (menu_h <= 0) return;
    int v = -1;
    char none[4];
    const int r = lap_dialog_poll(menu_h, none, sizeof(none), &v);
    if (!r) return;
    menu_h = 0;
    if (r != 1 || v < 0 || v >= menu_cnt) return;
    node_t *n = node_open ? node_get(open_num, false) : NULL;
    switch (menu_act[v]) {
    case MA_OPTIONS: open_options(); break;
    case MA_SHARE: share_open = true; scroll = 0; share_build(); break;
    case MA_IMPORT: open_text(DLG_IMPORT, "CHANNEL LINK (meshtastic.org/e/#...)", "", 767); break;
    case MA_WPT: if (cfg.tx_on) open_text(DLG_WPT, "WAYPOINT NAME (HERE, FOR A DAY)", "", 30); else say("TRANSMIT is off"); break;
    case MA_POS:
        if (!cfg.pos_mode) say("SHARE POSITION is off");
        else if (!send_position(MT_BROADCAST, 0)) say("no GPS fix, no fixed position, or POSITION ON IT is off");
        else say("position queued");
        break;
    case MA_HELLO: if (cfg.tx_on) { send_nodeinfo(true); say("hello sent: nodes in range answer"); } else say("TRANSMIT is off"); break;
    case MA_QUICK: open_quick(); break;
    case MA_XL: ui.xl_auto = !ui.xl_auto; if (ui.xl_auto) xl_fill_req = true; dirty |= SV_UI; break;
    case MA_XL_FROM: from_open(); break;
    case MA_XL_MORE: more_langs(); break;
    case MA_MOTION: ui.anim = !ui.anim; dirty |= SV_UI; break;
    case MA_CLEAR: msg_n = msg_head = 0; scroll = 0; memset(reacts, 0, sizeof(reacts)); memset(unread_ch, 0, sizeof(unread_ch));
        for (int i = 0; i < node_n; i++) nodes[i].unread = 0;
        unread = 0; say("chat cleared"); break;
    case MA_CLEAR_NODES:
        for (int i = 0; i < node_n; i++) { nodes[i].has_pos = false; publish(&nodes[i]); }
        node_n = 0; air_n = air_head = 0; hist_n = 0; memset(shared, 0, sizeof(shared)); node_open = false;
        dirty |= SV_NODES; say("node list cleared"); break;
    case MA_CLOSE: lap_exit(); break;
    case MA_DM: if (n) open_dm(n->num); break;
    case MA_FAV: if (n) { n->fav = !n->fav; nodes_dirty(); say(n->fav ? "%s is a favourite" : "%s is not a favourite", n->short_name); } break;
    case MA_IGNORE: if (n) { n->ignored = !n->ignored; nodes_dirty(); say(n->ignored ? "ignoring %s" : "hearing %s again", n->short_name); } break;
    case MA_ASK_POS:
        if (n && cfg.tx_on) { send_on = n->ch; if (send_position_ask(n->num, 0, true)) say("asked %s where it is", n->short_name); }
        else say("TRANSMIT is off");
        break;
    case MA_ASK_INFO: if (n && cfg.tx_on) { send_on = n->ch; send_nodeinfo_to(n->num, true); say("asked %s who it is", n->short_name); } else say("TRANSMIT is off"); break;
    case MA_ASK_TELE:
        if (n && cfg.tx_on) {
            uint8_t b[4] = { 0x12, 0x00 };                 /* Telemetry { device_metrics {} }, want_response */
            send_on = n->ch;
            if (send_packet(n->num, MT_PORT_TELEMETRY, b, 2, false, true, 0, 0)) say("asked %s for its telemetry", n->short_name);
        } else say("TRANSMIT is off");
        break;
    case MA_SF: if (n && cfg.tx_on) sf_ask(n->num); else say("TRANSMIT is off"); break;
    case MA_TRACE: if (n && cfg.tx_on) send_trace(n->num); else say("TRANSMIT is off"); break;
    case MA_REMOVE: if (n) { say("%s removed", n->short_name); remove_node(n->num); } break;
    default: break;
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
    if (!*t) { c->psk_len = 0; return true; }
    uint8_t k[40];
    const int n = mt_b64_decode(t, k, sizeof(k));
    if (n != 0 && n != 1 && n != 16 && n != 32) return false;
    memset(c->psk, 0, sizeof(c->psk));
    if (n == 0) { c->psk_len = 0; return true; }
    memcpy(c->psk, k, (size_t)n);
    c->psk_len = (uint8_t)n;
    return true;
}

static void handle_dialog(void)
{
    if (dialog < 0) return;
    static char text[769];               /* the keyboard holds 768 bytes */
    int v = 0;
    const int r = lap_dialog_poll(dialog, text, sizeof(text), &v);
    if (r == 0) return;
    const int kind = dialog_kind;
    dialog = -1;
    if (r == 1) switch (kind) {
    case DLG_TEXT: send_text_on(MT_BROADCAST, dlg_ch, text, 0); break;
    case DLG_DM: send_text_on(dm_to, 0, text, 0); break;
    case DLG_REPLY: send_text_on(reply_to, reply_ch, text, reply_id); pick_id = 0; break;
    case DLG_REACT: send_react(v); break;
    case DLG_LONG: case DLG_CALLSIGN:
        if (text[0]) {
            snprintf(cfg.long_name, sizeof(cfg.long_name), "%s", text);
            dirty |= SV_CFG;
        }
        if (cfg.tx_on) send_nodeinfo(false);
        break;
    case DLG_SHORT:
        if (text[0]) {
            snprintf(cfg.short_name, sizeof(cfg.short_name), "%.4s", text);
            dirty |= SV_CFG;
            if (cfg.tx_on) send_nodeinfo(false);
        }
        break;
    case DLG_FIXED: {
        double la = 0, lo = 0;
        const bool ok = parse_latlon(text, &la, &lo);
        if (!text[0]) { cfg.fixed = 0; say("fixed position cleared"); }
        else if (ok) {
            cfg.fixed = 1; cfg.fixed_lat = (int32_t)(la * 1e7); cfg.fixed_lon = (int32_t)(lo * 1e7);
            say("fixed position set");
        } else say("want lat,lon in degrees, e.g. -34.6037,-58.3816");
        dirty |= SV_CFG;
        break;
    }
    case DLG_CH_NAME: {
        chan_cfg_t *c = &chans[ch_sel];
        snprintf(c->name, sizeof(c->name), "%s", text);
        if (ch_sel && c->role == CH_OFF && text[0]) {          /* naming an unused channel turns it on */
            c->role = CH_SECONDARY;
            if (!c->psk_len) { c->psk_len = 1; c->psk[0] = 1; }
        }
        dirty |= SV_CHANS | SV_CFG;
        retune();
        say("channel %d: %s", ch_sel + 1, ch_name(ch_sel));
        break;
    }
    case DLG_CH_KEY:
        if (parse_key(text, &chans[ch_sel])) { dirty |= SV_CHANS; retune(); say("channel %d key set", ch_sel + 1); }
        else say("key: base64 of 1, 16 or 32 bytes (AQ== is the default)");
        break;
    case DLG_CH_PRIMARY:
        if (v == 1 && ch_sel > 0) {
            const chan_cfg_t t = chans[0];
            chans[0] = chans[ch_sel]; chans[ch_sel] = t;
            chans[0].role = CH_PRIMARY; chans[ch_sel].role = CH_SECONDARY;
            ch_sel = 0;
            dirty |= SV_CHANS | SV_CFG;
            retune();
            say("%s is the primary now", ch_name(0));
        }
        break;
    case DLG_IMPORT:
        if (mt_chset_parse(text, &pending_set, &pending_add)) {
            char m[96];
            snprintf(m, sizeof(m), pending_add ? "ADD %d CHANNEL%s FROM THE LINK?" : "REPLACE ALL CHANNELS WITH %d FROM THE LINK?",
                     pending_set.n, pending_set.n == 1 ? "" : "S");
            const int keep = opt_from;
            open_confirm(DLG_IMPORT_OK, m);
            opt_from = keep;
            return;                                      /* OPTIONS comes back after the answer */
        }
        say("not a channel link (want meshtastic.org/e/#...)");
        break;
    case DLG_IMPORT_OK: if (v == 1) import_apply(); break;
    case DLG_REGION:
        if (v >= 0 && v < MT_REGION_COUNT) {
            cfg.region = (uint8_t)v; ui.region_set = 1; dirty |= SV_UI | SV_CFG; retune();
            say("region %s", MT_REGIONS[v].name);
        }
        break;
    case DLG_ROLE:
        if (v >= 0 && v < ROLE_PICKS) {
            ext.role = ROLE_PICK[v]; dirty |= SV_EXT;
            if (cfg.tx_on) send_nodeinfo(false);
            say("role %s", mt_role_name(ext.role));
        }
        break;
    case DLG_WPT: if (text[0]) send_waypoint(text); break;
    case DLG_RESTORE_OK: if (v == 1) { dirty |= SV_RESTORE; say("reading the backup"); } break;
    case DLG_TRY:
        snprintf(try_es, sizeof(try_es), "%s", text);
        try_en[0] = 0;
        xl_try_req = true;                           /* translated on the next poll */
        break;
    case DLG_QUICK:
        if (v >= 0 && v < QUICKS) { ui.quick = (uint8_t)(v == 0 ? 0 : v == 1 ? 1 : MT_LANG_ES + v - 1); dirty |= SV_UI; }
        break;
    case DLG_PHRASE:
        if (v == 1) {
            if ((int)strlen(phrase_out) > room_for(phrase_to, false)) say("too long for this message");
            else send_text_on(phrase_to, phrase_ch, phrase_out, 0);
        }
        break;
    case DLG_FROM:
        if (v >= 0 && v < MT_LANG_COUNT - MT_LANG_ES) from_pick(MT_LANG_ES + v, true);   /* the list comes back */
        else if (v >= MT_LANG_COUNT - MT_LANG_ES && v < MT_LANG_COUNT - MT_LANG_ES + from_rm_n) {
            const int l = from_rm[v - (MT_LANG_COUNT - MT_LANG_ES)];
            char m[128], up[16];
            snprintf(up, sizeof(up), "%s", mt_lang_name((mt_lang_t)l));
            for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            snprintf(m, sizeof(m), "REMOVE %s (%u KB) FROM THE CARD? MESH LANGUAGES CAN GET IT AGAIN.", up, (unsigned)lf_kb[l]);
            del_lang = l;
            const int keep = opt_from;
            open_confirm(DLG_DEL, m);
            opt_from = keep;
            return;
        } else if (v == MT_LANG_COUNT - MT_LANG_ES + from_rm_n) more_langs();
        break;
    case DLG_DEL:
        if (v == 1 && del_lang >= 0) { lf_del = del_lang; lf_list_due = true; }    /* done in poll() */
        del_lang = -1;
        from_again = true;
        from_ms = lap_millis();
        break;
    default: break;
    }
    if (opt_from != -2) { opt_want = opt_from; opt_from = -2; }
}


static void to_tab(int t)
{
    tab = (t + TABS) % TABS;
    scroll = 0;
    share_open = false;
    pick_id = 0;
    lap_scroll_stop(&drag);
    if (tab == TAB_CHAT) chat_new = true;               /* open at the newest */
    langs_open = false;
    if (tab == TAB_LANG) { lang_retry(); lf_list_due = true; }   /* a file copied to the card since */
    if (tab == TAB_SCAN) scan_plan();
}
static void open_node(int i)
{
    if (i < 0 || i >= node_n) return;
    open_num = nodes[i].num;
    node_open = true;
    share_open = false;
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
    /* FROM back once the card has said (from_ms is newer than now_ms) */
    if (from_again && dialog < 0 && menu_h <= 0 && (from_settled() || lap_millis() - from_ms > 1500u)) {
        from_again = false;
        open_from();
    }
    /* OPTIONS back with fresh words, after a dialog or "< ALL SETTINGS" */
    if (opt_want != -2 && dialog < 0 && menu_h <= 0) {
        const int at = opt_want;
        opt_want = -2;
        lap_options_close();
        open_options_at(at);
    }
    if (pending_quick >= 0 && dialog < 0) { phrase_send(pending_quick); pending_quick = -1; }
    if (pending_phrase >= 0 && dialog < 0) { phrase_send(pending_phrase); pending_phrase = -1; }
    if (share_open) {
        static uint32_t built_ms;
        if (!share_url[0] || now_ms - built_ms > 5000u) { built_ms = now_ms; share_build(); }
    }
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
    const int un = unread_total();
    unread = (uint32_t)un;
    char sv[24];
    if (un && tab != TAB_CHAT) snprintf(sv, sizeof(sv), "%d/%d  %d NEW", tab + 1, TABS, un);
    else snprintf(sv, sizeof(sv), "%d/%d", tab + 1, TABS);
    static const char *const TAB_NAME[TABS] = { "CHAT", "NODES", "MESH MAP", "AIR", "SCAN", "TRANSLATE" };
    const char *label = share_open ? "SHARE" : tab == TAB_NODES && node_open ? "NODE" : tab == TAB_CHAT && conv_list ? "CHATS"
                      : tab == TAB_RADAR && wpt_list ? "WAYPOINTS" : TAB_NAME[tab];
    strip(sx, ty, sw, tr, label, sv, un && tab != TAB_CHAT);
    /* the panel */
    char bt[48];
    if (share_open) snprintf(bt, sizeof(bt), " SHARE CHANNELS  %d ", share_all ? ch_count() : 1);
    else if (langs_open) snprintf(bt, sizeof(bt), " MORE LANGUAGES ");
    else switch (tab) {
    case TAB_CHAT:
        if (conv_list) snprintf(bt, sizeof(bt), " CONVERSATIONS ");
        else snprintf(bt, sizeof(bt), " CHAT  %s ", conv_title());
        break;
    case TAB_NODES:
        if (node_open) { const int i = find_node(open_num); snprintf(bt, sizeof(bt), " %s ", i >= 0 ? nodes[i].short_name : "NODE"); }
        else snprintf(bt, sizeof(bt), " NODES %d  by %s ", node_n, SORT_NAMES[ui.sort % 4]);
        break;
    case TAB_RADAR:
        if (wpt_list) snprintf(bt, sizeof(bt), " WAYPOINTS %d ", wpt_n);
        else snprintf(bt, sizeof(bt), " MESH MAP  %d nodes ", node_n);
        break;
    case TAB_AIR: snprintf(bt, sizeof(bt), " AIR  %u heard ", (unsigned)heard); break;
    case TAB_SCAN:
        snprintf(bt, sizeof(bt), " SCAN %s  %u heard ", !scan_on ? "STOPPED" : scan.pin >= 0 ? "PINNED" : "RUNNING",
                 (unsigned)scan.total);
        break;
    default:
        if (xl_count() <= 1) snprintf(bt, sizeof(bt), " %s > ENGLISH ", mt_lang_name((mt_lang_t)xl_first()));
        else snprintf(bt, sizeof(bt), " %d LANGUAGES > ENGLISH ", xl_count());
        break;
    }
    lap_ui_bezel(px, py, pw, ph, bt);
    const int ix = px + 1, iy = py + 1, ih = ph - 2;
    int iw = pw - 2;
    if (cut_rows(iy, iy + ih) && cut_x > ix && cut_x < ix + iw) iw = cut_x - 1 - ix;   /* short of the hole */
    if (share_open) share_draw(ix, iy, iw, ih);
    else if (langs_open) langs_draw(ix, iy, iw, ih);
    else if (radio < 0) {
        clip0 = iy; clip1 = iy + ih;
        put(ix + 1, iy + 1, iw - 2, A(LAP_UI_ALARM), "No LoRa radio on this board.");
        put(ix + 1, iy + 2, iw - 2, LAP_UI_DIM, "MESHTASTIC needs the LR2021 or the SX1262.");
    } else {
        switch (tab) {
        case TAB_CHAT: draw_chat(ix, iy, iw, ih); break;
        case TAB_NODES: draw_nodes(ix, iy, iw, ih); break;
        case TAB_RADAR: draw_radar(ix, iy, iw, ih); break;
        case TAB_AIR: draw_air(ix, iy, iw, ih); break;
        case TAB_SCAN: draw_scan(ix, iy, iw, ih); break;
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

/* BACK: whatever is open over the tab closes first. */
static bool back(void)
{
    if (share_open) { share_open = false; scroll = 0; return true; }
    if (langs_open) { langs_open = false; scroll = 0; return true; }
    if (tab == TAB_CHAT && pick_id) { pick_id = 0; return true; }
    if (tab == TAB_CHAT && conv_list) { conv_list = false; scroll = 0; chat_new = true; return true; }
    if (tab == TAB_NODES && node_open) { node_open = false; scroll = 0; lap_scroll_stop(&drag); return true; }
    if (tab == TAB_RADAR && wpt_list) { wpt_list = false; scroll = 0; return true; }
    return false;
}

/* SCAN's own keys: G go/stop, P pin the chosen row, W dwell, K clear. */
static bool scan_act(char c)
{
    if (tab != TAB_SCAN || share_open || langs_open) return false;
    switch (c) {
    case 'g': scan_go(!scan_on); return true;
    case 'p': scan_pin(scan.pin >= 0 ? -1 : scan_sel); return true;
    case 'w':
        scan_dwell = (uint8_t)((scan_dwell + 1) % 5);
        mt_scan_set_dwell(&scan, SCAN_DWELL_S[scan_dwell] * 1000u);
        say("%u s a preset (long presets get more)", (unsigned)SCAN_DWELL_S[scan_dwell]);
        return true;
    case 'k': mt_scan_clear(&scan); scroll = 0; say("scan table cleared"); return true;
    default: return false;
    }
}

static void act(char c)
{
    if (scan_act(c)) return;
    switch (c) {
    case 's': open_send(NULL); break;
    case 'q': open_quick(); break;
    case 'c':
        if (tab != TAB_CHAT) to_tab(TAB_CHAT);
        conv_list = !conv_list; pick_id = 0; scroll = 0; conv_sel = 0;
        if (!conv_list) chat_new = true;
        break;
    case 'd':
        if (tab == TAB_NODES && node_open) open_dm(open_num);
        else if (tab == TAB_NODES && node_n) { sort_nodes(); open_dm(nodes[node_order[sel]].num); }
        else say("open a node in NODES first");
        break;
    case 'r':
        if (tab == TAB_CHAT && pick_id) { open_reply(); break; }
        if (!cfg.tx_on) { say("TRANSMIT is off"); break; }
        if (tab == TAB_NODES && node_open) send_trace(open_num);
        else if (tab == TAB_NODES && node_n) { sort_nodes(); send_trace(nodes[node_order[sel]].num); }
        break;
    case 'e': if (tab == TAB_CHAT && pick_id) open_react(); break;
    case 'p':
        if (tab == TAB_CHAT && !conv_list) {
            bool past = !pick_id;
            for (int k = 0; k < msg_n; k++) {
                const msg_t *m = &msgs[(msg_head - 1 - k + MAX_MSGS * 2) % MAX_MSGS];
                if (!conv_shows(m)) continue;
                if (past) { pick_id = m->id; pick_from = m->from; past = false; break; }
                if (picked(m)) past = true;
            }
            if (past) pick_id = 0;                       /* past the oldest: none */
        }
        break;
    case 'b': back(); break;
    case 'g': ui.sort = (uint8_t)((ui.sort + 1) % 4); dirty |= SV_UI; scroll = 0; say("nodes by %s", SORT_NAMES[ui.sort]); break;
    case 'n':
        if (!cfg.tx_on) { say("TRANSMIT is off"); break; }
        if (tab == TAB_NODES && node_open) { send_nodeinfo_to(open_num, true); say("asked %s who it is", who(open_num)); }
        else { send_nodeinfo(true); say("hello sent: nodes in range answer"); }
        break;
    case 'f':
        if (tab == TAB_LANG) { langs_open = false; from_open(); break; }
        if (tab == TAB_NODES && node_open) {
            node_t *n = node_get(open_num, false);
            if (n) { n->fav = !n->fav; nodes_dirty(); say(n->fav ? "%s is a favourite" : "%s is not a favourite", n->short_name); }
        }
        break;
    case 'v': to_tab(TAB_RADAR); break;
    case 'u': node_open = false; conv_list = false; to_tab(TAB_NODES); break;
    case 'z': ui.zoom = (uint8_t)((ui.zoom + 1) % ZOOMS); dirty |= SV_UI; break;
    case 'l':
        if (tab == TAB_LANG) { quick_next(); break; }
        ui.labels = !ui.labels; dirty |= SV_UI; break;
    case 'j': wpt_list = !wpt_list; scroll = 0; break;
    case 'k':
        if (tab == TAB_CHAT) { memset(unread_ch, 0, sizeof(unread_ch)); for (int i = 0; i < node_n; i++) nodes[i].unread = 0; unread = 0; }
        else { air_n = air_head = 0; scroll = 0; }
        break;
    case 'h': share_all = !share_all; share_build(); break;
    case 'i': open_text(DLG_IMPORT, "CHANNEL LINK (meshtastic.org/e/#...)", "", 767); break;
    case 'y': langs_open = false; open_try(); break;
    case 'x': more_langs(); break;                       /* MORE LANGUAGES */
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
    const bool list = tab == TAB_CHAT && conv_list && !share_open;
    switch (k) {
    case LAP_KEY_LEFT: to_tab(tab - 1); return true;
    case LAP_KEY_RIGHT: case LAP_KEY_TAB: to_tab(tab + 1); return true;
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP:
        if (list) { kbd_used = true; if (conv_sel) conv_sel--; if (conv_sel * 3 < scroll) scroll = conv_sel * 3; }
        else if (tab == TAB_NODES && !node_open && !share_open) { kbd_used = true; if (sel) sel--; if (sel * CARD < scroll) scroll = sel * CARD; }
        else if (tab == TAB_SCAN && !share_open && scan_sel > 0) scan_sel--;
        else step(-2);
        return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN:
        if (list) {
            kbd_used = true;
            conv_sel++;
            if ((conv_sel + 1) * 3 > scroll + ph - 2) scroll = (conv_sel + 1) * 3 - (ph - 2);
        } else if (tab == TAB_NODES && !node_open && !share_open) {
            kbd_used = true;
            if (sel < node_n - 1) sel++;
            if ((sel + 1) * CARD > scroll + ph - 2) scroll = (sel + 1) * CARD - (ph - 2);
        } else if (tab == TAB_SCAN && !share_open && scan_sel < scan.n - 1) scan_sel++;
        else step(2);
        return true;
    case LAP_KEY_PGUP: step(-page); return true;
    case LAP_KEY_PGDN: step(page); return true;
    case LAP_KEY_ENTER:
        if (share_open) act('b');
        else if (list) {
            static conv_t cs[MAX_NODES + NCH + 1];
            const int n = conv_collect(cs, (int)(sizeof(cs) / sizeof(cs[0])));
            if (conv_sel >= 0 && conv_sel < n) conv_open(cs[conv_sel].kind, cs[conv_sel].ch, cs[conv_sel].node);
        }
        else if (tab == TAB_NODES && !node_open && node_n) { sort_nodes(); open_node(node_order[sel]); }
        else if (tab == TAB_NODES && node_open) act('d');
        else if (tab == TAB_LANG) act('y');
        else if (tab == TAB_SCAN) scan_pin(scan_sel);
        else if (tab == TAB_CHAT && pick_id) act('r');
        else act('s');
        return true;
    case LAP_KEY_BACK: return back();
    case LAP_KEY_MENU: more(); return true;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    if (ch >= '1' && ch <= '0' + TABS) { to_tab(ch - '1'); return true; }
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    if (strchr("sqcdrebgnfvuzljkhiyawmotpx", ch)) { act(ch); return true; }
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
        else back();
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
    static uint32_t wpt_arm_ms;
    static int wpt_arm = -1;
    for (int i = hit_n - 1; i >= 0; i--) {
        const hit_t *h = &hits[i];
        if (col < h->x0 || col >= h->x1 || row < h->y0 || row >= h->y1) continue;
        switch (h->kind) {
        case HIT_MSG: {
            msg_t *m = &msgs[h->idx];
            if (!picked(m)) { pick_id = m->id; pick_from = m->from; return true; }   /* first tap: choose it */
            if (m->tr[0]) {                              /* again: both > as sent > English > both */
                const int cur = msg_mode(m), next = cur == 1 ? 0 : cur == 0 ? 2 : 1;
                m->view = (uint8_t)(next + 1);
            } else if (!m->mine && m->lang != MT_LANG_EN) {
                xl_msg_req = h->idx;                     /* translated on the next poll */
                m->view = 2;
            } else pick_id = 0;
            return true;
        }
        case HIT_CONV: {
            static conv_t cs[MAX_NODES + NCH + 1];
            const int n = conv_collect(cs, (int)(sizeof(cs) / sizeof(cs[0])));
            if (h->idx >= 0 && h->idx < n) conv_open(cs[h->idx].kind, cs[h->idx].ch, cs[h->idx].node);
            return true;
        }
        case HIT_WPT:
            if (wpt_arm == h->idx && now_ms - wpt_arm_ms < 3000u && h->idx < wpt_n) {
                say("waypoint %s removed", wpts[h->idx].name);
                wpt_drop(h->idx);
                wpt_arm = -1;
            } else { wpt_arm = h->idx; wpt_arm_ms = now_ms; say("tap again to remove %s", h->idx < wpt_n ? wpts[h->idx].name : ""); }
            return true;
        case HIT_SHARE_ALL: act('h'); return true;
        case HIT_SCAN:                                   /* a row: choose it; again: pin it (or let go) */
            if (scan_sel == h->idx) scan_pin(h->idx);
            else scan_sel = h->idx;
            return true;
        case HIT_NODE: kbd_used = false; open_node(h->idx); return true;
        case HIT_BLIP: open_node(h->idx); return true;
        case HIT_AIR: {
            const air_t *a = &air[(air_head - 1 - h->idx + MAX_AIR * 2) % MAX_AIR];
            const int ni = find_node(a->from);
            if (ni >= 0) open_node(ni);
            return true;
        }
        case HIT_PHRASE: pending_phrase = h->idx; return true;
        case HIT_XL_LANG: from_pick(h->idx, false); return true;
        case HIT_XL_MORE: more_langs(); return true;
        case HIT_QUICK_LANG: quick_next(); return true;
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
    if (tab == TAB_CHAT && pick_id) pick_id = 0;          /* a tap beside the bubbles lets go of the choice */
    return true;
}

LAP_ICON_PX("....................", "....................", "....................", ".......aa2..aaa2....",
            ".......aa2..aaa2....", "......aa2..aa.aa2...", "......aa2..aa.aa2...", ".....aa2..aa...aa2..",
            ".....aa2..aa...aa2..", "....aa2..aa.....aa2.", "....aa2..aa.....aa2.", "...aa2..aa.......aa2",
            "...aa2..aa.......aa2", "..aa2..aa.........aa", "..aa2..aa.........aa", ".aa2..aa...........a",
            "....................", "....................");

LAP_APP(.id = "meshtastic", .name = "MESHTASTIC", .sub = "be a Meshtastic node", .version = "1.5",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { "   /\\     ", "  /  \\ ((o", " / /\\ \\/  ", "/_/  \\_/\\_", "  ~ ~ ~ ~ ", "          " },
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX | LAP_CAP_SERVICE,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
