/*
 * diag_time.c — wall-clock offset implementation.
 *
 * Stores a single double: the (epoch - uptime) offset at the moment
 * a TIME message was received. Synthesizes dateTime by adding the
 * current uptime to the stored offset and formatting as RFC3339.
 *
 * Precision: millisecond. P25 frame events come in at ~30 Hz so
 * sub-ms precision would be theatre — and double has plenty of
 * headroom for epoch + fractional seconds well beyond 2106.
 */
#include "diag_time.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

/* Offset = wall_clock_epoch - boot_uptime_seconds.
 *
 * Read concurrently from the json/kv formatters; written rarely (once
 * per host time-set). Atomic load/store on double isn't portable, but
 * the underlying P4 RISC-V is 32-bit and uses two-word loads/stores;
 * the worst-case torn read produces a glitchy timestamp on the single
 * record bracketing the update — acceptable.
 */
static double s_offset_s     = 0.0;
static bool   s_synced       = false;

void diag_time_set_epoch(double epoch)
{
    double uptime = (double)esp_timer_get_time() / 1.0e6;
    s_offset_s = epoch - uptime;
    s_synced   = true;
}

bool diag_time_synced(void)
{
    return s_synced;
}

size_t diag_time_format_rfc3339(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 32) return 0;
    if (!s_synced) { buf[0] = 0; return 0; }

    double now = ((double)esp_timer_get_time() / 1.0e6) + s_offset_s;

    /* Split into integer epoch seconds + millisecond fraction. */
    time_t  secs = (time_t)now;
    int     ms   = (int)((now - (double)secs) * 1000.0);
    if (ms < 0)    ms = 0;
    if (ms > 999)  ms = 999;

    struct tm tm_utc;
    /* gmtime_r: thread-safe POSIX variant; ESP-IDF newlib provides it. */
    if (!gmtime_r(&secs, &tm_utc)) { buf[0] = 0; return 0; }

    int n = snprintf(buf, buf_len,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                     tm_utc.tm_year + 1900,
                     tm_utc.tm_mon  + 1,
                     tm_utc.tm_mday,
                     tm_utc.tm_hour,
                     tm_utc.tm_min,
                     tm_utc.tm_sec,
                     ms);
    if (n < 0 || (size_t)n >= buf_len) { buf[0] = 0; return 0; }
    return (size_t)n;
}
