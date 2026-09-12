/* See ls_rtc.h for why the low-voltage flag decides everything. */
#include "ls_rtc.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"

#include "ls_board.h"
#include "ls_i2c.h"
#include "core/ls_time.h"

static const char *TAG = "ls_rtc";

#if LS_HAS_RTC

/* Register map, NXP PCF8563 datasheet table 4. */
#define REG_CONTROL1   0x00
#define REG_SECONDS    0x02   /* bit 7 is VL - oscillator stopped since set */

#define VL_BIT         0x80

/* The part counts years 00..99 against a century bit in the month register.
   Only 2000..2099 is representable, which outlives the hardware. */
#define RTC_EPOCH_YEAR 2000
#define CENTURY_BIT    0x80

static i2c_master_dev_handle_t s_dev;
static bool s_present;

/* days_from_civil, Howard Hinnant's algorithm. newlib here does not declare
   timegm, and the alternatives are worse: mktime() applies the local zone,
   which on a device with no zone is a silent offset, and pulling in
   _GNU_SOURCE for one function changes what every other header exposes.
   This is exact for the whole range the part can represent and is pure
   arithmetic, so the bench can test it without a clock. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              /* 0..399   */
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* 0..146096 */
    return era * 146097 + (int64_t)doe - 719468;
}

static time_t epoch_from_tm_utc(const struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900,
                                   (unsigned)(tm->tm_mon + 1),
                                   (unsigned)tm->tm_mday);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

static uint8_t from_bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t to_bcd(uint8_t v)   { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t tmp[10];
    if (len + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 100);
}

esp_err_t ls_rtc_start(void)
{
    if (s_present) return ESP_OK;

    /* Same bus as the expander and the touch controller. 100 kHz, because
       that is what the shared bus runs at for this family of parts. */
    esp_err_t err = ls_i2c_device(LS_I2C_PRIMARY, LS_BOARD_RTC_I2C_ADDR,
                                  100 * 1000, &s_dev);
    if (err != ESP_OK) return err;

    err = ls_i2c_probe(LS_I2C_PRIMARY, LS_BOARD_RTC_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no ACK at 0x%02X", LS_BOARD_RTC_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    /* Control1: clear STOP so the counter runs. Leave everything else as it
       was - the alarm and timer are not this driver's business, and clearing
       them would throw away a wake source someone else configured. */
    uint8_t c1 = 0;
    if (rd(REG_CONTROL1, &c1, 1) == ESP_OK && (c1 & 0x20)) {
        c1 &= (uint8_t)~0x20;
        wr(REG_CONTROL1, &c1, 1);
        ESP_LOGI(TAG, "oscillator was stopped; started it");
    }

    s_present = true;
    return ESP_OK;
}

bool ls_rtc_present(void) { return s_present; }

esp_err_t ls_rtc_get(time_t *out)
{
    if (!s_present) { esp_err_t e = ls_rtc_start(); if (e != ESP_OK) return e; }

    uint8_t r[7] = {0};
    esp_err_t err = rd(REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) return err;

    if (r[0] & VL_BIT) return ESP_ERR_INVALID_STATE;

    struct tm tm = {0};
    tm.tm_sec  = from_bcd(r[0] & 0x7F);
    tm.tm_min  = from_bcd(r[1] & 0x7F);
    tm.tm_hour = from_bcd(r[2] & 0x3F);
    tm.tm_mday = from_bcd(r[3] & 0x3F);
    tm.tm_mon  = from_bcd(r[5] & 0x1F) - 1;
    tm.tm_year = from_bcd(r[6]) + (RTC_EPOCH_YEAR - 1900) + ((r[5] & CENTURY_BIT) ? 100 : 0);

    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59) {
        /* Registers that are in range individually can still be nonsense
           together - a bus glitch reads as BCD just fine. */
        return ESP_ERR_INVALID_RESPONSE;
    }

    time_t t = epoch_from_tm_utc(&tm);
    if (t <= 0) return ESP_ERR_INVALID_RESPONSE;
    if (out) *out = t;
    return ESP_OK;
}

esp_err_t ls_rtc_set(time_t t)
{
    if (!s_present) { esp_err_t e = ls_rtc_start(); if (e != ESP_OK) return e; }

    struct tm tm;
    gmtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    if (year < RTC_EPOCH_YEAR || year > RTC_EPOCH_YEAR + 199) return ESP_ERR_INVALID_ARG;

    uint8_t r[7];
    r[0] = to_bcd((uint8_t)tm.tm_sec);            /* writing clears VL */
    r[1] = to_bcd((uint8_t)tm.tm_min);
    r[2] = to_bcd((uint8_t)tm.tm_hour);
    r[3] = to_bcd((uint8_t)tm.tm_mday);
    r[4] = to_bcd((uint8_t)tm.tm_wday);
    r[5] = (uint8_t)(to_bcd((uint8_t)(tm.tm_mon + 1)) |
                     ((year >= RTC_EPOCH_YEAR + 100) ? CENTURY_BIT : 0));
    r[6] = to_bcd((uint8_t)((year - RTC_EPOCH_YEAR) % 100));
    return wr(REG_SECONDS, r, sizeof(r));
}

bool ls_rtc_seed_system_time(void)
{
    time_t t = 0;
    if (ls_rtc_get(&t) != ESP_OK) return false;
    /* Same plausibility window ls_time uses. A backup cell that browned out
       and came back can leave a running-but-wrong clock, and a confident
       2001 date is worse than no date. */
    if (t < 1735689600 /* 2025-01-01 */) return false;

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    /* Setting the clock is not enough: ls_time gates every wall-clock stamp
       on its own flag, and that flag only ever tracked SNTP. */
    ls_time_note_set();
    ESP_LOGI(TAG, "system clock seeded from RTC");
    return true;
}

void ls_rtc_diagnostics(void)
{
    esp_err_t err = ls_rtc_start();
    if (err != ESP_OK) {
        printf("rtc: not answering at 0x%02X (%s)\n",
               LS_BOARD_RTC_I2C_ADDR, esp_err_to_name(err));
        return;
    }
    time_t t = 0;
    err = ls_rtc_get(&t);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("rtc: present, oscillator has stopped - the time it holds is "
               "not a time. Set it with 'rtc set <unix>' or let SNTP do it.\n");
        return;
    }
    if (err != ESP_OK) {
        printf("rtc: present but read failed: %s\n", esp_err_to_name(err));
        return;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    printf("rtc: %s (unix %lld)\n", buf, (long long)t);
}

#else  /* board declares no RTC */

esp_err_t ls_rtc_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool      ls_rtc_present(void) { return false; }
esp_err_t ls_rtc_get(time_t *o) { (void)o; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_rtc_set(time_t t)  { (void)t; return ESP_ERR_NOT_SUPPORTED; }
bool      ls_rtc_seed_system_time(void) { return false; }
void      ls_rtc_diagnostics(void) { printf("rtc: board declares none\n"); }

#endif
