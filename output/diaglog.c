#include "diaglog.h"
#include "event_bus.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DIAGLOG_UART_NUM   3
#define DIAGLOG_UART_TX    2
#define DIAGLOG_UART_BAUD  115200
#define DIAGLOG_LINE_MAX   320

static bool s_ready = false;
static SemaphoreHandle_t s_mux = NULL;

static void write_line_locked(const char *buf, int len)
{
    if (len <= 0) return;
    uart_write_bytes(DIAGLOG_UART_NUM, buf, len);
    uart_write_bytes(DIAGLOG_UART_NUM, "\r\n", 2);
}

void diaglog_raw(const char *line, int n)
{
    if (!s_ready || !line || n <= 0) return;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    write_line_locked(line, n);
    if (s_mux) xSemaphoreGive(s_mux);
}

void diaglog_vwrite(char level, const char *tag, const char *fmt, va_list ap)
{
    if (!s_ready) return;
    char buf[DIAGLOG_LINE_MAX];
    int64_t t_ms = esp_timer_get_time() / 1000;
    int hdr = snprintf(buf, sizeof(buf), "%lld %c %s ",
                       (long long)t_ms, level ? level : 'I',
                       tag ? tag : "-");
    if (hdr < 0) return;
    if (hdr >= (int)sizeof(buf)) hdr = (int)sizeof(buf) - 1;
    int n = vsnprintf(buf + hdr, sizeof(buf) - hdr, fmt, ap);
    int total;
    if (n < 0) total = hdr;
    else if (hdr + n >= (int)sizeof(buf)) total = (int)sizeof(buf) - 1;
    else total = hdr + n;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    write_line_locked(buf, total);
    if (s_mux) xSemaphoreGive(s_mux);
}

void diaglog_write(char level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diaglog_vwrite(level, tag, fmt, ap);
    va_end(ap);
}

static void on_event(const event_t *e, void *user)
{
    (void)user;
    if (!e || !s_ready) return;
    const char *src = e->app[0] ? e->app : "?";
    switch (e->kind) {
    case EVT_BOOT:
        diaglog_write('I', "evt", "[%s] boot", src); break;
    case EVT_SHUTDOWN:
        diaglog_write('I', "evt", "[%s] shutdown", src); break;
    case EVT_DEVICE_ATTACHED:
        diaglog_write('I', "evt", "[%s] device attached", src); break;
    case EVT_DEVICE_DETACHED:
        diaglog_write('W', "evt", "[%s] device detached", src); break;
    case EVT_TUNER_LOCKED:
        diaglog_write('I', "evt", "[%s] tuner locked", src); break;
    case EVT_APP_SWITCHED:
        diaglog_write('I', "evt", "app: %s -> %s", e->u.sw.from, e->u.sw.to); break;
    case EVT_HEARTBEAT:
        diaglog_write('I', "evt",
                      "[%s] HB iq=%lu B/s msgs=%d crc=%d/%d ac=%d",
                      src,
                      (unsigned long)e->u.hb.bytes_per_sec,
                      e->u.hb.msgs_total,
                      e->u.hb.crc_good, e->u.hb.crc_err,
                      e->u.hb.active_count);
        break;
    case EVT_DECODE_ERROR:
        diaglog_write('W', "evt", "[%s] decode err: %s", src, e->u.text); break;
    case EVT_LOG: {
        char lvl;
        switch (e->u.log.level) {
        case 0: lvl = 'E'; break;
        case 1: lvl = 'W'; break;
        case 2: lvl = 'I'; break;
        default: lvl = 'D'; break;
        }
        diaglog_write(lvl, e->u.log.tag[0] ? e->u.log.tag : "log",
                      "%s", e->u.log.text);
        break;
    }
    default: break;
    }
}

void diaglog_init(void)
{
    if (s_ready) return;
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    if (!uart_is_driver_installed(DIAGLOG_UART_NUM)) {
        const uart_config_t cfg = {
            .baud_rate  = DIAGLOG_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(DIAGLOG_UART_NUM, 256, 4096, 0, NULL, 0) != ESP_OK) return;
        uart_param_config(DIAGLOG_UART_NUM, &cfg);
        uart_set_pin(DIAGLOG_UART_NUM, DIAGLOG_UART_TX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    s_ready = true;
    diaglog_write('I', "diaglog", "ready uart=%d tx=GPIO%d baud=%d",
                  DIAGLOG_UART_NUM, DIAGLOG_UART_TX, DIAGLOG_UART_BAUD);
    event_bus_subscribe(on_event, NULL);
}
