#include "p25_entry_settings.h"

#include "ls_nvs_safe.h"
#include "p25_controls.h"
#include "settings.h"

typedef struct {
    const app_t         *app;
    p25_entry_settings_t loaded;
} p25_entry_settings_job_t;

static void entry_defaults(const app_t *app, uint32_t fallback_hz,
                           p25_entry_settings_t *out)
{
    p25_cqpsk_config_defaults(&out->cqpsk);
    out->gain_tenths = app ? app->default_gain : -1;
    out->freq_hz = app ? app->default_freq : fallback_hz;
    out->auto_follow = true;
    out->skip_encrypted = true;
    out->encrypted_skip_ms = P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS;
}

static esp_err_t read_entry_settings(void *ctx)
{
    p25_entry_settings_job_t *job = (p25_entry_settings_job_t *)ctx;
    p25_entry_settings_t loaded = job->loaded;

    settings_get_p25_cqpsk(&loaded.cqpsk);
    if (job->app) {
        loaded.gain_tenths = settings_get_gain(job->app);
        loaded.freq_hz = settings_get_freq(job->app);
    }
    loaded.auto_follow = settings_get_p25_auto_follow();
    loaded.skip_encrypted = settings_get_p25_skip_encrypted();
    loaded.encrypted_skip_ms = settings_get_p25_encrypted_skip_ms();

    job->loaded = loaded;
    return ESP_OK;
}

esp_err_t p25_entry_settings_load(const app_t *app, uint32_t fallback_hz,
                                  p25_entry_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    /* LS-720: app-switch queue/task allocation failure runs on_enter on its
     * caller, which can be LVGL's PSRAM stack.  P25 entry used to issue six
     * direct NVS reads there and could reproduce the measured cache-off stack
     * assertion.  Keep the lifecycle fallback, but move its complete settings
     * snapshot onto ls_nvs_call's statically reserved, DRAM-checked stack.
     * If that worker is unavailable, entry uses defaults without touching NVS. */
    entry_defaults(app, fallback_hz, out);
    p25_entry_settings_job_t job = { .app = app, .loaded = *out };
    esp_err_t err = ls_nvs_call(read_entry_settings, &job, 0);
    if (err == ESP_OK) *out = job.loaded;
    return err;
}
