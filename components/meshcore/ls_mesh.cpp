/* See ls_mesh.h for the shape of the port and why polling is the
   right fit for this board. This file is the platform layer and nothing
   else: upstream/ is unmodified MeshCore and must stay that way, so every
   accommodation for this hardware lives here. */
#include "ls_mesh.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "MeshCore.h"
#include "Mesh.h"
#include "Dispatcher.h"
#include "Identity.h"
#include "Utils.h"
#include "StaticPoolPacketManager.h"
#include "SimpleMeshTables.h"
#include "helpers/AdvertDataHelpers.h"

extern "C" {
/* ls_board.h FIRST, and it is not optional. An undefined macro in an
   #if is silently 0, so without this the whole file compiled as the "board
   has no LoRa" stub and linked six do-nothing functions - on a board whose
   SX1262 had already been measured receiving at -116 dBm. It built clean and
   it did nothing, which is the worst combination available. */
#include "ls_board.h"
#include "ls_lora.h"
#include "ls_rtc.h"
#include "ls_gps.h"
#include "ls_gauge.h"
#include "ls_rlog.h"
#include "ls_track.h"
#include "ls_track_log.h"
}

#if !defined(LS_HAS_LORA)
#error "LS_HAS_LORA is not defined - ls_board.h was not included"
#endif

static const char *TAG = "ls_mesh";

#if LS_HAS_LORA

static const uint8_t PUBLIC_PSK[16] = {
    0x8B, 0x33, 0x87, 0xE9, 0xC5, 0xCD, 0xEA, 0x6A,
    0xC9, 0xE5, 0xED, 0xBA, 0xA1, 0x15, 0xCD, 0x72
};

static mesh::GroupChannel s_chan[LS_MESH_MAX_CHANNELS];
static ls_mesh_chan_t     s_chan_meta[LS_MESH_MAX_CHANNELS];
static bool               s_chan_ready[LS_MESH_MAX_CHANNELS];
static int                s_chan_active;
static bool               s_public_ready;

static int b64_decode(const char *in, uint8_t *out, int out_max)
{
    static const char *TBL =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int acc = 0, bits = 0, n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == ' ' || *p == '\r' || *p == '\n') continue;
        const char *q = strchr(TBL, *p);
        if (!q) return -1;                       /* not base64 at all */
        acc = (acc << 6) | (int)(q - TBL);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_max) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

static void chan_build(int i)
{
    s_chan_ready[i] = false;
    if (i == 0) {
        memset(&s_chan[0], 0, sizeof(s_chan[0]));
        memcpy(s_chan[0].secret, PUBLIC_PSK, sizeof(PUBLIC_PSK));
        mesh::Utils::sha256(s_chan[0].hash, PATH_HASH_SIZE,
                            s_chan[0].secret, sizeof(PUBLIC_PSK));
        s_chan_ready[0] = true;
        return;
    }
    if (!s_chan_meta[i].psk[0]) return;
    uint8_t key[32];
    const int len = b64_decode(s_chan_meta[i].psk, key, sizeof(key));
    if (len != 16 && len != 32) return;          /* the only lengths upstream takes */
    memset(&s_chan[i], 0, sizeof(s_chan[i]));
    memcpy(s_chan[i].secret, key, (size_t)len);
    mesh::Utils::sha256(s_chan[i].hash, PATH_HASH_SIZE, s_chan[i].secret, len);
    s_chan_ready[i] = true;
}

static ls_mesh_msg_t   s_msgs[LS_MESH_MAX_MSGS];
static int             s_msg_count;      /* how many slots are filled  */
static int             s_msg_head;       /* next slot to write         */
static volatile uint32_t s_msg_seq;

/* The message history, on the card. */

#define MSGLOG_PATH      "/sdcard/lakeshark/mesh.log"
#define MSGLOG_SETTLE_US 4000000

/* The radio lease. Two flags and not one: the request is the
   caller's and the acknowledgement is the task's, and collapsing them into a
   single bool means the caller cannot tell "I have asked" from "it has
   stopped" - which is the only distinction that matters when the next thing
   you do is retune the part underneath it. */
static volatile bool  s_radio_hold;
static volatile bool  s_radio_held;

#define SIGHTLOG_PATH "/sdcard/lakeshark/sightings.log"
#define SIGHT_MOVE_M  50.0f
#define SIGHT_MAX     512

typedef struct {
    uint32_t t;
    char     id[17];
    char     name[LS_MESH_PEER_NAME];
    int32_t  our_lat_e7, our_lon_e7;
    int16_t  rssi_dbm;
    uint8_t  flags;
    uint8_t  pad;
} ls_sight_rec_t;

#define SIGHT_F_EPOCH 0x01

static ls_rlog_t s_sightlog;

/* The peers, on the card - and only the half of a peer that keeps. */

#define PEERLOG_PATH "/sdcard/lakeshark/peers.log"

typedef struct {
    char     id[17];
    uint8_t  pub_key[32];
    char     name[LS_MESH_PEER_NAME];
    uint8_t  type;
    uint8_t  has_loc;
    int32_t  lat_e6, lon_e6;
} ls_mesh_peer_rec_t;

static ls_rlog_t      s_peerlog;
/* Bumped when a peer's IDENTITY changes - a new node, a name, a
   type, a position - and NOT when one is merely heard again. Every packet
   touches a peer; almost none of them change what the card holds, and a
   sequence that moved on every packet would rewrite the file for the life of
   the session and never settle. */
static volatile uint32_t s_peer_ident_seq;
static uint32_t       s_peerlog_seq;
static int64_t        s_peerlog_due_us;

static ls_rlog_t      s_msglog;
static ls_mesh_msg_t *s_msglog_buf;
static uint32_t       s_msglog_seq;      /* the s_msg_seq we last saw change */
static int64_t        s_msglog_due_us;   /* 0 when there is nothing pending  */

/* Hashes of our own recent sends, so a relayed copy coming back can
   be recognised. Four is enough: a flood is relayed within a second or two
   or not at all, and holding more would only keep matching stale traffic.
   `slot` is the index in s_msgs the hash belongs to. */
#define SENT_TRACK 4
typedef struct {
    uint8_t  hash[MAX_HASH_SIZE];
    int      slot;
    bool     used;
} sent_track_t;
/* The peers that matched the last searchPeersByHash. Upstream calls
   that, then calls getPeerSharedSecret with an index into what it returned,
   so the mapping back to our table has to survive between the two calls.
   Single-threaded: both happen inside one Dispatcher::loop. */
#define PEER_MATCH_MAX 4
static int s_match[PEER_MATCH_MAX];
static int s_match_n;

#define ACK_TRACK 4
typedef struct { uint32_t expect; int slot; bool used; } ack_track_t;
static ack_track_t s_ack[ACK_TRACK];
static int         s_ack_head;

static sent_track_t s_sent[SENT_TRACK];
static int          s_sent_head;

static bool            s_auto_listen = true;   /* , safe: emits nothing */
static bool            s_auto_tx     = false;  /* , never on by default */

static char            s_name[LS_MESH_NAME_MAX + 1];
static ls_mesh_radio_t s_radio_cfg;

static float    s_scope[LS_MESH_SCOPE_N];
static int      s_scope_head;
static int64_t  s_scope_next_us;
static int64_t  s_advert_next_us;

static ls_mesh_peer_t  s_peers[LS_MESH_MAX_PEERS];
static int             s_peer_count;
static ls_mesh_event_t s_events[LS_MESH_MAX_EVENTS];
static int             s_ev_head;          /* next slot to write */
static volatile uint32_t s_ev_seq;

static uint32_t mesh_now(void)
{
    time_t now = time(NULL);
    if (now > 1735689600) return (uint32_t)now;
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* See the header. The same clock every stamp in this file uses. */
extern "C" uint32_t ls_mesh_now(void) { return mesh_now(); }

static void id_hex(char out[17], const uint8_t *key)
{
    for (int i = 0; i < 8; i++) sprintf(&out[i * 2], "%02X", key[i]);
    out[16] = 0;
}

static void note_event(ls_mesh_ev_kind_t kind, const char *id,
                       float rssi, float snr, uint16_t len)
{
    ls_mesh_event_t *e = &s_events[s_ev_head];
    e->t    = mesh_now();
    e->kind = kind;
    e->rssi = rssi;
    e->snr  = snr;
    e->len  = len;
    /* memcpy plus an explicit terminator, not strncpy: the id is exactly 16
       characters and the field is 17, so strncpy(dst, src, 16) copies the lot
       and writes no NUL - which is correct here and is also exactly what
       -Wstringop-truncation exists to complain about. Say what is meant. */
    if (id) { memcpy(e->id, id, 16); e->id[16] = 0; }
    else    { e->id[0] = 0; }
    s_ev_head = (s_ev_head + 1) % LS_MESH_MAX_EVENTS;
    s_ev_seq++;
}

static void load_settings(void);

static int note_msg(const char *text, bool mine)
{
    const int slot = s_msg_head;
    ls_mesh_msg_t *m = &s_msgs[slot];
    m->t      = mesh_now();
    m->mine   = mine;
    m->state  = mine ? LS_MSG_SENDING : LS_MSG_IN;
    m->relays = 0;
    snprintf(m->text, sizeof(m->text), "%s", text ? text : "");
    s_msg_head = (s_msg_head + 1) % LS_MESH_MAX_MSGS;
    if (s_msg_count < LS_MESH_MAX_MSGS) s_msg_count++;
    s_msg_seq++;
    return slot;
}

/* Did this packet start here? Compares against the hashes of our
   recent sends. A match means a neighbour relayed our message and we heard
   it come back - the implicit acknowledgement. */
static void check_own_echo(mesh::Packet *pkt)
{
    if (!pkt) return;
    uint8_t h[MAX_HASH_SIZE];
    pkt->calculatePacketHash(h);
    for (int i = 0; i < SENT_TRACK; i++) {
        if (!s_sent[i].used) continue;
        if (memcmp(s_sent[i].hash, h, MAX_HASH_SIZE) != 0) continue;
        const int slot = s_sent[i].slot;
        if (slot >= 0 && slot < LS_MESH_MAX_MSGS && s_msgs[slot].mine) {
            s_msgs[slot].state = LS_MSG_HEARD;
            if (s_msgs[slot].relays < 255) s_msgs[slot].relays++;
            s_msg_seq++;
        }
        return;
    }
}

/* Hearing somebody is hearing somebody, whatever the packet was. */

static int peer_index(const char *id)
{
    if (!id || !*id) return -1;
    for (int i = 0; i < s_peer_count; i++)
        if (!strcmp(s_peers[i].id, id)) return i;
    return -1;
}

static void note_sighting(int pi);

static void touch_peer(int pi, float rssi, float snr)
{
    if (pi < 0 || pi >= s_peer_count) return;
    /* Every way of hearing a node passes through here (), so
       this is the one place the sighting log has to be fed from. It writes
       at most once per node per fifty metres, so the cost on a busy mesh is
       a distance comparison. */
    note_sighting(pi);
    s_peers[pi].last_heard = mesh_now();
    /* The signal from a data frame is as real a measurement as the one from
       an advert, and far more recent. Adverts are the only other source and
       they are minutes apart. */
    s_peers[pi].rssi = rssi;
    s_peers[pi].snr  = snr;
}

static void note_peer(const mesh::Identity &id_obj, const char *id,
                      const AdvertDataParser *adv, float rssi, float snr)
{
    uint32_t now = mesh_now();
    ls_mesh_peer_t *found = NULL;
    for (int i = 0; i < s_peer_count; i++)
        if (!strcmp(s_peers[i].id, id)) { found = &s_peers[i]; break; }

    if (found) {
        found->last_heard = now;
        found->adverts++;
        found->rssi = rssi;
        found->snr  = snr;
        memcpy(found->pub_key, id_obj.pub_key, sizeof(found->pub_key));
        if (adv && adv->isValid()) {
            /* Compared before it is written, so the card is rewritten
               when a node CHANGES its name and not every time it repeats
               it. */
            if (found->type != adv->getType()) s_peer_ident_seq++;
            found->type = adv->getType();
            if (adv->hasName()) {
                if (strncmp(found->name, adv->getName(), sizeof(found->name)))
                    s_peer_ident_seq++;
                snprintf(found->name, sizeof(found->name), "%s", adv->getName());
            }
            /* An exact 0,0 is not a place, it is a zeroed struct. */

            if (adv->hasLatLon() && !(adv->getLat() == 0 && adv->getLon() == 0)) {
                if (!found->has_loc || found->lat_e6 != adv->getLat() ||
                    found->lon_e6 != adv->getLon())
                    s_peer_ident_seq++;
                found->has_loc = true;
                found->lat_e6 = adv->getLat();
                found->lon_e6 = adv->getLon();
            }
        }
        return;
    }
    /* A node nobody has seen before is always worth writing down. */
    s_peer_ident_seq++;
    ls_mesh_peer_t *slot;
    if (s_peer_count < LS_MESH_MAX_PEERS) {
        slot = &s_peers[s_peer_count++];
    } else {
        /* Evict the one heard longest ago. A table this small will churn on a
           busy mesh, and the least useful entry is the stalest one. */
        slot = &s_peers[0];
        for (int i = 1; i < s_peer_count; i++)
            if (s_peers[i].last_heard < slot->last_heard) slot = &s_peers[i];
    }
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->id, id, 16); slot->id[16] = 0;
    memcpy(slot->pub_key, id_obj.pub_key, sizeof(slot->pub_key));
    slot->first_heard = slot->last_heard = now;
    slot->adverts = 1;
    slot->rssi = rssi;
    slot->snr  = snr;
    if (adv && adv->isValid()) {
        slot->type = adv->getType();
        if (adv->hasName())
            snprintf(slot->name, sizeof(slot->name), "%s", adv->getName());
        /* The same guard as the refresh branch above. Both copy a
           position in, so a check in only one of them protects a peer that
           was already known and not the one that just appeared. */
        if (adv->hasLatLon() && !(adv->getLat() == 0 && adv->getLon() == 0)) {
            slot->has_loc = true;
            slot->lat_e6 = adv->getLat();
            slot->lon_e6 = adv->getLon();
        }
    }
}

/* ---- mesh::MillisecondClock ------------------------------------------- */

class LsMillis : public mesh::MillisecondClock {
public:
    unsigned long getMillis() override
    {
        return (unsigned long)(esp_timer_get_time() / 1000);
    }
};

/* ---- mesh::RTCClock ---------------------------------------------------- */

class LsRtcClock : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override
    {

        time_t now = time(NULL);
        if (now > 1735689600) return (uint32_t)now;
        return (uint32_t)(esp_timer_get_time() / 1000000);
    }

    void setCurrentTime(uint32_t t) override
    {
        /* A time learned over the air is not authoritative enough to move
           this board's clock - accepting one would let any neighbour set it.
           Recorded and ignored on purpose. */
        ESP_LOGD(TAG, "peer offered time %lu; not adopting it", (unsigned long)t);
    }
};

/* ---- mesh::RNG --------------------------------------------------------- */

class LsRng : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override
    {
        /* esp_fill_random draws from the hardware RNG, which on the P4 is
           only truly random once RF or the ADC is running. The SX1262 is up
           by the time this is used, and this is a mesh node's key material,
           so it matters. */
        esp_fill_random(dest, sz);
    }
};

/* ---- mesh::MainBoard --------------------------------------------------- */

class LsBoard : public mesh::MainBoard {
public:
    /* The real gauge now. MeshCore uses this for telemetry and for
       its own low-battery behaviour, and 0 read as "flat" to anything that
       looked. */
    uint16_t getBattMilliVolts() override
    {
        ls_gauge_t g;
        return ls_gauge_get(&g) ? g.millivolts : 0;
    }
    const char *getManufacturerName() const override { return "LakeShark T-Display-P4"; }
    void reboot() override { esp_restart(); }
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

/* ---- mesh::Radio ------------------------------------------------------- */

class LsRadio : public mesh::Radio {
public:
    int recvRaw(uint8_t *bytes, int sz) override
    {
        float rssi = 0, snr = 0;
        int n = ls_lora_poll(bytes, (size_t)sz, &rssi, &snr);
        if (n > 0) {
            _rssi = rssi; _snr = snr; _rx++;
            note_event(LS_MESH_EV_RX, NULL, rssi, snr, (uint16_t)n);
            return n;
        }
        if (n < 0) { _bad++; note_event(LS_MESH_EV_ERR, NULL, 0, 0, 0); }
        return 0;
    }

    uint32_t getEstAirtimeFor(int len_bytes) override
    {
        return ls_lora_airtime_ms(len_bytes);
    }

    /* Upstream's scoring: a stronger, cleaner packet is worth relaying
       sooner. Kept identical to RadioLibWrappers so this node behaves the
       same as every other MeshCore node on the air. */
    float packetScore(float snr, int packet_len) override
    {
        uint32_t t = ls_lora_airtime_ms(packet_len);
        return (snr + 20.0f) / (float)(t == 0 ? 1 : t);
    }

    bool startSendRaw(const uint8_t *bytes, int len) override
    {
        if (!_tx_enabled) return false;
        if (ls_lora_send(bytes, (size_t)len) != ESP_OK) return false;
        _tx++;
        note_event(LS_MESH_EV_TX, NULL, 0, 0, (uint16_t)len);
        return true;
    }

    bool isSendComplete() override
    {
        const bool done = ls_lora_send_done();
        /* The transition the chip actually tells us about. Anything
           of ours still marked SENDING when the radio goes idle has left the
           antenna, so it is SENT - no more and no less. */
        if (done) {
            for (int i = 0; i < LS_MESH_MAX_MSGS; i++)
                if (s_msgs[i].mine && s_msgs[i].state == LS_MSG_SENDING) {
                    s_msgs[i].state = LS_MSG_SENT;
                    s_msg_seq++;
                }
        }
        return done;
    }

    void onSendFinished() override { ls_lora_receive(); }

    bool isInRecvMode() const override { return ls_lora_is_receiving(); }

    float getLastRSSI() const override { return _rssi; }
    float getLastSNR()  const override { return _snr; }

    int getNoiseFloor() const override
    {
        float dbm = 0;
        if (ls_lora_rssi_inst(&dbm) != ESP_OK) return 0;
        return (int)dbm;
    }

    void setTxEnabled(bool on) { _tx_enabled = on; }
    bool txEnabled() const     { return _tx_enabled; }

    uint32_t rx() const { return _rx; }
    uint32_t bad() const { return _bad; }
    uint32_t tx() const { return _tx; }

private:
    float _rssi = 0, _snr = 0;
    /* Off until armed. A stack that starts advertising the instant it boots
       is a stack that transmits without anyone having said so. */
    bool  _tx_enabled = false;
    uint32_t _rx = 0, _bad = 0, _tx = 0;
};

static LsRadio *s_radio;

static LsRng *s_rng;

/* ---- the node ---------------------------------------------------------- */

class LsMesh : public mesh::Mesh {
public:
    LsMesh(LsRadio &radio, LsMillis &ms, LsRng &rng, LsRtcClock &rtc,
           mesh::PacketManager &mgr, mesh::MeshTables &tables)
        : mesh::Mesh(radio, ms, rng, rtc, mgr, tables) {}

