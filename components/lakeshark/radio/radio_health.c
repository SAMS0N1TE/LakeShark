/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#include "radio_health.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rhealth";

#define RH_MAX_ENDPOINTS     8
#define RH_SETTLE_MS         3000
#define RH_STALL_S           8
#define RH_RECOVER_TRIES     4
#define RH_RECOVER_WAIT_MS   4000
#define RH_HEALTHY_MS        10000
#define RH_FAILED_RETRY_S    120

typedef struct {
    bool used;
    bool present;
    bool fault_pending;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    rh_state_t state;
    uint64_t last_total;
    uint64_t bps_base;
    int64_t last_change_us;
    int64_t state_since_us;
    int64_t bps_window_us;
    uint32_t bps;
    unsigned recover_tries;
    bool budget_fresh;
    uint32_t recoveries;
    uint32_t attaches;
    uint32_t detaches;
} health_slot_t;

static health_slot_t s_health[RH_MAX_ENDPOINTS];
static SemaphoreHandle_t s_health_lock;
static radio_health_hooks_t s_hooks;
static bool s_subscribed;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst_size) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static health_slot_t *find_slot_locked(const char *endpoint_id, bool create)
{
    health_slot_t *free_slot = NULL;
    for (size_t i = 0; i < RH_MAX_ENDPOINTS; ++i) {
        if (s_health[i].used &&
            strcmp(s_health[i].endpoint_id, endpoint_id) == 0)
            return &s_health[i];
        if (!s_health[i].used && !free_slot) free_slot = &s_health[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->state = RH_ABSENT;
    copy_string(free_slot->endpoint_id, sizeof(free_slot->endpoint_id),
                endpoint_id);
    return free_slot;
}

const char *radio_health_state_name(rh_state_t state)
{
    switch (state) {
    case RH_ABSENT:        return "absent";
    case RH_SETTLING:      return "settling";
    case RH_OK:            return "ok";
    case RH_STALLED:       return "stalled";
    case RH_RECOVERING:    return "recovering";
    case RH_POWER_CYCLING: return "powercycle";
    case RH_FAILED:        return "failed";
    }
    return "?";
}

static void go(health_slot_t *slot, rh_state_t next, int64_t now,
               const char *why)
{
    if (slot->state == next) return;
    ESP_LOGW(TAG, "%s %s -> %s (%s)", slot->endpoint_id,
             radio_health_state_name(slot->state),
             radio_health_state_name(next), why ? why : "-");
    if (next != RH_OK) slot->budget_fresh = false;
    slot->state = next;
    slot->state_since_us = now;
}

static void endpoint_event(const ls_radio_endpoint_event_t *event, void *user)
{
    (void)user;
    if (!event || !s_health_lock) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_health_lock, portMAX_DELAY);
    health_slot_t *slot = find_slot_locked(event->endpoint_id, true);
    if (slot) {
        if (event->kind == LS_RADIO_ENDPOINT_ATTACHED) {
            slot->present = true;
            slot->fault_pending = false;
            slot->recover_tries = 0;
            slot->last_total = 0;
            slot->bps_base = 0;
            slot->bps = 0;
            slot->last_change_us = now;
            slot->bps_window_us = now;
            ++slot->attaches;
            go(slot, RH_SETTLING, now, "endpoint attached");
        } else {
            slot->present = false;
            slot->fault_pending = false;
            slot->recover_tries = 0;
            slot->bps = 0;
            ++slot->detaches;
            go(slot, RH_ABSENT, now, "endpoint detached");
        }
    }
    xSemaphoreGive(s_health_lock);
}

void radio_health_note_fault(const char *endpoint_id)
{
    if (!endpoint_id || !s_health_lock) return;
    xSemaphoreTake(s_health_lock, portMAX_DELAY);
    health_slot_t *slot = find_slot_locked(endpoint_id, false);
    if (slot && slot->present) slot->fault_pending = true;
    xSemaphoreGive(s_health_lock);
}

void radio_health_note_progress_reset(const char *endpoint_id)
{
    if (!endpoint_id || !s_health_lock) return;
    ls_radio_endpoint_info_t info;
    uint64_t total = 0;
    if (ls_radio_endpoint_get(endpoint_id, &info) == LS_RADIO_OK)
        total = info.bytes_read;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_health_lock, portMAX_DELAY);
    health_slot_t *slot = find_slot_locked(endpoint_id, false);
    if (slot) {
        slot->last_total = total;
        slot->last_change_us = now;
        slot->bps_base = total;
        slot->bps_window_us = now;
        slot->fault_pending = false;
    }
    xSemaphoreGive(s_health_lock);
}

static int stall_seconds(const health_slot_t *slot, bool data_expected,
                         int64_t now)
{
    if (!data_expected || slot->last_change_us == 0 ||
        slot->state == RH_ABSENT || slot->state == RH_SETTLING)
        return 0;
    int64_t elapsed = now - slot->last_change_us;
    return elapsed > 0 ? (int)(elapsed / 1000000LL) : 0;
}

