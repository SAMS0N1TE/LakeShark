/* Host shim.  The real audio_player.h drags in FreeRTOS and driver/i2s_std.h;
   for the bench we only need the state enum and the getter, plus a test-side
   setter so a case can drive the state machine without a codec. */
#ifndef LS_SHIM_AUDIO_PLAYER_H
#define LS_SHIM_AUDIO_PLAYER_H

#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_PLAYER_STATE_IDLE,
    AUDIO_PLAYER_STATE_PLAYING,
    AUDIO_PLAYER_STATE_PAUSE,
    AUDIO_PLAYER_STATE_SHUTDOWN
} audio_player_state_t;

audio_player_state_t audio_player_get_state(void);

/* Test-only. Sets the value that audio_player_get_state() returns. */
void ls_shim_audio_state_set(audio_player_state_t state);

/**/
/* Only declared here so bsp_extra_player_open.c compiles against the shim.
   Cases that drive it define __wrap_ / stub implementations to control the
   return value and observe how the caller handles the FILE* on failure. */
esp_err_t audio_player_play(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif
