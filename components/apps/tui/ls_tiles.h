#ifndef LS_TILES_H
#define LS_TILES_H
#include "ls_app.h"
#include <stdint.h>
#define LS_TILES_MAX 48
typedef struct {
    char name[64], file[128], description[160], sha256[65];
    uint64_t bytes, local_bytes, partial_bytes;
    double bbox[4];
    int zmin, zmax;
    bool catalog, verified;
    int status; /* 0 absent, 1 installed, 2 partial, 3 update */
} ls_tile_region;
typedef struct {
    ls_tile_region region[LS_TILES_MAX];
    int count;
    uint64_t total, free, maps;
    bool sd, busy;
    char message[96];
    int last_error;
} ls_tiles_catalog;
typedef struct {
    uint64_t bytes, total;
    int64_t start_us, stage_us;
    char stage[40], last_error[96];
    int state; /* 0 idle, 1 running, 2 saved, 3 failed */
    bool receiving, verifying;
    int port;
    char code[33];
    bool has_bbox;
    double bbox[4];
    char message[96], file[128];
} ls_carto_transfer;
bool ls_tiles_snapshot(ls_tiles_catalog *out);
bool ls_tiles_refresh(bool online);
void ls_tiles_diagnostics(void);
bool ls_tiles_action(int action, const ls_tile_region *r); /* 0 fetch, 1 receive, 2 open, 3 delete, 4 verify */
int ls_carto_verify(const ls_tile_region *r);
void ls_carto_recover(void);
int ls_carto_unlink(const char *path);
void ls_carto_active_file(char *out,size_t size);
void ls_carto_transfer_snapshot(ls_carto_transfer *out);
void ls_carto_transfer_cancel(void);
void ls_carto_transfer_clear(void);
void ls_carto_transfer_fail(const char *why);
void ls_tiles_expected(const char *file,char sha[65],uint64_t *bytes);
extern const ls_tui_screen_t ls_scr_tiles;
extern const ls_app_doc_t ls_doc_tiles;
#endif
