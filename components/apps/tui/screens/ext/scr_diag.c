/* Diagnostics: the numbers you want when something is wrong. */

#include "../../ls_tui_screen.h"
#include "ls_vitals.h"
#include "ls_sdcard.h"
#include "ls_crash.h"
#include "ls_version.h"
#include "ls_safe_mode.h"
#include "../../ls_tui_touch.h"
#include "../../ls_tui_ui.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_timer.h"

#include "esp_heap_caps.h"

#include "apps/p25/p25_health.h"
#include "board/ls_haptic.h"
#include "../../ls_motion.h"
#include "../../ls_notify.h"
#include "core/perf.h"
#include "radio/radio_endpoint.h"
#include "radio/radio_health.h"
#include "board/ls_lora.h"
#include "board/ls_gps.h"
#include "board/ls_imu.h"
#include "board/ls_gauge.h"
#include "ls_mesh.h"

/* Which number just moved, as opposed to which has ever moved. */

#define DIAG_ROW_KEYS 40
#define DIAG_FLASH_MS 900
static ls_fresh_t s_row_fresh[DIAG_ROW_KEYS];

static uint32_t text_token(const char *t)
{
    uint32_t h = 2166136261u;
    for (; t && *t; t++) { h ^= (uint8_t)*t; h *= 16777619u; }
    return h;
}

static void row(tui_surface *sf, tui_rect a, int y, const char *label,
                const char *value, uint8_t va)
{
    tui_put_str(sf, a, a.x + 2, a.y + y, label,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));

    const int key = ((a.x > 0) ? 20 : 0) + (y % 20);
    const uint8_t level = ls_fresh(&s_row_fresh[key], text_token(value),
                                   DIAG_FLASH_MS);
    /* Inverse of whatever the row already was, so the highlight does not
       throw away what the colour was telling you. */
    const uint8_t attr = (level >= 128)
        ? TUI_ATTR(TUI_BLACK, (uint8_t)(va & 0x0F))
        : va;

    tui_put_str(sf, a, a.x + 15, a.y + y, value, attr);
}

/* A count is green at zero and red once it moves. Nothing in this file is a
   rate: these are all "has this ever gone wrong", and the answer wants to be
   visible from across a bench. */
static uint8_t count_attr(uint32_t n)
{
    return n ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
             : TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
}

/* A row's own history, drawn to the right of its number. */

static void trend_row(tui_surface *sf, tui_rect a, int y, const ls_trend_t *t,
                      int x0, int x1)
{
    const int n = ls_trend_len(t);
    const int w = x1 - x0;
    if (n <= 1 || w < 8) return;

    /* Scaled to the window that is DRAWN, not to the whole ring - see
       ls_trend.h. Measured on the board: every visible sample identical to
       the eye, none of them equal, because one higher reading ninety seconds
       off the left-hand edge owned the top of the scale. */
    const int shown = n < w ? n : w;
    const uint16_t lo = ls_trend_min(t, shown);
    const uint16_t hi = ls_trend_max(t, shown);

    /* Newest on the right, which is the direction every chart of time is read
       in, and is also where the eye already is after the value. */
    for (int i = 0; i < w && i < n; i++) {
        const int lvl = ls_trend_level(ls_trend_at(t, i), lo, hi);
        if (lvl < 1) continue;
        /* The colour says where in its own window this sample sits, which is
           the only comparison a trace scaled to itself can honestly make. */
        const uint8_t hue = lvl >= 4 ? (TUI_GREEN | TUI_BRIGHT)
                                     : (TUI_YELLOW | TUI_BRIGHT);
        tui_put_char(sf, a, x1 - 1 - i, a.y + y, LS_TUI_TRACE(lvl),
                     TUI_ATTR(hue, TUI_BLACK));
    }
}

/* Gigabytes, because a card is measured in them.

   This stopped at megabytes, which was right while everything it formatted
   was a heap. The first thing larger to reach it was the free space on the
   card and it came out as "29718.6 MB" - a true figure that a reader has to
   count digits in, on a row whose whole purpose is to be read at a glance. */
static void human_bytes(char *buf, size_t len, uint64_t n)
{
    if (n >= 1024ull * 1024u * 1024u)
        snprintf(buf, len, "%.1f GB", (double)n / (1024.0 * 1024 * 1024));
    else if (n >= 1024u * 1024u)
        snprintf(buf, len, "%.1f MB", (double)n / (1024 * 1024));
    else if (n >= 1024u)
        snprintf(buf, len, "%.1f kB", (double)n / 1024);
    else
        snprintf(buf, len, "%u B", (unsigned)n);
}

/* The one control on this page, and why a read-only screen grew one. */

static ls_haptic_diag_t s_hw;
static bool             s_hw_read;
static tui_rect         s_test_hit = { 0, -1, 0, 0 };
static tui_rect         s_sd_hit   = { 0, -1, 0, 0 };

typedef enum {
    D_NONE = 0,
    D_ENDPOINT,
    D_MEMORY,
    D_ALERTS,
    D_SENSORS,
    D__COUNT,
} diag_detail_t;

static diag_detail_t s_detail;
static tui_rect      s_band[D__COUNT];
static tui_rect      s_back_hit = { 0, -1, 0, 0 };

/* The one read on this screen that is NOT draw-path safe, cached. */

static ls_imu_sample_t s_imu;
static bool            s_imu_ok;
static int64_t         s_detail_us;

static void hw_refresh(void)
{
    /* Talks I2C and takes about 10 ms, so it happens on entering the screen
       and on a press - never in draw, which runs every frame. */
    s_hw_read = ls_haptic_diag(&s_hw);
}

static void detail_tick(int64_t now_us)
{
    if (s_detail_us && now_us >= s_detail_us && now_us - s_detail_us < 1000000)
        return;
    s_detail_us = now_us;
    s_imu_ok = ls_imu_present() && ls_imu_read(&s_imu);
    hw_refresh();
}

