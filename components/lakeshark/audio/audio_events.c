
#include "audio_events.h"
#include "audio_out.h"
#include "tone.h"
#include "speech.h"
#include "plane_audio.h"
#include "mesh_phrase.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio_evt";

typedef struct {
    audio_evt_kind_t kind;
    uint32_t         icao;
    char             callsign[9];
    plane_category_t cat;
    bool             crc_shaky;
    TickType_t       queued;
} audio_msg_t;

/* A contact callout that waited this long behind others is old news. */
#define AUDIO_STALE_TICKS pdMS_TO_TICKS(20000)

static volatile audio_mode_t s_mode[AUDIO_EVT_KIND_COUNT] = {
    [AUDIO_EVT_NONE]         = AUD_MODE_OFF,
    [AUDIO_EVT_BOOT]         = AUD_MODE_BEEP,
    [AUDIO_EVT_NEW_CONTACT]  = AUD_MODE_VOICE,
    [AUDIO_EVT_LOST_CONTACT] = AUD_MODE_BEEP,
    [AUDIO_EVT_POSITION]     = AUD_MODE_BEEP,
};

static QueueHandle_t s_audio_q = NULL;

/* A mesh message's phrase does not fit a queue entry; the latest one waits
   here and the queue carries only the fact that it arrived. */
#define AUDIO_EVT_MESH ((audio_evt_kind_t)AUDIO_EVT_KIND_COUNT)
static volatile audio_mesh_say_t s_mesh_say = AUD_MESH_SENDER;
static char        s_mesh_phrase[SPEECH_TEXT_MAX];
static portMUX_TYPE s_mesh_lock = portMUX_INITIALIZER_UNLOCKED;

/* Packed for settings: two bits per callout kind, the mesh choice at 10,
   bit 31 marks a saved value. */
static void callouts_save(void)
{
    uint32_t v = 1u << 31;
    for (int k = AUDIO_EVT_BOOT; k < AUDIO_EVT_KIND_COUNT; k++)
        v |= ((uint32_t)s_mode[k] & 3u) << (2 * k);
    v |= ((uint32_t)s_mesh_say & 3u) << 10;
    settings_set_callouts(v);
}

static void callouts_load(void)
{
    uint32_t v = 0;
    if (!settings_get_callouts(&v) || !(v & (1u << 31))) return;
    for (int k = AUDIO_EVT_BOOT; k < AUDIO_EVT_KIND_COUNT; k++) {
        const uint32_t m = (v >> (2 * k)) & 3u;
        if (m < AUD_MODE_COUNT) s_mode[k] = (audio_mode_t)m;
    }
    const uint32_t mesh = (v >> 10) & 3u;
    if (mesh < AUD_MESH_COUNT) s_mesh_say = (audio_mesh_say_t)mesh;
}

