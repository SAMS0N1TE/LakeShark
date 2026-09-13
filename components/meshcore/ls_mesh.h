

#ifndef LS_MESH_H
#define LS_MESH_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     running;
    bool     tx_enabled;
    bool     radio_ready;
    uint32_t rx_packets;      /* raw frames the radio handed up            */
    uint32_t rx_bad;          /* CRC or header errors                      */
    uint32_t tx_packets;
    uint32_t loops;           /* Dispatcher::loop() calls - liveness       */
    float    last_rssi;
    float    last_snr;
    uint32_t airtime_ms;      /* cumulative, from the Dispatcher           */
    /* What is left of the transmit budget the Dispatcher enforces.
       A duty cycle is a legal limit in most of the world and a courtesy
       everywhere else; a mesh tool that cannot show how much of it is spent
       is one you will exceed without noticing. */
    uint32_t tx_budget_ms;
    char     self_id[17];     /* first 8 bytes of our public key, hex      */
} ls_mesh_stats_t;

/* Start the background task. Brings the radio up at MeshCore's US915
   settings, loads or creates this node's identity in NVS, and begins
   listening. Safe to call more than once. */
esp_err_t ls_mesh_start(void);
void      ls_mesh_stop(void);
bool      ls_mesh_running(void);

/* Arm or disarm transmit. Off at boot, always. */
void ls_mesh_set_tx(bool enabled);
bool ls_mesh_tx_enabled(void);

void ls_mesh_get_stats(ls_mesh_stats_t *out);

/* Send a self-advert, the one outbound MeshCore action worth exposing before
   the higher layers are wired. Refused when transmit is disarmed. */
esp_err_t ls_mesh_advertise(void);

#define LS_MESH_MAX_PEERS   12
#define LS_MESH_MAX_EVENTS  24

#define LS_MESH_PEER_NAME 20

typedef struct {
    char     id[17];        /* first 8 bytes of the peer's key, hex      */
    uint8_t  pub_key[32];   /* the whole thing - needed to address them  */
    char     name[LS_MESH_PEER_NAME];  /* from the advert, may be empty  */
    uint8_t  type;          /* ADV_TYPE_*: chat, repeater, room, sensor  */
    bool     has_loc;
    int32_t  lat_e6, lon_e6;
    uint32_t first_heard;   /* epoch, or uptime seconds if no clock      */
    uint32_t last_heard;
    uint32_t adverts;
    float    rssi;
    float    snr;

    int32_t  logged_lat_e7, logged_lon_e7;
    bool     logged_any;
} ls_mesh_peer_t;

/* What we advertise ourselves as. Chat is a normal node; repeater says "I
   will relay for you", and claiming it without relaying is a lie other nodes
   will route around, so setting it turns forwarding on. */
#define LS_MESH_ROLE_CHAT     1
#define LS_MESH_ROLE_REPEATER 2
#define LS_MESH_ROLE_ROOM     3
#define LS_MESH_ROLE_SENSOR   4

typedef enum {
    LS_MESH_EV_NONE = 0,
    LS_MESH_EV_ADVERT,      /* an advert arrived and its signature verified */
    LS_MESH_EV_RX,          /* a frame arrived                             */
    LS_MESH_EV_TX,          /* we transmitted                              */
    LS_MESH_EV_ERR,         /* CRC or header error                         */
} ls_mesh_ev_kind_t;

typedef struct {
    uint32_t          t;      /* epoch, or uptime seconds if no clock */
    ls_mesh_ev_kind_t kind;
    char              id[17]; /* peer, when known; empty otherwise    */
    float             rssi;
    float             snr;
    uint16_t          len;
} ls_mesh_event_t;

/* Peers, most recently heard first. Returns how many were written.

   `out` must have room for `max` entries and the caller owns that
   memory. On a small stack, DO NOT ask for LS_MESH_MAX_PEERS - the array is
   about 1.1 KB and the console task has under 4 KB. Use ls_mesh_peer_at()
   for one at a time, or a static buffer as the MESH screen does. Two stack
   overflows in this project came from exactly this call. */