static void self_test(void)
{
    hw_refresh();

    ls_notice_t n;
    memset(&n, 0, sizeof(n));
    snprintf(n.title, sizeof(n.title), "SELF TEST");
    snprintf(n.body, sizeof(n.body), "if the alerts are on, this rings and buzzes");
    n.hue = TUI_CYAN;

    n.screen = -1;
    ls_notify_post(&n);
}

static esp_err_t s_sd_answer;
static bool      s_sd_asked;

static void check_sd(void)
{
    s_sd_answer = ls_sdcard_mount();
    s_sd_asked = true;
}

static void on_enter(void) { hw_refresh(); }

static bool on_key(ls_tk_t key, char ch)
{

    if (key == LS_TK_ESC || key == LS_TK_BACKSPACE) {
        if (s_detail != D_NONE) { s_detail = D_NONE; return true; }
        return false;
    }

    if (ch == 't' || ch == 'T') { self_test(); return true; }
    if (ch == 's' || ch == 'S') { check_sd(); return true; }

    switch (ch) {
    case 'e': case 'E': s_detail = D_ENDPOINT; s_detail_us = 0; return true;
    case 'm': case 'M': s_detail = D_MEMORY;   s_detail_us = 0; return true;
    case 'a': case 'A': s_detail = D_ALERTS;   s_detail_us = 0; return true;
    case 'r': case 'R': s_detail = D_SENSORS;  s_detail_us = 0; return true;
    default: break;
    }
    return false;
}

static bool hit(tui_rect r, int col, int row)
{
    return r.h > 0 && row >= r.y && row < r.y + r.h &&
           col >= r.x && col < r.x + r.w;
}

static bool on_touch(int col, int row)
{
    /* BACK first, and while a detail is open nothing else is live -
       the buttons and bands underneath were drawn by the list, which is not
       on the screen, and a stale rect that still fired would be a control
       acting from behind a page. */
    if (s_detail != D_NONE) {
        if (hit(s_back_hit, col, row)) { s_detail = D_NONE; return true; }
        return true;                    /* see the note below */
    }

    if (hit(s_test_hit, col, row)) { self_test(); return true; }
    if (hit(s_sd_hit, col, row))   { check_sd();  return true; }

    /* The blocks, last, so a button sitting inside one still wins. */
    for (int i = 1; i < D__COUNT; i++)
        if (hit(s_band[i], col, row)) {
            s_detail = (diag_detail_t)i;
            s_detail_us = 0;
            return true;
        }

    /* EVERY TAP IN THE PANE IS CLAIMED, including the gaps. */

    return true;
}

/* --------------------------------------------------- detail -- */

static const char *const DETAIL_TITLE[D__COUNT] = {
    "", "ENDPOINT", "MEMORY", "ALERTS", "RADIOS AND SENSORS",
};

static void group(tui_surface *sf, tui_rect a, int y, const char *t)
{
    tui_put_str(sf, a, a.x + 2, a.y + y, t, TUI_ATTR(TUI_CYAN, TUI_BLACK));
}

/* Everything the endpoint registry and the health record know. */

static void detail_endpoint(tui_surface *sf, tui_rect a, uint8_t val,
                            uint8_t good, uint8_t bad, uint8_t dim)
{
    char buf[64];

    radio_health_snapshot_t rh;
    memset(&rh, 0, sizeof(rh));
    const bool have = radio_health_get(NULL, &rh);

    int y = 1;

    group(sf, a, y++, "HEALTH RECORD");
    if (!have) {
        /* again, in the place that has room to say it properly: no
           record means nothing has DRIVEN this endpoint yet, which is the
           normal state whenever the receiver is parked. */
        row(sf, a, y++, "record", "none - nothing has driven it yet", dim);
        row(sf, a, y++, "meaning", "parked, not absent", dim);
    } else {
        row(sf, a, y++, "endpoint",
            rh.endpoint_id[0] ? rh.endpoint_id : "?", val);
        const char *sn = radio_health_state_name(rh.state);
        row(sf, a, y++, "state", sn ? sn : "?",
            rh.state == RH_OK ? good
            : (rh.state == RH_FAILED || rh.state == RH_STALLED) ? bad : val);
        snprintf(buf, sizeof(buf), "%u B/s", (unsigned)rh.bytes_per_second);
        row(sf, a, y++, "throughput", buf, rh.bytes_per_second ? good : dim);
        snprintf(buf, sizeof(buf), "%d s", rh.stall_s);
        row(sf, a, y++, "stalled", buf, rh.stall_s ? bad : good);
        snprintf(buf, sizeof(buf), "%u", (unsigned)rh.recoveries);
        row(sf, a, y++, "recoveries", buf, count_attr(rh.recoveries));
        snprintf(buf, sizeof(buf), "%u up / %u down",
                 (unsigned)rh.attaches, (unsigned)rh.detaches);
        row(sf, a, y++, "attach", buf, dim);
    }

    /* Who has it. A parked receiver and a receiver another app is using look
       identical from the health record, and only one of them is a problem. */
    y++;
    const char *who = ls_tui_radio_claimed();
    row(sf, a, y++, "claimed by", who && *who ? who : "nobody",
        who ? val : dim);

    y++;
    group(sf, a, y++, "DECLARED ENDPOINTS");
    const size_t n = ls_radio_endpoint_count();
    if (!n) {
        row(sf, a, y, "none", "this build declares no receiver", dim);
        return;
    }
    for (size_t i = 0; i < n && y < a.h - 2; i++) {
        ls_radio_endpoint_info_t info;
        if (ls_radio_endpoint_info(i, &info) != LS_RADIO_OK) continue;

        /* Plugged in, leased and streaming are three different
           states and the list above collapses all of them to one word. A
           dongle that is present and leased but not streaming is a receiver
           somebody stopped; one present and unleased is idle; one absent is
           a cable. Those want different actions. */
        char what[48];
        if (!info.present) {
            snprintf(what, sizeof(what), "not plugged in");
        } else if (info.streaming) {
            snprintf(what, sizeof(what), "streaming to %.24s",
                     info.owner[0] ? info.owner : "?");
        } else if (info.leased) {
            snprintf(what, sizeof(what), "held by %.24s, stopped",
                     info.owner[0] ? info.owner : "?");
        } else {
            snprintf(what, sizeof(what), "plugged in, idle");
        }
        row(sf, a, y++, info.endpoint_id[0] ? info.endpoint_id : "?", what,
            info.present ? (info.streaming ? good : val) : dim);

        /* What it has actually moved since boot, which is the one figure
           that cannot be argued with about whether the path works. */
        if (info.present && y < a.h - 2) {
            char seen[48];
            human_bytes(buf, sizeof(buf), info.bytes_read);
            snprintf(seen, sizeof(seen), "%.16s in %llu packets", buf,
                     (unsigned long long)info.packets_read);
            row(sf, a, y++, "  read", seen, info.bytes_read ? good : dim);
        }
    }
}

