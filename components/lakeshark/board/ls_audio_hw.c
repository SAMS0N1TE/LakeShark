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
#include "esp_attr.h"
#include "esp_timer.h"

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
/* Times the speaker's DMA ran out of samples: the player fell more than the
   DMA's 96 ms behind and the codec got the zeroes auto_clear left. */
static volatile uint32_t s_tx_starved;
static volatile int64_t s_tx_starved_first_us, s_tx_starved_last_us;
static IRAM_ATTR bool on_tx_starved(i2s_chan_handle_t h, i2s_event_data_t *ev, void *ctx)
{
    (void)h; (void)ev; (void)ctx;
    const int64_t now = esp_timer_get_time();
    if (!s_tx_starved) s_tx_starved_first_us = now;
    s_tx_starved_last_us = now;
    s_tx_starved++;
    return false;
}
uint32_t ls_audio_hw_starved(int64_t *first_us, int64_t *last_us)
{
    if (first_us) *first_us = s_tx_starved_first_us;
    if (last_us) *last_us = s_tx_starved_last_us;
    return s_tx_starved;
}
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
/* Parked: both channels disabled while nothing plays, so the word and bit
   clocks stop. Left running on silence they put a 16 kHz comb across the
   LR2021's receive band (151.553, 151.569, 151.585 MHz ...). A write, a
   read, a rate change or a teardown starts them again first. */
static bool s_parked;
static volatile int64_t s_last_read_us;
#define PARK_AFTER_READ_US 1000000
#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/i2s_ll.h"
#include "esp_private/periph_ctrl.h"
/* Disabled channels still leave MCLK running (in full duplex it follows
   whichever channel was set up last), and the ES8311 keeps counting off it:
   a 64 kHz comb, MCLK/64, at 151.553, 151.617, 152.577 MHz ... Gating the
   I2S0 module clocks stops MCLK as well. */
static void module_clocks(bool on)
{
    PERIPH_RCC_ATOMIC() {
        if (on) {
            i2s_ll_tx_enable_clock(&I2S0);
            if (s_rx) { i2s_ll_rx_enable_clock(&I2S0); }
        } else {
            i2s_ll_tx_disable_clock(&I2S0);
            if (s_rx) { i2s_ll_rx_disable_clock(&I2S0); }
        }
    }
}
#else
static void module_clocks(bool on) { (void)on; }
#endif
/* XL9535 IO6 switches the OUT_5V audio rail: the NS4150 amplifier and the
   RT9080 that makes the ES8311's analog 3.3 V (LilyGO lilygo_device_driver,
   t_display_p4 SetEs8311OperatingMode).
   The ES8311's output steps when its clocks start, muted or not, and with
   the rail up that is a snap from the speaker: at boot, and on an unpark
   once the clocks have been stopped for about a second. So the rail is off
   when the clocks start after a long park, and comes on once the codec has
   run on silence for AMP_SETTLE_MS.
   The rail goes off AMP_OFF_AFTER_MS after the clocks stop. With the codec
   still clocked, or right as the clocks stop, that snapped on every park;
   0.5 s later it was still above the room (21-41 against 12-31), 1, 2 and
   4 s later at it (17-31). At 600 ms it still snapped a couple of seconds
   after each P25 call, by ear; 2 s is inside the quiet range. Measured with
   the SX board's microphone beside the speaker (bench/lr151/RESULTS.md).
   An unpark inside AMP_KEEP_MS of the park keeps the rail up (0.3-0.5 s:
   quiet). Later, with the rail still up, the clocks starting under it
   snapped (1.5 s: 44-74), so the rail goes off first, with the codec long
   settled, and comes back AMP_SETTLE_MS after the clocks (10-21). */
static bool s_amp_on = true;
#ifdef LS_BOARD_XL_AUDIO_PWR_EN
#include "ls_xl9535.h"
static void amp(bool on) { (void)ls_xl9535_out(LS_BOARD_XL_AUDIO_PWR_EN, on); s_amp_on = on; }
#else
static void amp(bool on) { (void)on; }
#endif
#define AMP_SETTLE_MS    150
#define AMP_OFF_AFTER_MS 2000
#define AMP_KEEP_MS      1000
#define DAC_MUTE_MS      20
static int64_t s_parked_at_us;
/* The mute last asked for through ls_audio_hw_mute, restored after an unpark;
   the DAC is held muted across every clock and amplifier change. */
