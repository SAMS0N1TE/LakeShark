#ifndef NOTIFICATION_TONE_H
#define NOTIFICATION_TONE_H
/* Bounded, sample-free voice-band notification synthesis. Worker-owned state. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Presets 0 RADIO KEY, 1 ROGER, 2 CHIRP. Fixed 16 kHz mono PCM, <=4800 peak.
 * Render consecutive chunks by sample offset; returns zero at end/invalid ID. */
size_t notification_tone_render(int preset, size_t offset, int16_t *pcm, size_t capacity);
/* Atomic table replacement: version 1 JSON, three rows of nine integer fields:
 * start Hz, end Hz, ms, gap ms, start Hz, end Hz, ms, attack ms, release ms.
 * Call only on sound worker, outside rendering. Invalid files retain defaults. */
bool notification_tone_configure(const char *json);
void notification_tone_load(void); /* /sdcard/audio/notification.json, once per boot */
#ifdef __cplusplus
}
#endif
#endif