static void detail_memory(tui_surface *sf, tui_rect a, uint8_t val,
                          uint8_t good, uint8_t bad, uint8_t dim)
{
    char buf[64], b2[24], b3[24];

    static const struct { const char *name; unsigned caps; } POOL[] = {
        { "internal", MALLOC_CAP_INTERNAL },
        { "dma",      MALLOC_CAP_DMA },
        { "psram",    MALLOC_CAP_SPIRAM },
    };

    group(sf, a, 1, "FREE / LARGEST BLOCK / LOWEST EVER");
    for (int i = 0; i < 3; i++) {
        multi_heap_info_t hi;
        heap_caps_get_info(&hi, POOL[i].caps);
        human_bytes(buf, sizeof(buf), hi.total_free_bytes);
        human_bytes(b2, sizeof(b2), hi.largest_free_block);
        human_bytes(b3, sizeof(b3), hi.minimum_free_bytes);
        /* Padded, because three numbers under one heading are a table and a
           table that does not line up has to be read a row at a time. */
        char line[64];
        snprintf(line, sizeof(line), "%-9.9s %-9.9s %-9.9s", buf, b2, b3);
        /* A largest block far under the free total is fragmentation, and it
           is what actually refuses a big allocation. */
        const bool tight = hi.minimum_free_bytes &&
                           hi.minimum_free_bytes < 16u * 1024u &&
                           POOL[i].caps != MALLOC_CAP_SPIRAM;
        row(sf, a, 2 + i, POOL[i].name, line, tight ? bad : good);
    }

    /* And which way each of them has been going, which is the
       question a single reading cannot answer and the reason this page was
       worth adding the DMA pool to. */
    group(sf, a, 6, "WHAT THE TRACES COVER");
    const int held = ls_trend_len(ls_vitals_internal());
    if (held > 1) {
        const uint32_t watched = ls_vitals_watched_s();
        snprintf(buf, sizeof(buf), "%d samples over %lu s",
                 held, (unsigned long)watched);
        row(sf, a, 7, "window", buf, dim);

        static const struct {
            const char *name;
            const ls_trend_t *(*get)(void);
        } SPAN[] = {
            { "internal", ls_vitals_internal },
            { "dma",      ls_vitals_dma },
        };
        for (int i = 0; i < 2; i++) {
            const ls_trend_t *t = SPAN[i].get();
            const uint16_t lo = ls_trend_min(t, 0);
            const uint16_t hi2 = ls_trend_max(t, 0);
            human_bytes(buf, sizeof(buf), (uint64_t)lo * 64u);
            human_bytes(b2, sizeof(b2), (uint64_t)hi2 * 64u);
            char line[64];
            snprintf(line, sizeof(line), "%.20s to %.20s", buf, b2);
            /* Both ends of the window, not just the low one: a pool that has
               never moved and one that has fallen and come back read the
               same from a minimum alone. */
            row(sf, a, 8 + i, SPAN[i].name, line,
                lo == hi2 ? dim : val);
        }
    } else {
        row(sf, a, 7, "window", "nothing sampled yet", dim);
    }

    group(sf, a, 11, "WHY IT MATTERS HERE");
    row(sf, a, 12, "psram", "not valid for DMA, nor for a stack", dim);
    row(sf, a, 13, "dma", "what a driver buffer has to come from", dim);
}

/* The motor's own report, in full, and what it is being asked for.

   The list has room for the coil and the tracked resonance. This adds the
   header figure they are being compared AGAINST, which is what turns a
   reading into a verdict - records that the header number is a guess
   and that this measurement is what can correct it - and the latched faults,
   which are the difference between a driver that refused and a motor that is
   not there. */
static void detail_alerts(tui_surface *sf, tui_rect a, uint8_t val,
                          uint8_t good, uint8_t bad, uint8_t dim)
{
    char buf[64];

    group(sf, a, 1, "AW86224");
    if (!s_hw_read) {
        row(sf, a, 2, "driver", "no answer on the bus", dim);
    } else {
        snprintf(buf, sizeof(buf), "%.2f ohms", (double)s_hw.lra_ohms);
        row(sf, a, 2, "coil", buf,
            s_hw.lra_ohms < 1.0f ? bad : s_hw.lra_ohms > 60.0f ? val : good);
        /* The two ends of the same question, on adjacent rows on purpose. */
        snprintf(buf, sizeof(buf), "%.1f Hz", (double)s_hw.f0_hz);
        row(sf, a, 3, "tracked", s_hw.f0_hz > 0.0f ? buf : "nothing yet",
            s_hw.f0_hz > 0.0f ? val : dim);
#ifdef LS_BOARD_HAPTIC_F0_HZ
        snprintf(buf, sizeof(buf), "%d Hz", LS_BOARD_HAPTIC_F0_HZ);
        row(sf, a, 4, "header says", buf, dim);
#endif
        snprintf(buf, sizeof(buf), "%.2f V", (double)s_hw.vdd_v);
        row(sf, a, 5, "supply", buf, val);

        if (s_hw.over_current || s_hw.over_temp || s_hw.under_voltage) {
            snprintf(buf, sizeof(buf), "%s%s%s",
                     s_hw.over_current ? "over-current " : "",
                     s_hw.over_temp ? "over-temp " : "",
                     s_hw.under_voltage ? "under-volt" : "");
            row(sf, a, 6, "fault", buf, bad);
        } else {
            row(sf, a, 6, "fault", "none latched", good);
        }
    }

    group(sf, a, 8, "WHAT AN ALERT DOES");
    row(sf, a, 9, "sound", ls_notify_ring() ? "on" : "off",
        ls_notify_ring() ? good : dim);
    row(sf, a, 10, "vibrate", ls_notify_vibe() ? "on" : "off",
        ls_notify_vibe() ? good : dim);
    /* The measured reason the pulse length matters, on the page where
       somebody is asking why they cannot feel it. */
    row(sf, a, 12, "note", "below 20 ms the LRA never starts", dim);
}

