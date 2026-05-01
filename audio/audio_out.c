/* audio_out.c - I2S + ES8311 codec, shared mono sink. */

#include "audio_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_log.h"

#define I2C_SCL_PIN     8
#define I2C_SDA_PIN     7
#define I2S_MCLK_PIN    13
#define I2S_BCK_PIN     12
#define I2S_WS_PIN      10
#define I2S_DOUT_PIN    9
#define I2S_DIN_PIN     11
#define PA_EN_PIN       53
#define MCLK_MULTIPLE   384

static const char *TAG = "audio_out";

static i2s_chan_handle_t s_i2s_tx = NULL;
static volatile int      s_volume = 70;
static volatile bool     s_muted  = false;
static SemaphoreHandle_t s_tx_mtx = NULL;

void audio_write_mono(const int16_t *samples, int n)
{
    if (!s_i2s_tx || s_muted) return;
    if (s_tx_mtx) xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    static int16_t stereo[512];
    int written = 0;
    while (written < n) {
        int chunk = n - written;
        if (chunk > 256) chunk = 256;
        float vol = s_volume / 100.0f;
        for (int i = 0; i < chunk; i++) {
            int16_t s = (int16_t)(samples[written + i] * vol);
            stereo[i*2]   = s;
            stereo[i*2+1] = s;
        }
        size_t bytes_out = 0;
        i2s_channel_write(s_i2s_tx, stereo, chunk * 4,
                          &bytes_out, portMAX_DELAY);
        int samples_written = (int)(bytes_out / 4);
        if (samples_written <= 0) break;
        written += samples_written;
    }
    if (s_tx_mtx) xSemaphoreGive(s_tx_mtx);
}

void audio_write_p25_voice(const int16_t *src8k, int n)
{
    if (n <= 0) return;
    static int16_t up16k[1024];
    static int16_t prev = 0;
    int i = 0;
    while (i < n) {
        int chunk_in = n - i;
        if (chunk_in > 512) chunk_in = 512;
        for (int k = 0; k < chunk_in; k++) {
            int16_t s = src8k[i + k];
            up16k[k * 2]     = (int16_t)(((int)prev + (int)s) >> 1);
            up16k[k * 2 + 1] = s;
            prev = s;
        }
        audio_write_mono(up16k, chunk_in * 2);
        i += chunk_in;
    }
}

esp_err_t audio_out_init(void)
{
    if (!s_tx_mtx) s_tx_mtx = xSemaphoreCreateMutex();

    const i2c_config_t i2c_cfg = {
        .sda_io_num    = I2C_SDA_PIN,
        .scl_io_num    = I2C_SCL_PIN,
        .mode          = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_NUM_0, &i2c_cfg);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    es8311_handle_t es = es8311_create(I2C_NUM_0, 0x18);
    if (!es) {
        ESP_LOGW(TAG, "ES8311 not found - audio disabled");
        return ESP_FAIL;
    }
    const es8311_clock_config_t clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = AUDIO_RATE_HZ * MCLK_MULTIPLE,
        .sample_frequency   = AUDIO_RATE_HZ,
    };
    es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    es8311_sample_frequency_config(es, AUDIO_RATE_HZ * MCLK_MULTIPLE,
                                   AUDIO_RATE_HZ);
    es8311_voice_volume_set(es, s_volume, NULL);
    es8311_microphone_config(es, false);

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_chan_handle_t rx_tmp;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, &rx_tmp));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN, .bclk = I2S_BCK_PIN,
            .ws   = I2S_WS_PIN,   .dout = I2S_DOUT_PIN,
            .din  = I2S_DIN_PIN,
            .invert_flags = {0},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));

    gpio_config_t pa = { .pin_bit_mask = (1ULL << PA_EN_PIN),
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pa);
    gpio_set_level(PA_EN_PIN, 1);

    ESP_LOGI(TAG, "audio_out ready  vol=%d", s_volume);
    return ESP_OK;
}

void audio_toggle_mute(void)  { s_muted = !s_muted; }
bool audio_is_muted(void)     { return s_muted; }
int  audio_volume_get(void)   { return s_volume; }
void audio_volume_delta(int d)
{
    int v = s_volume + d;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    s_volume = v;
}
