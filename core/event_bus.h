/*
 * event_bus.h - publish/subscribe spine for the SDR tool.
 *
 * Apps publish events. Consumers (TUI, human log, event stream) subscribe.
 * Apps never call consumer code directly, which keeps the build matrix
 * (TUI on/off x log on/off x stream on/off) clean.
 *
 * Event payloads are union-typed; new event kinds extend the enum and the
 * union without breaking existing consumers (they just ignore unknown kinds).
 */
#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVT_MAX_SUBSCRIBERS  8
#define EVT_APP_NAME_MAX     8
#define EVT_TEXT_MAX         32

typedef enum {
    EVT_NONE = 0,

    EVT_BOOT,
    EVT_SHUTDOWN,

    EVT_DEVICE_ATTACHED,
    EVT_DEVICE_DETACHED,
    EVT_TUNER_LOCKED,

    EVT_APP_SWITCHED,

    EVT_CONTACT_NEW,
    EVT_CONTACT_CONFIRMED,
    EVT_CONTACT_LOST,
    EVT_CONTACT_POSITION,
    EVT_CONTACT_ALTITUDE,
    EVT_CONTACT_VELOCITY,
    EVT_CONTACT_IDENT,

    EVT_DECODE_ERROR,
    EVT_HEARTBEAT,

    EVT_LOG,

    EVT_KIND_COUNT
} evt_kind_t;

typedef struct {
    uint32_t icao;
    char     callsign[9];
    int      altitude;
    int      velocity;
    int      heading;
    float    lat;
    float    lon;
    bool     pos_valid;
    int      vert_rate;
    int      category;
    bool     crc_shaky;
} evt_contact_t;

typedef struct {
    uint32_t bytes_per_sec;
    int      msgs_total;
    int      msgs_per_sec;
    int      crc_good;
    int      crc_err;
    int      mag_avg;
    int      mag_peak;
    int      active_count;
} evt_heartbeat_t;

typedef struct {
    int  level;
    char tag[16];
    char text[96];
} evt_log_t;

typedef struct {
    char from[EVT_APP_NAME_MAX + 1];
    char to[EVT_APP_NAME_MAX + 1];
} evt_app_switch_t;

typedef struct {
    evt_kind_t kind;
    int64_t    ts_us;
    char       app[EVT_APP_NAME_MAX + 1];
    union {
        evt_contact_t    contact;
        evt_heartbeat_t  hb;
        evt_log_t        log;
        evt_app_switch_t sw;
        char             text[EVT_TEXT_MAX];
    } u;
} event_t;

typedef void (*event_subscriber_fn)(const event_t *e, void *user);

void event_bus_init(void);
int  event_bus_subscribe(event_subscriber_fn fn, void *user);
void event_bus_publish(const event_t *e);

void event_bus_publish_simple(evt_kind_t kind, const char *app);
void event_bus_publish_contact(evt_kind_t kind, const char *app,
                               const evt_contact_t *c);
void event_bus_publish_heartbeat(const char *app, const evt_heartbeat_t *hb);

const char *evt_kind_name(evt_kind_t k);

#ifdef __cplusplus
}
#endif

#endif
