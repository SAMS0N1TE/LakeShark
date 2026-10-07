#include "aprs_store.h"
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
static EXT_RAM_BSS_ATTR aprs_packet_t stations[APRS_STATIONS];
static aprs_options_t options = { .custom_hz = 144390000, .keep_minutes = 30,
    .max_km = 0, .messages = true, .map = true };
static int count;
static int64_t last_heard;
static bool expired(int64_t stamp, int64_t now)
{ return now >= stamp && now-stamp > (int64_t)options.keep_minutes*60000000; }
static void prune(int64_t now)
{
    for (int i = count-1; i >= 0; --i)
        if (expired(stations[i].heard_us, now)) {
            memmove(stations+i, stations+i+1, (count-i-1)*sizeof(*stations)); --count;
        }
}
#ifdef ESP_PLATFORM
typedef struct { aprs_options_t value; bool save; } option_job_t;
static esp_err_t option_job(void *arg)
{
    option_job_t *j = arg; nvs_handle_t h;
    esp_err_t err = nvs_open("aprs", j->save ? NVS_READWRITE : NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    uint32_t v[6]; size_t bytes = sizeof(v);
    if (j->save) {
        v[0] = j->value.custom_hz; v[1] = j->value.keep_minutes; v[2] = j->value.max_km;
        v[3] = j->value.preset; v[4] = j->value.messages; v[5] = j->value.map;
        err = nvs_set_blob(h, "options_v1", v, bytes);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_get_blob(h, "options_v1", v, &bytes);
        if (err == ESP_OK && bytes == sizeof(v) && v[0] >= 24000000 && v[0] <= 1766000000 &&
            v[1] >= 1 && v[1] <= 1440 && v[2] <= 20000 && v[3] <= 2 && v[4] <= 1 && v[5] <= 1) {
            j->value = (aprs_options_t){v[0], v[1], v[2], v[3], v[4], v[5]};
        } else err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h); return err;
}
#endif
void aprs_options_load(void)
{
#ifdef ESP_PLATFORM
    option_job_t j = {0};
    if (ls_nvs_run(option_job, &j, 0) == ESP_OK) { lock(); options = j.value; unlock(); }
#endif
}
void aprs_options_get(aprs_options_t *out) { if (out) { lock(); *out = options; unlock(); } }
void aprs_options_set(const aprs_options_t *o)
{
    if (!o || o->preset > 2 || o->keep_minutes < 1 || o->keep_minutes > 1440 ||
        o->max_km > 20000 || o->custom_hz < 24000000 || o->custom_hz > 1766000000) return;
    lock(); options = *o; unlock();
#ifdef ESP_PLATFORM
    option_job_t j = { .value = *o, .save = true }; ls_nvs_run(option_job, &j, 0);
#endif
}
uint32_t aprs_frequency(void)
{
    aprs_options_t o; aprs_options_get(&o);
    return o.preset == 0 ? 144390000 : o.preset == 1 ? 144800000 : o.custom_hz;
}
void aprs_store_clear(void) { lock(); count = 0; last_heard = 0; unlock(); }
int64_t aprs_store_last_heard(void) { lock(); int64_t v = last_heard; unlock(); return v; }
void aprs_store_receive(const aprs_packet_t *p, void *user)
{
    (void)user;
    if (!p) return;
    int64_t now = esp_timer_get_time();
    lock(); last_heard = now; prune(now);
    if (p->kind == APRS_MESSAGE && !options.messages) { unlock(); return; }
    int found = -1;
    for (int i = 0; i < count; ++i)
        if (!strcmp(p->call, stations[i].call) && !strcmp(p->name, stations[i].name)) { found = i; break; }
    if (p->killed) {
        if (found >= 0) { memmove(stations+found, stations+found+1, (count-found-1)*sizeof(*stations)); --count; }
        unlock(); return;
    }
    aprs_packet_t value = *p; value.heard_us = now;
    if (p->position) value.position_us = now;
    else if (found >= 0 && stations[found].position) {
        value.position = true; value.lat = stations[found].lat; value.lon = stations[found].lon;
        value.symbol = stations[found].symbol; value.table = stations[found].table;
        value.ambiguous = stations[found].ambiguous;
        value.position_us = stations[found].position_us;
    }
    if (found >= 0) { memmove(stations+found, stations+found+1, (count-found-1)*sizeof(*stations)); --count; }
    if (count == APRS_STATIONS) --count;
    memmove(stations+1, stations, count*sizeof(*stations)); stations[0] = value; ++count;
    unlock();
}
int aprs_store_count(int64_t now) { lock(); prune(now); int n = count; unlock(); return n; }
bool aprs_store_get(int i, int64_t now, aprs_packet_t *out)
{
    if (!out) return false;
    lock(); prune(now); bool ok = i >= 0 && i < count;
    if (ok) *out = stations[i];
    unlock(); return ok;
}
int aprs_store_snapshot(aprs_packet_t *out, int capacity, int64_t now)
{
    if (!out || capacity <= 0) return 0;
    lock(); prune(now);
    int n = count < capacity ? count : capacity;
    memcpy(out, stations, n*sizeof(*out)); unlock(); return n;
}
bool aprs_visible(const aprs_packet_t *p, const aprs_options_t *o, int64_t now,
                  bool fix, double lat, double lon, int64_t fix_us, double *km, double *bearing)
{
    if (km) *km = -1;
    if (bearing) *bearing = 0;
    if (!p || !o || now < p->heard_us || now-p->heard_us > (int64_t)o->keep_minutes*60000000 ||
        (p->kind == APRS_MESSAGE && !o->messages)) return false;
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
