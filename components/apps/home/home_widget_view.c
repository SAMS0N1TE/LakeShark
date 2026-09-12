#include "home_widget_view.h"
#include "ls_time.h"
#include <stdio.h>
#include <string.h>

void home_widget_present(home_widget_id_t id, const ls_hub_state_t *hub,
                         bool clock_valid, time_t now, home_widget_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (id == HOME_WIDGET_CLOCK) {
        snprintf(out->title, sizeof(out->title), "UTC CLOCK");
        if (clock_valid && now > 0) {
            char stamp[LS_TIME_STAMP_MAX];
            ls_time_render_stamp_at(stamp, sizeof(stamp), now, 0);
            snprintf(out->value, sizeof(out->value), "%.8s", stamp + 11);
            snprintf(out->detail, sizeof(out->detail), "%.10s  UTC", stamp);
        } else {
            snprintf(out->value, sizeof(out->value), "--:--:--");
            snprintf(out->detail, sizeof(out->detail), "Waiting for time sync");
        }
    } else if (id == HOME_WIDGET_SYSTEM) {
        snprintf(out->title, sizeof(out->title), "SYSTEM / LINK");
        snprintf(out->value, sizeof(out->value), "%s", !hub ? "UNKNOWN"
            : hub->c6_state == 1 ? "C6 LINKED" : hub->c6_state == 0 ? "C6 DOWN" : "C6 UNKNOWN");
        snprintf(out->detail, sizeof(out->detail), "RTL %s  |  SD %s",
            hub && hub->rtl_ready ? "attached" : "absent",
            hub && hub->sd_present ? "mounted" : "absent");
    } else {
        /* HOME unloads the receiver. Even retained active/signal fields are
         * historical here and must never imply live reception (). */
        snprintf(out->title, sizeof(out->title), "LAST USED: %s",
            hub && hub->target_app[0] ? hub->target_app : "RECEIVER");
        if (hub && hub->freq_hz)
            snprintf(out->value, sizeof(out->value), "%lu.%04lu MHz",
                (unsigned long)(hub->freq_hz / 1000000u),
                (unsigned long)((hub->freq_hz / 100u) % 10000u));
        else snprintf(out->value, sizeof(out->value), "NO LAST TUNE");
        snprintf(out->detail, sizeof(out->detail), "%s",
            hub && hub->target_app[0] ? "Receiver stopped - tap to reopen" : "Choose a receiver below");
    }
}
