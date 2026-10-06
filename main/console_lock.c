/* One command at a time through the console.

   esp_console_run copies the line into a single static buffer and splits it
   there, so the UART REPL and TERMINAL's runner running at once would hand a
   command the other one's arguments. Every call comes through here (the
   link wraps esp_console_run) and waits its turn. Recursive, for a command
   that runs another. */

#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

esp_err_t __real_esp_console_run(const char *cmdline, int *cmd_ret);

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

/* Before app_main, so neither caller can be first to an unmade lock. */
__attribute__((constructor)) static void console_lock_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
}

esp_err_t __wrap_esp_console_run(const char *cmdline, int *cmd_ret)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const esp_err_t e = __real_esp_console_run(cmdline, cmd_ret);
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
    return e;
}
