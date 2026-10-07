#ifndef CALL_STORE_H
#define CALL_STORE_H
#include "call_recorder.h"
#include <stdio.h>
#include <time.h>
#define CALL_PATH_MAX 192
#define CALL_RECENT_MAX 48
typedef struct {
    char path[CALL_PATH_MAX];
    call_meta_t meta;
    uint32_t duration_ms, drops;
} call_entry_t;
typedef struct {
    FILE *file;
    char path[CALL_PATH_MAX], temporary[CALL_PATH_MAX];
    call_meta_t meta;
    uint32_t bytes, errors;
    bool failed;
} call_writer_t;
bool call_store_step(call_writer_t *w, call_recorder_t *r, const char *root);
int call_store_scan(const char *root, call_entry_t *entries, int capacity);
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
#endif
