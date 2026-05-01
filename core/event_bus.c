/* event_bus.c - publisher/subscriber broadcast. */

#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    event_subscriber_fn fn;
    void               *user;
} sub_t;

static sub_t            s_subs[EVT_MAX_SUBSCRIBERS];
static int              s_sub_count = 0;
static SemaphoreHandle_t s_mux      = NULL;

void event_bus_init(void)
{
    if (s_mux) return;
    s_mux = xSemaphoreCreateMutex();
}

int event_bus_subscribe(event_subscriber_fn fn, void *user)
{
    if (!fn || s_sub_count >= EVT_MAX_SUBSCRIBERS) return -1;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    int id = s_sub_count;
    s_subs[s_sub_count].fn   = fn;
    s_subs[s_sub_count].user = user;
    s_sub_count++;
    if (s_mux) xSemaphoreGive(s_mux);
    return id;
}

void event_bus_publish(const event_t *e)
{
    if (!e) return;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int i = 0; i < s_sub_count; i++) {
        s_subs[i].fn(e, s_subs[i].user);
    }
    if (s_mux) xSemaphoreGive(s_mux);
}

void event_bus_publish_simple(evt_kind_t kind, const char *app)
{
    event_t e = { 0 };
    e.kind  = kind;
    e.ts_us = esp_timer_get_time();
    if (app) strncpy(e.app, app, EVT_APP_NAME_MAX);
    event_bus_publish(&e);
}

void event_bus_publish_contact(evt_kind_t kind, const char *app,
                               const evt_contact_t *c)
{
    event_t e = { 0 };
    e.kind  = kind;
    e.ts_us = esp_timer_get_time();
    if (app) strncpy(e.app, app, EVT_APP_NAME_MAX);
    if (c)   memcpy(&e.u.contact, c, sizeof(*c));
    event_bus_publish(&e);
}

void event_bus_publish_heartbeat(const char *app, const evt_heartbeat_t *hb)
{
    event_t e = { 0 };
    e.kind  = EVT_HEARTBEAT;
    e.ts_us = esp_timer_get_time();
    if (app) strncpy(e.app, app, EVT_APP_NAME_MAX);
    if (hb)  memcpy(&e.u.hb, hb, sizeof(*hb));
    event_bus_publish(&e);
}

static const char *KIND_NAMES[EVT_KIND_COUNT] = {
    [EVT_NONE]              = "none",
    [EVT_BOOT]              = "boot",
    [EVT_SHUTDOWN]          = "shutdown",
    [EVT_DEVICE_ATTACHED]   = "dev_attached",
    [EVT_DEVICE_DETACHED]   = "dev_detached",
    [EVT_TUNER_LOCKED]      = "tuner_locked",
    [EVT_APP_SWITCHED]      = "app_switched",
    [EVT_CONTACT_NEW]       = "contact_new",
    [EVT_CONTACT_CONFIRMED] = "contact_confirmed",
    [EVT_CONTACT_LOST]      = "contact_lost",
    [EVT_CONTACT_POSITION]  = "contact_position",
    [EVT_CONTACT_ALTITUDE]  = "contact_altitude",
    [EVT_CONTACT_VELOCITY]  = "contact_velocity",
    [EVT_CONTACT_IDENT]     = "contact_ident",
    [EVT_DECODE_ERROR]      = "decode_error",
    [EVT_HEARTBEAT]         = "heartbeat",
    [EVT_LOG]               = "log",
};

const char *evt_kind_name(evt_kind_t k)
{
    if (k < 0 || k >= EVT_KIND_COUNT || !KIND_NAMES[k]) return "?";
    return KIND_NAMES[k];
}
