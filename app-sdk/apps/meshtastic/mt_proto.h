/* The Meshtastic over-the-air format, from the published protocol description:
   a 16-byte little-endian header, then an AES-CTR encrypted Data protobuf.
   Only the parts a chat client needs: text, node info, position. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_BROADCAST     0xFFFFFFFFu
#define MT_HEADER_LEN    16
#define MT_MAX_PAYLOAD   239
#define MT_SYNC_WORD     0x2B
#define MT_PREAMBLE      16

/* Data.portnum values */
enum {
    MT_PORT_TEXT = 1, MT_PORT_POSITION = 3, MT_PORT_NODEINFO = 4, MT_PORT_ROUTING = 5,
    MT_PORT_TELEMETRY = 67, MT_PORT_TRACEROUTE = 70, MT_PORT_NEIGHBORINFO = 71,
};

typedef struct { const char *name; uint32_t lo_khz, hi_khz; } mt_region_t;
typedef struct { const char *name; uint32_t bw_hz; uint8_t sf, cr; } mt_preset_t;

extern const mt_region_t MT_REGIONS[];
extern const int MT_REGION_COUNT;
extern const mt_preset_t MT_PRESETS[];
extern const int MT_PRESET_COUNT;

/* The default frequency for a region, preset and channel name: djb2 of the
   name (the preset's name when the channel has none) picks the slot. */
uint64_t mt_frequency_hz(int region, int preset, const char *channel_name, int *slot, int *slots);

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

/* Data protobuf: portnum + payload (+ want_response). */
typedef struct {
    uint32_t portnum;
    const uint8_t *payload;
    size_t payload_len;
    bool want_response;
    uint32_t request_id;    /* a reply (ROUTING ack, position answer) names the packet it answers */
} mt_data_t;
int  mt_data_encode(const mt_data_t *d, uint8_t *out, size_t cap);
bool mt_data_decode(const uint8_t *in, size_t n, mt_data_t *d);

/* User (NODEINFO payload). */
typedef struct {
    char id[16], long_name[40], short_name[8];
    uint32_t hw_model, role;
    uint8_t public_key[32];  /* field 8: the X25519 key for direct messages */
    bool has_key;
} mt_user_t;
int  mt_user_encode(const mt_user_t *u, const uint8_t mac[6], uint8_t *out, size_t cap);
bool mt_user_decode(const uint8_t *in, size_t n, mt_user_t *u);

/* Position (POSITION payload): degrees, metres, unix time; has_* false when absent. */
typedef struct { double lat, lon; int32_t alt; uint32_t time; bool has_pos, has_alt; } mt_position_t;
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

const char *mt_port_name(uint32_t port);
