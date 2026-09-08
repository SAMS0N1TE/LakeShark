#pragma once
#include <stdbool.h>

/* Caller serializes these transitions; no task handle escapes the worker. */
typedef struct {
    bool active;
    bool running;
} settings_wifi_lifecycle_t;

static inline bool settings_wifi_claim_worker(settings_wifi_lifecycle_t *life)
{
    if (life->running) return false;
    life->running = true;
    return true;
}

static inline bool settings_wifi_retire_worker(settings_wifi_lifecycle_t *life,
                                                bool command_busy)
{
    if (life->active || command_busy) return false;
    life->running = false;
    return true;
}
