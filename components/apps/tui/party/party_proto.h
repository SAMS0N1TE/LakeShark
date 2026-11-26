/* PARTY's wire format: what one LakeShark says to the others in its party.

   Every datagram is small (a LoRa payload is at most 255 bytes, and the link
   adds its own nonce and tag on top), versioned, little endian, and starts
   with the same six bytes:

     byte 0   version (top 3 bits, PARTY_PROTO_VER) | type (low 5 bits)
     byte 1-2 party id: 16 bits of the key derived from the party code, so a
              board drops another party's traffic before decoding it
     byte 3   sender: a member number picked at random when the party starts
     byte 4   receiver: a member number, or PARTY_ALL
     byte 5   sequence: counts up per sender, for loss and duplicates

   then the body of the type:

     HELLO  (1) who I am: call sign, glyph, colour, what my radio can do
                (PARTY_CAP_*), how I share my position, which classes I want,
                battery. Sent on joining (three times) and every minute.
                call[8] glyph colour caps share want:u16 batt flags        16 B
     POS    (2) where I am: lat/lon in 1e-7 degrees (exact) or the centre of
                a 0.01 degree square (rough, flag PARTY_POS_ROUGH), accuracy
                in metres, speed in 0.5 m/s, course and compass heading in
                1/256 turns, battery, flags.                                15 B
     PING   (3) a detection: the sender's own number for it (a repeat has the
                same id), its class (PARTY_C_*), WARDRIVE's kind and WD_T_*
                tags, RSSI, where it was (lat/lon) with an uncertainty radius,
                how many seconds before sending it was heard, the device
                address (all zero when it has none, for vehicles the TPMS ID
                or MAC that lets a second member say "the same car"), and a
                label of up to 24 bytes.                   27 + label (<= 51 B)
     OFFER  (4) a file: id, size, CRC-32, chunk size, name (up to 32).
     CHUNK  (5) a piece of a file: id, index, bytes.
     ACK    (6) to the sender of a file: id, first missing chunk, a bitmap of
                the 32 chunks from there (bit set = have it), flags (DONE,
                REFUSED).
     BYE    (7) leaving the party.

   Nothing here encrypts: the link (ls_lsx) does, with the key from
   party_key_derive. */
#ifndef PARTY_PROTO_H
#define PARTY_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARTY_PROTO_VER   1
#define PARTY_HDR         6
#define PARTY_DGRAM_MAX   200      /* what PARTY ever hands the link; the link's MTU may be lower */
#define PARTY_ALL         0xFF
#define PARTY_CALL_MAX    8
#define PARTY_LABEL_MAX   24
#define PARTY_FNAME_MAX   32
#define PARTY_CHANNELS    8        /* the link maps a channel number to its frequencies */

enum {
    PARTY_HELLO = 1, PARTY_POS, PARTY_PING, PARTY_OFFER, PARTY_CHUNK, PARTY_ACK, PARTY_BYE,
};

/* What a member's radio and build can do (HELLO caps). */
#define PARTY_CAP_LORA    0x01
#define PARTY_CAP_GFSK    0x02
#define PARTY_CAP_FLRC    0x04     /* an LR2021 */
#define PARTY_CAP_FILES   0x08
#define PARTY_CAP_VOICE   0x10
#define PARTY_CAP_COMPASS 0x20

/* How a member shares its position. */
enum { PARTY_SHARE_OFF, PARTY_SHARE_ROUGH, PARTY_SHARE_EXACT, PARTY_SHARES };

/* POS flags. */
#define PARTY_POS_ROUGH    0x01
#define PARTY_POS_MOVING   0x02
#define PARTY_POS_VEHICLE  0x04
#define PARTY_POS_COURSE   0x08    /* course is good */
#define PARTY_POS_HEADING  0x10    /* compass heading is good */
#define PARTY_POS_HELP     0x20    /* asked for help and has not cleared it */

