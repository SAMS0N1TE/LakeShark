#include "rs41_store.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR rs41_sonde_t s_sondes[RS41_SONDES];
static int s_hours = 6;
static bool s_map = true;
int rs41_keep_hours(void) { portENTER_CRITICAL(&s_lock); int h = s_hours; portEXIT_CRITICAL(&s_lock); return h; }
void rs41_set_keep_hours(int h) { portENTER_CRITICAL(&s_lock); s_hours = h < 1 ? 1 : h > 24 ? 24 : h; portEXIT_CRITICAL(&s_lock); }
bool rs41_show_map(void) { portENTER_CRITICAL(&s_lock); bool v = s_map; portEXIT_CRITICAL(&s_lock); return v; }
void rs41_set_show_map(bool v) { portENTER_CRITICAL(&s_lock); s_map = v; portEXIT_CRITICAL(&s_lock); }
static void prune(rs41_sonde_t *s, int64_t now)
{
    if (!s->heard_us) return;
    int64_t keep = (int64_t)s_hours*3600000000LL;
    if (now >= s->heard_us && now-s->heard_us > keep) { memset(s,0,sizeof(*s)); return; }
    unsigned drop = 0;
    while (drop < s->count && now-s->track[drop].us > keep) ++drop;
    if (drop) { s->count -= drop; memmove(s->track,s->track+drop,s->count*sizeof(s->track[0])); }
    if (s->fix_us && now-s->fix_us > keep) { s->fix_us = 0; s->report.position = false; }
}
void rs41_store_clear(void)
{
    portENTER_CRITICAL(&s_lock); memset(s_sondes,0,sizeof(s_sondes)); portEXIT_CRITICAL(&s_lock);
}
bool rs41_store_copy(int slot, rs41_sonde_t *out, int64_t now)
{
    if (slot < 0 || slot >= RS41_SONDES || !out) return false;
    portENTER_CRITICAL(&s_lock);
    prune(&s_sondes[slot],now);
    bool found = s_sondes[slot].heard_us != 0;
    if (found) *out = s_sondes[slot];
    portEXIT_CRITICAL(&s_lock);
    return found;
}
void rs41_store_put(const rs41_report_t *r, int64_t now)
{
    /* Without a CRC-valid identity this frame cannot update another sonde. */
    if (!r->status || r->encrypted || now <= 0) return;
    portENTER_CRITICAL(&s_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < RS41_SONDES; ++i) {
        prune(&s_sondes[i],now);
        if (s_sondes[i].heard_us && !strcmp(s_sondes[i].report.serial,r->serial)) { slot = i; break; }
        if (s_sondes[i].heard_us < s_sondes[oldest].heard_us) oldest = i;
    }
    if (slot < 0) { slot = oldest; memset(&s_sondes[slot],0,sizeof(s_sondes[slot])); }
    rs41_sonde_t *s = &s_sondes[slot];
    bool duplicate = s->heard_us && s->report.frame == r->frame;
    double lat = s->report.lat, lon = s->report.lon, alt = s->report.alt;
    float climb = s->report.climb;
    s->report = *r; s->heard_us = now;
    if (!r->position || duplicate) {
        s->report.position = s->fix_us != 0;
        s->report.lat = lat; s->report.lon = lon; s->report.alt = alt; s->report.climb = climb;
    } else if (!duplicate) {
        s->fix_us = now;
        if (!s->count || now-s->track[s->count-1].us >= 10000000) {
            /* Thin old history in place to bound RAM while keeping the launch. */
            if (s->count == RS41_POINTS) {
                for (unsigned i = 0; i < RS41_POINTS/2; ++i) s->track[i] = s->track[2*i];
                s->count = RS41_POINTS/2;
            }
            s->track[s->count++] = (rs41_point_t){r->lat,r->lon,(float)r->alt,now};
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