    void onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id,
                      uint32_t timestamp, const uint8_t *app_data,
                      size_t app_data_len) override
    {
        char hex[17];
        id_hex(hex, id.pub_key);
        /* Read what they call themselves. Without this every peer is
           eight hex characters and a repeater is indistinguishable from a
           handset - which is most of what you want to know about a mesh. */
        AdvertDataParser adv(app_data, (uint8_t)app_data_len);
        /* Reached only after Mesh.cpp:268 verified the signature, so this is
           a peer that proved it holds the private key - not merely a frame
           that arrived. Worth being precise about: the peer table is what the
           interface shows as "who is out there". */
        /* mesh::Packet carries SNR but not RSSI - upstream only stores the
           one. The radio's last reading is the right substitute here because
           this is called from the same loop that just received the frame. */
        float snr  = packet ? packet->getSNR() : 0.0f;
        float rssi = s_radio ? s_radio->getLastRSSI() : 0.0f;
        note_peer(id, hex, &adv, rssi, snr);
        note_event(LS_MESH_EV_ADVERT, hex, rssi, snr, 0);
        ESP_LOGI(TAG, "advert from %s%s%s t=%lu", hex,
                 adv.isValid() && adv.hasName() ? " " : "",
                 adv.isValid() && adv.hasName() ? adv.getName() : "",
                 (unsigned long)timestamp);
        _adverts++;
    }

    /* Every received packet passes here on its way into the mesh.
       Checking for our own echo BEFORE chaining matters: Mesh::onRecvPacket
       drops anything already in the hasSeen table, and our own flood is
       exactly the thing that will be in it. */

    bool allowPacketForward(const mesh::Packet *packet) override
    {
        (void)packet;
        return s_radio_cfg.repeat && ls_mesh_tx_enabled();
    }

    mesh::DispatcherAction onRecvPacket(mesh::Packet *pkt) override
    {
        check_own_echo(pkt);
        /* Say what arrived, before the stack decides what to do with
           it. A frame that passes CRC and then vanishes is the hardest thing
           to diagnose from the outside - "rx=3, peers=0" says something was
           heard and nothing was understood, and does not say which of the
           several possible reasons it was. Payload type and length answer
           most of it in one line. */
        if (pkt) {
            /* For an advert the payload STARTS with the sender's
               public key, so four bytes of it says whose advert this is.
               Without that, "type=4 len=106" is indistinguishable between
               a peer announcing itself and our own advert being relayed
               back at us - and the two want completely different actions. */
            char who[12] = "";
            if (pkt->getPayloadType() == PAYLOAD_TYPE_ADVERT && pkt->payload_len >= 4)
                snprintf(who, sizeof(who), " from %02X%02X%02X%02X",
                         pkt->payload[0], pkt->payload[1],
                         pkt->payload[2], pkt->payload[3]);
            ESP_LOGI(TAG, "rx type=%u len=%u path=%u route=%s%s",
                     (unsigned)pkt->getPayloadType(),
                     (unsigned)pkt->payload_len,
                     (unsigned)pkt->getPathHashCount(),
                     pkt->isRouteFlood() ? "flood" : "direct", who);
        }
        return mesh::Mesh::onRecvPacket(pkt);
    }

    /* Which of our known peers could this packet be addressed to.
       The hash is a prefix of the destination's public key, so more than one
       peer can match and upstream expects every one of them - it tries each
       secret in turn. */
    int searchPeersByHash(const uint8_t *hash) override
    {
        s_match_n = 0;
        for (int i = 0; i < s_peer_count && s_match_n < PEER_MATCH_MAX; i++) {
            mesh::Identity id(s_peers[i].pub_key);
            if (id.isHashMatch(hash)) s_match[s_match_n++] = i;
        }
        return s_match_n;
    }

    /* The ed25519 key exchange. Nothing is stored: the secret is
       derived from our private key and their public key each time it is
       wanted, so there is no shared-secret cache to go stale when a peer
       rotates its identity. */
    void getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) override
    {
        if (peer_idx < 0 || peer_idx >= s_match_n) return;
        self_id.calcSharedSecret(dest_secret, s_peers[s_match[peer_idx]].pub_key);
    }

    void onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx,
                        const uint8_t *secret, uint8_t *data, size_t len) override
    {
        (void)secret;
        if (type != PAYLOAD_TYPE_TXT_MSG) return;
        if (len < 5) return;
        if ((data[4] >> 2) != 0) return;          /* not plain text */
        if (sender_idx < 0 || sender_idx >= s_match_n) return;

        const int pi = s_match[sender_idx];

        /* They just transmitted to us. That is the freshest evidence
           this table will ever get about that peer, and until now it was
           thrown away in favour of an advert from minutes ago. */
        touch_peer(pi, s_radio ? s_radio->getLastRSSI() : 0.0f,
                   packet ? packet->getSNR() : 0.0f);

        char text[LS_MESH_MSG_LEN];
        size_t n = len - 5;
        if (n >= sizeof(text)) n = sizeof(text) - 1;
        memcpy(text, &data[5], n);
        text[n] = 0;

        char line[LS_MESH_MSG_LEN];
        snprintf(line, sizeof(line), "%s: %s",
                 s_peers[pi].name[0] ? s_peers[pi].name : s_peers[pi].id, text);
        const int slot = note_msg(line, false);
        s_msgs[slot].direct = true;
        memcpy(s_msgs[slot].peer, s_peers[pi].id, sizeof(s_msgs[slot].peer));

        /* Acknowledge it, exactly as BaseChatMesh.cpp:229 does:
           four bytes of sha256 over the payload and the SENDER's public key,
           then the attempt byte, then one random byte so two acks for
           identical messages do not dedupe against each other. */
        const int text_len = (int)strlen((char *)&data[5]);
        uint8_t ack_hash[6];
        mesh::Utils::sha256(ack_hash, 4, data, 5 + text_len,
                            s_peers[pi].pub_key, PUB_KEY_SIZE);
        ack_hash[4] = data[5 + text_len + 1];
        /* Mesh::_rng is private; our own RNG instance is the same object
           the stack was constructed with, so this is the same source. */
        if (s_rng) s_rng->random(&ack_hash[5], 1);

        mesh::Packet *ack = createAck(ack_hash, 6);
        if (ack) sendFlood(ack);
        ESP_LOGI(TAG, "dm from %s: %s", s_peers[pi].id, text);
    }

    /* The far end answered. This is the only place a message becomes
       ACKED, and it means the recipient itself replied - not that somebody
       relayed it. */
    void onAckRecv(mesh::Packet *packet, uint32_t ack_crc) override
    {
        for (int i = 0; i < ACK_TRACK; i++) {
            if (!s_ack[i].used || s_ack[i].expect != ack_crc) continue;
            const int slot = s_ack[i].slot;
            if (slot >= 0 && slot < LS_MESH_MAX_MSGS && s_msgs[slot].mine) {
                s_msgs[slot].state = LS_MSG_ACKED;
                s_msg_seq++;
            }
            /* An ack is the strongest proof of life there is. */

            if (slot >= 0 && slot < LS_MESH_MAX_MSGS) {
                const int pi = peer_index(s_msgs[slot].peer);
                if (pi >= 0)
                    touch_peer(pi, s_radio ? s_radio->getLastRSSI() : 0.0f,
                               packet ? packet->getSNR() : 0.0f);
            }

            s_ack[i].used = false;
            packet->markDoNotRetransmit();   /* it was for us */
            ESP_LOGI(TAG, "ack received");
            return;
        }
    }

    /* Tell the stack we hold the public channel. Upstream's Mesh
       calls this with the channel hash off the wire; returning our channel
       when it matches is the whole of what "being on a channel" means. */
    int searchChannelsByHash(const uint8_t *hash, mesh::GroupChannel channels[],
                             int max_matches) override
    {
        if (max_matches < 1) return 0;
        int n = 0;
        for (int i = 0; i < LS_MESH_MAX_CHANNELS && n < max_matches; i++) {
            if (!s_chan_ready[i]) continue;
            if (memcmp(hash, s_chan[i].hash, PATH_HASH_SIZE) != 0) continue;
            channels[n++] = s_chan[i];
        }
        return n;
    }

    /* Group text, parsed exactly as BaseChatMesh.cpp:367 does:
         data[0..3] timestamp, data[4] text type, data[5..] "name: message".
       The length checks are upstream's, kept because a short or unsupported
       frame here is attacker-controlled input, not a programming error. */
    void onGroupDataRecv(mesh::Packet *packet, uint8_t type,
                         const mesh::GroupChannel &channel,
                         uint8_t *data, size_t len) override
    {
        (void)packet; (void)channel;
        if (type != PAYLOAD_TYPE_GRP_TXT) return;
        if (len < 5) return;
        if ((data[4] >> 2) != 0) return;     /* not a plain text type */

        char text[LS_MESH_MSG_LEN];
        size_t n = len - 5;
        if (n >= sizeof(text)) n = sizeof(text) - 1;
        memcpy(text, &data[5], n);
        text[n] = 0;
        /* The payload may be zero-padded by the block cipher; trim so the
           feed does not carry invisible trailing rubbish. */
        for (size_t i = 0; i < n; i++) if (!text[i]) { break; }
        note_msg(text, false);
        ESP_LOGI(TAG, "msg: %s", text);
    }

    void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override
    {
        (void)raw;
        ESP_LOGD(TAG, "raw %d bytes rssi=%.1f snr=%.1f", len, rssi, snr);
    }

    uint32_t adverts() const { return _adverts; }

private:
    uint32_t _adverts = 0;
};

/* ---- module state ------------------------------------------------------ */

#define MESH_POOL 12

#define MESH_TASK_STACK 8192
/* C6 pressure placed this polling task in RTC RAM. The radio and
 * shared-bus work must keep the same DRAM stack regardless of boot order. */
static DRAM_ATTR StackType_t s_mesh_stack[MESH_TASK_STACK / sizeof(StackType_t)]
    __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t s_mesh_tcb;

static LsMillis   *s_ms;
static LsRtcClock *s_rtc;
static LsBoard    *s_board;
static StaticPoolPacketManager *s_mgr;
static SimpleMeshTables        *s_tables;
static LsMesh     *s_mesh;

static TaskHandle_t s_task;
static volatile bool s_stop;
static volatile uint32_t s_loops;
static char s_self_id[17];

