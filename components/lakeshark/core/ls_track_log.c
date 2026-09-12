/* See ls_track_log.h. */
#include "ls_track_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ls_gps.h"
#include "ls_rlog.h"
#include "ls_track.h"

static const char *TAG = "ls_track";

#define TRACK_PATH "/sdcard/lakeshark/track.log"
#define TRACK_DIR  "/sdcard/lakeshark"

static ls_rlog_t       s_log;
static TaskHandle_t    s_task;
static volatile bool   s_stop;
static ls_track_pt_t   s_last;        /* the last point KEPT, not the last fix */
static bool            s_have_last;
static uint32_t        s_last_us_s;   /* when it was kept, in uptime seconds  */

static float    s_min_move_m = 10.0f;
static uint32_t s_max_gap_s  = 30;

void ls_track_set_rule(float min_move_m, uint32_t max_gap_s)
{
    if (min_move_m >= 0.0f) s_min_move_m = min_move_m;
    s_max_gap_s = max_gap_s;
}

void ls_track_get_rule(float *min_move_m, uint32_t *max_gap_s)
{
    if (min_move_m) *min_move_m = s_min_move_m;
    if (max_gap_s)  *max_gap_s  = s_max_gap_s;
}

static ls_track_wpt_fn s_sight_cb;

void ls_track_set_waypoint_source(ls_track_wpt_fn fn) { s_sight_cb = fn; }

bool ls_track_rec_running(void) { return s_task != NULL; }
int  ls_track_points(void)      { return ls_rlog_count(&s_log); }

/* See ls_track_log.h. */
int ls_track_attach(void)
{
    if (s_log.open) return ls_rlog_count(&s_log);

    /* Only one that already exists. ls_rlog_open creates the file when it
       cannot read a valid header, and attaching at every boot would put a
       64 kB track log on a card belonging to somebody who has never used
       the recorder. */
    FILE *probe = fopen(TRACK_PATH, "rb");
    if (!probe) return 0;
    fclose(probe);

    if (!ls_rlog_open(&s_log, TRACK_PATH, sizeof(ls_track_pt_t),
                      LS_TRACK_CAPACITY))
        return 0;
    return ls_rlog_count(&s_log);
}

/* The point's timestamp, and where it comes from. */

