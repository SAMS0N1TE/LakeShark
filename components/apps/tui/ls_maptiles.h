/* The world map: z/x/y vector tiles kept on the card and fetched from
   tiles.terminalbay.com when Wi-Fi is up. MAP asks here for a tile; a tile
   that is not on the card yet is queued and arrives a moment later.

   Card layout: /sdcard/maps/cache/{z}/{x}/{y}.mvt, an empty file for a tile
   the world has nothing in (open sea). Map data (c) OpenStreetMap
   contributors, ODbL; tiles from the Protomaps basemap. */
#ifndef LS_MAPTILES_H
#define LS_MAPTILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_MAPTILES_MAXZ      15     /* the finest zoom the server has */
#define LS_MAPTILES_AREA_MAXZ 14     /* "download this area" stops here */
#define LS_MAPTILES_AREA_MAX  20000  /* tiles in one area download */
#define LS_MAPTILES_ATTRIBUTION "(c) OpenStreetMap contributors"

typedef enum {
    LS_MT_HAVE = 0,   /* bytes copied out                          */
    LS_MT_EMPTY,      /* known to hold nothing                     */
    LS_MT_WAIT,       /* asked for; it will arrive                 */
    LS_MT_NONE,       /* not on the card and no way to get it now  */
} ls_mt_result;

/* One tile. `want` queues a download when it is missing and Wi-Fi is up. */
ls_mt_result ls_maptiles_get(int z, uint32_t x, uint32_t y, uint8_t *buf, size_t cap,
                             size_t *len, bool want);
/* True when the tile is on the card (or known empty), without reading it. */
bool ls_maptiles_has(int z, uint32_t x, uint32_t y);
/* Drop what is queued for a view nobody is looking at any more. */
void ls_maptiles_forget(void);
/* Counts every tile that arrived: a frame drawn before it is stale. */
uint32_t ls_maptiles_serial(void);
bool ls_maptiles_online(void);

typedef struct {
    uint32_t files;           /* tiles on the card, once counted        */
    uint64_t bytes, cap;      /* what they take, and the most they may  */
    bool     counted;         /* false until the card has been walked   */
    int      queued;          /* tiles MAP is waiting for               */
    bool     job;             /* an area download is running            */
    uint32_t job_done, job_total, job_failed;
    uint32_t fetched, failed; /* this boot                              */
    char     note[48];        /* the last thing worth saying            */
} ls_maptiles_stats_t;
void ls_maptiles_stats(ls_maptiles_stats_t *out);

/* How many tiles an area is, for zooms z0..z1. */
uint32_t ls_maptiles_area_tiles(double west, double south, double east, double north, int z0, int z1);
/* Fetch them in the background; tiles already on the card are skipped.
   False when one is already running, the area is too big or Wi-Fi is down. */
bool ls_maptiles_download_area(double west, double south, double east, double north, int z0, int z1);
void ls_maptiles_cancel(void);
/* Count the card's tiles, or delete them all, in the background. */
void ls_maptiles_count(void);
void ls_maptiles_clear(void);
void ls_maptiles_set_cap(uint64_t bytes);

#ifdef __cplusplus
}
#endif
#endif
