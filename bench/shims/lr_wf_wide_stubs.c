/* Driver calls only lr_wf_session's wide mode makes. Tests that leave wide
   mode off link these; test_lr_wf_wide fakes them for real. */
#include "ls_lora_lr20xx.h"
#include <string.h>

esp_err_t lr20xx_stream_live(const lr20xx_iq_config_t *config, lr20xx_iq_block_sink_t sink,
                             void *context, lr20xx_stream_result_t *result)
{
    (void)config; (void)sink; (void)context;
    if (result) { memset(result, 0, sizeof(*result)); result->capture_error = ESP_ERR_NOT_SUPPORTED; }
    return ESP_ERR_NOT_SUPPORTED;
}
lr20xx_rx_path_t lr20xx_path_for_hz(uint32_t hz) { (void)hz; return LR20XX_PATH_LF; }
