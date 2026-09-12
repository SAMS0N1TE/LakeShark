/* LS_TEST_SOURCES: ${FW}/components/lakeshark/apps/p25/p25_entry_settings.c
 *                  ${FW}/components/lakeshark/apps/p25/p25_cqpsk_controls.c
 *                  ${FW}/components/lakeshark/core/ls_flash_task.c
 *                  ${FW}/components/lakeshark/core/ls_nvs_safe.c */

#include "ls_test.h"

#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "ls_nvs_safe.h"
#include "p25_controls.h"
#include "p25_entry_settings.h"
#include "settings.h"

static unsigned s_settings_reads;

void settings_get_p25_cqpsk(p25_cqpsk_config_t *config)
{
    ++s_settings_reads;
    config->timing_gain = 0.0002f;
    config->carrier_gain = 0.02f;
}

int settings_get_gain(const app_t *app)
{
    ++s_settings_reads;
    LS_CHECK(app != NULL);
    return 321;
}

uint32_t settings_get_freq(const app_t *app)
{
    ++s_settings_reads;
    LS_CHECK(app != NULL);
    return 851012500u;
}

bool settings_get_p25_auto_follow(void)
{
    ++s_settings_reads;
    return false;
}

bool settings_get_p25_skip_encrypted(void)
{
    ++s_settings_reads;
    return false;
}

uint32_t settings_get_p25_encrypted_skip_ms(void)
{
    ++s_settings_reads;
    return 45678u;
}

static const app_t P25_TEST_APP = {
    .name = "P25",
    .default_freq = 154785000u,
    .default_gain = -1,
};

LS_CASE(entry_snapshot_reads_all_settings_on_cache_safe_static_worker)
{
    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    ls_shim_task_reset();
    ls_shim_task_execute_on_create(pdTRUE);
    s_settings_reads = 0;
    p25_entry_settings_t got;

    LS_EQ_INT(p25_entry_settings_load(&P25_TEST_APP, 111u, &got), ESP_OK);

    LS_EQ_UINT(s_settings_reads, 6);
    LS_EQ_STR(ls_shim_task_last_name(), "ls_nvs");
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), LS_NVS_WORKER_STACK_BYTES);
    LS_CHECK(ls_shim_task_last_static_stack() != NULL);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 1);
    LS_EQ_INT(got.gain_tenths, 321);
    LS_EQ_UINT(got.freq_hz, 851012500u);
    LS_CHECK(!got.auto_follow);
    LS_CHECK(!got.skip_encrypted);
    LS_EQ_UINT(got.encrypted_skip_ms, 45678u);
    LS_NEAR(got.cqpsk.timing_gain, 0.0002f, 0.0000001f);
    LS_NEAR(got.cqpsk.carrier_gain, 0.02f, 0.000001f);
}

LS_CASE(cache_safe_worker_allocation_failure_uses_defaults_without_nvs)
{
    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    ls_shim_task_reset();
    ls_shim_task_fail_create(pdTRUE);
    s_settings_reads = 0;
    p25_entry_settings_t got;

    LS_EQ_INT(p25_entry_settings_load(&P25_TEST_APP, 111u, &got),
              ESP_ERR_NO_MEM);

    LS_EQ_UINT(s_settings_reads, 0);
    LS_EQ_STR(ls_shim_task_last_name(), "ls_nvs");
    LS_EQ_UINT(ls_shim_task_static_create_count(), 1);
    LS_EQ_INT(got.gain_tenths, P25_TEST_APP.default_gain);
    LS_EQ_UINT(got.freq_hz, P25_TEST_APP.default_freq);
    LS_CHECK(got.auto_follow);
    LS_CHECK(got.skip_encrypted);
    LS_EQ_UINT(got.encrypted_skip_ms,
               P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS);
    LS_NEAR(got.cqpsk.timing_gain, P25_CQPSK_TIMING_GAIN_DEFAULT,
            0.000000001f);
    LS_NEAR(got.cqpsk.carrier_gain, P25_CQPSK_CARRIER_GAIN_DEFAULT,
            0.000001f);
}

LS_CASE(null_app_defaults_to_runtime_frequency_without_flash_access)
{
    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    ls_shim_task_reset();
    ls_shim_task_fail_create(pdTRUE);
    s_settings_reads = 0;
    p25_entry_settings_t got;

    LS_EQ_INT(p25_entry_settings_load(NULL, 162550000u, &got),
              ESP_ERR_NO_MEM);

    LS_EQ_UINT(s_settings_reads, 0);
    LS_EQ_INT(got.gain_tenths, -1);
    LS_EQ_UINT(got.freq_hz, 162550000u);
}