static void detail_sensors(tui_surface *sf, tui_rect a, uint8_t val,
                           uint8_t good, uint8_t bad, uint8_t dim)
{
    char buf[64];

    group(sf, a, 1, "IMU ICM20948");
    if (!s_imu_ok) {
        row(sf, a, 2, "sensor", ls_imu_present() ? "read failed"
                                                 : "not fitted",
            ls_imu_present() ? bad : dim);
    } else {
        const float g = sqrtf(s_imu.ax * s_imu.ax + s_imu.ay * s_imu.ay +
                              s_imu.az * s_imu.az);
        /* Screen axes, named, because the mounting has been got wrong
           twice and both times the argument was conducted in the dark. */
        snprintf(buf, sizeof(buf), "%+.2f %+.2f %+.2f  |a| %.2f",
                 (double)s_imu.ax, (double)s_imu.ay, (double)s_imu.az,
                 (double)g);
        /* At rest the magnitude is gravity whichever way it is held, so a
           number far from 1.00 is a scale factor wrong and not a board being
           moved - the cheapest health check this part has. */
        row(sf, a, 2, "accel g", buf,
            (g > 0.85f && g < 1.15f) ? good : val);

        snprintf(buf, sizeof(buf), "%+.1f %+.1f %+.1f", (double)s_imu.gx,
                 (double)s_imu.gy, (double)s_imu.gz);
        row(sf, a, 3, "gyro dps", buf, val);

        if (s_imu.mag_valid) {
            const float m = sqrtf(s_imu.mx * s_imu.mx + s_imu.my * s_imu.my +
                                  s_imu.mz * s_imu.mz);
            snprintf(buf, sizeof(buf), "%+.0f %+.0f %+.0f  |m| %.0f",
                     (double)s_imu.mx, (double)s_imu.my, (double)s_imu.mz,
                     (double)m);
            /* 25 to 65 uT is the earth's field; far outside it means
               something ferrous is close, or the part wants calibrating. */
            row(sf, a, 4, "mag uT", buf,
                (m > 20.0f && m < 80.0f) ? good : val);
            snprintf(buf, sizeof(buf), "%.0f deg   %.1f C",
                     (double)ls_imu_heading(), (double)s_imu.temp_c);
            row(sf, a, 5, "heading", buf, val);
        } else {
            row(sf, a, 4, "mag uT", "the magnetometer did not answer", bad);
        }
    }

    group(sf, a, 7, "GPS L76K");
    {
        ls_gps_state_t gp;
        ls_gps_get(&gp);
        if (!ls_gps_running()) {
            row(sf, a, 8, "state", "powered down", dim);
        } else {
            snprintf(buf, sizeof(buf), "%s  %u used / %u seen",
                     gp.fix ? "fix" : gp.alive ? "searching" : "silent",
                     (unsigned)gp.sats_used, (unsigned)gp.sats_visible);
            row(sf, a, 8, "state", buf,
                gp.fix ? good : gp.alive ? val : bad);
        }
        /*'s own note, and the reason these three are worth a row:
           bytes climbing with sentences flat means the baud rate is wrong,
           and both climbing with no fix means the antenna is the suspect. */
        snprintf(buf, sizeof(buf), "%lu B  %lu ok  %lu bad",
                 (unsigned long)gp.bytes, (unsigned long)gp.sentences,
                 (unsigned long)gp.checksum_errors);
        row(sf, a, 9, "wire", buf,
            gp.checksum_errors ? val : gp.bytes ? good : dim);
    }

    group(sf, a, 11, "LORA SX1262 / MESHCORE");
    if (!ls_lora_present()) {
        row(sf, a, 12, "radio", "not fitted", dim);
    } else {
        ls_mesh_stats_t ms;
        memset(&ms, 0, sizeof(ms));
        ls_mesh_get_stats(&ms);

        snprintf(buf, sizeof(buf), "%lu rx  %lu bad  %lu tx",
                 (unsigned long)ms.rx_packets, (unsigned long)ms.rx_bad,
                 (unsigned long)ms.tx_packets);
        row(sf, a, 12, "packets", buf, ms.rx_bad ? val : good);

        /*'s trap, and this is the page where it is worth naming: a
           spreading factor mismatch gives zero packets AND zero CRC errors,
           which is byte for byte what an empty room gives. */
        if (!ms.rx_packets && !ms.rx_bad)
            row(sf, a, 13, "note", "silent is also what a wrong SF gives",
                dim);
        else {
            snprintf(buf, sizeof(buf), "%.0f dBm  %.1f dB",
                     (double)ms.last_rssi, (double)ms.last_snr);
            row(sf, a, 13, "last heard", buf, val);
        }

        snprintf(buf, sizeof(buf), "%lu ms used  %lu ms left",
                 (unsigned long)ms.airtime_ms,
                 (unsigned long)ms.tx_budget_ms);
        row(sf, a, 14, "airtime", buf, ms.tx_budget_ms ? good : bad);
        row(sf, a, 15, "our id", ms.self_id[0] ? ms.self_id : "-", dim);
    }

    group(sf, a, 17, "BATTERY");
    {
        ls_gauge_t gz;
        if (!ls_gauge_get(&gz) || !gz.millivolts) {
            row(sf, a, 18, "gauge", "no reading", dim);
        } else {
            snprintf(buf, sizeof(buf), "%.2f V  %d mA", gz.millivolts / 1000.0,
                     (int)gz.milliamps);
            row(sf, a, 18, "pack", buf, gz.milliamps ? val : dim);

            snprintf(buf, sizeof(buf), "%u %%  %u of %u mAh  %.1f C",
                     (unsigned)gz.percent, (unsigned)gz.remaining_mah,
                     (unsigned)gz.full_mah, gz.temp_c10 / 10.0);
            row(sf, a, 19, "learned", buf, dim);
            if (gz.milliamps == 0)
                row(sf, a, 20, "note", "not sourcing - this is the charger",
                    dim);
        }
    }
}

