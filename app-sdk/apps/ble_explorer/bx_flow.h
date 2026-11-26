/* BLE EXPLORER flows: short scripted GATT sequences a user starts on
   purpose ("subscribe, write 10 04 01, wait for the reply"). The text format
   is in the app's README. This file is pure C: it parses flows, keeps the
   list, and runs one a step per frame through the same one request at a time
   machinery as the walk (bx_started, bx_event, bx_overdue's 5 s timeout), so
   the bench drives it with made-up events.

   No flow can loop: every step runs once, waits are capped, a flow holds at
   most BXF_STEPS steps. */
#ifndef BLE_EXPLORER_BX_FLOW_H
#define BLE_EXPLORER_BX_FLOW_H
#include "bx.h"

#define BXF_STEPS 32
#define BXF_FLOWS 96
#define BXF_NAME 32
#define BXF_ABOUT 96
#define BXF_MATCH 6
#define BXF_WAIT_MAX 30000u      /* ms, any one wait */

enum { BXF_ORIGIN_BUILTIN, BXF_ORIGIN_PACK, BXF_ORIGIN_USER };
enum { BXF_LOG = 1, BXF_READ, BXF_WRITE, BXF_NOTIFY, BXF_WAIT, BXF_WAIT_NOTIFY };
enum { BXF_M_SERVICE = 1, BXF_M_NAME };

typedef struct {
    uint8_t op;
    bool optional;              /* "? read 2a24": a failure is noted, the flow goes on */
    bool on;                    /* notify on / off */
    bool time;                  /* the data is $time, filled at run time */
    bool in_svc;                /* "1802/2a06": the characteristic in that service */
    bx_uuid_t uuid, svc;
    uint8_t data[BX_VAL], n;
    uint32_t ms;
    const char *src;            /* the step's line as written (for the screen) */
    uint8_t srclen;
} bxf_step_t;

typedef struct {
    uint8_t kind;
    bx_uuid_t uuid;
    char glob[24];
} bxf_match_t;

typedef struct {
    char name[BXF_NAME], about[BXF_ABOUT], confirm[BXF_ABOUT];
    bxf_match_t match[BXF_MATCH];
    uint8_t nmatch, nsteps;
    bool writes;                /* has write steps: the app asks first */
    bxf_step_t step[BXF_STEPS];
} bxf_flow_t;

/* One flow in the list: its header, and where its text is. The text must
   stay where it is while the list is used (the built-in string, or a buffer
   the app keeps). */
typedef struct {
    char name[BXF_NAME], about[BXF_ABOUT];
    bxf_match_t match[BXF_MATCH];
    uint8_t nmatch, origin, nsteps;
    bool writes;
    const char *text;
    uint32_t len;
    uint16_t line0;             /* the flow line's number in its file */
} bxf_entry_t;

typedef struct {
    char where[24];             /* the file */
    int line;
    char msg[48];
} bxf_err_t;

typedef struct {
    bxf_entry_t e[BXF_FLOWS];
    int n;
    int bad;                    /* flows dropped for a bad line */
    bxf_err_t err;              /* the first */
    int replaced;               /* flows a later source replaced by name */
    bxf_flow_t scratch;
} bxf_lib_t;

void bxf_lib_init(bxf_lib_t *L);
/* Add every flow in text (a file of one or many). A flow with the name of
   one already held replaces it in place. Returns flows taken. */
int bxf_lib_add(bxf_lib_t *L, const char *text, size_t len, uint8_t origin, const char *where);
/* Parse one flow's text (from "flow" to its end). time: the Current Time
   bytes for $time, or NULL. 0 good, else -1 with err filled. */
typedef struct { int year, month, day, hour, minute, second, weekday; } bxf_time_t;   /* weekday 0 Sunday */
int bxf_parse(const char *text, size_t len, int line0, bxf_flow_t *f, const bxf_time_t *time, bxf_err_t *err);
/* Does the flow fit this connection (a service it names, or its name)? */
bool bxf_matches(const bxf_entry_t *e, const bx_conn_t *c, const char *name);
/* The list in the order the FLOWS page shows it: the ones that fit first.
   Returns how many fit. */
int bxf_order(const bxf_lib_t *L, const bx_conn_t *c, const char *name, int *out);
bool bxf_glob(const char *pat, const char *s);  /* * and ?, any case */

/* ---- running one ---- */
enum { BXF_IDLE, BXF_RUNNING, BXF_DONE, BXF_FAILED, BXF_STOPPED };
enum { BXF_S_PENDING, BXF_S_NOW, BXF_S_OK, BXF_S_FAIL, BXF_S_SKIP };
typedef struct {
    uint8_t st;
    char text[48];
} bxf_result_t;
typedef struct {
    bxf_flow_t f;
    uint8_t state, origin;
    int at;
    bool out;                   /* a request of ours is out */
    uint32_t done_seen, t0, started, ended;
    int chr;                    /* the characteristic of the request out */
    uint32_t base[BX_MAX_CHR];  /* notifications per characteristic when we last looked */
    bxf_result_t res[BXF_STEPS];
    char why[64];
} bxf_run_t;
/* What to send this frame. */
typedef struct {
    bx_op_t op;                 /* BX_OP_READ, _WRITE or _SUBSCRIBE, manual */
    uint8_t data[BX_VAL], n;
    bool no_rsp;                /* the characteristic only takes writes without response */
} bxf_req_t;

/* Start a flow on a connection whose walk is done. 0 ok, -1 why in r->why. */
int bxf_begin(bxf_run_t *r, const bxf_entry_t *e, const bx_conn_t *c, const bxf_time_t *time, uint32_t now);
/* Once a frame: one step's worth. True when req should be sent now; the
   caller makes the lap_gatt_* call and hands its result to bxf_sent. */
bool bxf_tick(bxf_run_t *r, bx_conn_t *c, uint32_t now, bxf_req_t *req);
void bxf_sent(bxf_run_t *r, bx_conn_t *c, const bxf_req_t *req, int rc, uint32_t now);
void bxf_stop(bxf_run_t *r, uint32_t now);
const char *bxf_op_word(int op);

/* The built-in flows (bx_flows_builtin.c, the text of flows/ble-flows.txt),
   and where the others come from, in the order they load. */
extern const char bx_flows_builtin[];
extern const size_t bx_flows_builtin_len;
#define BXF_PACK_FILE "/sdcard/lakeshark/packs/ble-flows.bin"
#define BXF_USER_DIR "/sdcard/lakeshark/ble-flows"
#endif
