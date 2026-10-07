/* See ls_gps.h for why alive and fix are separate. */
#include "ls_gps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "soc/soc_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ls_board.h"

static const char *TAG = "ls_gps";

_Static_assert(sizeof(ls_gps_sat_t) == 6,
               "used and sys must share a byte: a snapshot carries a table "
               "of these and the parser's context two");

#if defined(LS_BOARD_GPS_RX_GPIO) && defined(LS_BOARD_GPS_UART_NUM)

_Static_assert(LS_BOARD_GPS_BAUD == 115200,
               "the bring-up raises the module with $PCAS01,5, which is 115200");

/* Which UART carries this is not a hardware fact: the P4 routes any UART to
   any pin through the GPIO matrix, and only GPIO22/23 are fixed.  The
   Flipper link claims a free port at boot and does not always land on the
   same one, so demanding a fixed number here loses a race that neither side
   is wrong about.  Claim the configured port when it is free and otherwise
   take any other free one, then report which. */
static uart_port_t s_port = (uart_port_t)LS_BOARD_GPS_UART_NUM;
/* True while the driver on s_port is the one this file installed. A start
   that finds it still there, after a stop that could not wait the reader
   out, takes it back instead of calling the port busy. */
static bool s_port_ours;

static bool claim_port(void)
{
    if (s_port_ours || !uart_is_driver_installed(s_port)) return true;
    for (int u = SOC_UART_HP_NUM - 1; u >= 0; u--) {
        if (u == CONFIG_ESP_CONSOLE_UART_NUM) continue;
        if (!uart_is_driver_installed(u)) {
            ESP_LOGW(TAG, "uart%d busy, using uart%d", (int)s_port, u);
            s_port = (uart_port_t)u;
            return true;
        }
    }
    return false;
}

#define GPS_UART   s_port

#define NMEA_MAX   82

/* PSRAM: the two satellite tables make this the largest thing the driver
   owns, and nothing here needs it in internal RAM. */
static EXT_RAM_BSS_ATTR ls_gps_parser_t s_parser;
static SemaphoreHandle_t s_lock;    /* s_parser and the raw lines            */

/* The last lines framed off the wire, for `gps raw`: the parsed table cannot
   show a sentence the parser does not know. More than one fix cycle of
   three constellations at 1 Hz. */
#define RAW_LINES 32
static EXT_RAM_BSS_ATTR char s_raw[RAW_LINES][NMEA_MAX + 1];
static uint8_t s_raw_next;          /* the slot written next (the oldest)    */
static uint8_t s_raw_count;
static SemaphoreHandle_t s_ctl;     /* start, stop and the baud scan         */
static portMUX_TYPE     s_make_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t     s_task;
static volatile bool    s_run;
static volatile bool    s_retired;    /* the reader left its loop; delete it  */
static bool             s_scanning;   /* the baud scan has the port          */

/* Bring-up, written by the reader and read by the console. The constellation
   set does not survive the module losing power, so it is sent once a boot. */
static volatile ls_gps_link_t s_link;
static volatile int     s_baud = LS_BOARD_GPS_BAUD;
static bool             s_configured;  /* constellations and rate sent       */
static bool             s_raised;      /* moved up from 9600 this boot       */

/* ---- parsing ---------------------------------------------------------- */

static bool checksum_ok(const char *s, int len)
{
    if (len < 4 || s[0] != '$') return false;
    int star = -1;
    for (int i = len - 1; i > 0 && i > len - 6; i--)
        if (s[i] == '*') { star = i; break; }
    if (star < 0 || star + 2 >= len) return false;

    uint8_t sum = 0;
    for (int i = 1; i < star; i++) sum ^= (uint8_t)s[i];

    char want[3] = { s[star + 1], s[star + 2], 0 };
    return (uint8_t)strtol(want, NULL, 16) == sum;
}

static int split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == ',' || *p == '*') { *p = 0; f[n++] = p + 1; }
    }
    return n;
}

static double to_degrees(const char *v, const char *hemi, int deg_digits)
{
    if (!v || !*v) return 0.0;
    char head[4] = { 0 };
    for (int i = 0; i < deg_digits && v[i]; i++) head[i] = v[i];
    double deg = atof(head);
    double min = atof(v + deg_digits);
    double d = deg + min / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}

static void parse_time(const char *v, ls_gps_state_t *st)
{
    if (!v || strlen(v) < 6) return;
    char b[3] = { 0 };
    b[0] = v[0]; b[1] = v[1]; st->hour   = (uint8_t)atoi(b);
    b[0] = v[2]; b[1] = v[3]; st->minute = (uint8_t)atoi(b);
    b[0] = v[4]; b[1] = v[5]; st->second = (uint8_t)atoi(b);
}