static uint32_t point_time(const ls_gps_state_t *g, uint8_t *flags)
{
    if (g->year >= 2020) {
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = (int)g->year - 1900;
        tmv.tm_mon  = (int)g->month - 1;
        tmv.tm_mday = (int)g->day;
        tmv.tm_hour = (int)g->hour;
        tmv.tm_min  = (int)g->minute;
        tmv.tm_sec  = (int)g->second;

        static const int DAYS[12] = { 0, 31, 59, 90, 120, 151,
                                      181, 212, 243, 273, 304, 334 };
        const int y = tmv.tm_year + 1900;
        long days = (long)(y - 1970) * 365 + ((y - 1969) / 4)
                  - ((y - 1901) / 100) + ((y - 1601) / 400);
        days += DAYS[tmv.tm_mon];
        /* The leap day only counts once it has happened. */
        if (tmv.tm_mon > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
            days += 1;
        days += tmv.tm_mday - 1;
        *flags = LS_TRACK_F_EPOCH;
        return (uint32_t)(days * 86400L + tmv.tm_hour * 3600L
                          + tmv.tm_min * 60L + tmv.tm_sec);
    }
    *flags = 0;
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void track_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "recording to %s", TRACK_PATH);

    while (!s_stop) {
        ls_gps_state_t g;
        ls_gps_get(&g);

        if (g.fix) {
            ls_track_pt_t p;
            memset(&p, 0, sizeof(p));
            p.lat_e7 = (int32_t)(g.lat_deg * 1e7);
            p.lon_e7 = (int32_t)(g.lon_deg * 1e7);
            p.alt_m  = (int16_t)g.alt_m;
            p.sats   = g.sats_used;
            p.t      = point_time(&g, &p.flags);

            const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
            float moved = 0.0f;
            uint32_t since = 0;
            if (s_have_last) {
                moved = ls_track_distance_m(s_last.lat_e7, s_last.lon_e7,
                                            p.lat_e7, p.lon_e7);
                since = up > s_last_us_s ? up - s_last_us_s : 0;
                /* A second fix in the same second as the last kept point has
                   moved nothing and waited no time, which is the ONE case
                   ls_track_should_log treats as "the first point". Nudge it
                   so a stationary receiver cannot re-trigger that branch. */
                if (since == 0) since = 1;
            }

            if (ls_track_should_log(moved, since, s_min_move_m, s_max_gap_s)) {
                if (ls_rlog_append(&s_log, &p)) {
                    s_last = p;
                    s_have_last = true;
                    s_last_us_s = up;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "recording stopped, %d points", ls_rlog_count(&s_log));
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ls_track_rec_start(void)
{
    if (s_task) return ESP_OK;

    mkdir(TRACK_DIR, 0777);
    if (!s_log.open &&
        !ls_rlog_open(&s_log, TRACK_PATH, sizeof(ls_track_pt_t),
                      LS_TRACK_CAPACITY))
        return ESP_ERR_NOT_FOUND;

    /* The receiver has to be on, or this records a thousand seconds of
       nothing and reports itself as working. */
    if (!ls_gps_running()) {
        const esp_err_t e = ls_gps_start();
        if (e != ESP_OK) return e;
    }

    s_have_last = false;
    s_stop = false;
    if (xTaskCreate(track_task, "ls_track", 3584, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ls_track_rec_stop(void)
{
    if (!s_task) return;
    s_stop = true;
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

bool ls_track_clear(void)
{
    if (s_task) return false;
    if (!s_log.open) return false;
    s_have_last = false;
    return ls_rlog_clear(&s_log);
}

int ls_track_export(const char *path, char *out_path, size_t out_cap)
{
    /* A track on the card can be exported whether or not the
       recorder is running. It could not before: the log is opened by
       rec_start and by nothing else, so after a reboot this returned -1 and
       the console called it a write failure. */
    if (!s_log.open) ls_track_attach();
    if (!s_log.open) return LS_TRACK_EXPORT_NO_LOG;
    const int n = ls_rlog_count(&s_log);
    if (n <= 0) return 0;

    char chosen[64];
    if (path && *path) {
        snprintf(chosen, sizeof(chosen), "%s", path);
    } else {

        int i;
        for (i = 0; i < 100; i++) {
            snprintf(chosen, sizeof(chosen), "%s/track-%02d.gpx", TRACK_DIR, i);
            FILE *t = fopen(chosen, "rb");
            if (!t) break;
            fclose(t);
        }
        if (i >= 100) return LS_TRACK_EXPORT_NO_WRITE;
    }

    FILE *f = fopen(chosen, "wb");
    if (!f) return LS_TRACK_EXPORT_NO_WRITE;

    char line[256];
    int len = ls_track_gpx_header(line, sizeof(line), "LakeShark track");
    if (len) fwrite(line, 1, (size_t)len, f);

    if (s_sight_cb) s_sight_cb((void *)f);

    len = ls_track_gpx_trk_open(line, sizeof(line), "walk");
    if (len) fwrite(line, 1, (size_t)len, f);

    int written = 0;
    for (int i = 0; i < n; i++) {
        ls_track_pt_t p;
        /* One at a time: 4096 points is 64 KB and this runs on the console
           task, whose stack is under four. */
        if (ls_rlog_read_at(&s_log, i, &p) != 1) break;
        len = ls_track_gpx_point(line, sizeof(line), &p);
        if (!len) continue;
        if (fwrite(line, 1, (size_t)len, f) != (size_t)len) break;
        written++;
    }

    len = ls_track_gpx_trk_close(line, sizeof(line));
    if (len) fwrite(line, 1, (size_t)len, f);
    len = ls_track_gpx_footer(line, sizeof(line));
    if (len) fwrite(line, 1, (size_t)len, f);
    fclose(f);

    if (out_path && out_cap) snprintf(out_path, out_cap, "%s", chosen);
    return written;
}
