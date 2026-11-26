/* BLE EXPLORER: the GATT table of each connection and the walk that fills
   it, the Bluetooth SIG names and value decoders, and the JSON the table is
   saved as. No board calls: the app (main.c) asks bx_next() which procedure
   to start and feeds every event back through bx_event(), so the bench can
   drive the same code with made-up events. */
#ifndef BLE_EXPLORER_BX_H
#define BLE_EXPLORER_BX_H
#include "lakeshark_app.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BX_MAX_CONN 3           /* the most the app will hold; the board may allow fewer */
#define BX_MAX_SVC 24
#define BX_MAX_CHR 96
#define BX_MAX_DSC 128
/* What one value keeps. API 2.9 boards exchange a bigger MTU and read long
   values whole (up to LAP_GATT_VALUE_MAX); a quarter of that keeps the
   tables of three links small. Longer values keep their first BX_VAL bytes. */
#if defined(LAP_GATT_VALUE_MAX)
#define BX_VAL (LAP_GATT_VALUE_MAX / 4)
#define BX_EVENT_MAX LAP_GATT_VALUE_MAX
#else
#define BX_VAL 20               /* MTU 23: what one read or notification carries */
#define BX_EVENT_MAX 20
#endif
#define BX_NOTES 64

typedef struct { uint8_t b[16], len; } bx_uuid_t;     /* little endian, len 2, 4 or 16 */

typedef struct {
    uint16_t start, end;
    bx_uuid_t uuid;
} bx_svc_t;

typedef struct {
    uint16_t decl, value, end;  /* declaration, value and last handle */
    uint8_t props;              /* 0x02 read 0x04 write-no-rsp 0x08 write 0x10 notify 0x20 indicate */
    uint8_t svc;
    bx_uuid_t uuid;
    uint8_t val[BX_VAL], vlen;
    bool has_value, notifying;
    int32_t error;              /* last read's error, 0 none */
    uint16_t cccd;              /* 0 when there is none */
    uint32_t notes;             /* notifications heard */
} bx_chr_t;

typedef struct {
    uint16_t handle;
    uint8_t chr;
    bx_uuid_t uuid;
    uint8_t val[BX_VAL], vlen;
    bool has_value;
    int32_t error;
} bx_dsc_t;

enum { BX_IDLE, BX_CONNECTING, BX_READY, BX_FAILED, BX_GONE };
enum { BX_P_SERVICES = 1, BX_P_CHARS, BX_P_DESCS, BX_P_READ_CHARS, BX_P_READ_DESCS, BX_P_DONE };
enum { BX_OP_NONE, BX_OP_DISCOVER, BX_OP_DESCRIPTORS, BX_OP_READ, BX_OP_WRITE, BX_OP_SUBSCRIBE };

typedef struct { uint8_t kind; uint16_t a, b; bool manual; bool on; } bx_op_t;

typedef struct {
    int handle;                 /* lap_ble_connect's; 0 when the slot is free */
    uint8_t state, phase;
    uint8_t addr[6], addr_type;
    char name[24];
    bx_svc_t svc[BX_MAX_SVC];
    bx_chr_t chr[BX_MAX_CHR];
    bx_dsc_t dsc[BX_MAX_DSC];
    int nsvc, nchr, ndsc, cur;
    bool busy;
    bx_op_t inflight;
    uint32_t sent_ms;           /* when bx_overdue first saw the request out, 0 none */
    int32_t error;              /* connect or last procedure error */
    uint32_t errors, dropped;   /* item errors seen during the walk; events lost */
    bool truncated;             /* a table filled up */
    uint32_t done_n;            /* requests ended so far (a flow waits on this) */
    int32_t done_err;           /* how the last one ended: 0, its error, or BX_NOREPLY */
    uint8_t done_kind;
} bx_conn_t;
#define BX_NOREPLY (-1900)      /* given up by bx_overdue */

typedef struct {
    uint32_t at_ms;
    uint8_t slot;
    uint16_t handle, uuid16;
    uint8_t val[BX_VAL], vlen;
    char text[40];              /* a decode made when it landed (a WeChat packet), or empty */
} bx_note_t;

/* A connection's slot after lap_ble_connect returned `handle`. */
void bx_begin(bx_conn_t *c, int handle, const uint8_t addr[6], uint8_t addr_type, const char *name);
/* The next procedure to start, or BX_OP_NONE while one runs or the walk is
   done. The caller starts it and calls bx_started (or bx_refused with the
   call's error, which skips that step). */
