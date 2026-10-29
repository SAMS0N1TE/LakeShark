/* RF SURVEY: walk or drive, and keep how loud each channel was where.

   A plan is a list of channels (a frequency and a width each), either named
   one by one or cut evenly from a range. A receiver measures every channel
   in turn; one pass over the whole list, at one place, is a ROW: the level
   of each channel, its learnt noise floor, whether it stood clear of that
   floor (occupied), and where and when. Rows go to a survey file and into a
   HEAT grid of square cells on the ground, which MAP draws as an overlay.

   Pure: no radio, sensor or storage is touched here. ls_rfs_run.c drives
   it on the board and the bench tests drive it directly.

   SURVEY FILE (.lsrf), version 1, every number little-endian:

     header   64 bytes
       0  char[8]  "LSRFSURV"
       8  u16      version (1)
      10  u16      header bytes, channel table included (64 + 16 * nch)
      12  u16      channels (nch, 1..LS_RFS_CH_MAX)
      14  u16      row bytes (24 + 4 * nch)
      16  u32      start, UTC seconds (0: the clock was not set)
      20  char[16] source name, NUL padded
      36  char[16] plan name, NUL padded
      52  i16      occupancy threshold, centi-dB over the floor
      54  u16      cell size of the heat grid, metres
      56  8 bytes  zero
     channel table, nch x 16 bytes
       0  u32      centre, Hz
       4  u32      width, Hz
       8  char[8]  label, NUL padded
     rows, row bytes each, until the end of the file (a short tail is a row
     that was being written when the card was pulled, and is ignored)
       0  u32      ms since the start
       4  u32      UTC seconds, 0 unknown
       8  i32      latitude, 1e-7 degrees
      12  i32      longitude, 1e-7 degrees
      16  u16      flags: LS_RFS_ROW_*
      18  u16      pass number, wrapping
      20  u32      occupied channels, one bit each
      24  nch x { i16 level, i16 floor }, centi-dB (dBm or dBFS as the
                  source reports; the source name says which) */

#ifndef LS_RFS_H
#define LS_RFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RFS_CH_MAX     32
#define LS_RFS_LABEL      8
#define LS_RFS_NAME       16
#define LS_RFS_HEADER     64
#define LS_RFS_CH_BYTES   16
#define LS_RFS_ROW_FIXED  24
#define LS_RFS_VERSION    1
#define LS_RFS_NO_LEVEL   INT16_MIN      /* a channel the pass did not reach */

enum {
    LS_RFS_ROW_FIX = 1,       /* the position is a GPS fix */
    LS_RFS_ROW_SIM = 2,       /* the position is the simulated walk */
};

typedef struct {
    uint32_t hz, bw_hz;
    char label[LS_RFS_LABEL + 1];
} ls_rfs_ch_t;

typedef struct {
    char name[LS_RFS_NAME + 1];
    int n;
    ls_rfs_ch_t ch[LS_RFS_CH_MAX];
    bool range;                    /* cut from lo..hi rather than listed */
} ls_rfs_plan_t;

/* The built-in plans: pagers, public safety, ISM, LoRa, FRS, cell
   downlinks. Plans in a card file come after them (ls_rfs_plans_parse). */
int  ls_rfs_preset_count(void);
bool ls_rfs_preset(int i, ls_rfs_plan_t *out);
/* `cells` channels of equal width covering lo..hi, labelled by MHz. */
bool ls_rfs_plan_range(ls_rfs_plan_t *out, const char *name, uint32_t lo_hz, uint32_t hi_hz, int cells);

/* Plans from a card file:
     { "schema": "lakeshark.rfsurvey.plans", "version": 1,
       "plans": [ { "name": "MY PAGERS",
                    "channels": [ { "hz": 152007500, "bw": 12500, "label": "P1" } ] },
                  { "name": "MY ISM", "range": { "lo": 902000000, "hi": 928000000, "cells": 16 } } ] }
   Strict: an unknown schema or version, a plan with neither channels nor a
   range, or a value out of range rejects the whole file. Returns how many
   plans were read into `out`, or -1 with `why` set. */
int  ls_rfs_plans_parse(const char *json, size_t len, ls_rfs_plan_t *out, int max, char *why, size_t why_cap);

typedef struct {
    uint32_t t_ms, utc_s;
    double lat, lon;
    uint16_t flags, pass;
    uint32_t occ;
    int16_t level[LS_RFS_CH_MAX];  /* centi-dB, LS_RFS_NO_LEVEL unheard */
    int16_t floor[LS_RFS_CH_MAX];
} ls_rfs_row_t;

typedef struct {
    uint16_t version, nch, row_bytes, cell_m;
    uint32_t start_utc;
    char source[LS_RFS_NAME + 1];
    char plan[LS_RFS_NAME + 1];
    int16_t threshold_cdb;
    ls_rfs_ch_t ch[LS_RFS_CH_MAX];
} ls_rfs_header_t;

