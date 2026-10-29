/* METERS: utility meters heard over the air (Itron ERT SCM, SCM+, IDM, as
   rtl_433's decoders read them through rec_ism), each with its reading, how
   fast it is moving and when it was last heard; pinned and named ones are
   kept in a small JSON file on the card.

   ls_meters.c is the store: pure, no clock, no lock, no file. The caller
   hands it the time and owns serialising access (ls_meters_rt.c does both
   on the board). */

#ifndef LS_METERS_H
#define LS_METERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "experiments/lr433_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LSM_MAX  24
#define LSM_HIST 48
#define LSM_NAME 16
/* A reading that has not moved is still kept this often, so a flat stretch
   counts as zero use instead of vanishing from the rate. */
#define LSM_FLAT_S 600

typedef struct { int64_t t, v; } lsm_point_t;

typedef struct {
    bool used, pinned;
    uint8_t proto, source;           /* lr433_proto_t, rec_source_t        */
    char id[16], name[LSM_NAME];
    int64_t value;                   /* the latest reading, -1 none yet    */
    int64_t first_s, last_s;         /* the caller's clock, 0 never heard  */
    uint32_t heard, hz;
    float rssi;                      /* NAN when the radio does not say    */
    uint8_t head, count;             /* hist ring                          */
    lsm_point_t hist[LSM_HIST];
} lsm_meter_t;

typedef struct {
    lsm_meter_t m[LSM_MAX];
    uint32_t seq;                    /* changes with every change          */
    bool pins_dirty;                 /* a pin or a name to write out       */
} lsm_store_t;

void lsm_init(lsm_store_t *s);

/* File a decoded message. Not a meter, or a full table of pinned meters:
   -1. Otherwise the slot. A full table drops its oldest unpinned meter. */
int lsm_receive(lsm_store_t *s, const lr433_msg_t *msg, uint32_t hz, float rssi,
                uint8_t source, int64_t now_s);

/* The slot of (proto, id), or -1. */
int lsm_find(const lsm_store_t *s, uint8_t proto, const char *id);

/* Pinned first, then most recently heard; returns how many. */
int lsm_order(const lsm_store_t *s, uint8_t out[LSM_MAX]);

/* The reading's i-th point, oldest first. */
const lsm_point_t *lsm_point(const lsm_meter_t *m, int i);

/* Units an hour over what history there is: the latest reading against the
   oldest point since the counter last went backwards, over at least a
   minute. NAN when there is not that much. */
double lsm_rate_per_hour(const lsm_meter_t *m);

/* Use per time bucket across the history (oldest point to the latest
   reading), readings joined by straight lines. Writes `buckets` values and
   returns the seconds one bucket spans, or 0 when there are fewer than two
   readings. */
int64_t lsm_usage(const lsm_meter_t *m, int buckets, float *out);

/* Pin and name. A name is printable ASCII, quotes and backslashes dropped,
   cut to LSM_NAME-1. Both mark the pins for writing. */
bool lsm_pin(lsm_store_t *s, int slot, bool pin);
bool lsm_name(lsm_store_t *s, int slot, const char *name);
/* Forget every meter that is neither pinned nor named. */
void lsm_clear(lsm_store_t *s);

/* The pins file: {"v":1,"meters":[{"type":"ERT-SCM","id":"...","name":"...","pin":1}]}.
   Only pinned or named meters are written. Returns the length, or 0 when it
   would not fit. */
size_t lsm_pins_json(const lsm_store_t *s, char *out, size_t n);
/* Apply a pins file: a meter not heard yet gets a row that waits for it.
   Returns how many entries were applied, -1 for a file that is not version
   1 of this schema. */
int lsm_pins_load(lsm_store_t *s, const char *json);

/* ------------------------------------------- the board's store (rt) -- */

#define LS_METERS_FILE "/sdcard/lakeshark/meters.json"

/* Start collecting: every meter rec_ism publishes, from any radio. Safe to
   call again. */
void ls_meters_begin(void);
/* File one message, as the sink does; for the console and the simulator. */
void ls_meters_inject(const lr433_msg_t *msg, uint32_t hz, float rssi, uint8_t source, int64_t now_s);
/* Copy the store when it has changed since `*seq`; true when it did. */
bool ls_meters_snapshot(lsm_store_t *out, uint32_t *seq);
int64_t ls_meters_now_s(void);
bool ls_meters_pin(uint8_t proto, const char *id, bool pin);
bool ls_meters_name(uint8_t proto, const char *id, const char *name);
void ls_meters_clear(void);
/* On a worker, never the UI task: read the pins file once, write it when a
   pin or a name changed. True while there is more to do. */
bool ls_meters_upkeep(void);
/* `meters` on the console: list | pin <id> | unpin <id> | name <id> <text>. */
int ls_meters_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LS_METERS_H */
