/* vhf_demod.c - stub: counts bytes received, no actual demod yet. */

#include "vhf_demod.h"
#include "perf.h"
#include "esp_log.h"

static const char *TAG = "vhf";
static uint64_t s_bytes = 0;

void vhf_demod_init(void)
{
    s_bytes = 0;
    ESP_LOGI(TAG, "VHF stub initialised - no demod implemented yet");
}

void vhf_on_sample(uint8_t *iq, int len)
{
    (void)iq;
    if (len <= 0) return;
    s_bytes += (uint64_t)len;
    perf_count_bytes((uint32_t)len);
}