int ls_mesh_peers(ls_mesh_peer_t *out, int max);

/* Add a contact by its public key, without having heard it. */

esp_err_t ls_mesh_add_contact(const char *hex, const char *name);

/* One peer by rank, most recently heard first. The safe way to walk the
   list from a small stack: one struct, not twelve. False when `rank` is
   past the end. */
bool ls_mesh_peer_at(int rank, ls_mesh_peer_t *out);

/* The event ring, newest first. Returns how many were written. */
int ls_mesh_events(ls_mesh_event_t *out, int max);

/* Monotonic counter of accepted events. A screen compares it against the
   value it last drew to know whether anything happened, which is what drives
   the activity animation without it having to diff the whole ring. */
/* The clock the peer and event stamps are taken against. */

uint32_t ls_mesh_now(void);

uint32_t ls_mesh_event_seq(void);

#define LS_MESH_MAX_MSGS 24
#define LS_MESH_MSG_LEN  84

/* Delivery state, and what it honestly means. */

typedef enum {
    LS_MSG_IN = 0,      /* somebody else's                              */
    LS_MSG_SENDING,     /* ours, still on the air                       */
    LS_MSG_SENT,        /* ours, transmission complete                  */
    LS_MSG_HEARD,       /* ours, and a neighbour relayed it back to us  */
    /* Only a direct message can reach this. It means the recipient
       itself answered with a matching ACK - not that somebody relayed it. */
    LS_MSG_ACKED,
} ls_mesh_msg_state_t;

typedef struct {
    uint32_t t;                     /* epoch, or uptime seconds          */
    bool     mine;                  /* we sent it                        */
    bool     direct;                /* a DM, not the public channel      */
    char     peer[17];              /* the other end of a DM, hex id     */
    ls_mesh_msg_state_t state;
    uint8_t  relays;                /* how many times we heard it back   */
    char     text[LS_MESH_MSG_LEN]; /* "name: message", as it goes on air */
} ls_mesh_msg_t;

esp_err_t ls_mesh_send_dm_id(const char *id, const char *text);
/* Bounded background outbox, serviced by the Mesh task; expires after 5 s.
   Acceptance is not proof of radio transmission or a recipient ACK. */
bool ls_mesh_queue_dm(const char *id, const char *text);

/* Is this node in the table - that is, can it be addressed at all? */
bool ls_mesh_peer_known(const char *id);

/* Remove a node from the table, and from the card. */

int ls_mesh_forget_peer(const char *id);

bool ls_mesh_radio_hold(bool on);
bool ls_mesh_radio_held(void);

int ls_mesh_sightings(void);
bool ls_mesh_sight_clear(void);

/* Oldest first - a chat reads downward. Returns how many were written. */
int ls_mesh_messages(ls_mesh_msg_t *out, int max);

/* Changes when a message arrives or is sent, so a screen can tell whether to
   scroll without diffing the ring. */
uint32_t ls_mesh_msg_seq(void);

/* ---- direct messages --------------------------------------------------- */

/* Index is into the list ls_mesh_peers() returns. ESP_ERR_NOT_FOUND when
   that peer is not known, ESP_ERR_NOT_ALLOWED when transmit is disarmed. */
esp_err_t ls_mesh_send_dm(int peer_index, const char *text);

/* Send on the public channel. ESP_ERR_NOT_ALLOWED when transmit is disarmed
   - the same refusal ls_mesh_advertise gives, for the same reason. */
esp_err_t ls_mesh_send_text(const char *text);

/* The name that prefixes every message and identifies this node to peers.
   Bounded, and sanitised - a name with a colon in it would break the
   "name: message" framing that MeshCore's own parser relies on. */
#define LS_MESH_NAME_MAX 16
const char *ls_mesh_name(void);
esp_err_t   ls_mesh_set_name(const char *name);

