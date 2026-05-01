/*
 * audio_events.h - event-driven audio dispatcher.
 *
 * Apps push semantic events (boot, new contact, lost, position) and the
 * dispatcher decides per-event whether to play a beep, speak via SAM TTS,
 * or stay silent. The mode for each event is independently runtime-cyclable
 * (V/C/L/P/B keys in the TUI) and persisted through the audio_event_mode
 * accessors.
 */
#ifndef AUDIO_EVENTS_H
#define AUDIO_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_EVT_NONE = 0,
    AUDIO_EVT_BOOT,
    AUDIO_EVT_NEW_CONTACT,
    AUDIO_EVT_LOST_CONTACT,
    AUDIO_EVT_POSITION,
    AUDIO_EVT_KIND_COUNT
} audio_evt_kind_t;

typedef enum {
    AUD_MODE_OFF = 0,
    AUD_MODE_BEEP,
    AUD_MODE_VOICE,
    AUD_MODE_COUNT
} audio_mode_t;

void audio_events_init(void);
void audio_events_play_boot(void);

void audio_events_publish(audio_evt_kind_t kind,
                          uint32_t icao,
                          const char *callsign,
                          bool crc_shaky);

audio_mode_t audio_event_mode_get(audio_evt_kind_t kind);
audio_mode_t audio_event_mode_cycle(audio_evt_kind_t kind);
void         audio_event_mode_set_all(audio_mode_t m);
const char  *audio_mode_label(audio_mode_t m);

#ifdef __cplusplus
}
#endif

#endif
