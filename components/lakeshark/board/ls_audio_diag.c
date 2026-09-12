/* The audio chain, end to end, and a way to try each link. */

#include "ls_audio_hw.h"
#include "ls_board.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LS_HAS_AUDIO

#include "ls_i2c.h"
#include "ls_xl9535.h"
#include "audio_out.h"
#include "tone.h"

int ls_audio_ws_gpio   = LS_BOARD_I2S_WS_GPIO;
int ls_audio_dout_gpio = LS_BOARD_I2S_DOUT_GPIO;

static const char *ok_str(esp_err_t e) { return e == ESP_OK ? "ok" : esp_err_to_name(e); }

void ls_audio_diag_report(void)
{
    printf("audio chain:\n");

    /* --- the control bus ------------------------------------------------ */
    /* Only the board that declares a codec bus gets probed on it. */

#if defined(LS_BOARD_CODEC_I2C_BUS) && defined(LS_BOARD_CODEC_I2C_ADDR)
    const esp_err_t probe = ls_i2c_probe(LS_BOARD_CODEC_I2C_BUS,
                                         LS_BOARD_CODEC_I2C_ADDR, 100);
    printf("  codec    ES8311 @ 0x%02x on bus %d: %s\n",
           LS_BOARD_CODEC_I2C_ADDR, (int)LS_BOARD_CODEC_I2C_BUS, ok_str(probe));
#else
    printf("  codec    this board declares no codec I2C bus or address\n");
#endif

    /* --- the data wires ------------------------------------------------- */
    printf("  i2s      mclk=%d bclk=%d ws=%d dout=%d%s\n",
           LS_BOARD_I2S_MCLK_GPIO, LS_BOARD_I2S_BCK_GPIO,
           ls_audio_ws_gpio, ls_audio_dout_gpio,
           (ls_audio_ws_gpio == LS_BOARD_I2S_WS_GPIO) ? "  (as the header says)"
                                                      : "  (SWAPPED at runtime)");

    /* --- the amplifier -------------------------------------------------- */
#ifdef LS_BOARD_XL_AUDIO_PWR_EN
    bool pa = false;
    const esp_err_t pae = ls_xl9535_get(LS_BOARD_XL_AUDIO_PWR_EN, &pa);
    printf("  amp      XL9535 IO6 (PWR_EN) reads %s%s\n",
           pae == ESP_OK ? (pa ? "HIGH" : "LOW") : "unreadable",
           pae == ESP_OK ? "" : " - expander not answering");
#else
    printf("  amp      no enable declared for this board\n");
#endif

    /* --- what the mixer thinks ------------------------------------------ */
    printf("  out      volume %d  mute %d\n",
           audio_volume_get(), audio_is_muted() ? 1 : 0);

    printf("  try:  audio swap   audio pa on|off   audio tone\n");
}

/* Tear the codec down and bring it back with whatever the pins are now. The
   audio task keeps running; it writes into a device that is briefly absent
   and gets an error back, which it already handles. */
esp_err_t ls_audio_diag_reinit(void)
{
    ls_audio_hw_deinit();
    const esp_err_t err = ls_audio_hw_init(false);
    if (err != ESP_OK) return err;
    int set = 0;
    (void)ls_audio_hw_volume(audio_volume_get(), &set);
    (void)ls_audio_hw_mute(false);
    return ESP_OK;
}

esp_err_t ls_audio_diag_swap_pins(void)
{
    const int t = ls_audio_ws_gpio;
    ls_audio_ws_gpio = ls_audio_dout_gpio;
    ls_audio_dout_gpio = t;
    printf("audio: ws=%d dout=%d, re-initialising\n",
           ls_audio_ws_gpio, ls_audio_dout_gpio);
    return ls_audio_diag_reinit();
}

esp_err_t ls_audio_diag_pa(bool on)
{
#ifdef LS_BOARD_XL_AUDIO_PWR_EN
    const esp_err_t e = ls_xl9535_out(LS_BOARD_XL_AUDIO_PWR_EN, on);
    printf("audio: amp enable -> %s (%s)\n", on ? "HIGH" : "LOW", ok_str(e));
    return e;
#else
    (void)on;
    printf("audio: this board declares no amplifier enable\n");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#endif /* LS_HAS_AUDIO */