static void parse_date(const char *v, ls_gps_state_t *st)
{
    if (!v || strlen(v) < 6) return;
    char b[3] = { 0 };
    b[0] = v[0]; b[1] = v[1]; st->day   = (uint8_t)atoi(b);
    b[0] = v[2]; b[1] = v[3]; st->month = (uint8_t)atoi(b);
    /* Two digits, with the pivot NMEA uses: 80 to 99 is the nineteen
       hundreds. Adding 2000 unconditionally put 1994 in 2094, and it matters
       beyond old logs - a receiver whose week counter has rolled over, or one
       running on the almanac its backup battery held, can report a date well
       in the past. */
    b[0] = v[4]; b[1] = v[5];
    int yy = atoi(b);
    st->year = (uint16_t)(yy >= 80 ? 1900 + yy : 2000 + yy);
}

/* The constellation a talker ID names. Field 0 is "$GPGSV" and the like, so
   the talker is the two characters after the '$'. GN speaks for several
   systems at once and says nothing about any one satellite. */
static uint8_t talker_sys(const char *f0)
{
    if (strlen(f0) < 3) return LS_GPS_SYS_UNKNOWN;
    const char a = f0[1], b = f0[2];
    if (a == 'G' && b == 'P') return LS_GPS_SYS_GPS;
    if (a == 'G' && b == 'L') return LS_GPS_SYS_GLONASS;
    if (a == 'G' && b == 'A') return LS_GPS_SYS_GALILEO;
    if ((a == 'G' && b == 'B') || (a == 'B' && b == 'D')) return LS_GPS_SYS_BEIDOU;
    if (a == 'G' && b == 'Q') return LS_GPS_SYS_QZSS;
    return LS_GPS_SYS_UNKNOWN;
}

/* Each constellation numbers its satellites from 1, so a PRN means nothing
   without its system. An unknown system on either side matches any: a GN
   GSA without NMEA 4.1's system ID can only be matched on the PRN. */
static bool same_sys(uint8_t a, uint8_t b)
{
    return a == b || a == LS_GPS_SYS_UNKNOWN || b == LS_GPS_SYS_UNKNOWN;
}

/* GSA and GSV are not ordered relative to each other by NMEA, so a GSA naming
   a satellite can arrive before the GSV that first reports it. This set
   remembers, for the cycle in progress, which satellites a GSA has marked
   used; a GSV that creates a new sat_acc entry consults it. Bounded and
   deduplicated so a receiver repeating or splitting GSA sentences cannot
   grow it past LS_GPS_MAX_SATS. */
static bool gsa_prn_used(const ls_gps_parser_t *p, uint8_t sys, int prn)
{
    for (int i = 0; i < p->gsa_used_count; i++)
        if (p->gsa_used_prns[i] == prn && same_sys(p->gsa_used_sys[i], sys))
            return true;
    return false;
}

static void gsa_prn_remember(ls_gps_parser_t *p, uint8_t sys, int prn)
{
    if (prn <= 0 || prn > UINT8_MAX) return;
    for (int i = 0; i < p->gsa_used_count; i++)
        if (p->gsa_used_prns[i] == prn && p->gsa_used_sys[i] == sys) return;
    if (p->gsa_used_count < LS_GPS_MAX_SATS) {
        p->gsa_used_sys[p->gsa_used_count] = sys;
        p->gsa_used_prns[p->gsa_used_count++] = (uint8_t)prn;
    }
}

/* What the table just published says about the sky. A satellite is tracked
   when GSV gives it a C/N0; GSV's in-view count also holds the ones the
   almanac only predicts, which have none. */
static void publish_tracked(ls_gps_state_t *g)
{
    uint8_t top[4] = { 0 };   /* the four strongest, strongest first */
    g->sats_tracked = 0;
    memset(g->tracked_by_sys, 0, sizeof(g->tracked_by_sys));
    for (int i = 0; i < g->sat_count; i++) {
        uint8_t c = g->sats[i].snr;
        if (!c) continue;
        g->sats_tracked++;
        const uint8_t sys = g->sats[i].sys;
        g->tracked_by_sys[sys < LS_GPS_SYS_COUNT ? sys : LS_GPS_SYS_UNKNOWN]++;
        for (int k = 0; k < 4; k++)
            if (c > top[k]) { const uint8_t t = top[k]; top[k] = c; c = t; }
    }
    unsigned sum = 0, n = 0;
    for (int k = 0; k < 4 && top[k]; k++) { sum += top[k]; n++; }
    g->cn0_best = top[0];
    g->cn0_top4 = n ? (uint8_t)((sum + n / 2) / n) : 0;
}

/* The talker ID varies with the constellation mix - GP, GL, GA, GN - so the
   type is matched on the last three characters only. */