static void draw_detail(tui_surface *sf, tui_rect a, int64_t now_us)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t bad   = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    detail_tick(now_us);

    tui_box(sf, a, DETAIL_TITLE[s_detail], frame);

    /* The page is drawn into the frame's interior, less the room BACK takes
       at the bottom, so a page can never reach under its own way out. */
    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 4);
    if (body.h < 6) body = a;

    switch (s_detail) {
    case D_ENDPOINT: detail_endpoint(sf, body, val, good, bad, dim); break;
    case D_MEMORY:   detail_memory(sf, body, val, good, bad, dim);   break;
    case D_ALERTS:   detail_alerts(sf, body, val, good, bad, dim);   break;
    case D_SENSORS:  detail_sensors(sf, body, val, good, bad, dim);  break;
    default: break;
    }

    const int w = a.w - 4 < 24 ? a.w - 4 : 24;
    tui_rect b = tui_rect_make(a.x + 2, a.y + a.h - 4, w, 3);
    if (w > 8 && b.y > a.y + 1) {
        ls_panel_box(sf, b, NULL, TUI_CYAN | TUI_BRIGHT);
        ls_fill_dither(sf, tui_rect_make(b.x + 1, b.y + 1, b.w - 2, b.h - 2),
                       LS_DITHER_LIGHT, TUI_CYAN);
        const char *label = "ESC  BACK";
        const int at = b.x + (b.w - (int)strlen(label)) / 2;
        tui_put_str(sf, b, at, b.y + 1, label,
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        s_back_hit = b;
    } else {
        s_back_hit = tui_rect_make(0, -1, 0, 0);
    }
}

/* The left half, as its own function.

   It was inline in draw() and had to come out when the detail pages arrived:
   the two are alternatives for the same rectangle, and expressing that with
   the list still inline meant either a goto over its declarations or wrapping
   three hundred lines in an else. A name is cheaper than both and says what
   the rectangle holds. Nothing inside moved. */