/* Radio parameters. Applied immediately, which restarts receive; a node that
   changes frequency is a node that stops hearing its old neighbours, and
   that is the caller's business to understand. Persisted. */
/* Everything here changes what goes on the air and every one of them
   is wired to something real. Nothing is listed because it sounded like a
   setting: a row that does nothing is the TRUNK MONITOR failure with a
   shorter feedback loop.

   Deliberately NOT here, because there is no implementation behind them:
   extra channels, direct-message keys, hop limits (MAX_PATH_SIZE is a
   compile-time constant upstream) and anything to do with contacts. */
typedef struct {
    uint32_t freq_hz;
    uint8_t  sf;
    uint32_t bw_hz;
    int8_t   power_dbm;
    uint8_t  cr;            /* 5..8, meaning 4/5 .. 4/8                    */
    uint8_t  sync_word;     /* 0x12 private mesh, 0x34 public LoRaWAN      */
    uint8_t  crc_on;        /* frames carry a CRC                          */
    uint16_t advert_secs;   /* periodic self-advert, 0 = never             */
    uint8_t  advert_boot;   /* advertise once when the stack starts        */
    /* What goes IN the advert, and what we do for the mesh. */
    uint8_t  role;          /* LS_MESH_ROLE_*, 0 = chat                    */
    uint8_t  repeat;        /* relay flood packets for other nodes         */

    uint8_t  share_loc;
    int32_t  lat_e6, lon_e6;/* degrees x 1e6, as MeshCore encodes them     */
} ls_mesh_radio_t;

/* Band presets. A frequency typed digit by digit is a frequency
   typed wrong, and the image-calibration band has to move with it or the
   receiver loses several dB without saying so. */
typedef enum {
    LS_MESH_BAND_US915 = 0,   /* 902-928, MeshCore default 910.525 */
    LS_MESH_BAND_EU868,       /* 863-870, MeshCore default 869.618 */
    LS_MESH_BAND__COUNT
} ls_mesh_band_t;

const char *ls_mesh_band_name(int band);
int         ls_mesh_band_current(void);
esp_err_t   ls_mesh_set_band(int band);

void      ls_mesh_get_radio(ls_mesh_radio_t *out);
esp_err_t ls_mesh_set_radio(const ls_mesh_radio_t *cfg);

bool      ls_mesh_radio_stored(void);
esp_err_t ls_mesh_radio_default(void);

#define LS_MESH_MAX_CHANNELS 4
#define LS_MESH_CHAN_NAME    16
#define LS_MESH_CHAN_PSK     48   /* base64 of 32 bytes is 44 + NUL */

typedef struct {
    char name[LS_MESH_CHAN_NAME];
    char psk[LS_MESH_CHAN_PSK];   /* base64, as entered; empty = unused */
    bool fixed;                   /* slot 0: the public channel          */
} ls_mesh_chan_t;

int       ls_mesh_channels(ls_mesh_chan_t *out, int max);
int       ls_mesh_channel_active(void);
esp_err_t ls_mesh_set_channel_active(int idx);
/* name or psk may be NULL to leave that half alone. An empty psk clears the
   slot. Returns ESP_ERR_INVALID_ARG when the base64 does not decode to 16
   or 32 bytes, which is the only length MeshCore accepts. */
esp_err_t ls_mesh_set_channel(int idx, const char *name, const char *psk_b64);

bool      ls_mesh_auto_listen(void);
bool      ls_mesh_auto_tx(void);
esp_err_t ls_mesh_set_auto(bool listen, bool tx);

/* Called once at boot. Starts the stack when auto listen is set, and arms
   transmit when auto transmit is also set. Does nothing otherwise. */
void ls_mesh_boot(void);

/* ---- channel scope ----------------------------------------------------- */

#define LS_MESH_SCOPE_N 96

/* Copies up to `max` samples, oldest first, into `out` as dBm. Returns how
   many were written. A slot never sampled reads 0. */
int ls_mesh_scope(float *out, int max);

/* Console helper. */
void ls_mesh_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_MESH_H */
