/* tui_task.c - UART poll, key dispatch, periodic redraw. */

#include "tui_task.h"
#include "tui.h"
#include "app_registry.h"
#include "audio_out.h"
#include "audio_events.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TUI_UART        UART_NUM_0
#define DRAW_HZ         6
#define DRAW_PERIOD_MS  (1000 / DRAW_HZ)

static bool dispatch_key(tui_key_t raw)
{
    if (raw == TK_NONE) return false;
    int k = (int)raw;

    /* When the settings page is mid-edit (frequency entry), every key
     * goes to settings_handle_key() first - including digits, which
     * would otherwise be intercepted by the page-switch shortcuts
     * below. The settings handler returns true if it consumed the key,
     * which it always does in edit mode. */
    if (settings_is_editing()) {
        if (settings_handle_key((tui_key_t)k)) return true;
    }

    switch (k) {
    case '1': page_set(PAGE_MAIN);      tui_mark_dirty(); return true;
    case '2': page_set(PAGE_SIGNAL);    tui_mark_dirty(); return true;
    case '3': page_set(PAGE_LOG);       tui_mark_dirty(); return true;
    case '4': page_set(PAGE_SETTINGS);  tui_mark_dirty(); return true;
    case '5': page_set(PAGE_DIAG);      tui_mark_dirty(); return true;
    case TK_TAB: page_cycle_next();     tui_mark_dirty(); return true;
    case 'a': case 'A':
        app_cycle_next();
        tui_waterfall_reset();
        tui_mark_dirty();
        return true;
    default: break;
    }

    if (settings_handle_key((tui_key_t)k)) return true;

    switch (k) {
    case 'm': case 'M':
        audio_toggle_mute();
        tui_log(2, "AUDIO    %s", audio_is_muted() ? "muted" : "unmuted");
        return true;
    case '+': case '=':
        audio_volume_delta(+10);
        tui_log(5, "AUDIO    vol %d%%", audio_volume_get());
        return true;
    case '-':
        audio_volume_delta(-10);
        tui_log(5, "AUDIO    vol %d%%", audio_volume_get());
        return true;
    case 'c': case 'C': {
        audio_mode_t m = audio_event_mode_cycle(AUDIO_EVT_NEW_CONTACT);
        tui_log(2, "CONTACT  audio=%s", audio_mode_label(m));
        tui_mark_dirty();
        return true;
    }
    case 'l': case 'L': {
        audio_mode_t m = audio_event_mode_cycle(AUDIO_EVT_LOST_CONTACT);
        tui_log(2, "LOST     audio=%s", audio_mode_label(m));
        tui_mark_dirty();
        return true;
    }
    case 'p': case 'P': {
        audio_mode_t m = audio_event_mode_cycle(AUDIO_EVT_POSITION);
        tui_log(2, "POSITION audio=%s", audio_mode_label(m));
        tui_mark_dirty();
        return true;
    }
    case 'b': case 'B': {
        audio_mode_t m = audio_event_mode_cycle(AUDIO_EVT_BOOT);
        tui_log(2, "BOOT     audio=%s", audio_mode_label(m));
        tui_mark_dirty();
        return true;
    }
    case 'v': case 'V': {
        audio_mode_t cur  = audio_event_mode_get(AUDIO_EVT_NEW_CONTACT);
        audio_mode_t next = (cur + 1) % AUD_MODE_COUNT;
        audio_event_mode_set_all(next);
        tui_log(2, "ALL AUDIO = %s", audio_mode_label(next));
        tui_mark_dirty();
        return true;
    }
    default: break;
    }

    const app_t *a = app_current();
    if (a && a->on_key) { a->on_key((tui_key_t)k); return true; }
    return false;
}

static void tui_task(void *arg)
{
    /* Install our own UART driver with an 8 KB TX ring buffer. Without this,
     * a full frame (~6-8 KB of ANSI escapes) at 115200 baud blocks for
     * ~500 ms in printf, starving IDLE0 and tripping the task watchdog. */
    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(TUI_UART, &uart_cfg);
    esp_err_t r = uart_driver_install(TUI_UART, 512, 8192, 0, NULL, 0);
    if (r != ESP_OK) {
        uart_driver_delete(TUI_UART);
        uart_driver_install(TUI_UART, 512, 8192, 0, NULL, 0);
    }
    extern void esp_vfs_dev_uart_use_driver(int uart_num);
    esp_vfs_dev_uart_use_driver(TUI_UART);

    tui_init();
    tui_log(1, "INIT     TUI task running");

    int64_t last_draw = 0;
    while (1) {
        uint8_t b;
        int drained = 0;
        while (uart_read_bytes(TUI_UART, &b, 1, 0) > 0 && drained < 16) {
            tui_key_t k = key_feed(b);
            if (k != TK_NONE) dispatch_key(k);
            drained++;
        }
        tui_key_t t = key_flush_timeout();
        if (t != TK_NONE) dispatch_key(t);

        int64_t now = esp_timer_get_time();
        if (now - last_draw >= DRAW_PERIOD_MS * 1000LL) {
            tui_draw();
            last_draw = now;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void tui_task_start(void)
{
    xTaskCreatePinnedToCore(tui_task, "tui", 8192, NULL, 4, NULL, 0);
}
