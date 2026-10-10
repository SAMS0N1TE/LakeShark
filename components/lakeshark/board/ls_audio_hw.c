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

/* Three locks. Control (I2C: volume, mute, gain, register reads) takes only
 * the control lock; a write takes only the TX lock and a read only the RX
 * lock. Lifecycle (init, set_fs, reinit, deinit) takes all three, in that
 * order, so no handle is freed under a transfer. One lock for everything
 * starved volume and mute: the player sits in the blocking DMA write at
 * priority 11 and retakes the lock before a waiter runs, so every codec
 * volume write timed out after 500 ms with the UI blocked behind it.
 * Bounded waits: reinit leaves the existing hardware intact if a
 * reader/writer cannot quiesce. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static StaticSemaphore_t s_ctrl_lock_memory, s_tx_lock_memory, s_rx_lock_memory;
static SemaphoreHandle_t s_ctrl_lock, s_tx_lock, s_rx_lock;
static unsigned s_hw_once;
/* Lifecycle callers waiting on a data lock; a data transfer yields once
   after releasing its lock while this is non-zero. */
static unsigned s_data_waiters;
#define HW_LOCK_WAIT pdMS_TO_TICKS(500)

static void hw_locks_once(void)
{
    unsigned expected = 0;
    if (__atomic_compare_exchange_n(&s_hw_once, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_ctrl_lock = xSemaphoreCreateRecursiveMutexStatic(&s_ctrl_lock_memory);
        s_tx_lock = xSemaphoreCreateRecursiveMutexStatic(&s_tx_lock_memory);
        s_rx_lock = xSemaphoreCreateRecursiveMutexStatic(&s_rx_lock_memory);
        __atomic_store_n(&s_hw_once, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&s_hw_once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    }
}
static bool take(SemaphoreHandle_t m)
{
    return m && xSemaphoreTakeRecursive(m, HW_LOCK_WAIT) == pdTRUE;
}
static bool ctrl_lock(void) { hw_locks_once(); return take(s_ctrl_lock); }
static void ctrl_unlock(void) { xSemaphoreGiveRecursive(s_ctrl_lock); }

static bool data_lock(SemaphoreHandle_t *m)
{
    hw_locks_once();
    return take(*m);
}
static void data_unlock(SemaphoreHandle_t m)
{
    xSemaphoreGiveRecursive(m);
    if (__atomic_load_n(&s_data_waiters, __ATOMIC_ACQUIRE)) vTaskDelay(1);
}

static bool hw_lock(void)
{
    if (!ctrl_lock()) return false;
    __atomic_add_fetch(&s_data_waiters, 1, __ATOMIC_ACQ_REL);
    bool tx = take(s_tx_lock);
    bool rx = tx && take(s_rx_lock);
    __atomic_sub_fetch(&s_data_waiters, 1, __ATOMIC_ACQ_REL);
    if (rx) return true;
    if (tx) xSemaphoreGiveRecursive(s_tx_lock);
    ctrl_unlock();
    return false;
}
static void hw_unlock(void)
{
    xSemaphoreGiveRecursive(s_rx_lock);
    xSemaphoreGiveRecursive(s_tx_lock);
    ctrl_unlock();
}
static i2s_chan_handle_t s_tx;
/* The receive half. NULL on a board with no microphone declared, and
   on this one it was NULL for a different reason: nobody had asked for it. */
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_dev;
/* Whether s_dev is open. Closing it disables both I2S channels, and opening
   it disables them again before setting the format, so a channel has to be
   enabled between the two or the driver logs an error for each. While the
   device is closed the channels are always enabled. */
static bool s_dev_open;

static void channels_enable(void)
{
    if (s_tx) i2s_channel_enable(s_tx);
    if (s_rx) i2s_channel_enable(s_rx);
}
static const audio_codec_data_if_t *s_data;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_codec;
bool ls_audio_hw_output_is_mono(void) { return true; } /* ES8311 */

static void codec_cleanup(void)
{
    /* A close of an open device has already disabled the channels. */
    const bool disabled = s_dev && s_dev_open;
    if (s_dev) { esp_codec_dev_close(s_dev); esp_codec_dev_delete(s_dev); s_dev = NULL; }
    s_dev_open = false;
    if (s_codec) { audio_codec_delete_codec_if(s_codec); s_codec = NULL; }
    if (s_gpio) { audio_codec_delete_gpio_if(s_gpio); s_gpio = NULL; }
    if (s_ctrl) { audio_codec_delete_ctrl_if(s_ctrl); s_ctrl = NULL; }
    if (s_data) { audio_codec_delete_data_if(s_data); s_data = NULL; }
    if (s_rx) {
        if (!disabled) i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx) {
        if (!disabled) i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
}

void ls_audio_hw_deinit(void) { if (hw_lock()) { codec_cleanup(); hw_unlock(); } }

static esp_err_t hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels);

static esp_err_t hw_init(bool speaker_only)
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
    /* A buffer the DMA has sent is zeroed, so when nothing new is written -
       music paused, a player between tracks - the codec gets silence instead
       of the last 35 ms of audio going round and round. */
    chan.auto_clear_after_cb = true;

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
    err = hw_set_fs(16000, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) goto fail;
    return ESP_OK;
fail:
    codec_cleanup();
    return err;
}

static esp_err_t hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate, .bits_per_sample = bits, .channel = channels,
    };
    if (s_dev_open) {
        esp_codec_dev_close(s_dev);
        s_dev_open = false;
        channels_enable();
    }
    s_dev_open = esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK;
    if (!s_dev_open) {
        /* A failed open can stop with the device marked open and the
           channels in either state; close it and enable them, so the next
           attempt starts from the same place as the first. */
        esp_codec_dev_close(s_dev);
        channels_enable();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t hw_write(void *data, size_t len, size_t *written, uint32_t timeout)
{
    (void)timeout;
    if (!written) return ESP_ERR_INVALID_ARG;
    *written = 0;
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (esp_codec_dev_write(s_dev, data, len) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    *written = len;
    return ESP_OK;
}

static esp_err_t hw_read(void *data, size_t len, size_t *got)
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
static esp_err_t hw_in_gain(float db)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!s_rx)  return ESP_ERR_NOT_SUPPORTED;
    return esp_codec_dev_set_in_gain(s_dev, db) == ESP_CODEC_DEV_OK
           ? ESP_OK : ESP_FAIL;
}

bool ls_audio_hw_has_mic(void) { if (!ctrl_lock()) return false; bool mic = s_rx != NULL; ctrl_unlock(); return mic; }

static esp_err_t hw_volume(int volume, int *actual)
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

static esp_err_t hw_mute(bool mute)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_out_mute(s_dev, mute) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t hw_reg_read(uint8_t reg, int *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    if (!s_codec || !s_codec->get_reg) return ESP_ERR_INVALID_STATE;
    return s_codec->get_reg(s_codec, reg, value) == ESP_CODEC_DEV_OK
        ? ESP_OK : ESP_FAIL;
}
esp_err_t ls_audio_hw_init(bool speaker_only)
{
    if (!hw_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_init(speaker_only);
    hw_unlock();
    return err;
}
esp_err_t ls_audio_hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{
    if (!hw_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_set_fs(rate, bits, channels);
    hw_unlock();
    return err;
}
esp_err_t ls_audio_hw_write(void *data, size_t len, size_t *written, uint32_t timeout)
{
    if (!data_lock(&s_tx_lock)) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_write(data, len, written, timeout);
    data_unlock(s_tx_lock);
    return err;
}
esp_err_t ls_audio_hw_read(void *data, size_t len, size_t *got)
{
    if (!data_lock(&s_rx_lock)) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_read(data, len, got);
    data_unlock(s_rx_lock);
    return err;
}
esp_err_t ls_audio_hw_in_gain(float db)
{
    if (!ctrl_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_in_gain(db);
    ctrl_unlock();
    return err;
}
esp_err_t ls_audio_hw_volume(int volume, int *actual)
{
    if (!ctrl_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_volume(volume, actual);
    ctrl_unlock();
    return err;
}
esp_err_t ls_audio_hw_mute(bool mute)
{
    if (!ctrl_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_mute(mute);
    ctrl_unlock();
    return err;
}
esp_err_t ls_audio_hw_reg_read(uint8_t reg, int *value)
{
    if (!ctrl_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = hw_reg_read(reg, value);
    ctrl_unlock();
    return err;
}
esp_err_t ls_audio_hw_reinit(bool swap, int volume, bool muted)
{
    if (!hw_lock()) return ESP_ERR_TIMEOUT;
    /* No other hardware user can enter until restoration is complete. */
    codec_cleanup();
    if (swap) { int pin = ls_audio_ws_gpio; ls_audio_ws_gpio = ls_audio_dout_gpio; ls_audio_dout_gpio = pin; }
    esp_err_t err = hw_init(false);
    int actual;
    if (err == ESP_OK) err = hw_volume(volume, &actual);
    if (err == ESP_OK) err = hw_mute(muted);
    if (err != ESP_OK) codec_cleanup();
    hw_unlock();
    return err;
}

#else
esp_err_t ls_audio_hw_reinit(bool swap, int volume, bool muted) { (void)swap; (void)volume; (void)muted; return ESP_ERR_NOT_SUPPORTED; }
bool ls_audio_hw_output_is_mono(void) { return false; }
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
esp_err_t ls_audio_hw_reg_read(uint8_t reg, int *value)
{ (void)reg; (void)value; return ESP_ERR_NOT_SUPPORTED; }

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
