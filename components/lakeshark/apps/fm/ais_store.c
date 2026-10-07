#include "ais_store.h"
#include <math.h>
#include <string.h>
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "ls_nvs_safe.h"
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static void lock(void) { portENTER_CRITICAL(&guard); }
static void unlock(void) { portEXIT_CRITICAL(&guard); }
#else
#define EXT_RAM_BSS_ATTR
static unsigned guard;
static void lock(void) { while (__atomic_exchange_n(&guard, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&guard, 0, __ATOMIC_RELEASE); }
#endif
static EXT_RAM_BSS_ATTR ais_vessel_t vessels[AIS_VESSELS];
static ais_options_t options = { .keep_minutes = 30, .names = true, .map = true };
static int count;
static int64_t last_heard;
static bool expired(int64_t stamp, int64_t now)
{ return now >= stamp && now-stamp > (int64_t)options.keep_minutes*60000000; }
static void prune(int64_t now)
{
    for (int i = count-1; i >= 0; --i)
        if (expired(vessels[i].heard_us, now)) {
            memmove(vessels+i, vessels+i+1, (count-i-1)*sizeof(*vessels)); --count;
        }
}
#ifdef ESP_PLATFORM
typedef struct { ais_options_t value; bool save; } option_job_t;
static esp_err_t option_job(void *arg)
{
    option_job_t *j = arg; nvs_handle_t h;
    esp_err_t err = nvs_open("ais", j->save ? NVS_READWRITE : NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    uint32_t v[4]; size_t bytes = sizeof(v);
    if (j->save) {
        v[0] = j->value.keep_minutes; v[1] = j->value.max_km;
        v[2] = j->value.names; v[3] = j->value.map;
        err = nvs_set_blob(h, "options_v1", v, bytes);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_get_blob(h, "options_v1", v, &bytes);
        if (err == ESP_OK && bytes == sizeof(v) && v[0] >= 1 && v[0] <= 1440 &&
            v[1] <= 20000 && v[2] <= 1 && v[3] <= 1)
            j->value = (ais_options_t){v[0], v[1], v[2], v[3]};
        else err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h); return err;
}
#endif
void ais_options_load(void)
{
#ifdef ESP_PLATFORM
    option_job_t j = {0};
    if (ls_nvs_run(option_job, &j, 0) == ESP_OK) { lock(); options = j.value; unlock(); }
#endif
}
void ais_options_get(ais_options_t *out) { if (out) { lock(); *out = options; unlock(); } }
void ais_options_set(const ais_options_t *o)
{
    if (!o || o->keep_minutes < 1 || o->keep_minutes > 1440 || o->max_km > 20000) return;
    lock(); options = *o; unlock();
#ifdef ESP_PLATFORM
    option_job_t j = { .value = *o, .save = true }; ls_nvs_run(option_job, &j, 0);
#endif
}
void ais_store_clear(void) { lock(); count = 0; last_heard = 0; unlock(); }
int64_t ais_store_last_heard(void) { lock(); int64_t v = last_heard; unlock(); return v; }
void ais_store_receive(const ais_vessel_t *p, void *user)
{
    (void)user;
    if (!p || !p->mmsi) return;
    int64_t now = esp_timer_get_time();
    lock(); last_heard = now; prune(now);
    int found = -1;
    for (int i = 0; i < count; ++i) if (p->mmsi == vessels[i].mmsi) { found = i; break; }
    ais_vessel_t value = found >= 0 ? vessels[found] : *p;
    value.mmsi = p->mmsi; value.message = p->message; value.channel = p->channel;
    value.heard_us = now; value.fields |= p->fields;
    if (p->fields & AIS_POSITION) {
        value.position = p->position; value.lat = p->lat; value.lon = p->lon;
        value.position_us = now;
    }
    if (p->fields & AIS_MOTION) {
        value.sog = p->sog; value.cog = p->cog; value.heading = p->heading; value.nav_status = p->nav_status;
    }
    if (p->fields & AIS_NAME) memcpy(value.name, p->name, sizeof(value.name));
    if (p->fields & AIS_STATIC) {
        value.ship_type = p->ship_type;
        /* Message 19 carries type and name but no callsign. */
        if (p->message != 19) memcpy(value.callsign, p->callsign, sizeof(value.callsign));
    }
    if (p->fields & AIS_VOYAGE) memcpy(value.destination, p->destination, sizeof(value.destination));
    if (found >= 0) { memmove(vessels+found, vessels+found+1, (count-found-1)*sizeof(*vessels)); --count; }
    if (count == AIS_VESSELS) --count;
    memmove(vessels+1, vessels, count*sizeof(*vessels)); vessels[0] = value; ++count;
    unlock();
}
int ais_store_snapshot(ais_vessel_t *out, int capacity, int64_t now)
{
    if (!out || capacity <= 0) return 0;
    lock(); prune(now);
    int n = count < capacity ? count : capacity;
    memcpy(out, vessels, n*sizeof(*out)); unlock(); return n;
}
bool ais_visible(const ais_vessel_t *p, const ais_options_t *o, int64_t now,
                  bool fix, double lat, double lon, int64_t fix_us, double *km, double *bearing)
{
    if (km) *km = -1;
    if (bearing) *bearing = 0;
    if (!p || !o || now < p->heard_us || now-p->heard_us > (int64_t)o->keep_minutes*60000000) return false;
    if (!p->position || now < p->position_us || now-p->position_us > (int64_t)o->keep_minutes*60000000 ||
        !fix || fix_us <= 0 || now < fix_us || now-fix_us > 10000000 ||
        !isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180) return true;
    const double r = 0.017453292519943295;
    double a = lat*r, b = p->lat*r, d = (p->lon-lon)*r;
    double h = pow(sin((b-a)/2), 2)+cos(a)*cos(b)*pow(sin(d/2), 2);
    h = fmax(0, fmin(1, h)); double distance = 12742*asin(sqrt(h));
    double brg = atan2(sin(d)*cos(b), cos(a)*sin(b)-sin(a)*cos(b)*cos(d))/r;
    if (km) *km = distance;
    if (bearing) *bearing = fmod(brg+360, 360);
    return !o->max_km || distance <= o->max_km;
}