static void update_progress(health_slot_t *slot, uint64_t total, int64_t now)
{
    if (total != slot->last_total) {
        slot->last_total = total;
        slot->last_change_us = now;
    }
    if (slot->bps_window_us == 0) {
        slot->bps_window_us = now;
        slot->bps_base = total;
    } else {
        int64_t window = now - slot->bps_window_us;
        if (window >= 1000000LL) {
            slot->bps = (uint32_t)((total - slot->bps_base) * 1000000ULL /
                                   (uint64_t)window);
            slot->bps_window_us = now;
            slot->bps_base = total;
        }
    }
}

static void tick_endpoint(const ls_radio_endpoint_info_t *info, int64_t now)
{
    void (*request_recovery)(const char *) = NULL;
    bool (*power_cycle)(const char *) = NULL;
    char action_id[LS_RADIO_ENDPOINT_ID_MAX] = {0};

    xSemaphoreTake(s_health_lock, portMAX_DELAY);
    health_slot_t *slot = find_slot_locked(info->endpoint_id, true);
    if (!slot) {
        xSemaphoreGive(s_health_lock);
        return;
    }
    if (!info->present) {
        slot->present = false;
        if (slot->state != RH_ABSENT)
            go(slot, RH_ABSENT, now, "endpoint unavailable");
        xSemaphoreGive(s_health_lock);
        return;
    }
    if (!slot->present) {
        slot->present = true;
        slot->last_change_us = now;
        slot->state_since_us = now;
        slot->state = RH_SETTLING;
    }

    update_progress(slot, info->bytes_read, now);
    bool expected = info->streaming;
    int stalled_s = stall_seconds(slot, expected, now);
    int64_t in_state_ms = (now - slot->state_since_us) / 1000;

    if (slot->fault_pending) {
        slot->fault_pending = false;
        slot->last_change_us = now - RH_STALL_S * 1000000LL;
        stalled_s = RH_STALL_S;
        go(slot, RH_STALLED, now, "driver reported an endpoint fault");
    }

    switch (slot->state) {
    case RH_ABSENT:
        go(slot, RH_SETTLING, now, "endpoint present");
        break;
    case RH_SETTLING:
        if (in_state_ms >= RH_SETTLE_MS) {
            slot->last_change_us = now;
            go(slot, RH_OK, now, "settled");
        }
        break;
    case RH_OK:
        if (!slot->budget_fresh && in_state_ms >= RH_HEALTHY_MS) {
            slot->budget_fresh = true;
            slot->recover_tries = 0;
        }
        if (stalled_s >= RH_STALL_S)
            go(slot, RH_STALLED, now, "no endpoint progress");
        break;
    case RH_STALLED:
        if (!expected) {
            slot->last_change_us = now;
            go(slot, RH_SETTLING, now, "endpoint is idle");
        } else if (stalled_s == 0) {
            go(slot, RH_OK, now, "endpoint progress resumed");
        } else if (slot->recover_tries < RH_RECOVER_TRIES &&
                   s_hooks.request_recovery) {
            ++slot->recover_tries;
            ++slot->recoveries;
            request_recovery = s_hooks.request_recovery;
            copy_string(action_id, sizeof(action_id), slot->endpoint_id);
            go(slot, RH_RECOVERING, now, "requested endpoint recovery");
        } else if (s_hooks.power_cycle &&
                   s_hooks.power_cycle_endpoint_id &&
                   strcmp(slot->endpoint_id,
                          s_hooks.power_cycle_endpoint_id) == 0) {
            power_cycle = s_hooks.power_cycle;
            copy_string(action_id, sizeof(action_id), slot->endpoint_id);
            go(slot, RH_POWER_CYCLING, now, "endpoint power cycle");
        } else {
            go(slot, RH_FAILED, now, "endpoint recovery exhausted");
        }
        break;
    case RH_RECOVERING:
        if (stalled_s == 0 && expected)
            go(slot, RH_OK, now, "recovery restored progress");
        else if (in_state_ms >= RH_RECOVER_WAIT_MS)
            go(slot, RH_STALLED, now, "recovery did not restore progress");
        break;
    case RH_POWER_CYCLING:
        if (in_state_ms >= RH_RECOVER_WAIT_MS) {
            slot->recover_tries = 0;
            slot->last_change_us = now;
            go(slot, RH_SETTLING, now, "power cycle wait complete");
        }
        break;
    case RH_FAILED:
        if (stalled_s == 0 && expected) {
            slot->recover_tries = 0;
            go(slot, RH_OK, now, "endpoint recovered independently");
        } else if (in_state_ms >= RH_FAILED_RETRY_S * 1000LL) {
            slot->recover_tries = 0;
            go(slot, RH_STALLED, now, "retrying endpoint recovery");
        }
        break;
    }
    xSemaphoreGive(s_health_lock);

    if (request_recovery) request_recovery(action_id);
    if (power_cycle && !power_cycle(action_id)) {
        xSemaphoreTake(s_health_lock, portMAX_DELAY);
        slot = find_slot_locked(action_id, false);
        if (slot) go(slot, RH_FAILED, esp_timer_get_time(),
                     "endpoint power cycle rejected");
        xSemaphoreGive(s_health_lock);
    }
}

