/* PARTY's view of WARDRIVE: the finds of a drive under way, newest first,
   read under wd_lock and copied out so the lock is held for one pass over
   the table and nothing else.

   With CONFIG_LS_APP_MODULES both apps are modules and neither can link the
   other, so the calls go by name through ls_mod_sym("wardrive", ...): that
   needs WARDRIVE's modules.json "syms" to list ls_wardrive_running,
   wd_lock, wd_unlock, wd_stats, wd_dev_count and wd_dev_at (proposed to the
   coordinator). Until then, and while WARDRIVE has not been opened, `hooked`
   says false and PARTY shows it. Built in (the simulator, a build without
   modules) the calls are direct. */
#include <string.h>

#include "party_core.h"
#include "../wardrive/wd_core.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && defined(CONFIG_LS_APP_MODULES) && CONFIG_LS_APP_MODULES
#include "../ls_mod.h"
typedef struct {
    bool (*running)(void);
    void (*lock)(void);
    void (*unlock)(void);
    const wd_stats_t *(*stats)(void);
    int (*count)(void);
    const wd_dev_t *(*at)(int);
} wd_api_t;
static bool api(wd_api_t *a)
{
    static wd_api_t s;
    if (!s.at) {
        s.running = (bool (*)(void))ls_mod_sym("wardrive", "ls_wardrive_running", false);
        s.lock = (void (*)(void))ls_mod_sym("wardrive", "wd_lock", false);
        s.unlock = (void (*)(void))ls_mod_sym("wardrive", "wd_unlock", false);
        s.stats = (const wd_stats_t *(*)(void))ls_mod_sym("wardrive", "wd_stats", false);
        s.count = (int (*)(void))ls_mod_sym("wardrive", "wd_dev_count", false);
        s.at = (const wd_dev_t *(*)(int))ls_mod_sym("wardrive", "wd_dev_at", false);
        if (!s.running || !s.lock || !s.unlock || !s.stats || !s.count) s.at = NULL;
    }
    *a = s;
    return s.at != NULL;
}
#else
#include "../wardrive/wd_feed.h"
bool ls_wardrive_running(void);
typedef struct {
    bool (*running)(void);
    void (*lock)(void);
    void (*unlock)(void);
    const wd_stats_t *(*stats)(void);
    int (*count)(void);
    const wd_dev_t *(*at)(int);
} wd_api_t;
static bool api(wd_api_t *a)
{
    a->running = ls_wardrive_running;
    a->lock = wd_lock;
    a->unlock = wd_unlock;
    a->stats = wd_stats;
    a->count = wd_dev_count;
    a->at = wd_dev_at;
    return true;
}
#endif

int party_wd_poll(party_find_t *out, int max, uint32_t since_ms,
                  bool (*want)(uint16_t tags, const char *name, const uint8_t *addr, uint8_t kind),
                  bool *running, bool *hooked)
{
    wd_api_t a;
    *running = false;
    *hooked = api(&a);
    if (!*hooked || !a.running()) return 0;
    int n = 0;
    a.lock();
    const wd_stats_t *st = a.stats();
    *running = st->running && !st->replay;
    if (*running) {
        const int cnt = a.count();
        for (int i = 0; i < cnt && n < max; i++) {
            const wd_dev_t *d = a.at(i);
            if (!d || (int32_t)(d->last_ms - since_ms) <= 0) continue;
            /* never echo the party back to itself: a device only members reported was
               first noted by WD_RADIO_PEER; one this board heard first is its own find */
            if (d->radio == WD_RADIO_PEER) continue;
            if (!want(d->tags, d->name, d->addr, d->kind)) continue;
            party_find_t *f = &out[n++];
            memset(f, 0, sizeof(*f));
            memcpy(f->addr, d->addr, 6);
            f->kind = d->kind;
            f->tags = d->tags;
            f->rssi = d->rssi;
            memcpy(f->name, d->name, sizeof(f->name));
            f->name[sizeof(f->name) - 1] = 0;
            f->placed = (d->flags & WD_F_PLACED) != 0;
            f->lat = d->lat;
            f->lon = d->lon;
            f->ms = d->last_ms;
        }
    }
    a.unlock();
    return n;
}