static void parse_sentence(char *s, int len, ls_gps_parser_t *p)
{
    char *f[24];
    int n = split(s, f, 24);
    if (n < 2) return;

    const char *type = f[0] + (strlen(f[0]) >= 3 ? strlen(f[0]) - 3 : 0);

    if (strcmp(type, "GGA") == 0 && n >= 10) {
        /* GGA opens a fix cycle, so the count the previous cycle's GSV
           sentences accumulated is complete; the next GSV starts a new one.

           The satellite table is published on the same boundary and for the
           same reason: a drawing path that read the accumulator directly
           would see a sky that is half this sweep and half the last one,
           which flickers as satellites appear and vanish between frames.

           The remembered GSA PRN set is cleared here too, once this cycle's
           satellites have published: a PRN a GSA marked used belongs only to
           the cycle it arrived in, not to whatever GSV shows up next. */
        p->st.sats_visible = p->sats_acc;
        p->sats_acc = 0;
        if (p->sat_acc_count) {
            memcpy(p->st.sats, p->sat_acc,
                   sizeof(p->st.sats[0]) * p->sat_acc_count);
            p->st.sat_count = p->sat_acc_count;
            p->sat_acc_count = 0;
            publish_tracked(&p->st);
        }
        p->gsa_used_count = 0;
        parse_time(f[1], &p->st);
        p->st.quality   = (uint8_t)atoi(f[6]);
        p->st.sats_used = (uint8_t)atoi(f[7]);
        p->st.hdop      = (float)atof(f[8]);
        if (*f[9]) p->st.alt_m = (float)atof(f[9]);
        if (p->st.quality > 0 && *f[2] && *f[4]) {
            p->st.lat_deg = to_degrees(f[2], f[3], 2);
            p->st.lon_deg = to_degrees(f[4], f[5], 3);
            p->st.fix = true;
            p->st.position_updates++;
        } else {
            p->st.fix = false;
        }
    } else if (strcmp(type, "RMC") == 0 && n >= 10) {
        parse_time(f[1], &p->st);
        bool valid = (f[2][0] == 'A');
        if (valid && *f[3] && *f[5]) {
            p->st.lat_deg = to_degrees(f[3], f[4], 2);
            p->st.lon_deg = to_degrees(f[5], f[6], 3);
            p->st.fix = true;
            p->st.position_updates++;
        } else p->st.fix = false;
        if (*f[7]) p->st.speed_kts  = (float)atof(f[7]);
        if (*f[8]) p->st.course_deg = (float)atof(f[8]);
        parse_date(f[9], &p->st);
    } else if (strcmp(type, "GSV") == 0 && n >= 4) {
        /* Each constellation sends its own GSV series and repeats its
           satellite count in every message of that series, so summing only
           the first message of each gives the true total across
           constellations without double counting.  The running total is
           published by the next GGA, which is what bounds a cycle. */
        if (atoi(f[2]) == 1)
            p->sats_acc = (uint8_t)(p->sats_acc + atoi(f[3]));

        /* NMEA 4.1 adds a signal ID after the last satellite. It never
           completes a group of four, so the loop below steps past it. */
        const uint8_t sys = talker_sys(f[0]);
        for (int i = 4; i + 3 < n && p->sat_acc_count < LS_GPS_MAX_SATS;
             i += 4) {
            if (!*f[i]) continue;
            const int prn = atoi(f[i]);
            ls_gps_sat_t *sat = &p->sat_acc[p->sat_acc_count++];
            sat->prn       = (uint8_t)prn;
            sat->elevation = (uint8_t)atoi(f[i + 1]);
            sat->azimuth   = (uint16_t)atoi(f[i + 2]);
            sat->snr       = (uint8_t)atoi(f[i + 3]);
            sat->sys       = sys;
            sat->used      = gsa_prn_used(p, sys, prn);
        }
    } else if (strcmp(type, "GSA") == 0 && n >= 15) {
        /* NMEA 4.1 puts the system ID after VDOP, in field 18 with the
           checksum after it. Without one the talker is all there is. */
        uint8_t sys = talker_sys(f[0]);
        if (n >= 20) {
            const int id = atoi(f[18]);
            if (id > LS_GPS_SYS_UNKNOWN && id < LS_GPS_SYS_COUNT)
                sys = (uint8_t)id;
        }
        for (int i = 3; i <= 14 && i < n; i++) {
            if (!*f[i]) continue;
            const int prn = atoi(f[i]);
            if (prn <= 0 || prn > UINT8_MAX) continue;
            for (int k = 0; k < p->sat_acc_count; k++)
                if (p->sat_acc[k].prn == prn &&
                    same_sys(p->sat_acc[k].sys, sys))
                    p->sat_acc[k].used = true;
            gsa_prn_remember(p, sys, prn);
        }
    } else if (strcmp(type, "TXT") == 0 && n >= 5) {
        /* The L76K reports its antenna feed in field 4 as "ANTENNA OK",
           "ANTENNA OPEN" or "ANTENNA SHORT". Other text leaves it alone. */
        if (strncmp(f[4], "ANTENNA ", 8) == 0)
            for (uint8_t a = LS_GPS_ANT_OK; a <= LS_GPS_ANT_SHORT; a++)
                if (strcmp(f[4] + 8, ls_gps_antenna_name(a)) == 0)
                    p->st.antenna = a;
    }

    (void)len;
}