bx_op_t bx_next(bx_conn_t *c);
void bx_started(bx_conn_t *c, const bx_op_t *op);
void bx_refused(bx_conn_t *c, const bx_op_t *op, int error);
/* Once a frame: a request with no reply after BX_REPLY_MS is given up (the
   board can lose a reply when a device floods notifications), counted as an
   error, and the walk goes on. True when it gave one up just now. */
#define BX_REPLY_MS 5000u
bool bx_overdue(bx_conn_t *c, uint32_t now_ms);
/* One event of this connection. Returns true when it was a notification. */
bool bx_event(bx_conn_t *c, const lap_ble_event_t *e, const uint8_t *data, size_t n);
/* Start the walk again from service discovery, keeping nothing. */
void bx_rewalk(bx_conn_t *c);
/* Read every value again, keeping the table (after pairing, the reads that
   were refused). Only once the walk is done; false otherwise. */
bool bx_reread(bx_conn_t *c);
int bx_progress(const bx_conn_t *c);   /* 0..100 */

/* Names and values. */
uint16_t bx_uuid16(const bx_uuid_t *u);         /* 0 when not a SIG short UUID */
void bx_uuid_text(const bx_uuid_t *u, char *out, size_t cap);
/* The 16 bytes in written order (a short UUID on the SIG base). */
void bx_uuid_be(const bx_uuid_t *u, uint8_t be[16]);
bool bx_uuid_same(const bx_uuid_t *a, const bx_uuid_t *b);
/* "180f", "0x180F", "6e400001-b5a3-f393-e0a9-e50e24dcca9e" or 32 hex digits;
   n is the text's length. A SIG-base 128-bit UUID comes back short. */
bool bx_uuid_parse(const char *s, size_t n, bx_uuid_t *out);
/* The key a name is filed under: "180f", else the whole 128-bit form. */
void bx_uuid_key(const bx_uuid_t *u, char *out, size_t cap);
/* Names come from the namer first (the app hands it the shared name
   library), then from the short built-in list below, which answers at once
   while a card lookup is still pending. kind: BX_NAME_SVC, _CHR or _DSC
   (the same numbers as LAP_IDS_SERVICE, _CHAR and _DESC). NULL: none. */
enum { BX_NAME_SVC = 2, BX_NAME_CHR = 5, BX_NAME_DSC = 6 };
typedef const char *(*bx_namer_t)(int kind, const uint8_t be[16]);
void bx_set_namer(bx_namer_t f);
const char *bx_service_name(const bx_uuid_t *u);    /* NULL when unknown */
const char *bx_char_name(const bx_uuid_t *u);
const char *bx_desc_name(const bx_uuid_t *u);
void bx_props_text(uint8_t props, char *out, size_t cap);   /* "R W N" style */
/* A value as words when the characteristic is a known one, else text when
   printable, else nothing (returns false; show hex). */
bool bx_decode(uint16_t uuid16, const uint8_t *v, size_t n, char *out, size_t cap);
bool bx_decode_desc(uint16_t uuid16, const uint8_t *v, size_t n, char *out, size_t cap);
void bx_hex(const uint8_t *v, size_t n, char *out, size_t cap);
const char *bx_error_text(int32_t e);
/* An advert's own name: the complete local name (AD 0x09), else the short
   one (0x08), made printable. Returns the AD type it came from, 0 none. */
int bx_ad_name(const uint8_t *p, size_t n, char *out, size_t cap);

/* WeChat AirSync (service fee7, indications on fec8): packets start
   fe 01 <len16> <cmd16> <seq16>, big endian, len the whole packet, and run
   over several notifications when longer than one. Feed every notification
   in order; true and a line in out when it is (part of) one. wechat: the
   characteristic is fec8 or sits in service fee7 (else only a header with a
   known command counts). */
typedef struct { uint16_t handle, need, have, cmd, seq; uint8_t parts; } bx_airsync_t;
bool bx_airsync(bx_airsync_t *a, uint16_t handle, bool wechat, const uint8_t *v, size_t n, char *out, size_t cap);

/* The table as JSON (with the notification log); returns bytes written, or
   the bytes needed when cap is too small. */
size_t bx_json(const bx_conn_t *c, const bx_note_t *notes, int nnotes, int64_t unix_time, char *out, size_t cap);
#endif
