/* TERMINAL's command runner on the chip: a worker task that runs one line at
   a time through esp_console_run with its own stdout pointed at the
   scrollback. stdout is per task in newlib, so only what the command (and
   its own log lines) print is captured; other tasks keep printing to the
   UART as before. See ls_term.h. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* fopencookie */
#endif

#include "ls_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_console.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"

/* The REPL task runs commands in 4 KB with about 1.4 KB to spare. */
#define TERM_STACK 4608

EXT_RAM_BSS_ATTR static ls_term_scroll_t s_scroll;
EXT_RAM_BSS_ATTR static char             s_line[LS_TERM_INPUT_MAX + 1];
EXT_RAM_BSS_ATTR static char             s_capbuf[256];

static SemaphoreHandle_t s_lock;           /* the scrollback and the state below */
static StaticSemaphore_t s_lock_buf;
static TaskHandle_t      s_task;
static bool              s_quit;
static volatile bool     s_busy;

static void lock_init(void)
{
    /* the UI task is the only caller before the worker exists */
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
}

static void put(const char *data, size_t n)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ls_term_scroll_put(&s_scroll, data, n);
    xSemaphoreGive(s_lock);
}

static ssize_t cap_write(void *cookie, const char *buf, size_t n)
{
    (void)cookie;
    put(buf, n);
    return (ssize_t)n;
}

static void report(esp_err_t e, int rc)
{
    char note[64];
    if (e == ESP_ERR_NOT_FOUND)          snprintf(note, sizeof(note), "unknown command ('help' lists them)\n");
    else if (e == ESP_ERR_INVALID_STATE) snprintf(note, sizeof(note), "the console is not running\n");
    else if (e != ESP_OK && e != ESP_ERR_INVALID_ARG) snprintf(note, sizeof(note), "could not run: %s\n", esp_err_to_name(e));
    else if (e == ESP_OK && rc)          snprintf(note, sizeof(note), "(exit %d)\n", rc);
    else return;
    put(note, strlen(note));
}

static void term_task(void *arg)
{
    (void)arg;
    cookie_io_functions_t io = { .write = cap_write };
    FILE *cap = fopencookie(NULL, "w", io);
    if (cap) setvbuf(cap, s_capbuf, _IOLBF, sizeof(s_capbuf));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool quit = s_quit;
        if (quit) s_task = NULL;          /* decided with the lock held: a start
                                             after this makes a new runner */
        xSemaphoreGive(s_lock);
        if (quit) break;
        if (!s_busy) continue;
        int rc = 0;
        esp_err_t e = ESP_FAIL;
        if (cap) {
            FILE *out = stdout, *err = stderr;
            stdout = cap;
            stderr = cap;
            e = esp_console_run(s_line, &rc);
            fflush(cap);
            stdout = out;
            stderr = err;
        }
        report(e, rc);
        s_busy = false;
    }
    if (cap) fclose(cap);
    vTaskDelete(NULL);
}

bool ls_term_start(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_quit = false;                        /* one still finishing is kept */
    bool ok = s_task != NULL;
    if (!ok) ok = xTaskCreatePinnedToCore(term_task, "term", TERM_STACK, NULL, 2, &s_task,
                                          tskNO_AFFINITY) == pdPASS;
    if (!ok) s_task = NULL;
    xSemaphoreGive(s_lock);
    return ok;
}

void ls_term_stop(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_quit = true;
    if (s_task) xTaskNotifyGive(s_task);
    xSemaphoreGive(s_lock);
}

bool ls_term_running(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = s_task != NULL && !s_quit;
    xSemaphoreGive(s_lock);
    return on;
}

bool ls_term_submit(const char *line)
{
    if (!line || s_busy) return false;
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_task != NULL && !s_quit;
    if (ok) {
        snprintf(s_line, sizeof(s_line), "%s", line);
        s_busy = true;
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool ls_term_busy(void) { return s_busy; }

void ls_term_out(const char *text)
{
    if (text) put(text, strlen(text));
}

void ls_term_clear(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ls_term_scroll_reset(&s_scroll);
    xSemaphoreGive(s_lock);
}

uint32_t ls_term_gen(void) { return s_scroll.gen; }

size_t ls_term_text(char *out, size_t n)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t k = ls_term_scroll_copy(&s_scroll, out, n);
    xSemaphoreGive(s_lock);
    return k;
}

int ls_term_complete(const char *line, char *out, size_t n, char *list, size_t list_n)
{
    if (out && n) out[0] = '\0';
    if (list && list_n) list[0] = '\0';
    if (!line || strchr(line, ' ')) return 0;      /* command names only */
    linenoiseCompletions lc = { 0, NULL };
    esp_console_get_completion(line, &lc);
    const int count = (int)lc.len;
    size_t k = 0;
    for (size_t i = 0; i < lc.len; i++) {
        if (count == 1 && out && n) snprintf(out, n, "%s ", lc.cvec[i]);
        if (list && k + 1 < list_n) {
            const int w = snprintf(list + k, list_n - k, "%s%s", k ? " " : "", lc.cvec[i]);
            if (w > 0) k = (size_t)w >= list_n - k ? list_n - 1 : k + (size_t)w;
        }
        free(lc.cvec[i]);
    }
    free(lc.cvec);
    return count;
}