bool ls_gps_parse_line(const char *line, int len, ls_gps_parser_t *p)
{
    if (!line || !p || len <= 0) return false;
    if (!checksum_ok(line, len)) return false;

    char work[NMEA_MAX + 1];
    if (len > NMEA_MAX) return false;
    memcpy(work, line, (size_t)len);
    work[len] = 0;

    parse_sentence(work, len, p);
    return true;
}

bool ls_gps_feed_line(const char *line, int len, ls_gps_parser_t *p,
                      int64_t now_us)
{
    if (!p) return false;
    const uint32_t position_updates = p->st.position_updates;
    if (!ls_gps_parse_line(line, len, p)) {
        p->st.checksum_errors++;
        return false;
    }
    p->st.sentences++;
    p->st.last_sentence_us = now_us;
    if (p->st.position_updates != position_updates) {
        p->st.last_fix_us = now_us;
        if (!p->st.first_fix_us) p->st.first_fix_us = now_us;
    }
    p->st.alive = true;
    return true;
}

const char *ls_gps_sys_name(uint8_t sys)
{
    static const char *const NAME[LS_GPS_SYS_COUNT] = {
        [LS_GPS_SYS_UNKNOWN] = "other",   [LS_GPS_SYS_GPS]    = "GPS",
        [LS_GPS_SYS_GLONASS] = "GLONASS", [LS_GPS_SYS_GALILEO] = "Galileo",
        [LS_GPS_SYS_BEIDOU]  = "BeiDou",  [LS_GPS_SYS_QZSS]   = "QZSS",
    };
    return sys < LS_GPS_SYS_COUNT ? NAME[sys] : NAME[LS_GPS_SYS_UNKNOWN];
}

const char *ls_gps_antenna_name(uint8_t antenna)
{
    switch (antenna) {
    case LS_GPS_ANT_OK:    return "OK";
    case LS_GPS_ANT_OPEN:  return "OPEN";
    case LS_GPS_ANT_SHORT: return "SHORT";
    default:               return "not reported";
    }
}

int ls_gps_describe_sky(char *out, size_t cap, const ls_gps_state_t *g,
                        int64_t now_us)
{
    /* The configured constellations first, in the order they are set. */
    static const uint8_t ORDER[] = {
        LS_GPS_SYS_GPS, LS_GPS_SYS_BEIDOU, LS_GPS_SYS_GLONASS,
        LS_GPS_SYS_GALILEO, LS_GPS_SYS_QZSS, LS_GPS_SYS_UNKNOWN,
    };
    char systems[96] = "";
    size_t at = 0;
    for (unsigned i = 0; i < sizeof(ORDER) && at < sizeof(systems); i++) {
        const uint8_t n = g->tracked_by_sys[ORDER[i]];
        if (n)
            at += (size_t)snprintf(systems + at, sizeof(systems) - at, "%s%s %u",
                                   at ? ", " : "", ls_gps_sys_name(ORDER[i]),
                                   (unsigned)n);
    }

    char when[40];
    if (g->first_fix_us)
        snprintf(when, sizeof(when), "first fix after %lld s",
                 (long long)((g->first_fix_us - g->start_us + 500000) / 1000000));
    else if (g->start_us)
        snprintf(when, sizeof(when), "no fix after %lld s",
                 (long long)((now_us - g->start_us) / 1000000));
    else
        snprintf(when, sizeof(when), "no fix");

    if (!g->sats_tracked)
        return snprintf(out, cap, "tracked 0, no C/N0, antenna %s, %s",
                        ls_gps_antenna_name(g->antenna), when);
    return snprintf(out, cap,
                    "tracked %u (%s), C/N0 best %u, top-4 mean %u dB-Hz, "
                    "antenna %s, %s",
                    (unsigned)g->sats_tracked, systems, (unsigned)g->cn0_best,
                    (unsigned)g->cn0_top4, ls_gps_antenna_name(g->antenna),
                    when);
}

/* ---- bringing the module up ------------------------------------------- */

