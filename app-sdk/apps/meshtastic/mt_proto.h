/* The Meshtastic over-the-air format: a 16-byte little-endian header, then an
   AES-CTR encrypted Data protobuf, and the messages this app reads and sends.

   Source-derived. Field numbers, enum values and defaults follow Meshtastic's
   protobufs (github.com/meshtastic/protobufs: mesh.proto, portnums.proto,
   config.proto, channel.proto, apponly.proto, storeforward.proto) and the
   behaviour of its firmware (github.com/meshtastic/firmware): the slot rule
   and "Custom"/"LongMod" names of RadioInterface.cpp, Channels.cpp and
   DisplayFormatters.cpp, the range test, neighbour info and store-and-forward
   modules. Both are Copyright (C) Meshtastic LLC and contributors, GPL-3.0;
   this file is GPL-3.0 as part of LakeShark. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_BROADCAST     0xFFFFFFFFu
#define MT_HEADER_LEN    16
#define MT_MAX_PAYLOAD   239        /* 255 on air, less the header */
#define MT_DATA_PAYLOAD  233        /* Data.payload's own cap (mesh.options) */
#define MT_SYNC_WORD     0x2B
#define MT_PREAMBLE      16

/* PortNum (portnums.proto) */
enum {
    MT_PORT_TEXT = 1, MT_PORT_POSITION = 3, MT_PORT_NODEINFO = 4, MT_PORT_ROUTING = 5, MT_PORT_ADMIN = 6,
    MT_PORT_WAYPOINT = 8, MT_PORT_STORE_FORWARD = 65, MT_PORT_RANGE_TEST = 66,
    MT_PORT_TELEMETRY = 67, MT_PORT_TRACEROUTE = 70, MT_PORT_NEIGHBORINFO = 71,
};

/* Config.DeviceConfig.Role and RebroadcastMode (config.proto) */
enum {
    MT_ROLE_CLIENT = 0, MT_ROLE_CLIENT_MUTE = 1, MT_ROLE_ROUTER = 2, MT_ROLE_REPEATER = 4, MT_ROLE_TRACKER = 5,
    MT_ROLE_SENSOR = 6, MT_ROLE_TAK = 7, MT_ROLE_CLIENT_HIDDEN = 8, MT_ROLE_LOST_AND_FOUND = 9,
    MT_ROLE_TAK_TRACKER = 10, MT_ROLE_ROUTER_LATE = 11, MT_ROLE_CLIENT_BASE = 12,
};
enum { MT_REBC_ALL = 0, MT_REBC_ALL_SKIP_DECODING, MT_REBC_LOCAL_ONLY, MT_REBC_KNOWN_ONLY, MT_REBC_NONE,
       MT_REBC_CORE_PORTNUMS_ONLY };
const char *mt_role_name(uint32_t role);        /* "CLIENT_MUTE", or "ROLE n" */
const char *mt_hw_name(uint32_t model);         /* HardwareModel (mesh.proto), NULL when unknown */

/* One region as Meshtastic defines it: band, duty cycle (% of an hour, 100 =
   none), power limit, wide = 2.4 GHz LoRa (bandwidths x3.25), and its
   RegionCode. The array keeps the order older versions of this app saved. */
typedef struct { const char *name; uint32_t lo_khz, hi_khz; uint8_t duty, power_dbm, wide, code; } mt_region_t;
/* A modem preset: its display name (the channel name when a channel has
   none), bandwidth, SF, CR and ModemPreset value. */
typedef struct { const char *name; uint32_t bw_hz; uint8_t sf, cr, code; } mt_preset_t;

extern const mt_region_t MT_REGIONS[];
extern const int MT_REGION_COUNT;
extern const mt_preset_t MT_PRESETS[];
extern const int MT_PRESET_COUNT;
int mt_region_by_code(uint32_t code);           /* index into MT_REGIONS, -1 */
int mt_preset_by_code(uint32_t code);           /* index into MT_PRESETS, -1 */

/* The LoRa settings that pick the frequency. custom = use_preset off: then
   bw_code (MT_BW_*), sf and cr stand and the name of an unnamed channel is
   "Custom". slot 0 = the name's djb2 picks it, else 1..N (channel_num).
   offset_hz is added to the slot's centre (frequency_offset). */
