/* log_out.c - human-readable event subscriber. */

#include "log_out.h"
#include "event_bus.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "evt";

static void on_event(const event_t *e, void *user)
{
    (void)user;
    if (!e) return;

    const char *src = e->app[0] ? e->app : "?";

    switch (e->kind) {
    case EVT_BOOT:
        ESP_LOGI(TAG, "[%s] boot", src); break;
    case EVT_SHUTDOWN:
        ESP_LOGI(TAG, "[%s] shutdown", src); break;
    case EVT_DEVICE_ATTACHED:
        ESP_LOGI(TAG, "[%s] device attached", src); break;
    case EVT_DEVICE_DETACHED:
        ESP_LOGW(TAG, "[%s] device detached", src); break;
    case EVT_TUNER_LOCKED:
        ESP_LOGI(TAG, "[%s] tuner locked", src); break;
    case EVT_APP_SWITCHED:
        ESP_LOGI(TAG, "app: %s -> %s", e->u.sw.from, e->u.sw.to); break;

    case EVT_CONTACT_NEW:
        ESP_LOGI(TAG, "[%s] new      %06lX",
                 src, (unsigned long)e->u.contact.icao); break;
    case EVT_CONTACT_CONFIRMED:
        ESP_LOGI(TAG, "[%s] confirm  %06lX %s%s", src,
                 (unsigned long)e->u.contact.icao,
                 e->u.contact.callsign,
                 e->u.contact.crc_shaky ? " (shaky)" : ""); break;
    case EVT_CONTACT_LOST:
        ESP_LOGW(TAG, "[%s] lost     %06lX %s", src,
                 (unsigned long)e->u.contact.icao,
                 e->u.contact.callsign); break;
    case EVT_CONTACT_POSITION:
        ESP_LOGI(TAG, "[%s] fix      %06lX  %+.4f  %+.4f", src,
                 (unsigned long)e->u.contact.icao,
                 e->u.contact.lat, e->u.contact.lon); break;
    case EVT_CONTACT_ALTITUDE:
        ESP_LOGI(TAG, "[%s] alt      %06lX  %d ft", src,
                 (unsigned long)e->u.contact.icao,
                 e->u.contact.altitude); break;
    case EVT_CONTACT_VELOCITY:
        ESP_LOGI(TAG, "[%s] vel      %06lX  %d kt  hdg=%d  vs=%d", src,
                 (unsigned long)e->u.contact.icao,
                 e->u.contact.velocity, e->u.contact.heading,
                 e->u.contact.vert_rate); break;
    case EVT_CONTACT_IDENT:
        ESP_LOGI(TAG, "[%s] ident    %06lX  %s", src,
                 (unsigned long)e->u.contact.icao,
                 e->u.contact.callsign); break;

    case EVT_DECODE_ERROR:
        ESP_LOGW(TAG, "[%s] decode err: %s", src, e->u.text); break;
    case EVT_HEARTBEAT:
        ESP_LOGI(TAG, "[%s] HB iq=%lu B/s msgs=%d (+%d/s) crc=%d/%d ac=%d "
                       "mag=%d/%d",
                 src,
                 (unsigned long)e->u.hb.bytes_per_sec,
                 e->u.hb.msgs_total, e->u.hb.msgs_per_sec,
                 e->u.hb.crc_good, e->u.hb.crc_err, e->u.hb.active_count,
                 e->u.hb.mag_avg, e->u.hb.mag_peak);
        break;
    case EVT_LOG:
        ESP_LOG_LEVEL((esp_log_level_t)e->u.log.level, e->u.log.tag,
                      "%s", e->u.log.text);
        break;
    default: break;
    }
}

void log_out_init(void)
{
    event_bus_subscribe(on_event, NULL);
}
