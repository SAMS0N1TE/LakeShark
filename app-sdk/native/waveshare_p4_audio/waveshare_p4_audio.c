#include "waveshare_p4_audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include <stdbool.h>
#include <stdlib.h>

struct waveshare_p4_audio {
    i2c_master_bus_handle_t bus;
    i2s_chan_handle_t tx;
    bool owns_bus;
    bool tx_enabled;
    const audio_codec_ctrl_if_t *control;
    const audio_codec_gpio_if_t *gpio;
    const audio_codec_data_if_t *data;
    const audio_codec_if_t *codec;
    esp_codec_dev_handle_t device;
};

static esp_err_t init_bus(waveshare_p4_audio *audio)
{
    if (i2c_master_get_bus_handle(I2C_NUM_0, &audio->bus) == ESP_OK) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_7,
        .scl_io_num = GPIO_NUM_8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t result = i2c_new_master_bus(&config, &audio->bus);
    audio->owns_bus = result == ESP_OK;
    return result;
}

static esp_err_t init_i2s(waveshare_p4_audio *audio, uint32_t rate)
{
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    config.dma_desc_num = 4;
    config.dma_frame_num = 256;
    config.auto_clear = true;
    esp_err_t result = i2s_new_channel(&config, &audio->tx, NULL);
    if (result != ESP_OK) return result;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10,
            .dout = GPIO_NUM_9, .din = I2S_GPIO_UNUSED,
        },
    };
    result = i2s_channel_init_std_mode(audio->tx, &standard);
    if (result != ESP_OK) return result;
    result = i2s_channel_enable(audio->tx);
    audio->tx_enabled = result == ESP_OK;
    return result;
}

static esp_err_t init_codec(waveshare_p4_audio *audio, uint32_t rate, unsigned volume)
{
    audio_codec_i2c_cfg_t i2c_config = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = audio->bus,
    };
    audio_codec_i2s_cfg_t i2s_config = {.port = I2S_NUM_0, .tx_handle = audio->tx};
    audio->control = audio_codec_new_i2c_ctrl(&i2c_config);
    audio->gpio = audio_codec_new_gpio();
    audio->data = audio_codec_new_i2s_data(&i2s_config);
    if (!audio->control || !audio->gpio || !audio->data) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = audio->control,
        .gpio_if = audio->gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_53,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5, .codec_dac_voltage = 3.3f},
    };
    audio->codec = es8311_codec_new(&codec_config);
    if (!audio->codec) return ESP_FAIL;
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = audio->codec, .data_if = audio->data,
    };
    audio->device = esp_codec_dev_new(&device_config);
    if (!audio->device) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t format = {.sample_rate = rate, .bits_per_sample = 16, .channel = 2};
    if (esp_codec_dev_set_out_vol(audio->device, (int)volume) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(audio->device, &format) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t waveshare_p4_audio_open(waveshare_p4_audio **out, uint32_t rate, unsigned volume)
{
    if (!out || !rate || volume > 100) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    waveshare_p4_audio *audio = calloc(1, sizeof(*audio));
    if (!audio) return ESP_ERR_NO_MEM;
    esp_err_t result = init_bus(audio);
    if (result == ESP_OK) result = init_i2s(audio, rate);
    if (result == ESP_OK) result = init_codec(audio, rate, volume);
    if (result != ESP_OK) {
        waveshare_p4_audio_close(audio);
        return result;
    }
    *out = audio;
    return ESP_OK;
}

esp_err_t waveshare_p4_audio_set_volume(waveshare_p4_audio *audio, unsigned volume)
{
    if (!audio || !audio->device || volume > 100) return ESP_ERR_INVALID_ARG;
    return esp_codec_dev_set_out_vol(audio->device, (int)volume) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t waveshare_p4_audio_write(waveshare_p4_audio *audio, const int16_t *frames,
                                   size_t count, size_t *written, uint32_t timeout_ms)
{
    if (!audio || !frames || count > SIZE_MAX / (2 * sizeof(*frames))) return ESP_ERR_INVALID_ARG;
    const size_t frame_size = 2 * sizeof(*frames);
    size_t bytes = 0;
    esp_err_t result = i2s_channel_write(audio->tx, frames, count * frame_size, &bytes, timeout_ms);
    if (written) *written = bytes / frame_size;
    return result;
}

void waveshare_p4_audio_close(waveshare_p4_audio *audio)
{
    if (!audio) return;
    if (audio->device) {
        esp_codec_dev_close(audio->device);
        esp_codec_dev_delete(audio->device);
    }
    if (audio->codec) audio_codec_delete_codec_if(audio->codec);
    if (audio->gpio) audio_codec_delete_gpio_if(audio->gpio);
    if (audio->control) audio_codec_delete_ctrl_if(audio->control);
    if (audio->data) audio_codec_delete_data_if(audio->data);
    if (audio->tx_enabled) i2s_channel_disable(audio->tx);
    if (audio->tx) i2s_del_channel(audio->tx);
    if (audio->owns_bus) i2c_del_master_bus(audio->bus);
    free(audio);
}
