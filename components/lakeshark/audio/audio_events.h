
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
void audio_events_play_test(void);

void audio_events_publish(audio_evt_kind_t kind,
                          uint32_t icao,
                          const char *callsign,
                          bool crc_shaky);

audio_mode_t audio_event_mode_get(audio_evt_kind_t kind);
audio_mode_t audio_event_mode_cycle(audio_evt_kind_t kind);
void         audio_event_mode_set_all(audio_mode_t m);
const char  *audio_mode_label(audio_mode_t m);

/* Incoming MeshCore messages, spoken. Saved with the callout modes. */
typedef enum {
    AUD_MESH_OFF = 0,
    AUD_MESH_SENDER,     /* "MESSAGE FROM NAME." */
    AUD_MESH_FULL,       /* and the message itself */
    AUD_MESH_COUNT
} audio_mesh_say_t;

audio_mesh_say_t audio_events_mesh_say_get(void);
audio_mesh_say_t audio_events_mesh_say_cycle(void);
const char      *audio_mesh_say_label(audio_mesh_say_t m);
/* text is the message as it goes on air, "name: message". */
void             audio_events_mesh_message(const char *text, bool direct);

#ifdef __cplusplus
}
#endif

#endif