enum { MT_BW_31, MT_BW_62, MT_BW_125, MT_BW_250, MT_BW_500, MT_BW_COUNT };
typedef struct {
    uint8_t region, preset, custom, bw_code, sf, cr;
    uint16_t slot;
    int32_t offset_hz;
} mt_lora_t;
uint32_t mt_lora_bw_hz(const mt_lora_t *l);
void     mt_lora_sf_cr(const mt_lora_t *l, uint8_t *sf, uint8_t *cr);
const char *mt_lora_name(const mt_lora_t *l);   /* the preset's name, or "Custom" */
/* Frequency for these settings and the primary channel's name (empty: the
   preset's name). 0 when the bandwidth is wider than the band. */
uint64_t mt_lora_frequency(const mt_lora_t *l, const char *channel_name, int *slot, int *slots);

/* The default frequency for a region, preset and channel name: djb2 of the
   name (the preset's name when the channel has none) picks the slot. */
uint64_t mt_frequency_hz(int region, int preset, const char *channel_name, int *slot, int *slots);
/* LoRa bandwidth of a preset in a region (wide regions: x3.25). */
uint32_t mt_bw_hz(int region, int preset);
/* Region for a position, 0 = none known (a coarse guess from the box a place is in). */
int mt_region_guess(double lat, double lon);

/* Channel key from a PSK: 0 bytes = no crypto, 1 byte N = the published default
   key with its last byte moved by N-1 (N=1 is the default "AQ=="), 16/32 = as is. */
int  mt_channel_key(const uint8_t *psk, int psk_len, uint8_t key[32]);
uint8_t mt_channel_hash(const char *name, const uint8_t *key, int key_len);

typedef struct {
    uint32_t dest, from, id;
    uint8_t hop_limit, hop_start, want_ack, via_mqtt;
    uint8_t channel, next_hop, relay;
} mt_header_t;

void mt_header_pack(const mt_header_t *h, uint8_t out[MT_HEADER_LEN]);
bool mt_header_unpack(const uint8_t *in, size_t n, mt_header_t *h);
/* AES-CTR in place with the sender and packet id nonce. key_len 0 = clear. */
void mt_crypt(const uint8_t *key, int key_len, uint32_t from, uint32_t id, uint8_t *buf, size_t n);

/* Data protobuf: portnum, payload, want_response, request_id, reply_id and
   emoji (a tapback: the payload is the emoji, reply_id the message it is on). */
typedef struct {
    uint32_t portnum;
    const uint8_t *payload;
    size_t payload_len;
    bool want_response;
    uint32_t request_id;    /* a reply (ROUTING ack, position answer) names the packet it answers */
    uint32_t reply_id;      /* a text that answers another */
    uint32_t emoji;         /* nonzero: the payload is a reaction */
} mt_data_t;
int  mt_data_encode(const mt_data_t *d, uint8_t *out, size_t cap);
bool mt_data_decode(const uint8_t *in, size_t n, mt_data_t *d);
/* The most text bytes one packet carries: room in a frame (less the PKI
   overhead for a private DM) after the Data fields, at most MT_DATA_PAYLOAD. */
int  mt_text_room(bool reply, bool emoji, int crypto_overhead);

/* User (NODEINFO payload). */
typedef struct {
    char id[16], long_name[40], short_name[8];
    uint32_t hw_model, role;
    uint8_t public_key[32];  /* field 8: the X25519 key for direct messages */
    bool has_key, licensed, unmessagable;
} mt_user_t;
int  mt_user_encode(const mt_user_t *u, const uint8_t mac[6], uint8_t *out, size_t cap);
bool mt_user_decode(const uint8_t *in, size_t n, mt_user_t *u);

/* Position (POSITION payload): degrees, metres, unix time; has_* false when absent. */
typedef struct { double lat, lon; int32_t alt; uint32_t time; bool has_pos, has_alt; uint8_t precision; } mt_position_t;
bool mt_position_decode(const uint8_t *in, size_t n, mt_position_t *p);
/* precision_bits 32 = exact; fewer keeps only the top bits (a coarser spot). */
int  mt_position_encode(double lat, double lon, int32_t alt, uint32_t time, int precision_bits,
                        uint8_t *out, size_t cap);

/* Telemetry: device metrics only. has false when the packet carries none. */
typedef struct { uint32_t battery, uptime; float voltage, ch_util, air_util; bool has; } mt_device_t;
bool mt_telemetry_decode(const uint8_t *in, size_t n, mt_device_t *d);
/* This node's device metrics; battery 101 = on external power, 0 = unknown (left out). */
int  mt_telemetry_encode(uint32_t time, const mt_device_t *d, uint8_t *out, size_t cap);

/* RouteDiscovery (TRACEROUTE payload): the nodes a trace passed each way and
   the SNR each hop heard it at, in quarter dB (-128 = not known). */
