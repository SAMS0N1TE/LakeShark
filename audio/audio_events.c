/* audio_events.c - audio event queue and dispatcher. */

#include "audio_events.h"
#include "audio_out.h"
#include "tone.h"
#include "sam_tts.h"
#include "plane_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "audio_evt";

typedef struct {
    audio_evt_kind_t kind;
    uint32_t         icao;
    char             callsign[9];
    plane_category_t cat;
    bool             crc_shaky;
} audio_msg_t;

static volatile audio_mode_t s_mode[AUDIO_EVT_KIND_COUNT] = {
    [AUDIO_EVT_NONE]         = AUD_MODE_OFF,
    [AUDIO_EVT_BOOT]         = AUD_MODE_BEEP,
    [AUDIO_EVT_NEW_CONTACT]  = AUD_MODE_VOICE,
    [AUDIO_EVT_LOST_CONTACT] = AUD_MODE_BEEP,
    [AUDIO_EVT_POSITION]     = AUD_MODE_BEEP,
};

static QueueHandle_t s_audio_q = NULL;

static void handle_msg(const audio_msg_t *m)
{
    audio_mode_t mode = s_mode[m->kind];
    if (mode == AUD_MODE_OFF) return;

    if (mode == AUD_MODE_BEEP) {
        switch (m->kind) {
            case AUDIO_EVT_BOOT:         snd_boot();         break;
            case AUDIO_EVT_NEW_CONTACT:  snd_new_contact();  break;
            case AUDIO_EVT_LOST_CONTACT: snd_lost_contact(); break;
            case AUDIO_EVT_POSITION:     snd_position_fix(); break;
            default: break;
        }
        return;
    }

    char phrase[160];
    switch (m->kind) {
        case AUDIO_EVT_BOOT:
            sam_tts_speak("RECEIVER READY.");
            break;
        case AUDIO_EVT_NEW_CONTACT:
            plane_phrase_new_contact(phrase, sizeof(phrase),
                                     m->icao, m->callsign, m->cat,
                                     m->crc_shaky);
            sam_tts_speak(phrase);
            break;
        case AUDIO_EVT_LOST_CONTACT:
            plane_phrase_lost_contact(phrase, sizeof(phrase),
                                      m->icao, m->callsign);
            sam_tts_speak(phrase);
            break;
        case AUDIO_EVT_POSITION:
            plane_phrase_position(phrase, sizeof(phrase),
                                  m->icao, m->callsign);
            sam_tts_speak(phrase);
            break;
        default: break;
    }
}

static void audio_task(void *arg)
{
    audio_msg_t msg;
    while (1) {
        if (xQueueReceive(s_audio_q, &msg, portMAX_DELAY) == pdTRUE) {
            handle_msg(&msg);
        }
    }
}

void audio_events_init(void)
{
    if (s_audio_q) return;
    s_audio_q = xQueueCreate(8, sizeof(audio_msg_t));
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 1);

    if (sam_tts_init(AUDIO_RATE_HZ, audio_write_mono) != ESP_OK) {
        ESP_LOGW(TAG, "SAM TTS init failed - voice events will be silent");
    }
}

void audio_events_play_boot(void)
{
    if (!s_audio_q) return;
    if (s_mode[AUDIO_EVT_BOOT] == AUD_MODE_OFF) return;
    audio_msg_t m = { .kind = AUDIO_EVT_BOOT };
    xQueueSend(s_audio_q, &m, 0);
}

void audio_events_publish(audio_evt_kind_t kind,
                          uint32_t icao,
                          const char *callsign,
                          bool crc_shaky)
{
    if (!s_audio_q) return;
    if (kind <= AUDIO_EVT_NONE || kind >= AUDIO_EVT_KIND_COUNT) return;
    if (s_mode[kind] == AUD_MODE_OFF) return;

    audio_msg_t m = { .kind = kind, .icao = icao, .crc_shaky = crc_shaky };
    if (callsign && *callsign) {
        strncpy(m.callsign, callsign, sizeof(m.callsign) - 1);
        m.callsign[sizeof(m.callsign) - 1] = 0;
    }
    m.cat = plane_classify(icao, m.callsign);
    xQueueSend(s_audio_q, &m, 0);
}

audio_mode_t audio_event_mode_get(audio_evt_kind_t kind)
{
    if (kind <= AUDIO_EVT_NONE || kind >= AUDIO_EVT_KIND_COUNT) return AUD_MODE_OFF;
    return s_mode[kind];
}
audio_mode_t audio_event_mode_cycle(audio_evt_kind_t kind)
{
    if (kind <= AUDIO_EVT_NONE || kind >= AUDIO_EVT_KIND_COUNT) return AUD_MODE_OFF;
    s_mode[kind] = (s_mode[kind] + 1) % AUD_MODE_COUNT;
    return s_mode[kind];
}
void audio_event_mode_set_all(audio_mode_t m)
{
    if (m >= AUD_MODE_COUNT) return;
    for (int i = 1; i < AUDIO_EVT_KIND_COUNT; i++) s_mode[i] = m;
}
const char *audio_mode_label(audio_mode_t m)
{
    switch (m) {
        case AUD_MODE_OFF:   return "OFF";
        case AUD_MODE_BEEP:  return "BEEP";
        case AUD_MODE_VOICE: return "VOICE";
        default:             return "?";
    }
}