size_t ls_rfs_header_bytes(int nch);
size_t ls_rfs_row_bytes(int nch);
/* Serialise into `out`; returns the bytes written, 0 when it does not fit. */
size_t ls_rfs_header_pack(const ls_rfs_header_t *h, uint8_t *out, size_t cap);
/* Parse the header at the start of a file; returns its length, 0 when it is
   not a survey file this version reads. */
size_t ls_rfs_header_unpack(const uint8_t *in, size_t len, ls_rfs_header_t *h);
size_t ls_rfs_row_pack(const ls_rfs_header_t *h, const ls_rfs_row_t *r, uint8_t *out, size_t cap);
bool   ls_rfs_row_unpack(const ls_rfs_header_t *h, const uint8_t *in, size_t len, ls_rfs_row_t *r);

/* ---- per-channel noise floor and occupancy ---------------------------- */

/* The floor follows a quiet channel down at once and climbs back slowly
   (0.05 dB a pass, a dB in twenty), so a carrier that keys up, or stays up
   for a while, is measured against the quiet under it rather than becoming
   the floor. */
typedef struct {
    bool seeded[LS_RFS_CH_MAX];
    float floor[LS_RFS_CH_MAX];
} ls_rfs_floor_t;
void  ls_rfs_floor_reset(ls_rfs_floor_t *f);
float ls_rfs_floor_update(ls_rfs_floor_t *f, int ch, float level_db);

/* ---- the HEAT grid ------------------------------------------------------ */

#define LS_RFS_CELLS 1024

typedef enum { LS_RFS_MAX = 0, LS_RFS_MEAN, LS_RFS_OCC, LS_RFS_OVER, LS_RFS_METRICS } ls_rfs_metric_t;
const char *ls_rfs_metric_name(int m);

typedef struct {
    int16_t ix, iy;
    uint16_t rows;
    int16_t max_cdb[LS_RFS_CH_MAX];
    int32_t sum_cdb[LS_RFS_CH_MAX];     /* for the mean */
    int32_t over_cdb[LS_RFS_CH_MAX];    /* level over floor, summed */
    uint16_t heard[LS_RFS_CH_MAX];
    uint16_t occ[LS_RFS_CH_MAX];
} ls_rfs_cell_t;

typedef struct {
    bool origin_set;
    double lat0, lon0, m_per_deg_lon;
    float cell_m;
    int nch;
    int count;
    uint32_t dropped;                   /* rows that found the grid full */
    uint32_t serial;                    /* bumps on every change */
    int16_t slot[2 * LS_RFS_CELLS];     /* hash: index into cell, -1 empty */
    ls_rfs_cell_t cell[LS_RFS_CELLS];
} ls_rfs_grid_t;

void ls_rfs_grid_reset(ls_rfs_grid_t *g, float cell_m, int nch);
/* Which cell a place falls in, on a flat projection about the first place
   added (or about this one, before any). */
void ls_rfs_grid_index(ls_rfs_grid_t *g, double lat, double lon, int *ix, int *iy);
/* The cell's index, -1 when there is no such cell (or no room for it). */
int  ls_rfs_grid_add(ls_rfs_grid_t *g, const ls_rfs_row_t *r);
int  ls_rfs_grid_find(const ls_rfs_grid_t *g, int ix, int iy);
/* A cell's centre. */
void ls_rfs_grid_centre(const ls_rfs_grid_t *g, int idx, double *lat, double *lon);
/* One number for a cell: channel `ch`, or the loudest channel for -1.
   MAX and MEAN are dB, OCC is percent of rows occupied, OVER is the mean dB
   over the floor. False when the cell never heard that channel. */
bool ls_rfs_grid_value(const ls_rfs_grid_t *g, int idx, int ch, ls_rfs_metric_t m, float *out);
/* The lowest and highest value over every cell, for a legend. */
bool ls_rfs_grid_range(const ls_rfs_grid_t *g, int ch, ls_rfs_metric_t m, float *lo, float *hi);

/* ---- when to keep a row -------------------------------------------------- */

/* A row is kept once the receiver has moved `step_m` from the last kept
   one; with step 0, every pass is kept. A first row is always kept. */
bool ls_rfs_moved_enough(bool have_last, double last_lat, double last_lon,
                         double lat, double lon, float step_m);
double ls_rfs_distance_m(double lat0, double lon0, double lat1, double lon1);

/* ---- export --------------------------------------------------------------- */

/* CSV, long form: one line per channel per row. */
int ls_rfs_csv_header(char *out, size_t cap);
/* Line `ch` of row `r`, without the newline at the end... with it. */
int ls_rfs_csv_line(const ls_rfs_header_t *h, const ls_rfs_row_t *r, int ch, bool occupied, char *out, size_t cap);
/* GeoJSON: a FeatureCollection of points, one per row, each carrying the
   levels, floors and occupied channels as arrays in channel order. */
int ls_rfs_geojson_begin(const ls_rfs_header_t *h, char *out, size_t cap);
int ls_rfs_geojson_feature(const ls_rfs_header_t *h, const ls_rfs_row_t *r, bool first, char *out, size_t cap);
int ls_rfs_geojson_end(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