#define MT_ROUTE_MAX 8
#define MT_SNR_UNKNOWN (-128)
typedef struct {
    uint32_t route[MT_ROUTE_MAX], back[MT_ROUTE_MAX];
    int8_t snr_to[MT_ROUTE_MAX + 1], snr_back[MT_ROUTE_MAX + 1];
    uint8_t n_route, n_back, n_snr_to, n_snr_back;
} mt_route_t;
int  mt_route_encode(const mt_route_t *r, uint8_t *out, size_t cap);
bool mt_route_decode(const uint8_t *in, size_t n, mt_route_t *r);

/* Routing: the error a delivery report carries (0 = delivered). */
bool mt_routing_decode(const uint8_t *in, size_t n, uint32_t *error);
int  mt_routing_ack(uint8_t *out, size_t cap);

/* Waypoint (WAYPOINT payload). expire: unix time it goes, 0 never. icon: a
   Unicode code point. */
typedef struct {
    uint32_t id, expire, locked_to, icon;
    double lat, lon;
    bool has_pos;
    char name[32], desc[64];
} mt_waypoint_t;
int  mt_waypoint_encode(const mt_waypoint_t *w, uint8_t *out, size_t cap);
bool mt_waypoint_decode(const uint8_t *in, size_t n, mt_waypoint_t *w);

/* NeighborInfo (NEIGHBORINFO payload): who a node hears directly, with SNR. */
#define MT_NBR_MAX 10
typedef struct {
    uint32_t node, last_sent_by, interval_s;
    uint32_t id[MT_NBR_MAX];
    float snr[MT_NBR_MAX];
    uint8_t n;
} mt_neighbors_t;
int  mt_neighbors_encode(const mt_neighbors_t *nb, uint8_t *out, size_t cap);
bool mt_neighbors_decode(const uint8_t *in, size_t n, mt_neighbors_t *nb);

/* StoreAndForward (STORE_FORWARD payload): the request a client sends and
   what a router answers. */
enum {
    MT_SF_ROUTER_ERROR = 1, MT_SF_ROUTER_HEARTBEAT = 2, MT_SF_ROUTER_PING = 3, MT_SF_ROUTER_PONG = 4,
    MT_SF_ROUTER_BUSY = 5, MT_SF_ROUTER_HISTORY = 6, MT_SF_ROUTER_STATS = 7, MT_SF_ROUTER_TEXT_DIRECT = 8,
    MT_SF_ROUTER_TEXT_BROADCAST = 9, MT_SF_CLIENT_ERROR = 64, MT_SF_CLIENT_HISTORY = 65, MT_SF_CLIENT_STATS = 66,
    MT_SF_CLIENT_PING = 67, MT_SF_CLIENT_PONG = 68, MT_SF_CLIENT_ABORT = 106,
};
typedef struct {
    uint32_t rr;
    uint32_t history_messages, window, period;  /* History: count, window; Heartbeat: period (s) */
    const uint8_t *text;                         /* ROUTER_TEXT_*: the stored text */
    size_t text_len;
} mt_sf_t;
/* A client's history request for the last `minutes` (0 = the router's default). */
int  mt_sf_history_request(uint32_t minutes, uint8_t *out, size_t cap);
bool mt_sf_decode(const uint8_t *in, size_t n, mt_sf_t *sf);

/* A channel set as the meshtastic.org/e/# link carries it (apponly.proto
   ChannelSet: ChannelSettings and the LoRa config). */
#define MT_SET_MAX 8
typedef struct {
    char name[12];
    uint8_t psk[32], psk_len, precision, muted, has_module;
} mt_chset_ch_t;
typedef struct {
    mt_chset_ch_t ch[MT_SET_MAX];
    int n;
    bool has_lora;
    mt_lora_t lora;         /* region < 0 code-wise: region_code 0 = unset */
    uint32_t region_code, hop_limit;
} mt_chset_t;
/* The link, https://meshtastic.org/e/#<base64url>. Returns its length, -1 when it does not fit. */
int  mt_chset_url(const mt_chset_t *s, char *out, int cap);
/* A link (or the part after #) back into a channel set; *add true for ?add=true. */
bool mt_chset_parse(const char *url, mt_chset_t *s, bool *add);
int  mt_chset_encode(const mt_chset_t *s, uint8_t *out, size_t cap);
bool mt_chset_decode(const uint8_t *in, size_t n, mt_chset_t *s);

const char *mt_port_name(uint32_t port);