/* PING flags. */
#define PARTY_PF_PLACED    0x01    /* lat/lon is where it was strongest, not where the sender is */
#define PARTY_PF_MANUAL    0x02    /* the member marked it by hand */
#define PARTY_PF_ROUGH     0x04    /* the place was coarsened (share ROUGH) */
#define PARTY_PF_REPEAT    0x08    /* a resend of an id already sent */

/* ACK flags. */
#define PARTY_ACK_DONE     0x01
#define PARTY_ACK_REFUSED  0x02

/* Detection classes, one bit each in a 16-bit mask. On the wire: new ones
   replace nothing, the numbers are fixed. */
enum {
    PARTY_C_MARK,      /* marked by hand: "look here" */
    PARTY_C_ALPR,      /* Flock and other plate readers */
    PARTY_C_CAMERA,
    PARTY_C_BODYCAM,
    PARTY_C_POLICE,
    PARTY_C_DRONE,
    PARTY_C_TRACKER,
    PARTY_C_VEHICLE,
    PARTY_C_FOLLOW,    /* moving with the member who saw it */
    PARTY_C_SKIMMER,
    PARTY_C_HACKTOOL,
    PARTY_C_WEARABLE,
    PARTY_C_INFRA,
    PARTY_C_WATCH,     /* on the sender's own watch list */
    PARTY_C_CUSTOM,    /* a name matched one of the profile's keywords */
    PARTY_C_HELP,      /* "come to me" */
    PARTY_CLASSES
};
#define PARTY_CMASK(c) ((uint16_t)(1u << (c)))

/* Upper-case word and three-letter tag of a class. */
const char *party_class_name(int c);
const char *party_class_tag(int c);
/* The class a WARDRIVE find belongs to: its WD_T_* tags first, by priority,
   then a keyword match on its name (CUSTOM). -1 for none. `kw` is up to
   `nkw` keywords, empty ones skipped, matched case-insensitively anywhere in
   the name. */
int party_class_of(uint16_t wd_tags, const char *name, const char (*kw)[13], int nkw);

typedef struct {
    uint8_t type, src, dst, seq;
    uint16_t party;
} party_hdr_t;

typedef struct {
    char call[PARTY_CALL_MAX + 1];
    uint8_t glyph, colour, caps, share, batt, flags;
    uint16_t want;
} party_hello_t;

typedef struct {
    int32_t lat_e7, lon_e7;
    uint16_t acc_m;
    uint8_t speed_half;        /* 0.5 m/s units, 255 = 127.5 or more */
    uint8_t course, heading;   /* 1/256 turns */
    uint8_t batt;              /* percent, 255 unknown */
    uint8_t flags;             /* PARTY_POS_* */
} party_pos_t;

typedef struct {
    uint16_t id;
    uint8_t cls, kind;
    uint16_t tags;
    int8_t rssi;
    uint8_t flags;             /* PARTY_PF_* */
    int32_t lat_e7, lon_e7;
    uint16_t unc_m;
    uint16_t age_s;
    uint8_t addr[6];
    char label[PARTY_LABEL_MAX + 1];
} party_ping_t;

typedef struct {
    uint16_t fid;
    uint32_t size, crc;
    uint8_t chunk;             /* bytes per chunk, all but the last */
    char name[PARTY_FNAME_MAX + 1];
} party_offer_t;

typedef struct {
    uint16_t fid, idx;
    uint8_t len;
    const uint8_t *data;       /* points into the decoded datagram */
} party_chunk_t;

typedef struct {
    uint16_t fid, base;
    uint32_t bits;             /* bit i: chunk base + i is here */
    uint8_t flags;
} party_ack_t;

typedef union {
    party_hello_t hello;
    party_pos_t pos;
    party_ping_t ping;
    party_offer_t offer;
    party_chunk_t chunk;
    party_ack_t ack;
} party_msg_t;

/* Encoders: the datagram's length, or -1 when it does not fit `cap`.
   h->type is set by each encoder. */
