/* FT8/FT4 77-bit message source encoding: text to payload and back.

   Covers what the bands are full of: standard messages (i3=1, i3=2 /P),
   non-standard calls (i3=4), free text (0.0), DXpedition (0.1, decode) and
   telemetry (0.5, decode). Layout from the protocol paper (Franke, Somerville
   and Taylor, QEX 2020); bit packing cross-checked against ft8_lib (MIT).

   Payload: 10 bytes, 77 bits MSB first; the low 3 bits of byte 9 are zero.

   Hashed callsigns: a call heard in full is remembered with its 22-bit hash,
   so a later <...> reference (10, 12 or 22 bits) prints as the call. The
   table is bounded and owned by the caller. */
#ifndef DIGI_MSG_H
#define DIGI_MSG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIGI_CALL_MAX   14      /* "<PJ4/K1ABC>" plus NUL, with room       */
#define DIGI_TEXT_MAX   40      /* the longest printed message             */
#define DIGI_HASH_SLOTS 128

typedef struct {
    uint32_t h22[DIGI_HASH_SLOTS];
    char call[DIGI_HASH_SLOTS][12];
    int next, used;
} digi_hash_t;

typedef enum {
    DIGI_MSG_FREE, DIGI_MSG_DXPED, DIGI_MSG_EUVHF, DIGI_MSG_FIELDDAY,
    DIGI_MSG_TELEMETRY, DIGI_MSG_STD, DIGI_MSG_RTTY, DIGI_MSG_NONSTD,
    DIGI_MSG_WWROF, DIGI_MSG_UNKNOWN,
} digi_msg_type_t;

/* What a screen or a log wants out of a decode. Empty strings when absent. */
typedef struct {
    digi_msg_type_t type;
    bool cq;                    /* call_to is CQ or CQ xxx                 */
    char call_to[DIGI_CALL_MAX];
    char call_de[DIGI_CALL_MAX];
    char extra[12];             /* grid, report, RRR, RR73, 73             */
    char grid[5];               /* a 4-character locator if one was sent   */
    int report;                 /* dB, valid when has_report               */
    bool has_report;
} digi_msg_fields_t;

void digi_hash_init(digi_hash_t *h);
/* Remember a call (no brackets); returns its 22-bit hash. */
uint32_t digi_hash_save(digi_hash_t *h, const char *call);
uint32_t digi_hash22(const char *call);

/* Pack text such as "CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC RR73",
   "CQ PJ4/K1ABC" or up to 13 characters of free text. Returns false when the
   text has no 77-bit form. `h` may be NULL. */
bool digi_msg_pack(const char *text, uint8_t payload[10], digi_hash_t *h);

/* Payload to printed text and fields. Returns false for reserved types that
   cannot be printed. `h` may be NULL (hashes print as <...>). */
bool digi_msg_unpack(const uint8_t payload[10], char *text, size_t cap,
                     digi_hash_t *h, digi_msg_fields_t *f);

/* Standard callsign helpers, shared with WSPR. "K1ABC" -> 6-char aligned
   " K1ABC"; false when the call is not a standard one. */
bool digi_call_align6(const char *call, char out[7]);
bool digi_grid4_valid(const char *g);

#endif
