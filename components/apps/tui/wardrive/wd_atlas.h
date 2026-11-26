/* WARDRIVE's memory across drives: every device ever heard, where, how
   often and on which drives, and the routes driven, kept on the card and
   merged after each drive. A route driven again (the commute) is recognised,
   so each run makes its data set better, whatever radios or jobs that run
   used. It also answers the home base (wd_atlas_delta / wd_atlas_apply).

   wd_core calls the note/session hooks under wd_lock (fast, no card IO);
   the worker calls wd_atlas_step with no lock held (card IO allowed). */
#ifndef WD_ATLAS_H
#define WD_ATLAS_H

#include "wd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A session begins or ends; replay true for a saved drive played back. */
void wd_atlas_session(bool begin, bool replay, uint32_t now);
/* Each sighting, after wd_core updated the device. May set e->tags
   (WD_T_REGULAR, WD_T_NEWHERE). */
void wd_atlas_note(int idx, wd_dev_t *e, bool added, uint32_t now);
/* The worker's pass, no lock held: load, merge, write. */
void wd_atlas_step(uint32_t now);

/* Where the board is from what it hears: the access points the atlas knows
   from two or more drives (fixed ones), weighted by how loud each is now,
   about their strongest places. For when the GPS is weak or gone (indoors,
   a garage, a city canyon). False with fewer than three of them, or when
   they disagree by more than ~150 m. Under wd_lock, on the worker. */
bool wd_atlas_locate(const wd_obs_t *o, int n, double *lat, double *lon, float *err_m, int *used);

/* ---- home base sync (records are the atlas's own; see wd_atlas.c) ---- */
/* The atlas's change counter: every merged record gets the next number. */
uint32_t wd_atlas_seq(void);
/* Records changed after `since`, up to cap bytes; *next is the seq to ask
   from next time. Returns bytes written, 0 when there is nothing newer. */
size_t wd_atlas_delta(uint32_t since, uint8_t *out, size_t cap, uint32_t *next);
/* Merge records a peer sent. `origin` names the board they came from.
   Returns records merged, -1 on a malformed buffer. */
int wd_atlas_apply(const uint8_t *buf, size_t len, uint32_t origin);

/* ---- what the ATLAS page reads (copies, taken under wd_lock) ---- */

/* The six sources coverage is kept by. Drones count as AIR. */
enum { WD_AS_WIFI, WD_AS_BT, WD_AS_RF, WD_AS_AIR, WD_AS_LORA, WD_AS_CELL, WD_AS_N };
int wd_atlas_src(int kind);
const char *wd_atlas_src_name(int s);       /* "WI-FI" "BT" "RF" "AIR" "LORA" "CELL" */

enum { WD_ATLAS_OFF, WD_ATLAS_LOADING, WD_ATLAS_READY, WD_ATLAS_NOCARD, WD_ATLAS_NOMEM };
enum { WD_ATLAS_JOB_NONE, WD_ATLAS_JOB_IMPORT, WD_ATLAS_JOB_COMPACT };

typedef struct {
    uint16_t num, ndev, nnew, ncells, ngain;    /* run number on its route; devices; new here; cells; cells never driven before */
    uint32_t start, dist_m;                     /* UTC (0 unknown); metres */
} wd_atlas_run_t;

typedef struct {
    char name[12];
    uint16_t runs, cells;
    wd_atlas_run_t best, last;                  /* best: the most devices heard */
} wd_atlas_route_t;

typedef struct {
    int32_t cx, cy;             /* grid cell: floor(lon*640), floor(lat*640) */
    uint16_t hits;              /* runs of the route through it */
    uint8_t mask;               /* WD_AS_* bits that ever covered it */
    uint8_t now;                /* WD_AS_* bits this run covered it with; 0 not driven this run */
    uint8_t passes[WD_AS_N];    /* runs through it with each source (15 = 15 or more) */
} wd_atlas_cell_t;

typedef struct {
    uint8_t kind;
    int8_t rssi;
    uint16_t drives;            /* earlier drives it was heard on */
    uint16_t dist_m;            /* MOVED: how far from where the atlas had it */
    uint8_t addr[6];
    char name[22];
} wd_atlas_item_t;

typedef struct {
    int state, job, job_pct;
    char note[56];
    uint32_t board, seq, devices, runs, cells, km, tail;
    uint32_t kind_dev[WD_KINDS];
    int routes;
    /* the drive now: live = a session is recording into the atlas */
    bool live, known_ready, provisional;
    bool recent;                /* not live, but a drive ended since boot: the run_* figures are that drive's */
    int route;                  /* the route this run is on, -1 none yet */
    uint16_t run_num;
    unsigned run_dev, run_new, run_reg, run_missing, run_moved, run_cells, run_gain;
    unsigned run_gain_src[WD_AS_N];
    uint32_t run_m;
    uint8_t run_src;
    int import_left;            /* drives on the card not in the atlas; -1 not counted yet */
} wd_atlas_info_t;

void wd_atlas_info(wd_atlas_info_t *out);
bool wd_atlas_route_info(int i, wd_atlas_route_t *out);
/* A route's cells, its first run's path first, in driving order. */
int  wd_atlas_route_cells(int i, wd_atlas_cell_t *out, int cap);
/* A route's finished runs by run number (the oldest first), up to cap. */
int  wd_atlas_route_runs(int i, wd_atlas_run_t *out, int cap);
/* The cells this run has driven, in order. */
int  wd_atlas_run_cells(wd_atlas_cell_t *out, int cap);
enum { WD_ATLAS_L_NEW, WD_ATLAS_L_MISSING, WD_ATLAS_L_MOVED, WD_ATLAS_L_REGULAR, WD_ATLAS_LISTS };
int  wd_atlas_list(int which, wd_atlas_item_t *out, int cap);
/* IMPORT the drives on the card; COMPACT the index now; COUNT the drives
   IMPORT would take. The worker does them (wd_atlas_step). */
enum { WD_ATLAS_REQ_IMPORT = 1, WD_ATLAS_REQ_COMPACT = 2, WD_ATLAS_REQ_COUNT = 4 };
void wd_atlas_request(int what);
/* Work left for wd_atlas_step: keep the worker going. */
bool wd_atlas_pending(void);
/* Forget what is in memory; the next step reads the card again. */
void wd_atlas_reload(void);
/* This atlas's board id (the origin its own records carry), 0 before loading. */
uint32_t wd_atlas_board(void);

#ifdef __cplusplus
}
#endif
#endif
