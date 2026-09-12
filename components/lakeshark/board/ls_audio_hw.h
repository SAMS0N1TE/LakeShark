#ifndef LS_AUDIO_HW_H
#define LS_AUDIO_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t ls_audio_hw_init(bool speaker_only);

/* Release the codec and the I2S channel so init can run again.

   Exists for the audio diagnostic, which changes a pin assignment and needs
   the channel rebuilt to try it. The board-native path already had the
   teardown as a private helper for its own error path; this is that helper,
   named. On a board using the vendor BSP there is nothing to release and
   this is a no-op, because the BSP owns its own lifetime. */
void ls_audio_hw_deinit(void);
esp_err_t ls_audio_hw_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels);
esp_err_t ls_audio_hw_write(void *data, size_t len, size_t *written, uint32_t timeout);

/* The other direction, which this board has had the hardware for
   since the first assembly and never had the code for. See ls_audio_hw.c:
   the I2S channel was opened with a NULL receive handle, so the electret
   microphone wired to the codec's ADC had nowhere to send anything. */
esp_err_t ls_audio_hw_read(void *data, size_t len, size_t *got);
esp_err_t ls_audio_hw_in_gain(float db);
bool      ls_audio_hw_has_mic(void);
esp_err_t ls_audio_hw_volume(int volume, int *actual);
esp_err_t ls_audio_hw_mute(bool mute);

/* The audio diagnostic: what every link in the chain is set to, and
   a way to change the two that could be wrong without a rebuild. Reached
   from the console as `audio`. */
void      ls_audio_diag_report(void);
esp_err_t ls_audio_diag_reinit(void);
esp_err_t ls_audio_diag_swap_pins(void);
esp_err_t ls_audio_diag_pa(bool on);
#ifdef __cplusplus
}
#endif
#endif