static bool s_out_mute;
static void dac_mute(bool mute) { if (s_dev) esp_codec_dev_set_out_mute(s_dev, mute); }
static void unpark(void)
{
    if (!s_parked) return;
    if (s_amp_on && esp_timer_get_time() - s_parked_at_us >= AMP_KEEP_MS * 1000LL) amp(false);
    module_clocks(true);
    channels_enable();
    vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
    if (!s_amp_on) {
        amp(true);
        vTaskDelay(pdMS_TO_TICKS(DAC_MUTE_MS));
    }
    dac_mute(s_out_mute);
    __atomic_store_n(&s_parked, false, __ATOMIC_RELEASE);
}
static const audio_codec_data_if_t *s_data;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_codec;
bool ls_audio_hw_output_is_mono(void) { return true; } /* ES8311 */

static void codec_cleanup(void)
{
    unpark();
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
    amp(false);
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
    const i2s_event_callbacks_t tx_cbs = { .on_send_q_ovf = on_tx_starved };
    (void)i2s_channel_register_event_callback(s_tx, &tx_cbs, NULL);
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
    vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
    amp(true);
    return ESP_OK;
fail:
    codec_cleanup();
    amp(true);
    return err;
}

static esp_err_t hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    unpark();
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
    s_out_mute = mute;
    if (s_parked) return ESP_OK;   /* applied by the unpark */
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
esp_err_t ls_audio_hw_park(bool park)
{
    if (!hw_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (!park) {
        unpark();
    } else if (!s_dev) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_parked) {
        /* Called again while parked: the rail goes off once the codec has
           settled. */
        if (s_amp_on &&
            esp_timer_get_time() - s_parked_at_us >= AMP_OFF_AFTER_MS * 1000LL)
            amp(false);
    } else {
        /* A microphone reader works in windows with gaps between them;
           parking in a gap would cost it the next window's start. */
        if (s_rx && esp_timer_get_time() - s_last_read_us < PARK_AFTER_READ_US) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            dac_mute(true);
            vTaskDelay(pdMS_TO_TICKS(DAC_MUTE_MS));
            if (s_tx) i2s_channel_disable(s_tx);
            if (s_rx) i2s_channel_disable(s_rx);
            module_clocks(false);
            s_parked_at_us = esp_timer_get_time();
            __atomic_store_n(&s_parked, true, __ATOMIC_RELEASE);
        }
    }
    hw_unlock();
    return err;
}
bool ls_audio_hw_parked(void) { return __atomic_load_n(&s_parked, __ATOMIC_ACQUIRE); }

/* Parking takes every lock, so it cannot land inside a transfer; a transfer
   that finds the channels parked drops its lock and unparks first. */
esp_err_t ls_audio_hw_write(void *data, size_t len, size_t *written, uint32_t timeout)
{
    if (!data_lock(&s_tx_lock)) return ESP_ERR_TIMEOUT;
    if (ls_audio_hw_parked()) {
        data_unlock(s_tx_lock);
        esp_err_t err = ls_audio_hw_park(false);
        if (err != ESP_OK) return err;
        if (!data_lock(&s_tx_lock)) return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = hw_write(data, len, written, timeout);
    data_unlock(s_tx_lock);
    return err;
}
esp_err_t ls_audio_hw_read(void *data, size_t len, size_t *got)
{
    if (!data_lock(&s_rx_lock)) return ESP_ERR_TIMEOUT;
    if (ls_audio_hw_parked()) {
        data_unlock(s_rx_lock);
        esp_err_t err = ls_audio_hw_park(false);
        if (err != ESP_OK) return err;
        if (!data_lock(&s_rx_lock)) return ESP_ERR_TIMEOUT;
    }
    s_last_read_us = esp_timer_get_time();
    esp_err_t err = hw_read(data, len, got);
    s_last_read_us = esp_timer_get_time();
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
{
    /* The full init opens the BSP's ES7210 microphone codec and asserts when
       it is not there, as on the Nano: boot never got past it. */
    return speaker_only || !LS_HAS_MIC ? bsp_extra_codec_init_speaker_only()
                                       : bsp_extra_codec_init();
}
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
esp_err_t ls_audio_hw_park(bool park) { (void)park; return ESP_ERR_NOT_SUPPORTED; }
bool ls_audio_hw_parked(void) { return false; }
uint32_t ls_audio_hw_starved(int64_t *first_us, int64_t *last_us)
{
    if (first_us) *first_us = 0;
    if (last_us) *last_us = 0;
    return 0;
}
#endif
