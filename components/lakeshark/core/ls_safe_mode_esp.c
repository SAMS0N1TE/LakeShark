/* ESP-IDF adapter for ls_safe_mode.c. */

#include "ls_safe_mode.h"
#include "ls_vitals.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_version.h"

static const char *TAG = "safe";

RTC_NOINIT_ATTR static ls_safe_state_t s_state;

static ls_safe_boot_t s_boot;
static bool           s_begun;
static int            s_dump_state = LS_SAFE_DUMP_UNKNOWN;
/* the healthy timer, LVGL callbacks and recovery console all rewrite
   the same CRC-sealed RTC record from different tasks. Serialize the whole
   validate/mutate/seal transaction so one writer cannot make another treat a
   temporarily stale CRC as corruption and discard recovery state. */
static portMUX_TYPE   s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static ls_safe_reset_t map_reset(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return LS_SAFE_RESET_POWERON;
    case ESP_RST_EXT:        return LS_SAFE_RESET_EXT;
    case ESP_RST_SW:         return LS_SAFE_RESET_SW;
    case ESP_RST_PANIC:      return LS_SAFE_RESET_PANIC;
    case ESP_RST_INT_WDT:    return LS_SAFE_RESET_INT_WDT;
    case ESP_RST_TASK_WDT:   return LS_SAFE_RESET_TASK_WDT;
    case ESP_RST_WDT:        return LS_SAFE_RESET_WDT;
    case ESP_RST_DEEPSLEEP:  return LS_SAFE_RESET_DEEPSLEEP;
    case ESP_RST_BROWNOUT:   return LS_SAFE_RESET_BROWNOUT;
    case ESP_RST_SDIO:       return LS_SAFE_RESET_SDIO;
    case ESP_RST_USB:        return LS_SAFE_RESET_USB;
    case ESP_RST_JTAG:       return LS_SAFE_RESET_JTAG;
    case ESP_RST_EFUSE:      return LS_SAFE_RESET_EFUSE;
    case ESP_RST_PWR_GLITCH: return LS_SAFE_RESET_PWR_GLITCH;
    case ESP_RST_CPU_LOCKUP: return LS_SAFE_RESET_CPU_LOCKUP;
    default:                 return LS_SAFE_RESET_UNKNOWN;
    }
}

const ls_safe_boot_t *ls_safe_boot_begin(void)
{
    if (s_begun) return &s_boot;
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_decide(&s_state, map_reset(esp_reset_reason()), &s_boot);
    s_begun = true;
    portEXIT_CRITICAL(&s_state_mux);

    if (s_boot.safe) {
        ESP_LOGE(TAG, "*** SAFE MODE *** (%s) - reset was %s, last boot "
                      "reached %s, %lu failed starts",
                 ls_safe_entry_name(s_boot.entry),
                 ls_safe_reset_name(s_boot.reset),
                 ls_safe_stage_name((ls_safe_stage_t)s_boot.prev_stage),
                 (unsigned long)s_boot.faults);
    } else if (s_boot.faults || s_boot.power_events) {
        ESP_LOGW(TAG, "normal boot: %lu failed start(s), %lu power reset(s) "
                      "recorded; safe mode at %lu",
                 (unsigned long)s_boot.faults,
                 (unsigned long)s_boot.power_events,
                 (unsigned long)LS_SAFE_FAULT_LIMIT);
    }
    return &s_boot;
}

const ls_safe_boot_t *ls_safe_boot_result(void) { return &s_boot; }

bool ls_safe_active(void) { return s_begun && s_boot.safe; }

void ls_safe_stage(ls_safe_stage_t stage)
{
    if (!s_begun) return;
    /* A memory reading per stage, BEFORE the critical section - the
       heap has its own lock and taking it inside this one is how two locks
       become an ordering problem. The stage names are already the vocabulary
       a failed boot reports in, so a profile keyed on them reads against the
       report without translation. */
    ls_vitals_mark(ls_safe_stage_name(stage));
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_note_stage(&s_state, stage);
    portEXIT_CRITICAL(&s_state_mux);
}

void ls_safe_app(const char *name)
{
    if (!s_begun) return;
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_note_app(&s_state, name);
    portEXIT_CRITICAL(&s_state_mux);
}

bool ls_safe_display_attempt(void)
{
    bool attempt;
    portENTER_CRITICAL(&s_state_mux);
    attempt = ls_safe_display_should_attempt(&s_state);
    if (attempt) ls_safe_display_begin(&s_state);
    portEXIT_CRITICAL(&s_state_mux);
    return attempt;
}

void ls_safe_display_done(bool painted)
{
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_display_complete(&s_state, painted);
    portEXIT_CRITICAL(&s_state_mux);
}

static void healthy_cb(void *arg)
{
    (void)arg;
    /* esp_timer's default dispatch is a task, so this is an ordinary context
       and the uptime it reads is the one the interval is defined against. */
    uint32_t up_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!ls_safe_healthy_due(up_ms)) return;
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_mark_healthy(&s_state);
    portEXIT_CRITICAL(&s_state_mux);
    ESP_LOGI(TAG, "boot healthy after %lu ms - startup fault counter cleared",
             (unsigned long)up_ms);
}

void ls_safe_healthy_arm(void)
{
    if (!s_begun || s_boot.safe) return;

    static esp_timer_handle_t s_timer;
    if (s_timer) return;

    const esp_timer_create_args_t args = {
        .callback = healthy_cb,
        .arg      = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ls_healthy",
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGW(TAG, "healthy timer not created - the fault counter will "
                      "only clear on an explicit 'safemode off'");
        s_timer = NULL;
        return;
    }
    /* One-shot measured from now. app_main runs within a few hundred ms of
       reset, so the difference from true uptime is immaterial and healthy_cb
       re-checks the real uptime anyway. */
    esp_timer_start_once(s_timer, (uint64_t)LS_SAFE_HEALTHY_MS * 1000ULL);
}

void ls_safe_note_dump(int dump_state) { s_dump_state = dump_state; }

size_t ls_safe_report(char *buf, size_t buflen)
{
    char id[LS_VERSION_LINE_MAX];
    ls_version_line(id, sizeof(id));
    return ls_safe_format(buf, buflen, &s_boot, id, s_dump_state);
}

void ls_safe_retry_normal_boot(void)
{
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_request_retry(&s_state);
    portEXIT_CRITICAL(&s_state_mux);
    ESP_LOGW(TAG, "operator asked for a normal boot - restarting with the "
                  "guard re-armed (one fault returns here)");
    esp_restart();
}

void ls_safe_stay_safe(void)
{
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_request_stay(&s_state);
    portEXIT_CRITICAL(&s_state_mux);
    ESP_LOGW(TAG, "staying in safe mode across resets until 'safemode off'");
}

void ls_safe_force(bool on)
{
    portENTER_CRITICAL(&s_state_mux);
    ls_safe_request_force(&s_state, on);
    portEXIT_CRITICAL(&s_state_mux);
}

bool ls_safe_forced(void)
{
    bool forced;
    portENTER_CRITICAL(&s_state_mux);
    forced = ls_safe_state_valid(&s_state) && s_state.forced != 0;
    portEXIT_CRITICAL(&s_state_mux);
    return forced;
}
