/* The boot log on the chip: the hook on the log, the identity and reset
   lines, the save 20 s into the run, and the console word. Every static
   here is in PSRAM: the Nano has no internal RAM to give. */

#include "ls_bootlog.h"
#include "ls_log_line.h"
#include "ls_task_reap.h"
#include "ls_version.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define SETTLE_US 20000000

EXT_RAM_BSS_ATTR static struct {
    atomic_flag busy;                   /* a line is being put together */
    bool open;                          /* `task` holds busy            */
    bool started, spawned;
    TaskHandle_t task;
    size_t len;
    int64_t since_us;                   /* when `task` took it          */
    vprintf_like_t next;
    char line[256];
} H;

/* One call's share of a line. Formatted into the one static buffer, so no
   logging task needs a deeper stack; a line that begins while another is
   being put together is left out and counted, never waited for. Log V2
   hands a line over in pieces (ls_log_line.h): the task whose prefix took
   the buffer keeps it until its newline. */
static void keep(const char *fmt, va_list args)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    const bool mine = H.open && H.task == me;
    if (ls_log_line_start(fmt)) {
        if (!mine) {
            /* A task that ended between its prefix and its newline would
               keep the buffer for good: after a second it is taken over. */
            if (atomic_flag_test_and_set(&H.busy) && esp_timer_get_time() - H.since_us < 1000000) {
                ls_bootlog_skip();
                return;
            }
            H.since_us = esp_timer_get_time();
            H.task = me;
            H.open = true;
        }
        H.len = 0;
    } else if (!mine) {
        return;
    }
    size_t at = H.len;                  /* read once: the write below stays inside whatever H.len does */
    if (at > sizeof(H.line) - 2) at = 0;
    const size_t room = sizeof(H.line) - 1 - at;        /* one kept for '\n' */
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(H.line + at, room, fmt, copy);
    va_end(copy);
    H.len = at + (n <= 0 ? 0 : (size_t)n < room ? (size_t)n : room - 1);
    const bool ended = H.len && H.line[H.len - 1] == '\n';
    const bool full = H.len >= sizeof(H.line) - 2;
    if (!LS_LOG_LINE_IN_PIECES || ended || full) {
        if (!ended) H.line[H.len++] = '\n';
        ls_bootlog_add(H.line, H.len);
        H.open = false;
        atomic_flag_clear(&H.busy);
    }
}

static int bootlog_vprintf(const char *fmt, va_list args)
{
    keep(fmt, args);
    return H.next ? H.next(fmt, args) : vprintf(fmt, args);
}

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "power on";
    case ESP_RST_EXT:        return "reset pin";
    case ESP_RST_SW:         return "restart (software)";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT:   return "TASK WATCHDOG";
    case ESP_RST_WDT:        return "HARDWARE WATCHDOG";
    case ESP_RST_DEEPSLEEP:  return "wake from deep sleep";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_PWR_GLITCH: return "POWER GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU LOCKUP";
    default:                 return "unknown";
    }
}

void ls_bootlog_start(void)
{
    if (H.started) return;
    H.started = true;
    char line[200];
    int n = snprintf(line, sizeof(line), "LakeShark boot log\n");
    ls_bootlog_add(line, (size_t)n);
    n = ls_version_line(line, sizeof(line) - 1);
    if (n > 0) { line[n++] = '\n'; ls_bootlog_add(line, (size_t)n); }
    n = snprintf(line, sizeof(line), "reset: %s (%d)\n", reset_text(esp_reset_reason()), (int)esp_reset_reason());
    ls_bootlog_add(line, (size_t)n);
    const vprintf_like_t prev = esp_log_set_vprintf(bootlog_vprintf);
    if (prev != bootlog_vprintf) H.next = prev;
}

static void save_task(void *arg)
{
    (void)arg;
    if (ls_bootlog_save(LS_BOOTLOG_DIR))
        ESP_LOGI("bootlog", "saved to the card: lakeshark/" LS_BOOTLOG_FILE);
    ls_task_retire_self();
}

void ls_bootlog_tick(void)
{
    if (H.spawned || esp_timer_get_time() < SETTLE_US) return;
    H.spawned = true;
    /* SD only from here: a PSRAM stack must never reach NVS or flash. */
    xTaskCreatePinnedToCoreWithCaps(save_task, "bootlog", 4096, NULL, 2, NULL, tskNO_AFFINITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static int cmd_bootlog(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "save")) {
        const bool ok = ls_bootlog_save(LS_BOOTLOG_DIR);
        printf(ok ? "saved: " LS_BOOTLOG_DIR "/" LS_BOOTLOG_FILE "\n" : "not saved: no SD card?\n");
        return ok ? 0 : 1;
    }
    if (!ls_bootlog_write(stdout)) printf("busy: try again\n");
    return 0;
}

void ls_bootlog_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "bootlog",
        .help = "This boot's log from the first line; 'save' writes it to the SD card "
                "(lakeshark/bootlog.txt; the boot before is bootlog-prev.txt)",
        .hint = "[save]",
        .func = &cmd_bootlog,
    };
    esp_console_cmd_register(&cmd);
}