static void draw_list(tui_surface *sf, tui_rect left)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t bad   = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48], b2[24];

    /* ---- radio endpoint ------------------------------------------------ */
    tui_box(sf, left, "RADIO", frame);

    radio_health_snapshot_t rh;
    memset(&rh, 0, sizeof(rh));
    bool have_rh = radio_health_get(NULL, &rh);

    if (!have_rh) {
        /* "none attached" is not the same as "not streaming", and this page said the first when it meant the second. */

        const size_t n = ls_radio_endpoint_count();
        ls_radio_endpoint_info_t info;
        bool present = false;
        for (size_t i = 0; i < n; i++)
            if (ls_radio_endpoint_info(i, &info) == LS_RADIO_OK && info.present) {
                present = true;
                break;
            }
        if (present)
            row(sf, left, 2, "endpoint", "attached, parked", val);
        else if (n)
            row(sf, left, 2, "endpoint", "declared, not plugged in", dim);
        else
            row(sf, left, 2, "endpoint", "none attached", dim);
    } else {
        row(sf, left, 2, "endpoint", rh.endpoint_id[0] ? rh.endpoint_id : "?", val);

        const char *sn = radio_health_state_name(rh.state);
        row(sf, left, 3, "state", sn ? sn : "?",
            rh.state == RH_OK ? good
            : (rh.state == RH_FAILED || rh.state == RH_STALLED) ? bad : val);

        snprintf(buf, sizeof(buf), "%u B/s", (unsigned)rh.bytes_per_second);
        row(sf, left, 4, "throughput", buf,
            rh.bytes_per_second ? good : dim);

        snprintf(buf, sizeof(buf), "%d s", rh.stall_s);
        row(sf, left, 5, "stalled", buf,
            rh.stall_s ? bad : good);

        snprintf(buf, sizeof(buf), "%u", (unsigned)rh.recoveries);
        row(sf, left, 6, "recoveries", buf, count_attr(rh.recoveries));

        snprintf(buf, sizeof(buf), "%u up / %u down",
                 (unsigned)rh.attaches, (unsigned)rh.detaches);
        row(sf, left, 7, "attach", buf, dim);
    }

    /* ---- memory --------------------------------------------------------
       Largest free block, not just total: fragmentation is what actually
       kills a large allocation, and the two diverge long before free hits
       zero. */
    size_t heap_int  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t heap_ps   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    tui_put_str(sf, left, left.x + 2, left.y + 9, "MEMORY",
                TUI_ATTR(TUI_CYAN, TUI_BLACK));

    /* The all-time low goes on the heading row, and it is the number
       that actually decides whether this board is about to fail.

       It is not sampled: heap_caps_get_info maintains minimum_free_bytes from
       every allocation, including the ones between two samples of the traces
       below. A watermark taken by polling can miss the spike that mattered;
       this one cannot, and it survives every screen change. */
    const int tx0 = left.x + 26;
    const int tx1 = left.x + left.w - 2;

    const uint32_t low_int = ls_vitals_internal_low();
    if (low_int) {
        human_bytes(b2, sizeof(b2), low_int);
        snprintf(buf, sizeof(buf), "low %s", b2);
        tui_put_str(sf, left, left.x + 15, left.y + 9, buf,
                    low_int < 16 * 1024 ? bad : dim);
    }

    /* And how much the traces actually cover, on the same row.

       Not uptime, and saying so matters: the sampler runs in the shell's
       frame loop, so it stops while the TUI is off and across the teardown a
       rotation costs. "4 min" under a line with a gap in it would be a claim
       about a record that was not kept. */
    {
        const uint32_t watched = ls_vitals_watched_s();
        if (ls_trend_len(ls_vitals_internal()) > 1) {
            if (watched < 120) snprintf(buf, sizeof(buf), "%lu s watched",
                                        (unsigned long)watched);
            else               snprintf(buf, sizeof(buf), "%lu min watched",
                                        (unsigned long)(watched / 60));
        } else {
            snprintf(buf, sizeof(buf), "no history yet");
        }
        const int x = tx1 - (int)strlen(buf);
        if (x > left.x + 27)
            tui_put_str(sf, left, x, left.y + 9, buf, dim);
    }

    human_bytes(buf, sizeof(buf), heap_int);
    row(sf, left, 10, "internal", buf,
        heap_int < 32 * 1024 ? bad : heap_int < 64 * 1024 ? val : good);
    trend_row(sf, left, 10, ls_vitals_internal(), tx0, tx1);

    human_bytes(buf, sizeof(buf), heap_big);
    row(sf, left, 11, "largest blk", buf,
        heap_big < 16 * 1024 ? bad : good);
    trend_row(sf, left, 11, ls_vitals_largest(), tx0, tx1);

    human_bytes(buf, sizeof(buf), heap_ps);
    row(sf, left, 12, "psram", buf, heap_ps ? good : dim);
    trend_row(sf, left, 12, ls_vitals_psram(), tx0, tx1);

    tui_put_str(sf, left, left.x + 2, left.y + 14, "ALERTS",
                TUI_ATTR(TUI_CYAN, TUI_BLACK));

    if (!ls_haptic_present()) {
        row(sf, left, 15, "haptic", "no driver", dim);
    } else if (!s_hw_read) {
        row(sf, left, 15, "haptic", "up, not measured", val);
    } else {
        snprintf(buf, sizeof(buf), "%.1f ohm coil", (double)s_hw.lra_ohms);
        row(sf, left, 15, "haptic", buf,
            s_hw.lra_ohms < 1.0f ? bad : good);

        if (s_hw.f0_hz > 0.0f)
            snprintf(buf, sizeof(buf), "%.1f Hz  %.2f V",
                     (double)s_hw.f0_hz, (double)s_hw.vdd_v);
        else
            snprintf(buf, sizeof(buf), "not run yet  %.2f V", (double)s_hw.vdd_v);
        row(sf, left, 16, "motor", buf, val);

        if (s_hw.over_current || s_hw.over_temp || s_hw.under_voltage) {
            snprintf(buf, sizeof(buf), "%s%s%s",
                     s_hw.over_current  ? "over-current " : "",
                     s_hw.over_temp     ? "over-temp " : "",
                     s_hw.under_voltage ? "under-volt" : "");
            row(sf, left, 17, "fault", buf, bad);
        }
    }

    snprintf(buf, sizeof(buf), "sound %s   vibrate %s",
             ls_notify_ring() ? "on" : "off",
             ls_notify_vibe() ? "on" : "off");
    row(sf, left, 18, "set to", buf,
        (ls_notify_ring() || ls_notify_vibe()) ? good : dim);

    /* A box you press, sized from its rect - the treatment SETTINGS
       and RADIOS use, because a one-cell row is 1.3 mm on this panel. */
    {
        const int room = left.w - 4;

        const int pair = (room - 2) / 2;
        const bool side_by_side = pair >= 14;
        const int w = side_by_side ? (pair > 24 ? 24 : pair)
                                   : (room < 24 ? room : 24);

        tui_rect b = tui_rect_make(left.x + 2, left.y + 20, w, 3);
        if (b.y + b.h <= left.y + left.h && w > 8) {
            ls_panel_box(sf, b, NULL, TUI_CYAN | TUI_BRIGHT);
            ls_fill_dither(sf, tui_rect_make(b.x + 1, b.y + 1, b.w - 2, b.h - 2),
                           LS_DITHER_LIGHT, TUI_CYAN);
            const char *label = "T  TEST ALERT";
            const int at = b.x + (b.w - (int)strlen(label)) / 2;
            tui_put_str(sf, b, at, b.y + 1, label,
                        TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            s_test_hit = b;
        } else {
            s_test_hit = tui_rect_make(0, -1, 0, 0);
        }

        if (side_by_side && s_test_hit.h > 0) {
            tui_rect c = tui_rect_make(b.x + w + 2, b.y, w, 3);
            ls_panel_box(sf, c, NULL, TUI_CYAN | TUI_BRIGHT);
            ls_fill_dither(sf, tui_rect_make(c.x + 1, c.y + 1, c.w - 2, c.h - 2),
                           LS_DITHER_LIGHT, TUI_CYAN);
            const char *label = "S  CHECK SD";
            const int at = c.x + (c.w - (int)strlen(label)) / 2;
            tui_put_str(sf, c, at, c.y + 1, label,
                        TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            s_sd_hit = c;
        } else {
            s_sd_hit = tui_rect_make(0, -1, 0, 0);
        }
    }

    /* The rest of the board. */

    {
        /* One row higher, and asking for what it actually needs. */

        const int y0 = left.y + 23;
        if (y0 + 5 <= left.y + left.h - 1) {
            tui_put_str(sf, left, left.x + 2, y0, "RADIOS AND SENSORS",
                        TUI_ATTR(TUI_CYAN, TUI_BLACK));

            /* Fitted or not is a board fact and never changes; what it is
               DOING is the part worth a colour. */
            row(sf, left, 24, "lora sx1262",
                ls_lora_present()
                    ? (ls_mesh_running() ? "up, mesh running" : "up, idle")
                    : "not fitted",
                ls_lora_present() ? good : dim);

            {
                ls_gps_state_t g;
                ls_gps_get(&g);
                if (!ls_gps_running()) {
                    row(sf, left, 25, "gps l76k", "powered down", dim);
                } else {
                    /* Three states and not two, the same distinction
                       ls_gps.h was built around: silent, talking, fixed. */
                    snprintf(buf, sizeof(buf), "%s  %u/%u sats",
                             g.fix ? "fix" : g.alive ? "searching" : "silent",
                             (unsigned)g.sats_used,
                             (unsigned)g.sats_visible);
                    row(sf, left, 25, "gps l76k", buf,
                        g.fix ? good : g.alive ? val : bad);
                }
            }

            {
                const ls_imu_pose_t pose = ls_imu_pose();

                static const char *const POSE[] = {
                    "flat", "portrait", "portrait flip",
                    "landscape", "landscape flip",
                };
                row(sf, left, 26, "imu icm20948",
                    ls_imu_present()
                        ? (pose <= LS_IMU_RIGHT ? POSE[pose] : "?")
                        : "not fitted",
                    ls_imu_present() ? good : dim);
            }

            {
                ls_gauge_t gz;
                if (!ls_gauge_get(&gz) || !gz.millivolts) {
                    row(sf, left, 27, "battery", "no gauge reading", dim);
                } else if (gz.milliamps == 0) {

                    snprintf(buf, sizeof(buf), "%.2f V, not sourcing (USB)",
                             gz.millivolts / 1000.0);
                    row(sf, left, 27, "battery", buf, dim);
                } else {
                    snprintf(buf, sizeof(buf), "%.2f V  %d mA",
                             gz.millivolts / 1000.0, (int)gz.milliamps);
                    row(sf, left, 27, "battery", buf,
                        gz.millivolts < 3400 ? bad
                        : gz.millivolts < 3700 ? val : good);

                    int pct = ((int)gz.millivolts - 3300) * 100 / 900;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    /* The bar is the block's sixth row and only
                       exists while the pack is sourcing, so it is asked for
                       on its own - a landscape half stops one row short of
                       it and the four figures above matter more. */
                    const int bw = left.w - 18;
                    if (bw > 4 && left.y + 28 < left.y + left.h - 1) {
                        const int fill = bw * pct / 100;
                        for (int i = 0; i < bw; i++)
                            tui_put_char(sf, left, left.x + 15 + i,
                                         left.y + 28,
                                         i < fill ? LS_TUI_BLOCK_FULL
                                                  : LS_TUI_TRACE(1),
                                         i < fill
                                            ? TUI_ATTR(pct < 20 ? TUI_RED
                                                     : pct < 50 ? TUI_YELLOW
                                                                : TUI_GREEN,
                                                       TUI_BLACK)
                                            : LS_ATTR_FAINT);
                    }
                }
            }
        }
    }

}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t bad   = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48], b2[24];

    tui_rect left, right;
    ls_tui_split(area, &left, &right);

    /* The left half is either the list or one page of detail, never
       both. The right half is drawn below either way, so the decode chain and
       the card stay on the screen while a block is open. */
    if (s_detail != D_NONE) {
        /* The list is not on the screen, so neither are its controls. A rect
           left behind would be a button acting from under a page. */
        for (int i = 0; i < D__COUNT; i++)
            s_band[i] = tui_rect_make(0, -1, 0, 0);
        s_test_hit = tui_rect_make(0, -1, 0, 0);
        s_sd_hit   = tui_rect_make(0, -1, 0, 0);
        draw_detail(sf, left, esp_timer_get_time());
    } else {
        s_back_hit = tui_rect_make(0, -1, 0, 0);

        s_band[D_ENDPOINT] = tui_rect_make(left.x + 1, left.y + 1,
                                           left.w - 2, 8);
        s_band[D_MEMORY]   = tui_rect_make(left.x + 1, left.y + 9,
                                           left.w - 2, 5);
        s_band[D_ALERTS]   = tui_rect_make(left.x + 1, left.y + 14,
                                           left.w - 2, 6);
        s_band[D_SENSORS]  = tui_rect_make(left.x + 1, left.y + 23,
                                           left.w - 2, 5);
        /* A band that does not fit the pane is not offered: the rows it
           would open on are not being drawn either. */
        for (int i = 1; i < D__COUNT; i++)
            if (s_band[i].y + s_band[i].h > left.y + left.h - 1)
                s_band[i] = tui_rect_make(0, -1, 0, 0);

        draw_list(sf, left);
    }

    /* ---- p25 decode chain ---------------------------------------------- */
    tui_box(sf, right, "P25 CHAIN", frame);

    p25_health_snapshot_t ph;
    memset(&ph, 0, sizeof(ph));
    if (!p25_health_read(&ph)) {
        row(sf, right, 2, "decoder", "not running", dim);
    } else {
        row(sf, right, 2, "demod",
            ph.effective_demod[0] ? ph.effective_demod : "?", val);

        snprintf(buf, sizeof(buf), "%u ok / %u bad",
                 (unsigned)ph.nid_valid, (unsigned)ph.nid_invalid);
        row(sf, right, 3, "nid", buf, ph.nid_invalid ? val : good);

        snprintf(buf, sizeof(buf), "%u ok / %u bad",
                 (unsigned)ph.tsbk_valid, (unsigned)ph.tsbk_invalid);
        row(sf, right, 4, "tsbk", buf, ph.tsbk_invalid ? val : good);

        snprintf(buf, sizeof(buf), "%u / %u", (unsigned)ph.ring_fill,
                 (unsigned)ph.ring_size);
        /* A ring that is nearly full is about to drop, which shows up later
           as audio damage with no obvious cause. Flag it before it does. */
        row(sf, right, 5, "ring", buf,
            (ph.ring_size && ph.ring_fill * 4 > ph.ring_size * 3) ? bad : good);

        snprintf(buf, sizeof(buf), "%u", (unsigned)ph.usb_read_errors);
        row(sf, right, 6, "usb errors", buf, count_attr(ph.usb_read_errors));

        human_bytes(b2, sizeof(b2), ph.usb_dropped_bytes);
        row(sf, right, 7, "usb dropped", b2,
            count_attr((uint32_t)ph.usb_dropped_bytes));

        snprintf(buf, sizeof(buf), "%u drop / %u under",
                 (unsigned)ph.audio_drops, (unsigned)ph.audio_underruns);
        row(sf, right, 8, "audio", buf,
            count_attr(ph.audio_drops + ph.audio_underruns));

        if (ph.rf_level_permille) {
            snprintf(buf, sizeof(buf), "%u.%u %%",
                     (unsigned)(ph.rf_level_permille / 10),
                     (unsigned)(ph.rf_level_permille % 10));
            row(sf, right, 9, "rf level", buf, val);
        }

        if (ph.cpu_valid) {
            snprintf(buf, sizeof(buf), "%u %% / %u %%",
                     (unsigned)ph.cpu_core0_pct, (unsigned)ph.cpu_core1_pct);
            row(sf, right, 10, "cpu 0/1", buf,
                (ph.cpu_core0_pct > 90 || ph.cpu_core1_pct > 90) ? bad : good);
        }

        if (ph.unhandled_total) {
            snprintf(buf, sizeof(buf), "%u in %u kinds",
                     (unsigned)ph.unhandled_total,
                     (unsigned)ph.unhandled_distinct);
            row(sf, right, 11, "unhandled", buf, val);
        }
    }

    /* STORAGE AND FIRMWARE, in the seventeen rows this panel has been leaving empty in both postures. */

    if (right.h >= 24) {
        tui_put_str(sf, right, right.x + 2, right.y + 13,
                    "STORAGE AND FIRMWARE", TUI_ATTR(TUI_CYAN, TUI_BLACK));

        if (ls_sdcard_mounted()) {
            uint64_t total = 0, freeb = 0;
            const char *name = ls_sdcard_name();
            if (ls_sdcard_size(&total, &freeb) && total) {
                human_bytes(b2, sizeof(b2), freeb);
                snprintf(buf, sizeof(buf), "%s  %s free",
                         name ? name : "mounted", b2);
            } else {
                snprintf(buf, sizeof(buf), "%s  mounted",
                         name ? name : "card");
            }
            /* Nearly full is the state that turns every log on this unit
               into a silent no-op, so it is the one worth a colour. */
            row(sf, right, 14, "sd card", buf,
                (total && freeb * 20 < total) ? val : good);
        } else if (s_sd_asked && s_sd_answer == ESP_ERR_NOT_SUPPORTED) {

            row(sf, right, 14, "sd card", "no slot on this board", dim);
        } else if (s_sd_asked) {
            snprintf(buf, sizeof(buf), "not mounted - %s",
                     esp_err_to_name(s_sd_answer));
            row(sf, right, 14, "sd card", buf, bad);
        } else {
            row(sf, right, 14, "sd card", "not mounted", bad);
        }

        {
            ls_version_info_t v;
            ls_version_get(&v);
            snprintf(buf, sizeof(buf), "%s%s", v.version ? v.version : "?",
                     ls_version_is_dirty(v.version) ? "  DIRTY" : "");
            row(sf, right, 15, "firmware", buf,
                ls_version_is_dirty(v.version) ? val : good);
            row(sf, right, 16, "board", v.board ? v.board : "?", dim);
        }

        /* Whether one is stored, said in this screen's own words. */

        row(sf, right, 17, "last crash",
            ls_crash_present() ? "a dump is stored" : "none recorded",
            ls_crash_present() ? bad : good);

        /* WHAT KIND OF BOOT THIS IS, which is the question the row above cannot answer and the one that actually matters. */

        if (right.h >= 26) {
            const ls_safe_boot_t *sb = ls_safe_boot_result();
            if (sb) {
                const char *why = ls_safe_reset_name(sb->reset);
                snprintf(buf, sizeof(buf), "%s%s", why ? why : "?",
                         sb->safe ? "   IN SAFE MODE" : "");
                row(sf, right, 18, "this boot", buf,
                    sb->safe ? bad
                    : sb->reset_class == LS_SAFE_CLASS_FAULT ? bad
                    : sb->reset_class == LS_SAFE_CLASS_POWER ? val
                    : good);

                /* Only when it is not zero. A counter reading nought on a
                   healthy unit is a row that never says anything, and this
                   panel has better uses for a line. */
                if (sb->faults || sb->power_events) {
                    snprintf(buf, sizeof(buf), "%lu failed start%s  %lu power",
                             (unsigned long)sb->faults,
                             sb->faults == 1 ? "" : "s",
                             (unsigned long)sb->power_events);
                    row(sf, right, 19, "counted", buf, val);
                }
            }
        }
    }

    uint32_t us = 0; int cells = 0;
    ls_tui_last_cost(&us, &cells);
    snprintf(buf, sizeof(buf), "%u cells  %u us", (unsigned)cells, (unsigned)us);
    row(sf, right, right.h - 4, "last frame", buf,
        us > 8000 ? bad : us > 4000 ? val : good);

    /* Rotations, and what the last rebuild cost. A number that grows with
       every turn says the teardown is leaving something behind; one that
       stays flat says to look at the receiver instead. */
    snprintf(buf, sizeof(buf), "%u turns  last %u ms",
             (unsigned)ls_tui_rebuild_count(), (unsigned)ls_tui_rebuild_ms());
    row(sf, right, right.h - 5, "rebuild", buf,
        ls_tui_rebuild_ms() > 400 ? bad : ls_tui_rebuild_ms() > 200 ? val : good);

    /* Touch, because a tap that does nothing looks exactly like a tap that
       missed. Reads climbing with taps flat means the controller answers and
       the gesture is being rejected; both flat means nothing is arriving. */
    uint32_t reads = 0, taps = 0;
    int tc = -1, tr = -1;
    ls_tui_touch_stats(&reads, &taps, &tc, &tr);
    if (taps)
        snprintf(buf, sizeof(buf), "%lu reads  %lu taps  last %d,%d",
                 (unsigned long)reads, (unsigned long)taps, tc, tr);
    else
        snprintf(buf, sizeof(buf), "%lu reads  no taps yet",
                 (unsigned long)reads);
    row(sf, right, right.h - 3, "touch", buf,
        taps ? good : reads ? val : dim);
}

const ls_tui_screen_t ls_scr_diag = {
    .name = "DIAG",
    /* What the colours mean, said accurately. "RED is moving" read
       as "red while it is moving", and these counters are cumulative - red
       means it moved at some point, which is a different and more useful
       claim. The highlight is the one that means now. */
    .hint = "TAP A BLOCK for detail  E M A R  T alerts  S card  ESC back",
    .enter = on_enter,
    .leave = NULL,
    .draw = draw,
    .key = on_key,
    .touch = on_touch,
};
