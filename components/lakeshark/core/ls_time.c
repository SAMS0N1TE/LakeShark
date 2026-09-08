/* LS-200  Wall clock, and the honest fallback when there is not one yet.

   Two entry points that matter:

     - ls_time_render_stamp_at(): the pure-logic renderer. Given a wall time
       and an uptime it produces one of two forms, and the bench pins that
       "not synced" NEVER yields a plausible date. Lives here so the bench
       can drive it with a fixed uptime instead of esp_timer_get_time().

     - ls_time_sntp_start(): fires SNTP once station mode has an IP. The
       synced flag flips when the callback comes back. Boot is NOT blocked
       on this: the timeout the sync notification honours only bounds how
       long the callback thread stays parked, not app_main. */

#include "ls_time.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#if !LS_TIME_HOST
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char *TAG = "ls_time";
#endif

/* "Plausibly real" bound. A real GPS/NTP-set clock is post-2024; anything
   before 2024-01-01 is either 1970 or a stale RTC that was never trusted
   to hold time, and either way we prefer the honest uptime marker. The
   upper bound guards a wildly overshot NTP or a corrupt RTC.

   Time here is UNIX epoch seconds:
     2024-01-01T00:00:00Z = 1704067200
     2100-01-01T00:00:00Z = 4102444800 */
#define LS_TIME_MIN_REAL   1704067200LL
#define LS_TIME_MAX_REAL   4102444800LL

static volatile bool s_synced = false;

bool ls_time_is_synced(void) { return s_synced; }

void ls_time_test_set_synced(bool synced) { s_synced = synced; }

size_t ls_time_render_stamp_at(char *out, size_t cap,
                               time_t real_time, int64_t uptime_us)
{
    if (!out || cap == 0) return 0;

    /* The task's rule: never render a wall-clock timestamp before it has
       been set. The bounds check catches a synced-but-implausible reading
       too (an RTC that came up at 2000-01-01 has "been set", but is not a
       date we should stamp a page with). */
    long long r = (long long)real_time;
    if (r >= LS_TIME_MIN_REAL && r <= LS_TIME_MAX_REAL) {
        struct tm tm;
#if defined(_WIN32)
        gmtime_s(&tm, &real_time);
#else
        gmtime_r(&real_time, &tm);
#endif
        int n = snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (n < 0) { out[0] = '\0'; return 0; }
        if ((size_t)n >= cap) return cap - 1;
        return (size_t)n;
    }

    /* Uptime fallback. The "up " prefix is the visible marker the task
       requires: nothing the ISO branch above emits starts with "up ", so
       these two forms cannot be confused for one another, and neither can
       be confused for a real date. */
    long long secs = uptime_us / 1000000LL;
    if (secs < 0) secs = 0;
    int n = snprintf(out, cap, "up %llds", secs);
    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap - 1;
    return (size_t)n;
}

size_t ls_time_render_stamp(char *out, size_t cap)
{
    time_t now = 0;
    int64_t up = 0;

    if (s_synced) {
        now = time(NULL);
    }
#if !LS_TIME_HOST
    up = esp_timer_get_time();
#endif
    return ls_time_render_stamp_at(out, cap, now, up);
}

static size_t filename_safe(char *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (out[i] == ':' || out[i] == ' ') out[i] = '-';
    }
    return n;
}

size_t ls_time_render_filename_stamp_at(char *out, size_t cap,
                                        time_t real_time, int64_t uptime_us)
{
    return filename_safe(out, ls_time_render_stamp_at(out, cap,
                                                       real_time, uptime_us));
}

size_t ls_time_render_filename_stamp(char *out, size_t cap)
{
    return filename_safe(out, ls_time_render_stamp(out, cap));
}

static size_t join_filename(char *out, size_t cap, const char *label,
                            const char *stamp)
{
    if (!out || cap == 0 || !label || !stamp) return 0;
    int n = snprintf(out, cap, "%s_%s", label, stamp);
    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return cap - 1;
    return (size_t)n;
}

size_t ls_time_render_filename_at(char *out, size_t cap, const char *label,
                                  time_t real_time, int64_t uptime_us)
{
    char stamp[LS_TIME_STAMP_MAX];
    ls_time_render_filename_stamp_at(stamp, sizeof(stamp),
                                      real_time, uptime_us);
    return join_filename(out, cap, label, stamp);
}

size_t ls_time_render_filename(char *out, size_t cap, const char *label)
{
    char stamp[LS_TIME_STAMP_MAX];
    ls_time_render_filename_stamp(stamp, sizeof(stamp));
    return join_filename(out, cap, label, stamp);
}

#if !LS_TIME_HOST

/*LS-200  SNTP sync callback. Called on the SNTP daemon's thread; keep it
   short. All we do here is decide whether the reading is plausible and
   flip the synced flag. `now.tv_sec` is what the sntp module just wrote
   into the system clock, so `time(NULL)` and this must agree. */
static void sntp_sync_notif(struct timeval *now)
{
    time_t s = now ? now->tv_sec : time(NULL);
    if ((long long)s < LS_TIME_MIN_REAL || (long long)s > LS_TIME_MAX_REAL) {
        ESP_LOGW(TAG, "SNTP returned an implausible time (%lld) - ignoring",
                 (long long)s);
        return;
    }
    s_synced = true;
    struct tm tm;
    gmtime_r(&s, &tm);
    ESP_LOGW(TAG, "wall clock set: %04d-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/*LS-200  Fired once per boot from the WiFi got-IP path. The IDF SNTP
   helper is idempotent for the same server list, but we still gate on our
   own flag so we do not stack notification callbacks on a reconnect. */
static volatile bool s_started = false;

void ls_time_sntp_start(void)
{
    if (s_started) return;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start                 = true;
    cfg.sync_cb               = sntp_sync_notif;
    cfg.smooth_sync           = false;

    esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(e));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "SNTP started - pool.ntp.org, waiting for a reply "
                  "(boot is NOT blocked on this)");
}

void ls_time_sntp_stop(void)
{
    if (!s_started) return;
    esp_netif_sntp_deinit();
    s_started = false;
}

#else  /* LS_TIME_HOST */

/* Host-side stubs. The bench never brings up SNTP; it drives the render
   function directly and toggles the synced flag with ls_time_test_set_synced. */
void ls_time_sntp_start(void) { }
void ls_time_sntp_stop(void)  { }

#endif
