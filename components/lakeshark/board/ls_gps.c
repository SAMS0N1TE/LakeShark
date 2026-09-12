/* See ls_gps.h for why alive and fix are separate. */
#include "ls_gps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ls_board.h"

static const char *TAG = "ls_gps";

#if defined(LS_BOARD_GPS_RX_GPIO) && defined(LS_BOARD_GPS_UART_NUM)

/* Which UART carries this is not a hardware fact: the P4 routes any UART to
   any pin through the GPIO matrix, and only GPIO22/23 are fixed.  The
   Flipper link claims a free port at boot and does not always land on the
   same one, so demanding a fixed number here loses a race that neither side
   is wrong about.  Claim the configured port when it is free and otherwise
   take any other free one, then report which. */
static uart_port_t s_port = (uart_port_t)LS_BOARD_GPS_UART_NUM;

static bool claim_port(void)
{
    if (!uart_is_driver_installed(s_port)) return true;
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

static ls_gps_state_t   s_st;
static SemaphoreHandle_t s_lock;
static TaskHandle_t     s_task;
static volatile bool    s_run;

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

/* The talker ID varies with the constellation mix - GP, GL, GA, GN - so the
   type is matched on the last three characters only. */
static void parse_sentence(char *s, int len, ls_gps_state_t *out)
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
           which flickers as satellites appear and vanish between frames. */
        out->sats_visible = out->sats_acc;
        out->sats_acc = 0;
        if (out->sat_acc_count) {
            memcpy(out->sats, out->sat_acc,
                   sizeof(out->sats[0]) * out->sat_acc_count);
            out->sat_count = out->sat_acc_count;
            out->sat_acc_count = 0;
        }
        parse_time(f[1], out);
        out->quality   = (uint8_t)atoi(f[6]);
        out->sats_used = (uint8_t)atoi(f[7]);
        out->hdop      = (float)atof(f[8]);
        if (*f[9]) out->alt_m = (float)atof(f[9]);
        if (out->quality > 0 && *f[2] && *f[4]) {
            out->lat_deg = to_degrees(f[2], f[3], 2);
            out->lon_deg = to_degrees(f[4], f[5], 3);
            out->fix = true;
        } else if (out->quality == 0) {
            out->fix = false;
        }
    } else if (strcmp(type, "RMC") == 0 && n >= 10) {
        parse_time(f[1], out);
        bool valid = (f[2][0] == 'A');
        if (valid && *f[3] && *f[5]) {
            out->lat_deg = to_degrees(f[3], f[4], 2);
            out->lon_deg = to_degrees(f[5], f[6], 3);
            out->fix = true;
        }
        if (*f[7]) out->speed_kts  = (float)atof(f[7]);
        if (*f[8]) out->course_deg = (float)atof(f[8]);
        parse_date(f[9], out);
    } else if (strcmp(type, "GSV") == 0 && n >= 4) {
        /* Each constellation sends its own GSV series and repeats its
           satellite count in every message of that series, so summing only
           the first message of each gives the true total across
           constellations without double counting.  The running total is
           published by the next GGA, which is what bounds a cycle. */
        if (atoi(f[2]) == 1)
            out->sats_acc = (uint8_t)(out->sats_acc + atoi(f[3]));

        for (int i = 4; i + 3 < n && out->sat_acc_count < LS_GPS_MAX_SATS;
             i += 4) {
            if (!*f[i]) continue;
            ls_gps_sat_t *sat = &out->sat_acc[out->sat_acc_count++];
            sat->prn       = (uint8_t)atoi(f[i]);
            sat->elevation = (uint8_t)atoi(f[i + 1]);
            sat->azimuth   = (uint16_t)atoi(f[i + 2]);
            sat->snr       = (uint8_t)atoi(f[i + 3]);
            sat->used      = false;
        }
    } else if (strcmp(type, "GSA") == 0 && n >= 15) {

        for (int i = 3; i <= 14 && i < n; i++) {
            if (!*f[i]) continue;
            const int prn = atoi(f[i]);
            for (int k = 0; k < out->sat_acc_count; k++)
                if (out->sat_acc[k].prn == prn) out->sat_acc[k].used = true;
        }
    }

    (void)len;
}

bool ls_gps_parse_line(const char *line, int len, ls_gps_state_t *st)
{
    if (!line || !st || len <= 0) return false;
    if (!checksum_ok(line, len)) return false;

    char work[NMEA_MAX + 1];
    if (len > NMEA_MAX) return false;
    memcpy(work, line, (size_t)len);
    work[len] = 0;

    parse_sentence(work, len, st);
    return true;
}

/* ---- reader task ------------------------------------------------------ */

