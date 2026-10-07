#ifndef LS_SURVEY_H
#define LS_SURVEY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define LS_SURVEY_CAP 512
typedef enum { LS_SURVEY_WIFI, LS_SURVEY_BLE } ls_survey_kind_t;
typedef struct {
    bool fix;
    double lat, lon;
    int64_t fix_us;
} ls_survey_fix_t;
typedef struct {
    ls_survey_kind_t kind;
    uint8_t addr[6], addr_type, channel, auth;
    char name[33];
    int rssi;
    uint32_t first_s, last_s, best_s, first_ms;
    bool epoch, positioned;
    double lat, lon;
} ls_survey_entry_t;
typedef struct {
    ls_survey_entry_t *entries;
    unsigned count, cap, dropped;
    bool only_fix;
} ls_survey_session_t;
typedef struct {
    bool wifi, ble, only_fix;
    unsigned interval_s, keep;
} ls_survey_options_t;
typedef struct {
    bool running, busy, gps_fresh;
    unsigned wifi, ble, new_minute, dropped, elapsed_s;
    char status[96], path[128];
    ls_survey_options_t options;
} ls_survey_view_t;

void ls_survey_init(ls_survey_session_t *s, ls_survey_entry_t *table, unsigned cap, bool only_fix);
bool ls_survey_fix_fresh(const ls_survey_fix_t *fix, int64_t now_us);
bool ls_survey_note(ls_survey_session_t *s, const ls_survey_entry_t *obs,
                    const ls_survey_fix_t *fix, int64_t now_us, uint32_t now_ms);
bool ls_survey_csv(FILE *f, const ls_survey_session_t *s);
bool ls_survey_filename(char *out, size_t cap, uint32_t seconds, bool epoch);
int ls_survey_compare(const ls_survey_entry_t *a, const ls_survey_entry_t *b, int sort);

/* UI requests are consumed by the wireless worker, including SD writes. */
bool ls_survey_run(bool start, const ls_survey_options_t *options);
void ls_survey_view(ls_survey_view_t *out);
bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out);
void ls_survey_ble(const uint8_t *addr, uint8_t type, const char *name, unsigned len, int rssi, int64_t us);
#endif