/* The notice chime for the same message is usually still ringing. */
static bool say_mesh(void)
{
    for (int i = 0; i < 150 && snd_test_busy(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    char phrase[SPEECH_TEXT_MAX];
    taskENTER_CRITICAL(&s_mesh_lock);
    memcpy(phrase, s_mesh_phrase, sizeof(phrase));
    s_mesh_phrase[0] = 0;
    taskEXIT_CRITICAL(&s_mesh_lock);
    if (!phrase[0]) return true;
    audio_out_ensure_unmuted();
    return speech_say(phrase) != SPEECH_STOPPED;
}

static void play_tone(audio_evt_kind_t kind)
{
    switch (kind) {
        case AUDIO_EVT_BOOT:         snd_boot();         break;
        case AUDIO_EVT_NEW_CONTACT:  snd_new_contact();  break;
        case AUDIO_EVT_LOST_CONTACT: snd_lost_contact(); break;
        case AUDIO_EVT_POSITION:     snd_position_fix(); break;
        default: break;
    }
}

/* False when live radio or a cancel stopped it. A phrase the engine could
   not say still gets its tone. */
static bool announce(const char *text, audio_evt_kind_t kind)
{
    const speech_result_t r = speech_say(text);
    if (r == SPEECH_FAILED || r == SPEECH_UNAVAILABLE || r == SPEECH_BUSY)
        play_tone(kind);
    return r != SPEECH_STOPPED;
}

static bool handle_msg(const audio_msg_t *m)
{
    if (m->kind == AUDIO_EVT_NONE) {
        audio_out_ensure_unmuted();
        return announce("TEST. THIS IS THE A D S B VOICE CHECK.", AUDIO_EVT_NONE);
    }

    if (m->kind != AUDIO_EVT_BOOT &&
        (TickType_t)(xTaskGetTickCount() - m->queued) > AUDIO_STALE_TICKS)
        return true;
    if (m->kind == AUDIO_EVT_MESH) return say_mesh();

    audio_mode_t mode = s_mode[m->kind];
    if (mode == AUD_MODE_OFF) return true;
    if (mode == AUD_MODE_VOICE && !speech_available()) mode = AUD_MODE_BEEP;

    audio_out_ensure_unmuted();

    if (mode == AUD_MODE_BEEP) {
        play_tone(m->kind);
        return true;
    }

    char phrase[160];
    switch (m->kind) {
        case AUDIO_EVT_BOOT:
            return announce("RECEIVER READY.", m->kind);
        case AUDIO_EVT_NEW_CONTACT:
            plane_phrase_new_contact(phrase, sizeof(phrase),
                                     m->icao, m->callsign, m->cat,
                                     m->crc_shaky);
            return announce(phrase, m->kind);
        case AUDIO_EVT_LOST_CONTACT:
            plane_phrase_lost_contact(phrase, sizeof(phrase),
                                      m->icao, m->callsign);
            return announce(phrase, m->kind);
        case AUDIO_EVT_POSITION:
            plane_phrase_position(phrase, sizeof(phrase),
                                  m->icao, m->callsign);
            return announce(phrase, m->kind);
        default:
            return true;
    }
}

static void audio_task(void *arg)
{
    audio_msg_t msg;
    while (1) {
        if (xQueueReceive(s_audio_q, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.kind == AUDIO_EVT_NEW_CONTACT &&
            s_mode[AUDIO_EVT_NEW_CONTACT] == AUD_MODE_VOICE &&
            speech_available()) {
            int extra = 0;
            audio_msg_t m2;
            UBaseType_t pending = uxQueueMessagesWaiting(s_audio_q);
            for (UBaseType_t i = 0; i < pending; i++) {
                if (xQueueReceive(s_audio_q, &m2, 0) != pdTRUE) break;
                if (m2.kind == AUDIO_EVT_NEW_CONTACT) extra++;
                else handle_msg(&m2);
            }
            /* A contact cut off by live radio takes its summary with it. */
            if (handle_msg(&msg) && extra > 0) {
                char buf[48];
                snprintf(buf, sizeof(buf), "AND %d MORE.", extra);
                audio_out_ensure_unmuted();
                announce(buf, AUDIO_EVT_NONE);
            }
            continue;
        }

        handle_msg(&msg);
    }
}

void audio_events_init(void)
{
    if (s_audio_q) return;
    /* Speech stands on its own: the greeting and the console use it even if
       the callouts below cannot start. */
    if (speech_init() != ESP_OK) {
        ESP_LOGW(TAG, "speech unavailable - voice events fall back to tones");
    }

    /* PSRAM queue and stack: this task builds phrases, plays tones and hands
       text to the speech worker, and never touches NVS or flash. Its 3 KB of
       internal stack paid for the speech worker's internal TCB and more. */
    QueueHandle_t q = xQueueCreateWithCaps(8, sizeof(audio_msg_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q) {
        ESP_LOGW(TAG, "no memory for the event queue - callouts off");
        return;
    }
    callouts_load();
    s_audio_q = q;
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", 4096, NULL, 6, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdTRUE) {
        ESP_LOGW(TAG, "event task create failed - callouts off");
        s_audio_q = NULL;
        vQueueDeleteWithCaps(q);
    }
}

void audio_events_play_test(void)
{
    if (!s_audio_q) return;
    audio_msg_t m = { .kind = AUDIO_EVT_NONE, .queued = xTaskGetTickCount() };
    xQueueSend(s_audio_q, &m, 0);
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

    audio_msg_t m = { .kind = kind, .icao = icao, .crc_shaky = crc_shaky,
                      .queued = xTaskGetTickCount() };
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
    callouts_save();
    return s_mode[kind];
}

audio_mesh_say_t audio_events_mesh_say_get(void) { return s_mesh_say; }

audio_mesh_say_t audio_events_mesh_say_cycle(void)
{
    s_mesh_say = (audio_mesh_say_t)((s_mesh_say + 1) % AUD_MESH_COUNT);
    callouts_save();
    return s_mesh_say;
}

const char *audio_mesh_say_label(audio_mesh_say_t m)
{
    switch (m) {
        case AUD_MESH_OFF:    return "off";
        case AUD_MESH_SENDER: return "sender";
        case AUD_MESH_FULL:   return "sender and text";
        default:              return "?";
    }
}

void audio_events_mesh_message(const char *text, bool direct)
{
    const audio_mesh_say_t say = s_mesh_say;
    if (!s_audio_q || !text || say == AUD_MESH_OFF || !speech_available()) return;
    char phrase[SPEECH_TEXT_MAX];
    mesh_phrase(phrase, sizeof(phrase), text, direct, say == AUD_MESH_FULL);
    taskENTER_CRITICAL(&s_mesh_lock);
    memcpy(s_mesh_phrase, phrase, sizeof(phrase));
    taskEXIT_CRITICAL(&s_mesh_lock);
    audio_msg_t m = { .kind = AUDIO_EVT_MESH, .queued = xTaskGetTickCount() };
    xQueueSend(s_audio_q, &m, 0);
}
void audio_event_mode_set_all(audio_mode_t m)
{
    if (m >= AUD_MODE_COUNT) return;
    for (int i = 1; i < AUDIO_EVT_KIND_COUNT; i++) s_mode[i] = m;
    if (m == AUD_MODE_OFF) s_mesh_say = AUD_MESH_OFF;
    callouts_save();
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