/* Identity lives in NVS. A mesh node that generates a fresh key pair on
   every boot is a different node every time - it cannot be addressed, and it
   pollutes its neighbours' peer tables. */
static bool load_or_create_identity(mesh::LocalIdentity &id, LsRng &rng)
{
    nvs_handle_t h;
    uint8_t blob[PUB_KEY_SIZE + PRV_KEY_SIZE];
    size_t len = sizeof(blob);

    if (nvs_open("meshcore", NVS_READWRITE, &h) != ESP_OK) return false;

    if (nvs_get_blob(h, "identity", blob, &len) == ESP_OK && len == sizeof(blob)) {
        BufferStream in(blob, sizeof(blob), sizeof(blob));
        bool ok = id.readFrom(in);
        nvs_close(h);
        if (ok) { ESP_LOGI(TAG, "identity loaded from NVS"); return true; }
        ESP_LOGW(TAG, "stored identity did not parse; making a new one");
        nvs_open("meshcore", NVS_READWRITE, &h);
    }

    id = mesh::LocalIdentity(&rng);
    BufferStream out(blob, sizeof(blob));
    if (!id.writeTo(out) || out.length() != sizeof(blob)) {
        nvs_close(h);
        ESP_LOGE(TAG, "could not serialise the new identity");
        return false;
    }
    esp_err_t err = nvs_set_blob(h, "identity", blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {

        ESP_LOGW(TAG, "identity not saved (%s); it will change on reboot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "new identity created and saved");
    }
    return true;
}

/* Settings live in the same NVS namespace as the identity, because
   they are the same kind of thing: what this node IS between reboots. */
static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("meshcore", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_name);
    if (nvs_get_str(h, "name", s_name, &len) != ESP_OK) s_name[0] = 0;
    len = sizeof(s_radio_cfg);
    if (nvs_get_blob(h, "radio", &s_radio_cfg, &len) != ESP_OK || len != sizeof(s_radio_cfg)) {

        memset(&s_radio_cfg, 0, sizeof(s_radio_cfg));
    }
    for (int i = 1; i < LS_MESH_MAX_CHANNELS; i++) {
        char key[12];
        snprintf(key, sizeof(key), "chname%d", i);
        size_t l = sizeof(s_chan_meta[i].name);
        if (nvs_get_str(h, key, s_chan_meta[i].name, &l) != ESP_OK)
            s_chan_meta[i].name[0] = 0;
        snprintf(key, sizeof(key), "chpsk%d", i);
        l = sizeof(s_chan_meta[i].psk);
        if (nvs_get_str(h, key, s_chan_meta[i].psk, &l) != ESP_OK)
            s_chan_meta[i].psk[0] = 0;
    }
    uint8_t a = 0;
    if (nvs_get_u8(h, "chactive", &a) == ESP_OK && a < LS_MESH_MAX_CHANNELS)
        s_chan_active = a;

    uint8_t v = 1;
    if (nvs_get_u8(h, "autolisten", &v) == ESP_OK) s_auto_listen = (v != 0);
    v = 0;
    if (nvs_get_u8(h, "autotx", &v) == ESP_OK)     s_auto_tx = (v != 0);
    nvs_close(h);
}

static esp_err_t save_setting_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("meshcore", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t save_setting_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("meshcore", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t save_setting_blob(const char *key, const void *val, size_t n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("meshcore", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, val, n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool our_position(int32_t *lat_e7, int32_t *lon_e7,
                         uint32_t *t, uint8_t *flags)
{
    ls_gps_state_t g;
    ls_gps_get(&g);
    if (!g.fix) return false;
    *lat_e7 = (int32_t)(g.lat_deg * 1e7);
    *lon_e7 = (int32_t)(g.lon_deg * 1e7);
    if (g.year >= 2020) {
        /* The receiver's own UTC, for the same reason the track uses it: this
           board's RTC may never have been set. */
        *flags = SIGHT_F_EPOCH;
        *t = (uint32_t)time(NULL);
        if (*t < 1735689600u) { *flags = 0; *t = mesh_now(); }
    } else {
        *flags = 0;
        *t = mesh_now();
    }
    return true;
}

static void note_sighting(int pi)
{
    if (!s_sightlog.open || pi < 0 || pi >= s_peer_count) return;

    int32_t lat = 0, lon = 0;
    uint32_t when = 0;
    uint8_t flags = 0;
    if (!our_position(&lat, &lon, &when, &flags)) return;

    ls_mesh_peer_t *p = &s_peers[pi];
    if (p->logged_any) {
        const float moved = ls_track_distance_m(p->logged_lat_e7,
                                                p->logged_lon_e7, lat, lon);
        if (moved < SIGHT_MOVE_M) return;
    }

    ls_sight_rec_t r;
    memset(&r, 0, sizeof(r));
    r.t = when;
    snprintf(r.id, sizeof(r.id), "%s", p->id);
    snprintf(r.name, sizeof(r.name), "%s", p->name);
    r.our_lat_e7 = lat;
    r.our_lon_e7 = lon;
    r.rssi_dbm = (int16_t)p->rssi;
    r.flags = flags;
    if (!ls_rlog_append(&s_sightlog, &r)) return;

    p->logged_lat_e7 = lat;
    p->logged_lon_e7 = lon;
    p->logged_any = true;
}

static void sight_waypoints(void *file)
{
    FILE *f = (FILE *)file;
    if (!f || !s_sightlog.open) return;

    const int n = ls_rlog_count(&s_sightlog);
    for (int i = 0; i < n; i++) {
        ls_sight_rec_t r;
        if (ls_rlog_read_at(&s_sightlog, i, &r) != 1) break;

        ls_track_pt_t pt;
        memset(&pt, 0, sizeof(pt));
        pt.lat_e7 = r.our_lat_e7;
        pt.lon_e7 = r.our_lon_e7;

        char desc[48];
        snprintf(desc, sizeof(desc), "%s at %d dBm",
                 r.id, (int)r.rssi_dbm);

        char line[256];
        const int len = ls_track_gpx_waypoint(line, sizeof(line), &pt,
                                              r.name[0] ? r.name : r.id, desc);
        if (len) fwrite(line, 1, (size_t)len, f);
    }
}

extern "C" int ls_mesh_sightings(void) { return ls_rlog_count(&s_sightlog); }

extern "C" bool ls_mesh_sight_clear(void)
{
    for (int i = 0; i < s_peer_count; i++) s_peers[i].logged_any = false;
    return ls_rlog_clear(&s_sightlog);
}

/* Write the peers out. Called only from the mesh task.

   One record per peer, in table order - there is no ring behaviour wanted
   here, the table IS the set of peers and replacing it wholesale is what
   keeps the card and the table saying the same thing. A peer that ages out
   of the table should age out of the card with it. */
static void peerlog_flush(void)
{
    if (!s_peerlog.open) return;

    ls_rlog_clear(&s_peerlog);
    for (int i = 0; i < s_peer_count; i++) {
        ls_mesh_peer_rec_t r;
        memset(&r, 0, sizeof(r));
        snprintf(r.id, sizeof(r.id), "%s", s_peers[i].id);
        memcpy(r.pub_key, s_peers[i].pub_key, sizeof(r.pub_key));
        snprintf(r.name, sizeof(r.name), "%s", s_peers[i].name);
        r.type    = s_peers[i].type;
        r.has_loc = s_peers[i].has_loc ? 1 : 0;
        r.lat_e6  = s_peers[i].lat_e6;
        r.lon_e6  = s_peers[i].lon_e6;
        if (!ls_rlog_append(&s_peerlog, &r)) break;
    }
}

static void peerlog_service(void)
{
    if (!s_peerlog.open) return;
    const uint32_t seq = s_peer_ident_seq;
    if (seq != s_peerlog_seq) {
        s_peerlog_seq   = seq;
        s_peerlog_due_us = esp_timer_get_time() + 4000000;
        return;
    }
    if (!s_peerlog_due_us || esp_timer_get_time() < s_peerlog_due_us) return;
    s_peerlog_due_us = 0;
    peerlog_flush();
}

static void peerlog_open(void)
{
    mkdir("/sdcard/lakeshark", 0777);
    if (!ls_rlog_open(&s_peerlog, PEERLOG_PATH,
                      sizeof(ls_mesh_peer_rec_t), LS_MESH_MAX_PEERS)) {
        ESP_LOGI(TAG, "no peer history (no card)");
        return;
    }

    int restored = 0;
    for (int i = 0; i < LS_MESH_MAX_PEERS && s_peer_count < LS_MESH_MAX_PEERS; i++) {
        ls_mesh_peer_rec_t r;
        /* One at a time, for the stack reason above. */
        if (ls_rlog_read_at(&s_peerlog, i, &r) != 1) break;
        if (!r.id[0]) continue;
        ls_mesh_peer_t *p = &s_peers[s_peer_count];
        memset(p, 0, sizeof(*p));
        snprintf(p->id, sizeof(p->id), "%s", r.id);
        memcpy(p->pub_key, r.pub_key, sizeof(p->pub_key));
        snprintf(p->name, sizeof(p->name), "%s", r.name);
        p->type    = r.type;
        p->has_loc = r.has_loc != 0;
        p->lat_e6  = r.lat_e6;
        p->lon_e6  = r.lon_e6;
        s_peer_count++;
        restored++;
    }
    s_peerlog_seq    = s_peer_ident_seq;
    s_peerlog_due_us = 0;
    ESP_LOGI(TAG, "peers: %d restored as contacts, none heard yet", restored);
}

/* Write the ring out, oldest first. Called only from the mesh task,
   which is the only thing that touches s_msgs. */
static void msglog_flush(void)
{
    if (!s_msglog.open) return;
    if (!s_msglog_buf) {
        s_msglog_buf = (ls_mesh_msg_t *)heap_caps_malloc(
            sizeof(ls_mesh_msg_t) * LS_MESH_MAX_MSGS, MALLOC_CAP_SPIRAM);
        if (!s_msglog_buf) { s_msglog.open = false; return; }
    }
    const int n = ls_mesh_messages(s_msglog_buf, LS_MESH_MAX_MSGS);
    ls_rlog_replace(&s_msglog, s_msglog_buf, n);
}

static void msglog_service(void)
{
    if (!s_msglog.open) return;

    const uint32_t seq = s_msg_seq;
    if (seq != s_msglog_seq) {
        s_msglog_seq    = seq;
        s_msglog_due_us = esp_timer_get_time() + MSGLOG_SETTLE_US;
        return;
    }
    if (!s_msglog_due_us || esp_timer_get_time() < s_msglog_due_us) return;
    s_msglog_due_us = 0;
    msglog_flush();
}

/* Open the log and read what is in it back into the live ring.

   The records come back oldest first, so writing them into slots 0..n-1 with
   the head following puts the ring in the same arrangement the file is in.
   A card that is not there, or a file whose geometry no longer matches this
   build's ls_mesh_msg_t, leaves the ring empty and every later call a no-op -
   which is what the firmware did before this existed. */
static void msglog_open(void)
{
    mkdir("/sdcard/lakeshark", 0777);      /* already-exists is the normal case */
    if (!ls_rlog_open(&s_msglog, MSGLOG_PATH,
                      sizeof(ls_mesh_msg_t), LS_MESH_MAX_MSGS)) {
        ESP_LOGI(TAG, "no message history (no card)");
        return;
    }
    const int n = ls_rlog_read(&s_msglog, s_msgs, LS_MESH_MAX_MSGS);
    if (n > 0) {
        s_msg_count = n;
        s_msg_head  = n % LS_MESH_MAX_MSGS;
        s_msg_seq++;
    }
    s_msglog_seq    = s_msg_seq;
    s_msglog_due_us = 0;
    ESP_LOGI(TAG, "message history: %d restored", n);
}

static void mesh_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "background task up");
    while (!s_stop) {
        /* Held: the radio belongs to somebody else. Everything that
           touches it is skipped - the dispatcher, the advert timer and the
           RSSI sample - and nothing else is: the message log still settles
           and still writes, because that is a card and not a radio. */
        if (s_radio_hold) {
            s_radio_held = true;
            msglog_service();
            peerlog_service();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        s_radio_held = false;

        s_mesh->loop();
        s_loops++;

        /* Periodic self-advert. This is how a mesh node stays visible:
           a neighbour that has never heard you cannot route to you. Off by
           default, because it transmits, and it does nothing at all while
           the transmitter is disarmed. */
        if (s_radio_cfg.advert_secs && ls_mesh_tx_enabled()) {
            const int64_t now_a = esp_timer_get_time();
            if (s_advert_next_us == 0)
                s_advert_next_us = now_a + (int64_t)s_radio_cfg.advert_secs * 1000000;
            else if (now_a >= s_advert_next_us) {
                s_advert_next_us = now_a + (int64_t)s_radio_cfg.advert_secs * 1000000;
                ls_mesh_advertise();
            }
        }

        /* One RSSI sample every 100 ms while receiving. Skipped while
           transmitting - the reading is meaningless then, and ls_lora_rssi_inst
           refuses anyway, which would write a zero and put a false notch in
           the trace. */
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= s_scope_next_us) {
            s_scope_next_us = now_us + 100000;
            float dbm = 0;
            if (ls_lora_rssi_inst(&dbm) == ESP_OK) {
                s_scope[s_scope_head] = dbm;
                s_scope_head = (s_scope_head + 1) % LS_MESH_SCOPE_N;
            }
        }
        msglog_service();
        peerlog_service();

        /* 5 ms. Below the ~10 ms DIO1 polling floor the expander read
           dominates and the I2C bus is shared with touch, so going faster
           costs the interface and buys nothing. */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "background task down");
    /* Stop owns deletion, after suspension, before this storage is reused. */
    vTaskSuspend(NULL);
}

extern "C" esp_err_t ls_mesh_start(void)
{
    if (s_task) return ESP_OK;

    /* The radio first: everything else is pointless without it, and this is
       the step that can actually fail. */
    ls_lora_cfg_t cfg;
    ls_lora_cfg_default(&cfg);          /* US915 910.525 MHz SF7 BW62.5k */

    load_settings();
    if (s_radio_cfg.freq_hz)   cfg.freq_hz   = s_radio_cfg.freq_hz;
    if (s_radio_cfg.sf)      { cfg.sf        = s_radio_cfg.sf;
                               cfg.preamble  = (cfg.sf <= 8) ? 32 : 16; }
    if (s_radio_cfg.bw_hz)     cfg.bw_hz     = s_radio_cfg.bw_hz;
    if (s_radio_cfg.power_dbm) cfg.power_dbm = s_radio_cfg.power_dbm;
    if (s_radio_cfg.cr)        cfg.cr        = s_radio_cfg.cr;
    if (s_radio_cfg.sync_word) cfg.sync_word = s_radio_cfg.sync_word;
    if (s_radio_cfg.freq_hz) {
        /* The image calibration band has to follow the frequency or the
           receiver quietly loses several dB. Chosen from the band the
           frequency actually falls in, not from a stored guess. */
        const uint32_t mhz = s_radio_cfg.freq_hz / 1000000u;
        if (mhz >= 902 && mhz <= 928)      { cfg.cal_min_mhz = 902; cfg.cal_max_mhz = 928; }
        else if (mhz >= 863 && mhz <= 870) { cfg.cal_min_mhz = 863; cfg.cal_max_mhz = 870; }
    }

    if (s_radio_cfg.crc_on)    cfg.crc_on    = (s_radio_cfg.crc_on == 2);
    esp_err_t err = ls_lora_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio not configured: %s", esp_err_to_name(err));
        return err;
    }

    s_ms     = new LsMillis();
    s_rtc    = new LsRtcClock();
    s_rng    = new LsRng();
    s_board  = new LsBoard();
    s_radio  = new LsRadio();
    s_mgr    = new StaticPoolPacketManager(MESH_POOL);
    s_tables = new SimpleMeshTables();
    if (!s_ms || !s_rtc || !s_rng || !s_board || !s_radio || !s_mgr || !s_tables)
        return ESP_ERR_NO_MEM;

    s_mesh = new LsMesh(*s_radio, *s_ms, *s_rng, *s_rtc, *s_mgr, *s_tables);
    if (!s_mesh) return ESP_ERR_NO_MEM;

    if (!load_or_create_identity(s_mesh->self_id, *s_rng)) return ESP_FAIL;
    for (int i = 0; i < 8; i++)
        sprintf(&s_self_id[i * 2], "%02X", s_mesh->self_id.pub_key[i]);

    /* The public channel: hash is the first PATH_HASH_SIZE bytes of
       SHA256(secret), which is what upstream's addChannel does. */
    for (int i = 0; i < LS_MESH_MAX_CHANNELS; i++) chan_build(i);
    s_public_ready = s_chan_ready[0];
    if (s_chan_active < 0 || s_chan_active >= LS_MESH_MAX_CHANNELS ||
        !s_chan_ready[s_chan_active])
        s_chan_active = 0;

    s_mesh->begin();
    ls_lora_receive();

    /* Before the task exists, because the task is the only other thing that
       touches the ring. */
    msglog_open();
    peerlog_open();
    /* And the sighting log, plus the registration that puts its
       waypoints into an exported track. */
    if (ls_rlog_open(&s_sightlog, SIGHTLOG_PATH, sizeof(ls_sight_rec_t),
                     SIGHT_MAX))
        ls_track_set_waypoint_source(sight_waypoints);

    s_stop = false;
    s_task = xTaskCreateStaticPinnedToCore(mesh_task, "meshcore", MESH_TASK_STACK,
        nullptr, 4, s_mesh_stack, &s_mesh_tcb, tskNO_AFFINITY);
    if (!s_task) {
        ESP_LOGE(TAG, "could not create the task");
        return ESP_ERR_NO_MEM;
    }
    /* The one place auto transmit takes effect. Deliberately AFTER
       everything else has succeeded, so a board that failed to bring the
       radio up never ends up armed. */
    if (s_auto_tx) s_radio->setTxEnabled(true);

    /* Announce ourselves once on start, if asked. Only meaningful
       when the transmitter is armed, and silently skipped otherwise rather
       than pretending. */
    s_advert_next_us = 0;
    if (s_radio_cfg.advert_boot && ls_mesh_tx_enabled()) ls_mesh_advertise();

    ESP_LOGI(TAG, "MeshCore up as %s, transmit %s", s_self_id,
             s_auto_tx ? "ARMED (auto)" : "DISARMED");
    return ESP_OK;
}

extern "C" void ls_mesh_stop(void)
{
    if (!s_task) return;
    s_stop = true;
    for (int i = 0; i < 100 && eTaskGetState(s_task) != eSuspended; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (eTaskGetState(s_task) != eSuspended) {
        ESP_LOGW(TAG, "stop pending; task has not released the radio");
        return;
    }
    vTaskDelete(s_task);
    s_task = nullptr;
    /* The task is gone, so nothing else is writing s_msgs: a last write here
       is what keeps the tail of a conversation the debounce had not got
       round to yet. */
    msglog_flush();
    peerlog_flush();
}

extern "C" bool ls_mesh_running(void) { return s_task != nullptr; }

extern "C" bool ls_mesh_radio_held(void)
{
    /* With no task there is nothing to hold the radio away from, so the
       answer is yes and the caller may proceed. Saying no would make a
       spectrum refuse to run on a board whose mesh was never started. */
    if (!s_task) return true;

    return s_radio_held && ls_lora_send_done();
}

extern "C" bool ls_mesh_radio_hold(bool on)
{
    s_radio_hold = on;
    if (!on) { s_radio_held = false; return true; }

    return ls_mesh_radio_held();
}

extern "C" void ls_mesh_set_tx(bool on)
{
    if (s_radio) s_radio->setTxEnabled(on);
}

extern "C" bool ls_mesh_tx_enabled(void)
{
    return s_radio && s_radio->txEnabled();
}

extern "C" void ls_mesh_get_stats(ls_mesh_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->running     = s_task != nullptr;
    out->radio_ready = ls_lora_cfg() != nullptr;
    out->loops       = s_loops;
    if (s_radio) {
        out->tx_enabled = s_radio->txEnabled();
        out->rx_packets = s_radio->rx();
        out->rx_bad     = s_radio->bad();
        out->tx_packets = s_radio->tx();
        out->last_rssi  = s_radio->getLastRSSI();
        out->last_snr   = s_radio->getLastSNR();
    }
    if (s_mesh) {
        out->airtime_ms   = (uint32_t)s_mesh->getTotalAirTime();
        out->tx_budget_ms = (uint32_t)s_mesh->getRemainingTxBudget();
    }
    memcpy(out->self_id, s_self_id, sizeof(out->self_id));
}

extern "C" esp_err_t ls_mesh_advertise(void)
{
    if (!s_mesh) return ESP_ERR_INVALID_STATE;
    if (!ls_mesh_tx_enabled()) return ESP_ERR_NOT_ALLOWED;
    /* With app_data now, not bare. A bare advert verifies and relays
       but carries no name and no type, so peers cannot name us and a
       repeater's neighbour table - which filters on ADV_TYPE_REPEATER -
       ignores us entirely. That was observed on the bench: the T-Beam
       relayed our packets all afternoon and its `neighbors` list stayed
       empty. */
    const uint8_t role = s_radio_cfg.role ? s_radio_cfg.role : LS_MESH_ROLE_CHAT;
    uint8_t app_data[MAX_ADVERT_DATA_SIZE];
    uint8_t app_len;

    bool have_loc = false;
    double lat = 0, lon = 0;
    if (s_radio_cfg.share_loc == 1) {
        ls_gps_state_t g;
        ls_gps_get(&g);
        if (g.fix) { have_loc = true; lat = g.lat_deg; lon = g.lon_deg; }
    } else if (s_radio_cfg.share_loc == 2) {
        have_loc = true;
        lat = s_radio_cfg.lat_e6 / 1e6;
        lon = s_radio_cfg.lon_e6 / 1e6;
    }

    if (have_loc) {
        AdvertDataBuilder b(role, ls_mesh_name(), lat, lon);
        app_len = b.encodeTo(app_data);
    } else {
        AdvertDataBuilder b(role, ls_mesh_name());
        app_len = b.encodeTo(app_data);
    }

    mesh::Packet *pkt = s_mesh->createAdvert(s_mesh->self_id, app_data, app_len);
    if (!pkt) return ESP_ERR_NO_MEM;
    s_mesh->sendFlood(pkt);
    return ESP_OK;
}

extern "C" int ls_mesh_channels(ls_mesh_chan_t *out, int max)
{
    if (!out || max <= 0) return 0;
    const int n = max < LS_MESH_MAX_CHANNELS ? max : LS_MESH_MAX_CHANNELS;
    for (int i = 0; i < n; i++) {
        out[i] = s_chan_meta[i];
        out[i].fixed = (i == 0);
        if (i == 0) {
            snprintf(out[i].name, sizeof(out[i].name), "Public");
            snprintf(out[i].psk, sizeof(out[i].psk), "izOH6cXN6mrJ5e26oRXNcg==");
        }
    }
    return n;
}

extern "C" int ls_mesh_channel_active(void) { return s_chan_active; }

extern "C" esp_err_t ls_mesh_set_channel_active(int idx)
{
    if (idx < 0 || idx >= LS_MESH_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;

    if (!s_chan_ready[idx]) return ESP_ERR_INVALID_STATE;
    s_chan_active = idx;
    return save_setting_u8("chactive", (uint8_t)idx);
}

extern "C" esp_err_t ls_mesh_set_channel(int idx, const char *name, const char *psk_b64)
{
    if (idx < 1 || idx >= LS_MESH_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;

    if (psk_b64) {
        if (*psk_b64) {
            uint8_t key[32];
            const int len = b64_decode(psk_b64, key, sizeof(key));
            if (len != 16 && len != 32) return ESP_ERR_INVALID_ARG;
        }
        snprintf(s_chan_meta[idx].psk, sizeof(s_chan_meta[idx].psk), "%s", psk_b64);
    }
    if (name)
        snprintf(s_chan_meta[idx].name, sizeof(s_chan_meta[idx].name), "%s", name);

    chan_build(idx);
    /* A slot that just became unusable must not stay selected. */
    if (!s_chan_ready[idx] && s_chan_active == idx) s_chan_active = 0;

    char key[12];
    snprintf(key, sizeof(key), "chname%d", idx);
    esp_err_t err = save_setting_str(key, s_chan_meta[idx].name);
    snprintf(key, sizeof(key), "chpsk%d", idx);
    if (err == ESP_OK) err = save_setting_str(key, s_chan_meta[idx].psk);
    return err;
}

extern "C" const char *ls_mesh_band_name(int band)
{
    switch (band) {
        case LS_MESH_BAND_US915: return "US915";
        case LS_MESH_BAND_EU868: return "EU868";
        default: return "custom";
    }
}

extern "C" int ls_mesh_band_current(void)
{
    ls_mesh_radio_t r;
    ls_mesh_get_radio(&r);
    const uint32_t mhz = r.freq_hz / 1000000u;
    if (mhz >= 902 && mhz <= 928) return LS_MESH_BAND_US915;
    if (mhz >= 863 && mhz <= 870) return LS_MESH_BAND_EU868;
    return LS_MESH_BAND__COUNT;      /* reads as "custom" */
}

extern "C" esp_err_t ls_mesh_set_band(int band)
{
    ls_mesh_radio_t r;
    ls_mesh_get_radio(&r);
    /* MeshCore's own defaults for each band, so a preset lands this node
       where the rest of the mesh already is. */
    switch (band) {
        case LS_MESH_BAND_US915: r.freq_hz = 910525000u; break;
        case LS_MESH_BAND_EU868: r.freq_hz = 869618000u; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    return ls_mesh_set_radio(&r);
}

extern "C" bool ls_mesh_auto_listen(void) { return s_auto_listen; }
extern "C" bool ls_mesh_auto_tx(void)     { return s_auto_tx; }

extern "C" esp_err_t ls_mesh_set_auto(bool listen, bool tx)
{
    s_auto_listen = listen;
    s_auto_tx     = tx;
    nvs_handle_t h;
    esp_err_t err = nvs_open("meshcore", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "autolisten", listen ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "autotx", tx ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    /* Apply immediately to a stack that is already up, so the setting and
       the state never disagree while you are looking at them. */
    if (s_task && s_radio) s_radio->setTxEnabled(tx);
    return err;
}

extern "C" void ls_mesh_boot(void)
{
    load_settings();
    if (!s_auto_listen) return;
    const esp_err_t err = ls_mesh_start();
    if (err != ESP_OK) {
        /* A radio that is absent or busy at boot is not a reason to stop
           booting. Say so once and carry on. */
        ESP_LOGW(TAG, "auto listen: %s", esp_err_to_name(err));
    }
}

extern "C" int ls_mesh_scope(float *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = max < LS_MESH_SCOPE_N ? max : LS_MESH_SCOPE_N;
    /* Oldest first, so the caller draws left to right and time runs the way
       everyone reads it. */
    int start = (s_scope_head - n + LS_MESH_SCOPE_N * 2) % LS_MESH_SCOPE_N;
    for (int i = 0; i < n; i++) out[i] = s_scope[(start + i) % LS_MESH_SCOPE_N];
    return n;
}

extern "C" esp_err_t ls_mesh_add_contact(const char *hex, const char *name)
{
    if (!hex) return ESP_ERR_INVALID_ARG;
    /* 64 hex characters, nothing else. A short key would address a
       different node, silently. */
    size_t n = strlen(hex);
    if (n != PUB_KEY_SIZE * 2) return ESP_ERR_INVALID_SIZE;

    uint8_t key[PUB_KEY_SIZE];
    for (size_t i = 0; i < PUB_KEY_SIZE; i++) {
        unsigned byte = 0;
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        char *end = NULL;
        byte = (unsigned)strtoul(pair, &end, 16);
        if (end != pair + 2) return ESP_ERR_INVALID_ARG;
        key[i] = (uint8_t)byte;
    }

    char id[17];
    id_hex(id, key);

    mesh::Identity ident(key);
    /* No advert, so no signal reading and no advert count - the table shows
       it as never heard, which is exactly what it is. */
    note_peer(ident, id, NULL, 0.0f, 0.0f);

    for (int i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].id, id)) continue;
        if (name && *name)
            snprintf(s_peers[i].name, sizeof(s_peers[i].name), "%s", name);
        /* And the TIME, which the comment above has claimed since this was written and the code did not do. */

        s_peers[i].adverts     = 0;      /* it announced nothing   */
        s_peers[i].last_heard  = 0;      /* and at no time         */
        s_peers[i].first_heard = 0;
        s_peers[i].rssi        = 0.0f;
        s_peers[i].snr         = 0.0f;
        break;
    }
    ESP_LOGI(TAG, "contact added by key: %s", id);
    return ESP_OK;
}

extern "C" bool ls_mesh_peer_at(int rank, ls_mesh_peer_t *out)
{
    if (!out || rank < 0 || rank >= s_peer_count) return false;
    /* Same ranking ls_mesh_peers() produces, without building the list. */
    for (int i = 0; i < s_peer_count; i++) {
        int r = 0;
        for (int j = 0; j < s_peer_count; j++)
            if (j != i && s_peers[j].last_heard > s_peers[i].last_heard) r++;
        if (r == rank) { *out = s_peers[i]; return true; }
    }
    return false;
}

extern "C" int ls_mesh_messages(ls_mesh_msg_t *out, int max)
{
    if (!out || max <= 0) return 0;
    /* Oldest first: a chat is read downward, and a screen that shows the
       last N wants the last N in order, not reversed. */
    int n = s_msg_count < max ? s_msg_count : max;
    int start = (s_msg_head - n + LS_MESH_MAX_MSGS * 2) % LS_MESH_MAX_MSGS;
    for (int i = 0; i < n; i++)
        out[i] = s_msgs[(start + i) % LS_MESH_MAX_MSGS];
    return n;
}

extern "C" uint32_t ls_mesh_msg_seq(void) { return s_msg_seq; }

extern "C" const char *ls_mesh_name(void)
{
    if (s_name[0]) return s_name;

    return s_self_id[0] ? s_self_id : "LakeShark";
}

extern "C" esp_err_t ls_mesh_set_name(const char *name)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;

    size_t j = 0;
    for (size_t i = 0; name[i] && j < LS_MESH_NAME_MAX; i++) {
        char c = name[i];
        if (c == ':' || (unsigned char)c < 0x20 || (unsigned char)c > 0x7E) continue;
        s_name[j++] = c;
    }
    s_name[j] = 0;
    if (!j) return ESP_ERR_INVALID_ARG;
    return save_setting_str("name", s_name);
}

extern "C" void ls_mesh_get_radio(ls_mesh_radio_t *out)
{
    if (!out) return;
    /* The behaviour fields live only in the stored settings; the radio ones
       are read back from the radio when it is up, because that is what is
       actually on the air. */
    *out = s_radio_cfg;
    const ls_lora_cfg_t *c = ls_lora_cfg();
    if (c) {
        out->freq_hz   = c->freq_hz;
        out->sf        = c->sf;
        out->bw_hz     = c->bw_hz;
        out->power_dbm = c->power_dbm;
        out->cr        = c->cr;
        out->sync_word = c->sync_word;
        out->crc_on    = c->crc_on ? 2 : 1;
    }
}

extern "C" esp_err_t ls_mesh_set_radio(const ls_mesh_radio_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    ls_lora_cfg_t lc;
    ls_lora_cfg_default(&lc);
    lc.freq_hz   = cfg->freq_hz;
    lc.sf        = cfg->sf;
    lc.bw_hz     = cfg->bw_hz;
    lc.power_dbm = cfg->power_dbm;
    if (cfg->cr)        lc.cr        = cfg->cr;
    if (cfg->sync_word) lc.sync_word = cfg->sync_word;
    if (cfg->crc_on)    lc.crc_on    = (cfg->crc_on == 2);
    {
        const uint32_t mhz = cfg->freq_hz / 1000000u;
        if (mhz >= 902 && mhz <= 928)      { lc.cal_min_mhz = 902; lc.cal_max_mhz = 928; }
        else if (mhz >= 863 && mhz <= 870) { lc.cal_min_mhz = 863; lc.cal_max_mhz = 870; }
    }

    lc.preamble  = (lc.sf <= 8) ? 32 : 16;

    esp_err_t err = ls_lora_configure(&lc);
    if (err != ESP_OK) return err;
    ls_lora_receive();

    s_radio_cfg = *cfg;
    return save_setting_blob("radio", &s_radio_cfg, sizeof(s_radio_cfg));
}

/* Everything after the peer has been identified, which is the same
   whether the caller named a row or a node. Split out so the two entry
   points cannot drift: the payload layout, the acknowledgement hash and the
   shared secret are the protocol, and one copy of the protocol is the rule
   this file already keeps with upstream. */
static esp_err_t send_dm_to(int pi, const char *text)
{

    /* Resolve the caller's row WITHOUT materialising the list. */

    if (pi < 0 || pi >= s_peer_count) return ESP_ERR_NOT_FOUND;

    /* The payload, from BaseChatMesh.cpp:408:
         [0..3] timestamp   [4] attempt & 3   [5..] text NUL       */
    uint8_t temp[5 + LS_MESH_MSG_LEN + 2];
    const uint32_t ts = s_rtc ? s_rtc->getCurrentTimeUnique() : mesh_now();
    memcpy(temp, &ts, 4);
    temp[4] = 0;
    int tlen = (int)strlen(text);
    if (tlen > LS_MESH_MSG_LEN - 8) tlen = LS_MESH_MSG_LEN - 8;
    memcpy(&temp[5], text, (size_t)tlen);
    temp[5 + tlen] = 0;

    uint32_t expect = 0;
    mesh::Utils::sha256((uint8_t *)&expect, 4, temp, 5 + tlen,
                        s_mesh->self_id.pub_key, PUB_KEY_SIZE);

    uint8_t secret[PUB_KEY_SIZE];
    s_mesh->self_id.calcSharedSecret(secret, s_peers[pi].pub_key);

    mesh::Identity dest(s_peers[pi].pub_key);
    mesh::Packet *pkt = s_mesh->createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret,
                                               temp, (size_t)(5 + tlen));
    if (!pkt) return ESP_ERR_NO_MEM;
    s_mesh->sendFlood(pkt);

    /* One buffer, not two - see above. */
    char full[LS_MESH_MSG_LEN];
    snprintf(full, sizeof(full), "%s -> %.12s: %s", ls_mesh_name(),
             s_peers[pi].name[0] ? s_peers[pi].name : s_peers[pi].id, text);
    const int slot = note_msg(full, true);
    s_msgs[slot].direct = true;
    memcpy(s_msgs[slot].peer, s_peers[pi].id, sizeof(s_msgs[slot].peer));

    s_ack[s_ack_head].expect = expect;
    s_ack[s_ack_head].slot   = slot;
    s_ack[s_ack_head].used   = true;
    s_ack_head = (s_ack_head + 1) % ACK_TRACK;
    return ESP_OK;
}

/* Resolve the caller's row WITHOUT materialising the list. */

extern "C" esp_err_t ls_mesh_send_dm(int peer_index, const char *text)
{
    if (!s_mesh) return ESP_ERR_INVALID_STATE;
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    if (!ls_mesh_tx_enabled()) return ESP_ERR_NOT_ALLOWED;
    if (peer_index < 0 || peer_index >= s_peer_count) return ESP_ERR_NOT_FOUND;

    int pi = -1;
    for (int i = 0; i < s_peer_count; i++) {
        int rank = 0;
        for (int j = 0; j < s_peer_count; j++)
            if (j != i && s_peers[j].last_heard > s_peers[i].last_heard) rank++;
        if (rank == peer_index) { pi = i; break; }
    }
    return send_dm_to(pi, text);
}

extern "C" esp_err_t ls_mesh_send_dm_id(const char *id, const char *text)
{
    if (!s_mesh) return ESP_ERR_INVALID_STATE;
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    if (!ls_mesh_tx_enabled()) return ESP_ERR_NOT_ALLOWED;
    /* No rank, no row, no sort. The id IS the peer, so nothing that
       happens to the list between locking a chat and sending on it can move
       the message to a different node. */
    return send_dm_to(peer_index(id), text);
}

extern "C" bool ls_mesh_peer_known(const char *id)
{
    return peer_index(id) >= 0;
}

extern "C" int ls_mesh_forget_peer(const char *id)
{
    if (!id || !*id || !strcasecmp(id, "all")) {
        const int n = s_peer_count;
        s_peer_count = 0;

        s_peer_ident_seq++;
        peerlog_flush();
        return n;
    }

    const int i = peer_index(id);
    if (i < 0) return 0;
    /* Order in this table is not meaningful - ls_mesh_peers sorts by
       last_heard for display - so closing the gap with the last entry is
       correct and cheaper than shifting. */
    s_peers[i] = s_peers[s_peer_count - 1];
    s_peer_count--;
    s_peer_ident_seq++;
    peerlog_flush();
    return 1;
}

extern "C" esp_err_t ls_mesh_send_text(const char *text)
{
    if (!s_mesh) return ESP_ERR_INVALID_STATE;
    if (s_chan_active < 0 || s_chan_active >= LS_MESH_MAX_CHANNELS ||
        !s_chan_ready[s_chan_active])
        return ESP_ERR_INVALID_STATE;
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    if (!ls_mesh_tx_enabled()) return ESP_ERR_NOT_ALLOWED;

    /* The wire format, from BaseChatMesh.cpp:475:
         [0..3] timestamp   [4] TXT_TYPE_PLAIN   [5..] "name: message" NUL
       The timestamp is not used for ordering - upstream's comment calls it
       "mostly an extra blob to help make packet_hash unique", which matters
       because two identical messages would otherwise dedupe against each
       other and the second would vanish. */
    uint8_t temp[5 + LS_MESH_MSG_LEN + 8];
    uint32_t ts = s_rtc ? s_rtc->getCurrentTimeUnique() : mesh_now();
    memcpy(temp, &ts, 4);
    temp[4] = 0;                            /* TXT_TYPE_PLAIN */

    int prefix = snprintf((char *)&temp[5], LS_MESH_MSG_LEN, "%s: ", ls_mesh_name());
    if (prefix < 0) return ESP_FAIL;
    int room = LS_MESH_MSG_LEN - 1 - prefix;
    if (room <= 0) return ESP_ERR_INVALID_SIZE;
    int tlen = (int)strlen(text);
    if (tlen > room) tlen = room;
    memcpy(&temp[5 + prefix], text, (size_t)tlen);
    temp[5 + prefix + tlen] = 0;

    mesh::Packet *pkt = s_mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT,
                                                    s_chan[s_chan_active],
                                                    temp, (size_t)(5 + prefix + tlen));
    if (!pkt) return ESP_ERR_NO_MEM;

    /* The hash BEFORE sendFlood: the packet is owned by the dispatcher once
       it is queued and must not be touched afterwards. */
    uint8_t h[MAX_HASH_SIZE];
    pkt->calculatePacketHash(h);

    s_mesh->sendFlood(pkt);

    const int slot = note_msg((const char *)&temp[5], true);
    memcpy(s_sent[s_sent_head].hash, h, MAX_HASH_SIZE);
    s_sent[s_sent_head].slot = slot;
    s_sent[s_sent_head].used = true;
    s_sent_head = (s_sent_head + 1) % SENT_TRACK;
    return ESP_OK;
}

extern "C" int ls_mesh_peers(ls_mesh_peer_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = s_peer_count < max ? s_peer_count : max;
    memcpy(out, s_peers, (size_t)n * sizeof(*out));
    /* Most recently heard first: a screen that shows three of twelve should
       show the three that matter. Insertion sort - n is at most 12. */
    for (int i = 1; i < n; i++) {
        ls_mesh_peer_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].last_heard < key.last_heard) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return n;
}