ls_gps_do_t ls_gps_link_step(ls_gps_link_t *link, bool framed,
                             int64_t waited_us, bool configured)
{
    const ls_gps_do_t up = configured ? LS_GPS_DO_NOTHING : LS_GPS_DO_CONFIGURE;
    switch (*link) {
    case LS_GPS_LINK_PROBE:
        if (framed) { *link = LS_GPS_LINK_UP; return up; }
        if (waited_us < LS_GPS_PROBE_US) return LS_GPS_DO_NOTHING;
        *link = LS_GPS_LINK_TRY_9600;
        return LS_GPS_DO_LISTEN_9600;
    case LS_GPS_LINK_TRY_9600:
        if (framed) { *link = LS_GPS_LINK_CONFIRM; return LS_GPS_DO_RAISE; }
        if (waited_us < LS_GPS_TRY_9600_US) return LS_GPS_DO_NOTHING;
        *link = LS_GPS_LINK_SILENT;
        return LS_GPS_DO_GIVE_UP;
    case LS_GPS_LINK_CONFIRM:
        /* Saved before anything else is sent, and only once it is heard at
           the new rate: a module that never moved is not told to keep it. */
        if (framed) { *link = LS_GPS_LINK_UP; return LS_GPS_DO_SAVE; }
        if (waited_us < LS_GPS_CONFIRM_US) return LS_GPS_DO_NOTHING;
        *link = LS_GPS_LINK_AT_9600;
        return LS_GPS_DO_STAY_9600;
    case LS_GPS_LINK_UP:
        return up;
    case LS_GPS_LINK_SILENT:
        /* Silent at both rates: keep listening at 115200, and configure
           the module if it ever starts talking there. */
        if (framed) { *link = LS_GPS_LINK_UP; return up; }
        return LS_GPS_DO_NOTHING;
    case LS_GPS_LINK_AT_9600:
    default:
        return LS_GPS_DO_NOTHING;
    }
}

static void send_cmd(const char *cmd)
{
    uart_write_bytes(GPS_UART, cmd, strlen(cmd));
    uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(100));
}

static void set_baud(int baud)
{
    uart_set_baudrate(GPS_UART, (uint32_t)baud);
    uart_flush_input(GPS_UART);
    s_baud = baud;
}

/* Carries out one step. True when the UART's rate changed, which leaves
   whatever line was half assembled meaningless. */
static bool link_act(ls_gps_do_t act)
{
    switch (act) {
    case LS_GPS_DO_LISTEN_9600:
        ESP_LOGW(TAG, "nothing framed at %d baud, listening at 9600",
                 LS_BOARD_GPS_BAUD);
        set_baud(9600);
        return true;
    case LS_GPS_DO_RAISE:
        ESP_LOGW(TAG, "module talking at 9600, moving it to %d",
                 LS_BOARD_GPS_BAUD);
        send_cmd(LS_GPS_CMD_115200);
        vTaskDelay(pdMS_TO_TICKS(100));
        send_cmd(LS_GPS_CMD_115200);
        vTaskDelay(pdMS_TO_TICKS(200));
        set_baud(LS_BOARD_GPS_BAUD);
        return true;
    case LS_GPS_DO_SAVE:
        send_cmd(LS_GPS_CMD_SAVE);
        s_raised = true;
        ESP_LOGI(TAG, "module heard at %d, rate saved", LS_BOARD_GPS_BAUD);
        return false;
    case LS_GPS_DO_CONFIGURE:
        send_cmd(LS_GPS_CMD_GNSS);
        vTaskDelay(pdMS_TO_TICKS(100));
        send_cmd(LS_GPS_CMD_RATE_1HZ);
        s_configured = true;
        ESP_LOGI(TAG, "set GPS+BeiDou+GLONASS at 1 Hz");
        return false;
    case LS_GPS_DO_STAY_9600:
        ESP_LOGW(TAG, "module did not move to %d, reading it at 9600",
                 LS_BOARD_GPS_BAUD);
        set_baud(9600);
        return true;
    case LS_GPS_DO_GIVE_UP:
        ESP_LOGW(TAG, "nothing framed at %d or 9600 baud, listening at %d",
                 LS_BOARD_GPS_BAUD, LS_BOARD_GPS_BAUD);
        set_baud(LS_BOARD_GPS_BAUD);
        return true;
    default:
        return false;
    }
}

/* ---- reader task ------------------------------------------------------ */

static void gps_task(void *arg)
{
    (void)arg;
    static char line[NMEA_MAX + 1];
    int  fill = 0;
    bool overrun = false;
    uint8_t buf[256];
    /* The bring-up's clock and baseline: when the current step began and how
       many sentences had framed by then. Only this task writes the count
       while it runs, so it is read here without the lock. */
    int64_t  since = esp_timer_get_time();
    uint32_t seen  = s_parser.st.sentences;

    while (s_run) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(200));

        if (n > 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_parser.st.bytes += (uint32_t)n;

            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];
                if (c == '$') { fill = 0; overrun = false; }
                if (c == '\r' || c == '\n') {
                    if (fill > 0 && !overrun) {
                        line[fill] = 0;
                        memcpy(s_raw[s_raw_next], line, (size_t)fill + 1);
                        s_raw_next = (uint8_t)((s_raw_next + 1) % RAW_LINES);
                        if (s_raw_count < RAW_LINES) s_raw_count++;
                        ls_gps_feed_line(line, fill, &s_parser,
                                         esp_timer_get_time());
                    }
                    fill = 0;
                    overrun = false;
                    continue;
                }
                if (fill < NMEA_MAX) line[fill++] = c;
                else overrun = true;
            }
            xSemaphoreGive(s_lock);
        }

        /* Commands go out with the lock released: a step can take a few
           hundred milliseconds and ls_gps_get must not wait on it. */
        ls_gps_link_t link = s_link;
        const ls_gps_do_t act = ls_gps_link_step(&link,
                                                 s_parser.st.sentences != seen,
                                                 esp_timer_get_time() - since,
                                                 s_configured);
        if (act != LS_GPS_DO_NOTHING && link_act(act)) {
            fill = 0;
            overrun = false;
        }
        if (act != LS_GPS_DO_NOTHING || link != s_link) {
            s_link = link;
            since = esp_timer_get_time();
            seen = s_parser.st.sentences;
        }
    }

    /* Whoever stops the reader deletes it (reap_locked). A task made with
       caps that deletes itself has IDF create a helper task in internal RAM
       to free it, and IDF aborts when that allocation fails. */
    s_retired = true;
    vTaskSuspend(NULL);
}

