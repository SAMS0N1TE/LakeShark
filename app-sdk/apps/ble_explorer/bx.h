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
#define BX_VAL 20               /* MTU 23: what one read or notification carries */
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
    int32_t error;              /* connect or last procedure error */
    uint32_t errors, dropped;   /* item errors seen during the walk; events lost */
    bool truncated;             /* a table filled up */
} bx_conn_t;

typedef struct {
    uint32_t at_ms;
    uint8_t slot;
    uint16_t handle, uuid16;
    uint8_t val[BX_VAL], vlen;
} bx_note_t;

/* A connection's slot after lap_ble_connect returned `handle`. */
void bx_begin(bx_conn_t *c, int handle, const uint8_t addr[6], uint8_t addr_type, const char *name);
/* The next procedure to start, or BX_OP_NONE while one runs or the walk is
   done. The caller starts it and calls bx_started (or bx_refused with the
   call's error, which skips that step). */
bx_op_t bx_next(bx_conn_t *c);
void bx_started(bx_conn_t *c, const bx_op_t *op);
void bx_refused(bx_conn_t *c, const bx_op_t *op, int error);
/* One event of this connection. Returns true when it was a notification. */
bool bx_event(bx_conn_t *c, const lap_ble_event_t *e, const uint8_t *data, size_t n);
/* Start the walk again from service discovery, keeping nothing. */
void bx_rewalk(bx_conn_t *c);
int bx_progress(const bx_conn_t *c);   /* 0..100 */

/* Names and values. */
uint16_t bx_uuid16(const bx_uuid_t *u);         /* 0 when not a SIG short UUID */
void bx_uuid_text(const bx_uuid_t *u, char *out, size_t cap);
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

/* The table as JSON (with the notification log); returns bytes written, or
   the bytes needed when cap is too small. */
size_t bx_json(const bx_conn_t *c, const bx_note_t *notes, int nnotes, int64_t unix_time, char *out, size_t cap);
#endif