extern "C" int ls_mesh_events(ls_mesh_event_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 1; i <= LS_MESH_MAX_EVENTS && n < max; i++) {
        int idx = (s_ev_head - i + LS_MESH_MAX_EVENTS * 2) % LS_MESH_MAX_EVENTS;
        if (s_events[idx].kind == LS_MESH_EV_NONE) continue;
        out[n++] = s_events[idx];
    }
    return n;
}

extern "C" uint32_t ls_mesh_event_seq(void) { return s_ev_seq; }

extern "C" void ls_mesh_diagnostics(void)
{
    ls_mesh_stats_t st;
    ls_mesh_get_stats(&st);
    if (!st.running) {
        printf("mesh: not running - 'mesh start'\n");
        return;
    }
    printf("mesh: node %s  transmit %s\n", st.self_id,
           st.tx_enabled ? "ARMED" : "disarmed");
    /* High-water mark, so nobody has to guess again. This is the
       LOWEST free stack the task has ever had, which is the only number
       that means anything - the idle figure is what got this wrong. */
    if (s_task)
        printf("mesh: stack headroom %u bytes of %d (low water)\n",
               (unsigned)uxTaskGetStackHighWaterMark(s_task), MESH_TASK_STACK);

    printf("mesh: tx budget %lu ms remaining in the duty-cycle window\n",
           (unsigned long)(s_mesh ? s_mesh->getRemainingTxBudget() : 0));
    printf("mesh: loops=%lu rx=%lu bad=%lu tx=%lu airtime=%lu ms\n",
           (unsigned long)st.loops, (unsigned long)st.rx_packets,
           (unsigned long)st.rx_bad, (unsigned long)st.tx_packets,
           (unsigned long)st.airtime_ms);
    if (st.rx_packets)
        printf("mesh: last rssi=%.1f dBm snr=%.1f dB\n", st.last_rssi, st.last_snr);
    /* The feed, with delivery state, so the implicit acknowledgement
       is visible without a screen. `*` means a neighbour relayed it back. */
    /* Walk the ring in place. */

    const int nm = s_msg_count;
    if (nm) {
        const int start = (s_msg_head - nm + LS_MESH_MAX_MSGS * 2) % LS_MESH_MAX_MSGS;
        printf("mesh: %d message%s\n", nm, nm == 1 ? "" : "s");
        for (int i = 0; i < nm; i++) {
            const ls_mesh_msg_t *m = &s_msgs[(start + i) % LS_MESH_MAX_MSGS];
            char mark = '<';
            if (m->mine) {
                mark = m->state == LS_MSG_SENDING ? '.'
                     : m->state == LS_MSG_HEARD   ? '*' : '>';
            }
            printf("  %c %s%s\n", mark, m->text,
                   m->state == LS_MSG_HEARD ? "   [relayed]" : "");
        }
    }

    /* Fetched once, outside the loop: it was being rebuilt on every
       iteration, and it is 272 bytes on a caller stack that has already
       been overflowed twice (, ). */
    ls_mesh_chan_t chans[LS_MESH_MAX_CHANNELS];
    ls_mesh_channels(chans, LS_MESH_MAX_CHANNELS);
    for (int i = 0; i < LS_MESH_MAX_CHANNELS; i++) {
        if (!s_chan_ready[i] && i) continue;
        printf("mesh: channel %d %-10s %s\n", i,
               chans[i].name[0] ? chans[i].name : "(unnamed)",
               i == s_chan_active ? "<- sending here" : "");
    }

    printf("mesh: auto listen %s, auto transmit %s\n",
           s_auto_listen ? "on" : "off",
           s_auto_tx ? "ON - this board can emit from boot" : "off");

    const ls_lora_cfg_t *c = ls_lora_cfg();
    if (c)
        printf("mesh: %.4f MHz SF%u BW%lu CR4/%u sync 0x%02X\n",
               c->freq_hz / 1e6, (unsigned)c->sf, (unsigned long)c->bw_hz,
               (unsigned)c->cr, (unsigned)c->sync_word);

    if (ls_mesh_radio_stored())
        printf("mesh: these are STORED settings and override the built-in "
               "defaults - 'mesh radio default' to go back to them\n");
}

