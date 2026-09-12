/* Codec dispatch belongs to the board layer. Both boot and the radio
   backend use this seam, so neither can reach the Waveshare codec on a board
   whose ES8311 lives on the secondary I2C bus. */
#include "ls_audio_hw.h"
#include "ls_board.h"
#include "bsp_board_extra.h"

#if defined(LS_BOARD_CODEC_I2C_BUS)
#include "ls_i2c.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"

/* Defined in ls_audio_diag.c, which owns the runtime override; declared here
   because this is where they are used. */
extern int ls_audio_ws_gpio;
extern int ls_audio_dout_gpio;

static i2s_chan_handle_t s_tx;
/* The receive half. NULL on a board with no microphone declared, and
   on this one it was NULL for a different reason: nobody had asked for it. */
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_dev;
static const audio_codec_data_if_t *s_data;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_codec;

static void codec_cleanup(void)
{
    if (s_dev) { esp_codec_dev_close(s_dev); esp_codec_dev_delete(s_dev); s_dev = NULL; }
    if (s_codec) { audio_codec_delete_codec_if(s_codec); s_codec = NULL; }
    if (s_gpio) { audio_codec_delete_gpio_if(s_gpio); s_gpio = NULL; }
    if (s_ctrl) { audio_codec_delete_ctrl_if(s_ctrl); s_ctrl = NULL; }
    if (s_data) { audio_codec_delete_data_if(s_data); s_data = NULL; }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
}

void ls_audio_hw_deinit(void) { codec_cleanup(); }

esp_err_t ls_audio_hw_init(bool speaker_only)
{
    (void)speaker_only;
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus;
    esp_err_t err = ls_i2c_bus(LS_BOARD_CODEC_I2C_BUS, &bus);
    if (err != ESP_OK) return err;
    err = ls_i2c_probe(LS_BOARD_CODEC_I2C_BUS, LS_BOARD_CODEC_I2C_ADDR, 100);
    if (err != ESP_OK) return err;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 256;

#if LS_HAS_MIC
    err = i2s_new_channel(&chan, &s_tx, speaker_only ? NULL : &s_rx);
#else
    err = i2s_new_channel(&chan, &s_tx, NULL);
#endif
    if (err != ESP_OK) return err;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        /* From the overridable pair, not the macros. */

        .gpio_cfg = {
            .mclk = LS_BOARD_I2S_MCLK_GPIO,
            .bclk = LS_BOARD_I2S_BCK_GPIO,
            .ws = ls_audio_ws_gpio,
            .dout = ls_audio_dout_gpio,
#if LS_HAS_MIC
            .din = LS_BOARD_I2S_DIN_GPIO,
#else
            .din = I2S_GPIO_UNUSED,
#endif
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) goto fail;
    if (s_rx) {
        /* The same standard config drives both directions: one word clock,
           one bit clock, two data lines. Configuring the receive channel
           differently is how a capture ends up half a sample out. */
        err = i2s_channel_init_std_mode(s_rx, &cfg);
        if (err != ESP_OK) goto fail;
        err = i2s_channel_enable(s_rx);
        if (err != ESP_OK) goto fail;
    }
    audio_codec_i2s_cfg_t data_cfg = {
        .port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = s_rx,
    };
    s_data = audio_codec_new_i2s_data(&data_cfg);
    /* esp_codec_dev takes the 8-bit wire address; ls_i2c takes 7 bits. */
    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = 1, .addr = LS_BOARD_CODEC_I2C_ADDR << 1, .bus_handle = bus,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    s_gpio = audio_codec_new_gpio();
    err = ESP_ERR_NO_MEM;
    if (!s_data || !s_ctrl || !s_gpio) goto fail;
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = s_ctrl, .gpio_if = s_gpio,
        /* BOTH when there is a microphone to listen to, so the
           ES8311's ADC is clocked and its input path configured. DAC-only
           leaves the ADC asleep and a read returns silence with every layer
           reporting success - which is the same shape of failure records for the word-clock and data pins. */
        .codec_mode = s_rx ? ESP_CODEC_DEV_WORK_MODE_BOTH
                           : ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, .use_mclk = true, .mclk_div = 256,
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    s_codec = es8311_codec_new(&codec_cfg);
    if (!s_codec) { err = ESP_FAIL; goto fail; }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = s_rx ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec, .data_if = s_data,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) goto fail;
    err = ls_audio_hw_set_fs(16000, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) goto fail;
    return ESP_OK;