void radio_health_tick(void)
{
    if (!s_health_lock) return;
    int64_t now = esp_timer_get_time();
    size_t count = ls_radio_endpoint_count();
    for (size_t i = 0; i < count; ++i) {
        ls_radio_endpoint_info_t info;
        if (ls_radio_endpoint_info(i, &info) == LS_RADIO_OK)
            tick_endpoint(&info, now);
    }
}

static bool health_copy(const char *endpoint_id, bool expected,
                        radio_health_snapshot_t *out)
{
    if (!endpoint_id || !out || !s_health_lock) return false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_health_lock, portMAX_DELAY);
    health_slot_t *slot = find_slot_locked(endpoint_id, false);
    if (!slot) {
        xSemaphoreGive(s_health_lock);
        return false;
    }
    memset(out, 0, sizeof(*out));
    copy_string(out->endpoint_id, sizeof(out->endpoint_id), slot->endpoint_id);
    out->state = slot->state;
    out->stall_s = stall_seconds(slot, expected, now);
    out->bytes_per_second = slot->bps;
    out->recoveries = slot->recoveries;
    out->attaches = slot->attaches;
    out->detaches = slot->detaches;
    xSemaphoreGive(s_health_lock);
    return true;
}

bool radio_health_get_for_endpoint(const ls_radio_endpoint_info_t *endpoint,
                                   radio_health_snapshot_t *out)
{
    return endpoint && health_copy(endpoint->endpoint_id,
                                   endpoint->present && endpoint->streaming,
                                   out);
}

bool radio_health_get(const char *endpoint_id,
                      radio_health_snapshot_t *out)
{
    ls_radio_endpoint_info_t info;
    bool have_endpoint = endpoint_id &&
        ls_radio_endpoint_get(endpoint_id, &info) == LS_RADIO_OK;
    return health_copy(endpoint_id,
                       have_endpoint && info.present && info.streaming, out);
}

int radio_health_report(const char *endpoint_id, char *buf, size_t len)
{
    if (!buf || len == 0) return 0;
    radio_health_snapshot_t health;
    ls_radio_endpoint_info_t info;
    bool have_health = radio_health_get(endpoint_id, &health);
    bool have_endpoint = endpoint_id &&
        ls_radio_endpoint_get(endpoint_id, &info) == LS_RADIO_OK;
    if (!have_health)
        return snprintf(buf, len, "%s health=unknown present=%s stream=%s",
                        endpoint_id ? endpoint_id : "radio",
                        have_endpoint ? (info.present ? "1" : "0") : "?",
                        have_endpoint ? (info.streaming ? "1" : "0") : "?");
    return snprintf(buf, len,
                    "%s %s present=%d stream=%d bps=%lu stall=%ds "
                    "rec=%lu attach=%lu detach=%lu",
                    health.endpoint_id, radio_health_state_name(health.state),
                    have_endpoint && info.present ? 1 : 0,
                    have_endpoint && info.streaming ? 1 : 0,
                    (unsigned long)health.bytes_per_second, health.stall_s,
                    (unsigned long)health.recoveries,
                    (unsigned long)health.attaches,
                    (unsigned long)health.detaches);
}

void radio_health_init(const radio_health_hooks_t *hooks)
{
    if (!s_health_lock) s_health_lock = xSemaphoreCreateMutex();
    if (!s_health_lock) {
        ESP_LOGE(TAG, "health mutex allocation failed");
        return;
    }
    memset(&s_hooks, 0, sizeof(s_hooks));
    if (hooks) s_hooks = *hooks;
    if (!s_subscribed)
        s_subscribed = ls_radio_endpoint_subscribe(endpoint_event, NULL) >= 0;

    size_t count = ls_radio_endpoint_count();
    for (size_t i = 0; i < count; ++i) {
        ls_radio_endpoint_info_t info;
        if (ls_radio_endpoint_info(i, &info) != LS_RADIO_OK || !info.present)
            continue;
        ls_radio_endpoint_event_t event = {
            .kind = LS_RADIO_ENDPOINT_ATTACHED,
            .capabilities = info.capabilities,
        };
        copy_string(event.endpoint_id, sizeof(event.endpoint_id),
                    info.endpoint_id);
        endpoint_event(&event, NULL);
    }

    /* the LCD first enabled health by allocating a 3072-byte
     * internal-stack task before an IQ app started.  On the measured P4 this
     * left no 4096-byte block for rtl_pump; all 16 posted transfers completed
     * once (262144 bytes) and then had no task to repost them.  Health is a
     * bounded tick already called by the LCD and headless 200/250 ms service
     * loops, so it must not compete with the USB pump for cache-safe RAM. */
    ESP_LOGI(TAG, "endpoint health watchdog initialized (stall=%ds, recoveries=%d)",
             RH_STALL_S, RH_RECOVER_TRIES);
}
