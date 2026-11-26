/* HOME BASE sync over LSX (ls_lsx.h): the home base (the desk's Nano) is the
   session master and owns the master atlas; a field unit (a T-Display in the
   car) answers it.

     home -> field  PULL  since = the unit's seq the home already has
     field -> home  DELTA wd_atlas_delta(since) records, with next and pending
     ... until a DELTA comes back empty, then
     home -> field  PUSH  the home's own delta since what the unit was last
                          given: what every other unit found
     home           BYE

   Each object is at most 16 KB of records, so a session that breaks loses
   at most one, and that one resumes (LSX keeps the partial transfer and the
   field unit offers the same bytes again for the same `since`).

   The records are the atlas's own: this layer never looks inside them. A
   store (ls_lsx_store_t) hands them over: on the Nano the desk binds
   wd_atlas directly; on a T-Display WARDRIVE lives in a firmware module and
   registers its store when it loads (ls_lsx_set_store in ls_lsx_svc.h). */
#ifndef LS_LSX_SYNC_H
#define LS_LSX_SYNC_H

#include "ls_lsx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LSX_PORT_ATLAS  1
#define LSX_PORT_TEST   9       /* 'lsx push': raw bytes, timed, thrown away */
#define LSX_SYNC_CHUNK  16384u  /* records an object carries at most */
#define LSX_UNITS       12
#define LSX_RESYNC_S    300     /* a unit with nothing new is left alone this long */

typedef struct {
    void *ctx;
    uint32_t (*seq)(void *ctx);
    size_t (*delta)(void *ctx, uint32_t since, uint8_t *out, size_t cap, uint32_t *next);
    int (*apply)(void *ctx, const uint8_t *buf, size_t len, uint32_t origin);
} ls_lsx_store_t;

/* What the home base remembers of each field unit (kept in NVS). */
typedef struct {
    uint32_t id;
    char     name[LSX_NAME];
    uint32_t pulled;        /* the unit's atlas seq the home has up to */
    uint32_t pushed;        /* the home's atlas seq the unit has up to */
    uint32_t last_sync_s;   /* uptime seconds of the last finished sync; 0 never */
    uint32_t bytes_in, bytes_out;
    uint32_t recs_in, recs_out;
    uint16_t syncs;
    int8_t   rssi;
    uint8_t  caps;
} ls_lsx_unit_t;

enum { LSX_PH_IDLE, LSX_PH_PULL, LSX_PH_PUSH, LSX_PH_DONE, LSX_PH_N };

typedef struct {
    uint8_t  role, phase;
    uint32_t peer;
    char     peer_name[LSX_NAME];
    int      progress_pct;      /* -1 when not syncing */
    uint32_t recs_in, recs_out; /* this session */
    uint32_t last_sync_s;       /* field: when the last sync finished; 0 never */
    uint32_t last_home_ms;      /* field: since the home was last heard; UINT32_MAX never */
    char     home_name[LSX_NAME];
    uint32_t test_bytes, test_ms;  /* the last 'lsx push' received */
    bool     store;             /* a store is bound */
    uint32_t now_s;             /* the clock last_sync_s values are on */
} ls_lsx_sync_status_t;

typedef struct ls_lsx_sync ls_lsx_sync_t;

ls_lsx_sync_t *ls_lsx_sync_create(int role);
void ls_lsx_sync_destroy(ls_lsx_sync_t *s);
/* The callbacks to put in ls_lsx_cfg_t.app; then bind the engine. */
ls_lsx_app_t ls_lsx_sync_app(ls_lsx_sync_t *s);
void ls_lsx_sync_bind(ls_lsx_sync_t *s, ls_lsx_t *x);
void ls_lsx_sync_set_store(ls_lsx_sync_t *s, const ls_lsx_store_t *store);
/* After each ls_lsx_step, with the same clock. */
void ls_lsx_sync_tick(ls_lsx_sync_t *s, uint32_t now_ms);
/* Field: sync at the next probe even if the home thinks it is up to date. */
void ls_lsx_sync_force(ls_lsx_sync_t *s);
void ls_lsx_sync_status(const ls_lsx_sync_t *s, ls_lsx_sync_status_t *out);
int  ls_lsx_sync_units(const ls_lsx_sync_t *s, ls_lsx_unit_t *out, int max);
/* The unit table as one blob for NVS, and back. dirty: changed since the
   last ls_lsx_sync_take_dirty. */
size_t ls_lsx_sync_units_blob(const ls_lsx_sync_t *s, void *out, size_t cap);
void ls_lsx_sync_units_load(ls_lsx_sync_t *s, const void *blob, size_t len);
bool ls_lsx_sync_take_dirty(ls_lsx_sync_t *s);
/* Home: keep the session open after the sync (no BYE) while on, for
   'lsx push' rate tests. */
void ls_lsx_sync_hold(ls_lsx_sync_t *s, bool on);
/* 'lsx push': send `bytes` test bytes to the session peer. */
int  ls_lsx_sync_push_test(ls_lsx_sync_t *s, uint32_t bytes);

/* A store of numbered 32-byte test records, for the bench and 'lsx testdata'
   on boards without WARDRIVE: `count` records of its own, plus what it was
   given (counted, not kept). */
const ls_lsx_store_t *ls_lsx_test_store(uint32_t count, uint32_t salt);
uint32_t ls_lsx_test_store_applied(const ls_lsx_store_t *store);
/* `n` more records of its own (the drive goes on). */
void ls_lsx_test_store_add(const ls_lsx_store_t *store, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif
