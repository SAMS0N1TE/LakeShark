/* What the board's watchers see, for the native app that watches.
 * Producers and the queue are here; the app reads it through lap_seen_*
 * (ls_lap_events.c). See ls_seen.h. */
#include "ls_seen.h"
#include <stdio.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_drone_watch.h"
#include "ls_dronelink.h"
#include "ls_notify.h"
#include "ls_haptic.h"
#include "ls_tui_screen.h"
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&mux)
#define UNLOCK() portEXIT_CRITICAL(&mux)
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
#define EXT_RAM_BSS_ATTR
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
int64_t (*ls_seen_host_clock)(void);
static int64_t now_us(void) { return ls_seen_host_clock ? ls_seen_host_clock() : 0; }
#endif

static EXT_RAM_BSS_ATTR ls_seen_t queue[LS_SEEN_QUEUE];
static unsigned head, count;
static uint32_t wanted, dropped;
/* The same sighting twice in one scan pass is one. */
typedef struct { uint64_t entity; int64_t at; uint8_t source, category, section; bool validated; } duplicate_t;
static EXT_RAM_BSS_ATTR duplicate_t duplicates[64];
static unsigned duplicate_at;
static uint64_t secret[2];
static bool keyed;
/* Drone watch's own alert while no app takes it: the newest waits. */
static struct { bool on, approaching; int rssi; unsigned band; } watch_notice;
static int64_t watch_notice_at;

uint32_t ls_seen_wanted(void) { return __atomic_load_n(&wanted, __ATOMIC_ACQUIRE); }
void ls_seen_want(uint32_t sources) {
    sources &= LS_SEEN_ALL;
    LOCK();
    if (sources != wanted) { head = count = 0; memset(duplicates, 0, sizeof(duplicates)); }
    __atomic_store_n(&wanted, sources, __ATOMIC_RELEASE);
    if (!sources) dropped = 0;
    UNLOCK();
}
bool ls_seen_take(ls_seen_t *out) {
    bool ok = false;
    LOCK();
    if (count) { *out = queue[head]; head = (head + 1) % LS_SEEN_QUEUE; count--; ok = true; }
    UNLOCK();
    return ok;
}
uint32_t ls_seen_dropped(void) { return __atomic_load_n(&dropped, __ATOMIC_RELAXED); }

/* Lock held. */
static void push(const ls_seen_t *s) {
    if (count == LS_SEEN_QUEUE) { dropped++; return; }
    queue[(head + count++) % LS_SEEN_QUEUE] = *s;
}
static int8_t dbm(int rssi) { return (int8_t)(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi); }

/* SipHash-2-4 over one eight-byte address input, keyed once a boot. The key never leaves. */
static uint64_t rot(uint64_t x, unsigned n) { return (x << n) | (x >> (64 - n)); }
#define ROUND() do { v0 += v1; v1 = rot(v1, 13); v1 ^= v0; v0 = rot(v0, 32); v2 += v3; v3 = rot(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = rot(v3, 21); v3 ^= v0; v2 += v1; v1 = rot(v1, 17); v1 ^= v2; v2 = rot(v2, 32); } while (0)
static uint64_t pseudonym(const uint8_t *mac, uint8_t type, uint8_t source) {
    uint64_t m = type | ((uint64_t)(source == LS_SEEN_WIFI) << 8);
    for (unsigned i = 0; i < 6; i++) m |= (uint64_t)mac[i] << (16 + i * 8);
    uint64_t v0 = secret[0] ^ UINT64_C(0x736f6d6570736575), v1 = secret[1] ^ UINT64_C(0x646f72616e646f6d),
             v2 = secret[0] ^ UINT64_C(0x6c7967656e657261), v3 = secret[1] ^ UINT64_C(0x7465646279746573);
    v3 ^= m; ROUND(); ROUND(); v0 ^= m; m = UINT64_C(8) << 56; v3 ^= m; ROUND(); ROUND(); v0 ^= m; v2 ^= 255;
    ROUND(); ROUND(); ROUND(); ROUND();
    return v0 ^ v1 ^ v2 ^ v3;
}
#undef ROUND
static void key(void) {
    if (keyed) return;
#if defined(ESP_PLATFORM)
    esp_fill_random(secret, sizeof(secret));
#else
    secret[0] = UINT64_C(0x123456789abcdef); secret[1] = UINT64_C(0xfedcba9876543210);
#endif
    keyed = true;
}