int party_enc_hello(uint8_t *out, int cap, party_hdr_t *h, const party_hello_t *m);
int party_enc_pos(uint8_t *out, int cap, party_hdr_t *h, const party_pos_t *m);
int party_enc_ping(uint8_t *out, int cap, party_hdr_t *h, const party_ping_t *m);
int party_enc_offer(uint8_t *out, int cap, party_hdr_t *h, const party_offer_t *m);
int party_enc_chunk(uint8_t *out, int cap, party_hdr_t *h, const party_chunk_t *m);
int party_enc_ack(uint8_t *out, int cap, party_hdr_t *h, const party_ack_t *m);
int party_enc_bye(uint8_t *out, int cap, party_hdr_t *h);

/* Decode one datagram: its type, or a negative PARTY_E_*. */
enum { PARTY_E_SHORT = -1, PARTY_E_VERSION = -2, PARTY_E_TYPE = -3, PARTY_E_PARTY = -4, PARTY_E_BODY = -5 };
int party_decode(const uint8_t *d, int n, uint16_t party, party_hdr_t *h, party_msg_t *m);

/* Positions on the wire. */
int32_t party_deg_e7(double deg);
double  party_e7_deg(int32_t e7);
/* The centre of the 0.01 degree square a place is in (about 1.1 km north to
   south), for a rough share. */
double  party_rough(double deg, double step);

/* ---- the party code ----
   A code is what members agree on out loud ("fox den 7"). Case, spaces and
   punctuation do not count: "FOX-DEN 7" is the same party. The key is 2048
   rounds of SHA-256 over a fixed salt and the normalised code; the party id
   and the channel come from the same digest, so the same code always finds
   the same party and a different one almost never shares its id. */
typedef struct {
    uint8_t key[16];
    uint16_t party;
    uint8_t channel;           /* 0..PARTY_CHANNELS-1 */
} party_key_t;
void party_code_norm(const char *code, char *out, size_t cap);
/* False for a code with fewer than 4 letters or digits once normalised. */
bool party_key_derive(const char *code, party_key_t *out);

/* ---- files ----
   The sender cuts a file into chunks of `chunk` bytes; a receiver keeps a
   bitmap of what it has and answers with ACKs; the sender resends what any
   receiver is missing. */
#define PARTY_FILE_MAX     (64 * 1024)
#define PARTY_FILE_CHUNKS  1024    /* PARTY_FILE_MAX at the smallest chunk, 64 */

typedef struct {
    uint16_t fid;
    uint32_t size, crc;
    uint8_t chunk;
    uint16_t nchunks, got;
    uint8_t bits[PARTY_FILE_CHUNKS / 8];
} party_frx_t;
/* False when the offer cannot be taken (too big, chunk too small). */
bool party_frx_begin(party_frx_t *r, const party_offer_t *o);
/* 1 a new chunk (write c->data at *offset), 0 a duplicate, -1 not this file
   or out of range. */
int  party_frx_put(party_frx_t *r, const party_chunk_t *c, uint32_t *offset);
bool party_frx_complete(const party_frx_t *r);
void party_frx_ack(const party_frx_t *r, party_ack_t *a);

typedef struct {
    uint16_t fid;
    uint32_t size;
    uint8_t chunk;
    uint16_t nchunks;
    uint16_t cursor;           /* next chunk to look at */
    uint8_t need[PARTY_FILE_CHUNKS / 8];   /* still to send */
} party_ftx_t;
void party_ftx_begin(party_ftx_t *t, uint16_t fid, uint32_t size, uint8_t chunk);
/* The next chunk to send, or -1 when nothing is waiting. */
int  party_ftx_next(party_ftx_t *t);
/* A receiver's ACK: what it is missing in the window goes back in `need`. */
void party_ftx_ack(party_ftx_t *t, const party_ack_t *a);
int  party_ftx_pending(const party_ftx_t *t);
uint8_t party_chunk_size(int mtu);

uint32_t party_crc32(uint32_t crc, const void *p, size_t n);
void party_sha256(const void *p, size_t n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