/* ---- api -------------------------------------------------------------- */

/* The task handle is the answer, less a reader that has left its loop and
   waits to be deleted. See the note in the header for what using `alive`
   instead was costing. */
bool ls_gps_running(void) { return s_task != NULL && !s_retired; }

/* With s_ctl held. Deletes a reader that has left its loop; deleting another
   task frees its stack and TCB without allocating anything. */
static void reap_locked(void)
{
    if (!s_task || !s_retired) return;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    vTaskDeleteWithCaps(s_task);
#else
    vTaskDelete(s_task);
#endif
    s_task = NULL;
    s_retired = false;
}

/* Both locks, made once. Two first callers can race here, so each makes a
   pair and only the first to publish one keeps it. */
static bool make_locks(void)
{
    if (s_ctl) return true;
    SemaphoreHandle_t ctl = xSemaphoreCreateMutex();
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    bool kept = false;
    if (ctl && lock) {
        portENTER_CRITICAL(&s_make_mux);
        if (!s_ctl) { s_lock = lock; s_ctl = ctl; kept = true; }
        portEXIT_CRITICAL(&s_make_mux);
    }
    if (!kept) {
        if (ctl) vSemaphoreDelete(ctl);
        if (lock) vSemaphoreDelete(lock);
    }
    return s_ctl != NULL;
}

