#ifndef LS_SURVEY_H
#define LS_SURVEY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define LS_SURVEY_CAP 4096
typedef enum { LS_SURVEY_WIFI, LS_SURVEY_BLE } ls_survey_kind_t;
typedef struct {
    bool fix;
    double lat, lon;
    int64_t fix_us;
    float altitude, hdop;
} ls_survey_fix_t;
typedef struct {
    ls_survey_kind_t kind;
    uint8_t addr[6], addr_type, channel, auth;
    char name[33];
    int rssi;
    uint32_t first_s, last_s, best_s, first_ms;
    bool epoch, positioned;
    double lat, lon;
    uint32_t last_ms, samples;
    bool logged, row_positioned;
    int row_rssi;
    double row_lat, row_lon;
    /* Weighted spherical centroid, including sightings not selected for export. */
    double weight, x, y, z;
} ls_survey_entry_t;
typedef struct {
    ls_survey_entry_t *entries;
    unsigned count, cap, dropped;
    bool only_fix;
    unsigned current;
    unsigned totals[2], security[5], channels[15];
    uint32_t minute_stamp[60];
    unsigned minute[60][2];
    /* Evicted centroid segments are preserved here; a returning address starts
     * a new segment and counts as new again after leaving the bounded cache. */
    FILE *summary;
    bool summary_error;
} ls_survey_session_t;
typedef struct {
    bool wifi, ble, only_fix;
    unsigned interval_s, keep;
} ls_survey_options_t;
typedef struct {
    bool running, busy, gps_fresh;
    unsigned wifi, ble, new_minute, dropped, elapsed_s;
    uint32_t activity_revision;
    unsigned cached, wifi_minute, ble_minute, security[5], top_channel[3], top_count[3];
    unsigned rows;
    uint64_t free_bytes;
    bool sd_known, utc_valid;
    char status[96], path[128];
    ls_survey_options_t options;
} ls_survey_view_t;

void ls_survey_init(ls_survey_session_t *s, ls_survey_entry_t *table, unsigned cap, bool only_fix);
bool ls_survey_fix_fresh(const ls_survey_fix_t *fix, int64_t now_us);
bool ls_survey_note(ls_survey_session_t *s, const ls_survey_entry_t *obs,
                    const ls_survey_fix_t *fix, int64_t now_us, uint32_t now_ms);
const char *ls_survey_auth(unsigned auth);
unsigned ls_survey_security(unsigned auth);
bool ls_survey_header(FILE *f, const char *release, const char *board);
bool ls_survey_row(FILE *f, const ls_survey_entry_t *observation,
                   const ls_survey_fix_t *fix, uint32_t first_seen);
bool ls_survey_relog(const ls_survey_entry_t *e, const ls_survey_entry_t *o,
                     const ls_survey_fix_t *g, int64_t us);
void ls_survey_logged(ls_survey_entry_t *e, const ls_survey_entry_t *o,
                      const ls_survey_fix_t *g, int64_t us);
bool ls_survey_centroid(const ls_survey_entry_t *e, double *lat, double *lon);
bool ls_survey_summary(FILE *f, const ls_survey_entry_t *e);
bool ls_survey_filename(char *out, size_t cap, uint32_t seconds, bool epoch);
int ls_survey_compare(const ls_survey_entry_t *a, const ls_survey_entry_t *b, int sort);

/* UI requests/flushes run on the wireless worker. BLE advertisement-worker
 * observations append under the same mutex; never from an ISR or GAP callback. */
bool ls_survey_run(bool start, const ls_survey_options_t *options);
void ls_survey_view(ls_survey_view_t *out);
bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out);
void ls_survey_ble(const uint8_t *addr, uint8_t type, const char *name, unsigned len, int rssi, int64_t us);
#endif
