#ifndef P25_ENTRY_SETTINGS_H
#define P25_ENTRY_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "app_registry.h"
#include "esp_err.h"
#include "p25_cqpsk_controls.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    p25_cqpsk_config_t cqpsk;
    int                gain_tenths;
    uint32_t           freq_hz;
    bool               auto_follow;
    bool               skip_encrypted;
    uint32_t           encrypted_skip_ms;
} p25_entry_settings_t;

/* Snapshot every flash-backed setting consumed by P25 app entry.  On dispatch
 * failure, out still contains bounded defaults derived from app/fallback_hz. */
esp_err_t p25_entry_settings_load(const app_t *app, uint32_t fallback_hz,
                                  p25_entry_settings_t *out);

#ifdef __cplusplus
}
#endif

#endif
