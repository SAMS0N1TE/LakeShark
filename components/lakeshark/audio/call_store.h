#ifndef CALL_STORE_H
#define CALL_STORE_H
#include "call_recorder.h"
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <time.h>
#define CALL_PATH_MAX 192
#define CALL_RECENT_MAX 48
typedef struct {
    char path[CALL_PATH_MAX];
    call_meta_t meta;
    uint32_t duration_ms, drops, bytes;
    bool kept;
} call_entry_t;
typedef struct {
    FILE *file;
    char path[CALL_PATH_MAX], temporary[CALL_PATH_MAX];
    call_meta_t meta;
    uint32_t bytes, errors;
    bool failed;
    /* Writers are allocated in PSRAM on the board. Keep stdio's buffer here. */
    char buffer[4096];
} call_writer_t;
bool call_store_step(call_writer_t *w, call_recorder_t *r, const char *root);
void call_store_close(call_writer_t *w);
FILE *call_store_playback_open(const char *path);
void call_store_playback_close(FILE *file);
uint64_t call_store_size(const char *root);
int call_store_scan(const char *root, call_entry_t *entries, int capacity);
bool call_store_protect(const char *root,const char *path,bool protect);
bool call_store_delete(const char *root, const char *path);
void call_store_retain(const char *root, int64_t today, unsigned keep);
/* The UI and SD task convert dates concurrently; each owns its tm storage. */
static inline bool call_utc_tm(time_t stamp, struct tm *out)
{
#ifdef _WIN32
    return gmtime_s(out, &stamp) == 0;
#else
    return gmtime_r(&stamp, out) != NULL;
#endif
}
#ifdef __cplusplus
}
#endif
#endif
