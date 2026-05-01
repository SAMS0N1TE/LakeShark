/* tui_log_hook.c - ESP_LOG capture into the TUI log page.
 *
 * Two capture paths feed the TUI log buffer:
 *
 *   1. ESP_LOG vprintf hook (esp_log_set_vprintf) - catches every
 *      ESP_LOGI/W/E from anywhere in the codebase. Strips colour
 *      escapes, classifies by level letter.
 *
 *   2. event_bus subscriber for EVT_LOG - catches structured log
 *      events emitted by app code via sys_log() / equivalent. This
 *      path is independent of CONFIG_OUTPUT_HUMAN_LOG; it always runs
 *      so the TUI log page is populated regardless of which other
 *      output sinks are enabled. Without this subscriber, sys_log()
 *      events are published to the bus but nothing turns them into
 *      visible log entries unless the optional log_out module is
 *      compiled in. */

#include "tui_log_hook.h"
#include "tui.h"
#include "event_bus.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Forward declaration: on_evt_log is the EVT_LOG event-bus subscriber,
 * defined below; we install it from tui_install_log_hook(). */
static void on_evt_log(const event_t *e, void *user);

static int tui_vprintf_hook(const char *fmt, va_list args)
{
    char buf[128];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) return n;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    /* Strip leading ANSI colour escape "\033[0;xx;xxm" emitted by ESP_LOG
     * when CONFIG_LOG_COLORS=y. */
    const char *p = buf;
    if (*p == '\033') {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }

    int color = 0;
    if      (p[0] == 'E') color = 4;
    else if (p[0] == 'W') color = 2;
    else if (p[0] == 'I') color = 1;
    else if (p[0] == 'D') color = 5;
    else if (p[0] == 'V') color = 5;

    int len = (int)strlen(p);
    while (len > 0) {
        char c = p[len - 1];
        if (c == '\n' || c == '\r') { len--; continue; }
        break;
    }
    if (len >= 4 && p[len - 4] == '\033') len -= 4;

    char clean[96];
    if (len >= (int)sizeof(clean)) len = sizeof(clean) - 1;
    memcpy(clean, p, len);
    clean[len] = 0;

    if (len > 0) tui_log(color, "%s", clean);
    return n;
}

void tui_install_log_hook(void)
{
    typedef int (*vprintf_like_t)(const char *, va_list);
    extern vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
    esp_log_set_vprintf(tui_vprintf_hook);

    /* Independent path: catch EVT_LOG events emitted via sys_log()
     * and friends. These are published to the event bus regardless
     * of which output sinks are compiled in, so subscribing here
     * guarantees the TUI log page is populated with app activity
     * without depending on CONFIG_OUTPUT_HUMAN_LOG. */
    event_bus_subscribe(on_evt_log, NULL);
}

/* EVT_LOG -> tui_log subscriber. Mirrors the colour mapping used by
 * the vprintf hook (level 1=ERROR red, 2=WARN amber, 3=INFO cyan,
 * else dim) so events from the bus and from ESP_LOG render
 * indistinguishably on the log page. */
static void on_evt_log(const event_t *e, void *user)
{
    (void)user;
    if (!e || e->kind != EVT_LOG) return;

    int color;
    switch (e->u.log.level) {
        case 1: color = 4; break;   /* ESP_LOG_ERROR */
        case 2: color = 2; break;   /* ESP_LOG_WARN  */
        case 3: color = 0; break;   /* ESP_LOG_INFO  */
        default: color = 5; break;
    }

    /* Build a "[app] text" string so the source app is visible on
     * the log page; without this, P25 status lines are
     * indistinguishable from radio-stack lines. */
    if (e->app[0]) {
        char line[120];
        snprintf(line, sizeof(line), "[%s] %s", e->app, e->u.log.text);
        tui_log(color, "%s", line);
    } else {
        tui_log(color, "%s", e->u.log.text);
    }
}
