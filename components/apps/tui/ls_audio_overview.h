#ifndef LS_AUDIO_OVERVIEW_H
#define LS_AUDIO_OVERVIEW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_OV_WAVE 1024u
#define LS_OV_COLUMNS 512u
#define LS_OV_BINS 128u
typedef struct ls_audio_overview ls_audio_overview;
typedef void (*ls_ov_service_fn)(void *user);
void ls_audio_overview_set_service(ls_audio_overview *o,ls_ov_service_fn fn,void *user);
ls_audio_overview *ls_audio_overview_new(const char *cache_directory);
/* False retains the object if its worker has not parked. */
bool ls_audio_overview_free(ls_audio_overview *o);
bool ls_audio_overview_request(ls_audio_overview *o, const char *path,
                               bool refresh);
/* Background worker on device; deterministic explicit slices on the bench. */
void ls_audio_overview_step(ls_audio_overview *o, unsigned frames_budget);
unsigned ls_audio_overview_progress(const ls_audio_overview *o);
bool ls_audio_overview_ready(const ls_audio_overview *o);
bool ls_audio_overview_cached(const ls_audio_overview *o);
unsigned ls_audio_overview_rate(const ls_audio_overview *o);
uint32_t ls_audio_overview_duration_ms(const ls_audio_overview *o);
const char *ls_audio_overview_error(const ls_audio_overview *o);
bool ls_audio_overview_wave(const ls_audio_overview *o, unsigned bucket,
                            int16_t *lo, int16_t *hi);
uint8_t ls_audio_overview_spectro(const ls_audio_overview *o, unsigned column,
                                  unsigned bin);
size_t ls_audio_overview_bytes(void);
/* Queue a precise source-time STFT slice after the overview pass. */
bool ls_audio_overview_slice_request(ls_audio_overview *o, uint32_t time_ms,
                                     unsigned fft_size, unsigned window);
bool ls_audio_overview_slice_ready(const ls_audio_overview *o,
                                   uint32_t *time_ms, unsigned *bins);
float ls_audio_overview_slice_db(const ls_audio_overview *o, unsigned bin);
#ifdef __cplusplus
}
#endif
#endif
