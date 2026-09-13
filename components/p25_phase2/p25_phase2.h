#ifndef LS_P25_PHASE2_H
#define LS_P25_PHASE2_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct p25p2_decoder p25p2_decoder_t;
typedef void (*p25p2_audio_fn)(const int16_t *pcm, size_t count, void *context);
typedef struct {
  uint32_t symbols, bursts, control_ok, control_errors;
  uint32_t voice_frames, audio_frames, muted_frames;
  uint16_t talkgroup;
  uint32_t source;
  uint8_t slot, algorithm;
  bool synchronized, clear_confirmed;
} p25p2_status_t;
p25p2_decoder_t *p25p2_create(p25p2_audio_fn output, void *context);
void p25p2_destroy(p25p2_decoder_t *decoder);
bool p25p2_configure(p25p2_decoder_t *decoder, uint32_t wacn, uint16_t system,
                     uint16_t nac, unsigned slot);
void p25p2_reset(p25p2_decoder_t *decoder);
void p25p2_push(p25p2_decoder_t *decoder, const uint8_t *dibits, size_t count);
void p25p2_status(const p25p2_decoder_t *decoder, p25p2_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
