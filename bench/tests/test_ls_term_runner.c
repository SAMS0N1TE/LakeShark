/* Drive the production worker at notification boundaries without host threads. */
#include "ls_test.h"
#include <setjmp.h>
#include <stdio.h>
#undef stdout
#undef stderr
static FILE *s_test_stdout, *s_test_stderr;
#define stdout s_test_stdout
#define stderr s_test_stderr
#include "../../components/apps/tui/ls_term_esp.c"

static jmp_buf s_yield;
static TaskFunction_t s_worker;
static unsigned s_notifications, s_calls, s_deletes;
static bool s_stop_in_command, s_fail_create, s_fail_cookie;
static char s_ran[LS_TERM_INPUT_MAX + 1];

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf) { return buf; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait)
{ (void)lock; (void)wait; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock) { (void)lock; return pdTRUE; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *task, BaseType_t core)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    if (s_fail_create) return pdFAIL;
    s_worker = fn;
    *task = &s_worker;
    return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{ (void)task; s_notifications++; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear; (void)wait;
    LS_CHECK(s_notifications > 0);
    s_notifications = 0;
    return 1;
}
void vTaskDelete(TaskHandle_t task)
{ (void)task; s_deletes++; longjmp(s_yield, 1); }
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io)
{ (void)cookie; (void)mode; (void)io; return s_fail_cookie ? NULL : tmpfile(); }
esp_err_t esp_console_run(const char *line, int *rc)
{
    s_calls++;
    snprintf(s_ran, sizeof(s_ran), "%s", line);
    *rc = 0;
    if (s_stop_in_command) ls_term_stop();
    return ESP_OK;
}
void esp_console_get_completion(const char *line, linenoiseCompletions *lc)
{ (void)line; lc->len = 0; lc->cvec = NULL; }

static void drain(void)
{
    if (!setjmp(s_yield)) s_worker(NULL);
}

LS_CASE(queued_command_is_cancelled_on_exit_and_restart_accepts_work)
{
    s_calls = s_deletes = 0;
    LS_CHECK(ls_term_start());
    LS_CHECK(ls_term_submit("abandoned"));
    LS_CHECK(!ls_term_submit("overwrite"));
    ls_term_stop();
    drain();
    LS_EQ_INT(s_calls, 0);
    LS_CHECK(!ls_term_running());
    LS_CHECK(!ls_term_busy());
    LS_EQ_INT(s_deletes, 1);
    LS_CHECK(ls_term_start());
    LS_CHECK(ls_term_submit("help"));
    s_stop_in_command = true;
    FILE *out = stdout, *err = stderr;
    drain();
    LS_EQ_INT(s_calls, 1);
    LS_EQ_STR(s_ran, "help");
    LS_CHECK(stdout == out && stderr == err);
    LS_CHECK(!ls_term_busy());
    LS_CHECK(!ls_term_running());
    LS_EQ_INT(s_deletes, 2);
    s_stop_in_command = false;
}

LS_CASE(failed_start_and_missing_capture_can_retire_and_restart)
{
    s_fail_create = true;
    LS_CHECK(!ls_term_start());
    LS_CHECK(!ls_term_submit("help"));
    s_fail_create = false;
    LS_CHECK(ls_term_start());
    LS_CHECK(ls_term_submit("help"));
    s_fail_cookie = true;
    ls_term_stop();
    drain();
    LS_CHECK(!ls_term_busy());
    s_fail_cookie = false;
    LS_CHECK(ls_term_start());
    ls_term_stop();
    drain();
    LS_CHECK(!ls_term_running());
}