/* True when any field of the stored blob is set, which is what makes
   ls_mesh_start prefer it over the compiled default. */
extern "C" bool ls_mesh_radio_stored(void)
{
    return s_radio_cfg.freq_hz || s_radio_cfg.sf || s_radio_cfg.bw_hz ||
           s_radio_cfg.power_dbm || s_radio_cfg.cr || s_radio_cfg.sync_word ||
           s_radio_cfg.crc_on;
}

extern "C" esp_err_t ls_mesh_radio_default(void)
{
    memset(&s_radio_cfg, 0, sizeof(s_radio_cfg));
    esp_err_t err = save_setting_blob("radio", &s_radio_cfg,
                                      sizeof(s_radio_cfg));
    if (err != ESP_OK) return err;

    ls_lora_cfg_t lc;
    ls_lora_cfg_default(&lc);
    err = ls_lora_configure(&lc);
    if (err == ESP_OK) ls_lora_receive();
    return err;
}

#else  /* board has no LoRa */

extern "C" esp_err_t ls_mesh_start(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void ls_mesh_stop(void) {}
extern "C" bool ls_mesh_running(void) { return false; }
extern "C" void ls_mesh_set_tx(bool on) { (void)on; }
extern "C" bool ls_mesh_tx_enabled(void) { return false; }
extern "C" void ls_mesh_get_stats(ls_mesh_stats_t *o) { if (o) memset(o, 0, sizeof(*o)); }
extern "C" esp_err_t ls_mesh_advertise(void) { return ESP_ERR_NOT_SUPPORTED; }

extern "C" int ls_mesh_peers(ls_mesh_peer_t *o, int m) { (void)o; (void)m; return 0; }
extern "C" esp_err_t ls_mesh_add_contact(const char *h, const char *n)
{ (void)h; (void)n; return ESP_ERR_NOT_SUPPORTED; }
extern "C" int ls_mesh_messages(ls_mesh_msg_t *o, int m) { (void)o; (void)m; return 0; }
extern "C" int ls_mesh_scope(float *o, int m) { (void)o; (void)m; return 0; }
extern "C" int ls_mesh_channels(ls_mesh_chan_t *o, int m) { (void)o; (void)m; return 0; }
extern "C" int ls_mesh_channel_active(void) { return 0; }
extern "C" esp_err_t ls_mesh_set_channel_active(int i) { (void)i; return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t ls_mesh_set_channel(int i, const char *n, const char *p)
{ (void)i; (void)n; (void)p; return ESP_ERR_NOT_SUPPORTED; }
extern "C" const char *ls_mesh_band_name(int b) { (void)b; return "-"; }
extern "C" int ls_mesh_band_current(void) { return 0; }
extern "C" esp_err_t ls_mesh_set_band(int b) { (void)b; return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool ls_mesh_radio_stored(void) { return false; }
extern "C" esp_err_t ls_mesh_radio_default(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool ls_mesh_auto_listen(void) { return false; }
extern "C" bool ls_mesh_auto_tx(void) { return false; }
extern "C" esp_err_t ls_mesh_set_auto(bool l, bool t) { (void)l; (void)t; return ESP_ERR_NOT_SUPPORTED; }
extern "C" void ls_mesh_boot(void) {}
extern "C" uint32_t ls_mesh_msg_seq(void) { return 0; }
extern "C" const char *ls_mesh_name(void) { return "LakeShark"; }
extern "C" esp_err_t ls_mesh_set_name(const char *n) { (void)n; return ESP_ERR_NOT_SUPPORTED; }
extern "C" void ls_mesh_get_radio(ls_mesh_radio_t *o) { if (o) memset(o, 0, sizeof(*o)); }
extern "C" esp_err_t ls_mesh_set_radio(const ls_mesh_radio_t *c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t ls_mesh_send_text(const char *t) { (void)t; return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t ls_mesh_send_dm(int i, const char *t) { (void)i; (void)t; return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t ls_mesh_send_dm_id(const char *i, const char *t)
{ (void)i; (void)t; return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool ls_mesh_peer_known(const char *i) { (void)i; return false; }
extern "C" int ls_mesh_forget_peer(const char *i) { (void)i; return 0; }
extern "C" int ls_mesh_events(ls_mesh_event_t *o, int m) { (void)o; (void)m; return 0; }
extern "C" uint32_t ls_mesh_event_seq(void) { return 0; }
extern "C" void ls_mesh_diagnostics(void) { printf("mesh: board has no LoRa radio\n"); }

extern "C" uint32_t ls_mesh_now(void) { return 0; }
extern "C" bool ls_mesh_peer_at(int rank, ls_mesh_peer_t *out)
{ (void)rank; (void)out; return false; }
extern "C" bool ls_mesh_radio_hold(bool on) { (void)on; return false; }
extern "C" bool ls_mesh_radio_held(void) { return false; }
extern "C" int  ls_mesh_sightings(void) { return 0; }
extern "C" bool ls_mesh_sight_clear(void) { return false; }

#endif