fail:
    codec_cleanup();
    return err;
}

esp_err_t ls_audio_hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate, .bits_per_sample = bits, .channel = channels,
    };
    esp_codec_dev_close(s_dev);
    return esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t ls_audio_hw_write(void *data, size_t len, size_t *written, uint32_t timeout)
{
    (void)timeout;
    if (!written) return ESP_ERR_INVALID_ARG;
    *written = 0;
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (esp_codec_dev_write(s_dev, data, len) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    *written = len;
    return ESP_OK;
}

esp_err_t ls_audio_hw_read(void *data, size_t len, size_t *got)
{
    if (!got) return ESP_ERR_INVALID_ARG;
    *got = 0;
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!s_rx)  return ESP_ERR_NOT_SUPPORTED;
    if (esp_codec_dev_read(s_dev, data, len) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    *got = len;
    return ESP_OK;
}

/* Microphone gain in dB. The ES8311's PGA runs 0 to 42 dB in 6 dB steps; an
   electret into a handheld wants most of it. */
esp_err_t ls_audio_hw_in_gain(float db)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!s_rx)  return ESP_ERR_NOT_SUPPORTED;
    return esp_codec_dev_set_in_gain(s_dev, db) == ESP_CODEC_DEV_OK
           ? ESP_OK : ESP_FAIL;
}

bool ls_audio_hw_has_mic(void) { return s_rx != NULL; }

esp_err_t ls_audio_hw_volume(int volume, int *actual)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    if (esp_codec_dev_set_out_vol(s_dev, volume) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    /* esp_codec_dev_set_out_vol discards codec->set_vol's I2C error.
       Verify ES8311 DAC register 0x32 before claiming the requested volume.
       The default curve is -50..0 dB and this board's hw_gain is zero:
       register 191 is 0 dB, matching LilyGO's SetDacVolume(191). No boost.
       Without readback, a failed write can leave 15% audible while UI says 100. */
    int reg = -1;
    int expected = volume == 0 ? 0 : 91 + volume;
    if (!s_codec->get_reg ||
        s_codec->get_reg(s_codec, 0x32, &reg) != ESP_CODEC_DEV_OK || reg != expected)
        return ESP_FAIL;
    if (actual) *actual = volume;
    return ESP_OK;
}

esp_err_t ls_audio_hw_mute(bool mute)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_out_mute(s_dev, mute) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
#else
void ls_audio_hw_deinit(void) { /* the BSP owns its own lifetime */ }
esp_err_t ls_audio_hw_init(bool speaker_only)
{ return speaker_only ? bsp_extra_codec_init_speaker_only() : bsp_extra_codec_init(); }
esp_err_t ls_audio_hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{ return bsp_extra_codec_set_fs(rate, bits, channels); }
esp_err_t ls_audio_hw_write(void *data, size_t len, size_t *written, uint32_t timeout)
{ return bsp_extra_i2s_write(data, len, written, timeout); }
esp_err_t ls_audio_hw_volume(int volume, int *actual)
{ return bsp_extra_codec_volume_set(volume, actual); }
esp_err_t ls_audio_hw_mute(bool mute)
{ return bsp_extra_codec_mute_set(mute); }

/* Capture, which this half of the file never had. */

bool ls_audio_hw_has_mic(void) { return false; }

esp_err_t ls_audio_hw_read(void *data, size_t len, size_t *got)
{
    (void)data; (void)len;
    if (got) *got = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ls_audio_hw_in_gain(float db) { (void)db; return ESP_ERR_NOT_SUPPORTED; }
#endif
