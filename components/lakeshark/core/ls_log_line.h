/* Which log line a call into an esp_log_set_vprintf hook belongs to.

   Log V1 hands the hook a whole line per call, its format beginning with
   the level letter: "E (%lu) %s: ...\n" (colours are off on every board).
   Log V2 (CONFIG_LOG_VERSION_2: the prefix is no longer stored in every
   string, about 60 KB on the T-Display) hands it over in three calls under
   the stdout lock: the prefix through the one format below, whose second
   argument is the letter, then the message in the caller's own format,
   then "\n". The letter is kept, for the task whose line it is, until that
   task's next prefix. */
#ifndef LS_LOG_LINE_H
#define LS_LOG_LINE_H

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_LOG_VERSION_2
#define LS_LOG_LINE_IN_PIECES 1
/* the prefix format in ESP-IDF 5.5's esp_log_format() */
#define LS_LOG_V2_PREFIX_FMT "%s%c %s%s%s%s%s"
#else
#define LS_LOG_LINE_IN_PIECES 0
#endif

typedef struct {
    TaskHandle_t task;          /* whose line the level belongs to */
    char level;
} ls_log_line_t;

/* True when this call begins a log line. */
static inline bool ls_log_line_start(const char *fmt)
{
    if (!fmt) return false;
#if LS_LOG_LINE_IN_PIECES
    return strcmp(fmt, LS_LOG_V2_PREFIX_FMT) == 0;
#else
    return fmt[0] && fmt[1] == ' ' && fmt[2] == '(';
#endif
}

/* 'E', 'W', 'I', 'D' or 'V' for the line this call belongs to; 0 for
   output that is not part of a line seen to begin. */
static inline char ls_log_line_level(ls_log_line_t *l, const char *fmt, va_list ap)
{
#if LS_LOG_LINE_IN_PIECES
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (ls_log_line_start(fmt)) {
        va_list c;
        va_copy(c, ap);
        (void)va_arg(c, const char *);              /* colour: "" when off */
        l->level = (char)va_arg(c, int);
        va_end(c);
        l->task = me;
        return l->level;
    }
    return l->level && l->task == me ? l->level : 0;
#else
    (void)l; (void)ap;
    return ls_log_line_start(fmt) ? fmt[0] : 0;
#endif
}

#endif