/* With s_ctl held. */
static esp_err_t start_locked(void)
{
    /* A reader that retired after its stop gave up waiting goes now; one
       still retiring blocks the start. */
    reap_locked();
    if (s_task) return s_run ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_ERR_INVALID_STATE;

    if (!claim_port()) {
        ESP_LOGE(TAG, "every UART is claimed - nothing left for the GPS");
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_port_ours) {
        uart_config_t cfg = {
            .baud_rate  = LS_BOARD_GPS_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        /* Order matters: param_config before driver_install leaves the baud
           rate applied to a port the driver then takes over cleanly.  The
           reverse order works too but has bitten on other ports when the
           driver's own reset raced the config write. */
        esp_err_t err = uart_param_config(GPS_UART, &cfg);
        if (err != ESP_OK) return err;

        err = uart_set_pin(GPS_UART, LS_BOARD_GPS_TX_GPIO, LS_BOARD_GPS_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) return err;

        /* 2 KB is about three seconds of 9600 baud, which is more slack than
           the 200 ms read loop needs and cheap enough not to tune. */
        err = uart_driver_install(GPS_UART, 2048, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
        s_port_ours = true;
    } else {
        /* Ours, still installed: take it back at the board's rate. */
        set_baud(LS_BOARD_GPS_BAUD);
    }
    s_baud = LS_BOARD_GPS_BAUD;
    s_link = LS_GPS_LINK_PROBE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_parser.st.alive = false;
    s_parser.st.start_us = esp_timer_get_time();
    s_parser.st.first_fix_us = 0;
    xSemaphoreGive(s_lock);

    s_retired = false;
    s_run = true;
    /* This UART/parser task never writes flash. Keep its stack out of the
       DMA heap needed by the shared radios and USB receiver. */
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    BaseType_t created=xTaskCreateWithCaps(gps_task,"ls_gps",3072,NULL,5,&s_task,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    BaseType_t created=xTaskCreate(gps_task,"ls_gps",3072,NULL,5,&s_task);
#endif
    if (created != pdPASS) {
        s_run = false;
        uart_driver_delete(GPS_UART);
        s_port_ours = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "L76K reader up on RX%d/TX%d at %d baud",
             LS_BOARD_GPS_RX_GPIO, LS_BOARD_GPS_TX_GPIO, LS_BOARD_GPS_BAUD);
    return ESP_OK;
}

/* With s_ctl held. */
static void stop_locked(void)
{
    if (!s_task) return;
    s_run = false;
    /* The reader notices within its 200 ms read timeout, or once a bring-up
       step finishes, which is under 400 ms more. */
    for (int i = 0; i < 40 && !s_retired; i++) vTaskDelay(pdMS_TO_TICKS(25));
    if (!s_retired) {
        /* Deleting the driver under a reader still inside it would crash it.
           The port stays ours and the next start takes it back. */
        ESP_LOGW(TAG, "reader did not retire; leaving its UART installed");
        return;
    }
    reap_locked();
    uart_driver_delete(GPS_UART);
    s_port_ours = false;
}

/* The work runs in the reader task, so this returns as soon as the UART is
   up and the task exists. */
esp_err_t ls_gps_start(void)
{
    if (s_task && s_run) return ESP_OK;
    if (!make_locks()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    const esp_err_t err = start_locked();
    xSemaphoreGive(s_ctl);
    return err;
}

void ls_gps_stop(void)
{
    if (!s_task || !make_locks()) return;
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    stop_locked();
    xSemaphoreGive(s_ctl);
}

void ls_gps_get(ls_gps_state_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_parser.st;
    xSemaphoreGive(s_lock);
}

/* Bytes arriving that never frame into a sentence is the one failure this
   driver cannot diagnose from its own counters: it looks identical whether
   the baud rate is wrong or the pin is picking up someone else's traffic.
   So dump what is actually on the wire, at each rate the module might have
   been left at, and let the bytes settle it.  Runs with the reader stopped
   because both want the same port. */
void ls_gps_scan_baud(void)
{
    static const int RATES[] = { 9600, 115200, 38400, 57600, 19200, 4800 };

    if (!make_locks()) { printf("gps: no memory for its locks\n"); return; }
    /* The port is held for the scan by refusing starts, not by keeping the
       lock: a caller of ls_gps_start must not wait seven seconds. */
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    const bool was_running = ls_gps_running();
    if (s_task) stop_locked();
    const bool free_port = !s_task && !s_port_ours && claim_port();
    s_scanning = free_port;
    xSemaphoreGive(s_ctl);
    if (!free_port) {
        printf("gps: no free UART\n");
        if (was_running) ls_gps_start();
        return;
    }

    printf("gps: listening on GPIO%d for 1200 ms at each rate\n",
           LS_BOARD_GPS_RX_GPIO);

    for (unsigned r = 0; r < sizeof(RATES) / sizeof(RATES[0]); r++) {
        uart_config_t cfg = {
            .baud_rate  = RATES[r],
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(GPS_UART, 4096, 0, 0, NULL, 0) != ESP_OK) {
            printf("gps: cannot install driver\n");
            break;
        }
        uart_param_config(GPS_UART, &cfg);
        uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, LS_BOARD_GPS_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

        uint8_t buf[512];
        int total = 0, printable = 0, dollars = 0, eol = 0;
        char sample[65];
        int  smp = 0;
        int64_t end = esp_timer_get_time() + 1200 * 1000;
        while (esp_timer_get_time() < end) {
            int n = uart_read_bytes(GPS_UART, buf, sizeof(buf),
                                    pdMS_TO_TICKS(100));
            for (int i = 0; i < n; i++) {
                uint8_t c = buf[i];
                total++;
                if (c >= 0x20 && c < 0x7F) printable++;
                if (c == '$') dollars++;
                if (c == '\n' || c == '\r') eol++;
                if (smp < (int)sizeof(sample) - 1 && c >= 0x20 && c < 0x7F)
                    sample[smp++] = (char)c;
            }
        }
        sample[smp] = 0;
        uart_driver_delete(GPS_UART);

        /* Printable ratio is the discriminator.  NMEA is entirely ASCII, so
           the correct rate reads near 100 %; a wrong one lands around 40 %
           because framing errors scatter bytes across the whole range. */
        int pct = total ? (printable * 100 / total) : 0;
        printf("gps: %6d  %5d bytes  %3d%% ascii  %2d '$'  %2d eol  %s\n",
               RATES[r], total, pct, dollars, eol, sample);
    }

    printf("gps: the right rate is the one with near-100%% ascii and '$' "
           "counts in the tens.\n");
    xSemaphoreTake(s_ctl, portMAX_DELAY);
    s_scanning = false;
    xSemaphoreGive(s_ctl);
    if (was_running) ls_gps_start();
}

/* The console's snapshot. Static and in PSRAM because it carries the
   satellite table, and the console task's stack is internal RAM. */
static EXT_RAM_BSS_ATTR ls_gps_state_t s_console;

/* Start the reader for a console helper, and give it time to say something. */
static bool console_start(int wait_ms)
{
    if (ls_gps_running()) return true;
    const esp_err_t err = ls_gps_start();
    printf("gps: start %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    return true;
}

static const char *link_words(void)
{
    switch (s_link) {
    case LS_GPS_LINK_PROBE:    return "listening at 115200";
    case LS_GPS_LINK_TRY_9600: return "silent at 115200, listening at 9600";
    case LS_GPS_LINK_CONFIRM:  return "told to move to 115200, listening there";
    case LS_GPS_LINK_UP:
        return s_configured ? "set to GPS+BeiDou+GLONASS at 1 Hz"
                            : "talking at 115200, not configured yet";
    case LS_GPS_LINK_AT_9600:  return "stayed at 9600; read there, not configured";
    case LS_GPS_LINK_SILENT:   return "nothing framed at 115200 or 9600";
    }
    return "?";
}

void ls_gps_diagnostics(void)
{
    /* One second is two or three sentence cycles, enough to say whether
       anything is arriving at all. */
    if (!console_start(1200)) return;

    ls_gps_state_t *g = &s_console;
    ls_gps_get(g);

    printf("gps: uart%d rx=GPIO%d tx=GPIO%d %d baud  bytes=%lu sentences=%lu csum_err=%lu\n",
           (int)s_port, LS_BOARD_GPS_RX_GPIO, LS_BOARD_GPS_TX_GPIO,
           (int)s_baud,
           (unsigned long)g->bytes, (unsigned long)g->sentences,
           (unsigned long)g->checksum_errors);

    if (!g->alive) {
        if (g->bytes == 0)
            printf("gps: nothing on the wire. The module is always powered "
                   "and held awake, so this is a pin or a rail, not a sky "
                   "problem.\n");
        else
            printf("gps: %lu bytes but no framed sentence - wrong baud rate "
                   "or the wrong pin is picking up crosstalk.\n",
                   (unsigned long)g->bytes);
        printf("gps: module %s\n", link_words());
        return;
    }

    printf("gps: alive, %u sats visible, %u used, quality %u, hdop %.1f\n",
           g->sats_visible, g->sats_used, g->quality, (double)g->hdop);

    if (g->fix)
        printf("gps: fix %.6f %.6f  alt %.0f m  %.1f kts  %04u-%02u-%02u "
               "%02u:%02u:%02uZ\n",
               g->lat_deg, g->lon_deg, (double)g->alt_m, (double)g->speed_kts,
               g->year, g->month, g->day, g->hour, g->minute, g->second);
    else
        printf("gps: no fix yet. Sentences are framing correctly, so the "
               "receiver works; it needs sky and, cold, a few minutes.\n");

    char sky[160];
    ls_gps_describe_sky(sky, sizeof(sky), g, esp_timer_get_time());
    printf("gps: %s\n", sky);
    printf("gps: module %s%s\n", link_words(),
           s_raised ? ", moved up from 9600 this boot" : "");
}

void ls_gps_print_sky(void)
{
    /* A table is published by the GGA that closes a cycle, so wait two. */
    if (!console_start(2200)) return;

    ls_gps_state_t *g = &s_console;
    ls_gps_get(g);
    if (!g->sat_count) {
        printf("gps: no satellites reported yet\n");
        return;
    }

    printf("gps: %u in the table, %u tracked; -- is not reported\n",
           (unsigned)g->sat_count, (unsigned)g->sats_tracked);
    printf("gps: system   prn  elev  azim  c/n0  used\n");
    for (int i = 0; i < g->sat_count && i < LS_GPS_MAX_SATS; i++) {
        const ls_gps_sat_t *s = &g->sats[i];
        char el[8] = "--", az[8] = "--", cn[8] = "--";
        /* GSV leaves both empty for a satellite it cannot place. */
        if (s->elevation || s->azimuth) {
            snprintf(el, sizeof(el), "%u", (unsigned)s->elevation);
            snprintf(az, sizeof(az), "%u", (unsigned)s->azimuth);
        }
        if (s->snr) snprintf(cn, sizeof(cn), "%u", (unsigned)s->snr);
        printf("gps: %-7s  %3u  %4s  %4s  %4s  %s\n",
               ls_gps_sys_name(s->sys), (unsigned)s->prn, el, az, cn,
               s->used ? "yes" : "-");
    }
}

void ls_gps_print_raw(void)
{
    if (!console_start(1200)) return;

    /* Copied under the lock, printed after it: the console is slow and the
       reader must not wait on it. */
    static EXT_RAM_BSS_ATTR char copy[RAW_LINES][NMEA_MAX + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_raw_count;
    const int first = (s_raw_next + RAW_LINES - n) % RAW_LINES;
    for (int i = 0; i < n; i++)
        memcpy(copy[i], s_raw[(first + i) % RAW_LINES], NMEA_MAX + 1);
    xSemaphoreGive(s_lock);

    if (!n) {
        printf("gps: nothing framed yet\n");
        return;
    }
    printf("gps: the last %d lines as received, oldest first\n", n);
    for (int i = 0; i < n; i++) printf("gps: %s\n", copy[i]);
}

#else  /* board declares no GPS */

bool ls_gps_running(void) { return false; }
esp_err_t ls_gps_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void ls_gps_stop(void) { }
void ls_gps_get(ls_gps_state_t *out) { if (out) memset(out, 0, sizeof(*out)); }
void ls_gps_diagnostics(void) { printf("gps: board declares none\n"); }
void ls_gps_print_sky(void) { printf("gps: board declares none\n"); }
void ls_gps_print_raw(void) { printf("gps: board declares none\n"); }

#endif