static void gps_task(void *arg)
{
    (void)arg;
    static char line[NMEA_MAX + 1];
    int  fill = 0;
    bool overrun = false;
    uint8_t buf[256];

    while (s_run) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.bytes += (uint32_t)n;

        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') { fill = 0; overrun = false; }
            if (c == '\r' || c == '\n') {
                if (fill > 0 && !overrun) {
                    line[fill] = 0;
                    if (ls_gps_parse_line(line, fill, &s_st)) {
                        s_st.sentences++;
                        s_st.last_sentence_us = esp_timer_get_time();
                        s_st.alive = true;
                    } else {
                        s_st.checksum_errors++;
                    }
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

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- api -------------------------------------------------------------- */

/* The task handle is the answer: it exists between start and stop
   and at no other time. See the note in the header for what using `alive`
   instead was costing. */
bool ls_gps_running(void) { return s_task != NULL; }

esp_err_t ls_gps_start(void)
{
    if (s_task) return ESP_OK;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    if (!claim_port()) {
        ESP_LOGE(TAG, "every UART is claimed - nothing left for the GPS");
        return ESP_ERR_NOT_FOUND;
    }

    uart_config_t cfg = {
        .baud_rate  = LS_BOARD_GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Order matters: param_config before driver_install leaves the baud rate
       applied to a port the driver then takes over cleanly.  The reverse
       order works too but has bitten on other ports when the driver's own
       reset raced the config write. */
    esp_err_t err = uart_param_config(GPS_UART, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(GPS_UART, LS_BOARD_GPS_TX_GPIO, LS_BOARD_GPS_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    /* 2 KB is about three seconds of 9600 baud, which is more slack than the
       200 ms read loop needs and cheap enough not to tune. */
    err = uart_driver_install(GPS_UART, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;

    s_run = true;
    if (xTaskCreate(gps_task, "ls_gps", 3072, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        uart_driver_delete(GPS_UART);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "L76K reader up on RX%d/TX%d at %d baud",
             LS_BOARD_GPS_RX_GPIO, LS_BOARD_GPS_TX_GPIO, LS_BOARD_GPS_BAUD);
    return ESP_OK;
}

void ls_gps_stop(void)
{
    if (!s_task) return;
    s_run = false;
    /* The task blocks for at most the 200 ms read timeout, so this is the
       longest it can take to notice and retire itself. */
    for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
    uart_driver_delete(GPS_UART);
}

void ls_gps_get(ls_gps_state_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
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

    bool was_running = (s_task != NULL);
    if (was_running) ls_gps_stop();
    if (!claim_port()) { printf("gps: no free UART\n"); return; }

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
            return;
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
    if (was_running) ls_gps_start();
}

void ls_gps_diagnostics(void)
{
    if (!s_task) {
        esp_err_t err = ls_gps_start();
        printf("gps: start %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return;
        /* One second is two or three sentence cycles, enough to say whether
           anything is arriving at all. */
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    ls_gps_state_t g;
    ls_gps_get(&g);

    printf("gps: uart%d rx=GPIO%d tx=GPIO%d %d baud  bytes=%lu sentences=%lu csum_err=%lu\n",
           (int)s_port, LS_BOARD_GPS_RX_GPIO, LS_BOARD_GPS_TX_GPIO,
           LS_BOARD_GPS_BAUD,
           (unsigned long)g.bytes, (unsigned long)g.sentences,
           (unsigned long)g.checksum_errors);

    if (!g.alive) {
        if (g.bytes == 0)
            printf("gps: nothing on the wire. The module has no enable to "
                   "assert, so this is a pin or a rail, not a sky problem.\n");
        else
            printf("gps: %lu bytes but no framed sentence - wrong baud rate "
                   "or the wrong pin is picking up crosstalk.\n",
                   (unsigned long)g.bytes);
        return;
    }

    printf("gps: alive, %u sats visible, %u used, quality %u, hdop %.1f\n",
           g.sats_visible, g.sats_used, g.quality, (double)g.hdop);

    if (g.fix)
        printf("gps: fix %.6f %.6f  alt %.0f m  %.1f kts  %04u-%02u-%02u "
               "%02u:%02u:%02uZ\n",
               g.lat_deg, g.lon_deg, (double)g.alt_m, (double)g.speed_kts,
               g.year, g.month, g.day, g.hour, g.minute, g.second);
    else
        printf("gps: no fix yet. Sentences are framing correctly, so the "
               "receiver works; it needs sky and, cold, a few minutes.\n");
}

#else  /* board declares no GPS */

bool ls_gps_running(void) { return false; }
esp_err_t ls_gps_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void ls_gps_stop(void) { }
void ls_gps_get(ls_gps_state_t *out) { if (out) memset(out, 0, sizeof(*out)); }
void ls_gps_diagnostics(void) { printf("gps: board declares none\n"); }

#endif