void ls_seen_observe(const uint8_t mac[6], uint8_t type, uint8_t source, uint8_t category,
                     int rssi, bool validated, bool flood, uint8_t section, int64_t now) {
    if (!mac || now < 0 || (source != LS_SEEN_RID && source != LS_SEEN_BLE && source != LS_SEEN_WIFI)) return;
    if (!(ls_seen_wanted() & source)) return;
    key();
    const uint64_t entity = pseudonym(mac, type, source);
    ls_seen_t s = {.source = source, .category = category, .section = section, .rssi = dbm(rssi),
                   .flags = (validated ? LS_SEEN_F_VALIDATED : 0) | (flood ? LS_SEEN_F_FLOOD : 0),
                   .entity = entity, .at_us = now};
    LOCK();
    bool dup = false;
    for (unsigned i = 0; i < 64 && !dup; i++) {
        const duplicate_t *d = &duplicates[i];
        dup = d->source == source && d->entity == entity && d->at == now && d->category == category &&
              d->validated == validated && d->section == section;
    }
    if (!dup) {
        duplicates[duplicate_at++ % 64] = (duplicate_t){entity, now, source, category, section, validated};
        push(&s);
    }
    UNLOCK();
}
void ls_seen_drone(unsigned band, unsigned channel, unsigned rate_guess, int rssi, unsigned hits_per_min, int64_t now) {
    if ((band != 868 && band != 915) || now < 0 || !(ls_seen_wanted() & LS_SEEN_LORA)) return;
    ls_seen_t s = {.source = LS_SEEN_LORA, .category = 2 /* SW_DRONE */, .section = 255, .rssi = dbm(rssi),
                   .band = (uint16_t)band, .entity = ((uint64_t)band << 32) | channel, .a = channel,
                   .b = rate_guess, .hits_per_min = hits_per_min, .at_us = now};
    LOCK(); push(&s); UNLOCK();
}
void ls_seen_signal(const ls_det_signal_t *g) {
    if (!g || !(ls_seen_wanted() & g->source & DET_SIGNALS)) return;
    ls_seen_t s = {.source = g->source, .category = 255, .section = 255, .rssi = g->rssi, .a = g->a, .b = g->b,
                   .at_us = now_us()};
    if (g->flag & 1) s.flags = g->source == DET_P25 ? LS_SEEN_F_EMERGENCY : (g->source & (DET_REC | DET_CATCH)) ? LS_SEEN_F_NEW : 0;
    memcpy(s.text, g->text, sizeof(s.text) - 1);
    LOCK(); push(&s); UNLOCK();
}
bool ls_seen_drone_watch(uint64_t id, int rssi, bool calibrated, unsigned band, bool approaching, int64_t now) {
    if (!id || now < 0) return false;
    if (ls_seen_wanted() & LS_SEEN_DRONE) {
        ls_seen_t s = {.source = LS_SEEN_DRONE, .category = 2 /* SW_DRONE */, .section = 255, .rssi = dbm(rssi),
                       .flags = (calibrated ? LS_SEEN_F_VALIDATED : 0) | (approaching ? LS_SEEN_F_APPROACHING : 0),
                       .band = (uint16_t)band, .entity = id, .at_us = now};
        bool ok;
        LOCK(); ok = count < LS_SEEN_QUEUE; push(&s); UNLOCK();
        return ok;
    }
    /* No app takes it: the board's own banner and vibration, at most one
       every 2 s, posted from the TUI task. */
    bool ok = false;
    LOCK();
    if (!watch_notice_at || now - watch_notice_at >= 2000000) {
        watch_notice.on = true; watch_notice.approaching = approaching; watch_notice.rssi = rssi; watch_notice.band = band;
        watch_notice_at = now; ok = true;
    }
    UNLOCK();
    return ok;
}

bool ls_seen_pending(void) {
#if defined(ESP_PLATFORM)
    return ls_seen_wanted() || ls_dronelink_pending();
#else
    return ls_seen_wanted() != 0;
#endif
}
void ls_seen_tick(void) {
#if defined(ESP_PLATFORM)
    /* One RF task owns all LoRa calls; this worker only posts intent. */
    ls_drone_watch_legacy((ls_seen_wanted() & LS_SEEN_LORA) != 0);
#endif
}
void ls_seen_banner_poll(void) {
    bool on, approaching; int rssi; unsigned band;
    LOCK();
    on = watch_notice.on; approaching = watch_notice.approaching; rssi = watch_notice.rssi; band = watch_notice.band;
    watch_notice.on = false;
    UNLOCK();
    if (!on) return;
#if defined(ESP_PLATFORM)
    ls_notice_t n = {0};
    snprintf(n.title, sizeof(n.title), "DRONE WATCH");
    snprintf(n.body, sizeof(n.body), "Drone link nearby, %u MHz %d dB%s", band, rssi, approaching ? ", approaching" : "");
    n.screen = -1;
    for (int i = 0; i < ls_tui_screen_count(); i++) {
        const char *name = ls_tui_screen_name(i);
        if (name && !strcmp(name, "DRONES")) { n.screen = i; break; }
    }
    n.hue = n.accent = TUI_CYAN;
    n.quiet = n.steady = true;
    ls_notify_post_quiet(&n);
    if (ls_haptic_present()) ls_haptic_play(LS_HAPTIC_ALERT);
#else
    (void)approaching; (void)rssi; (void)band;
#endif
}
