#ifndef LS_AUDIO_ANALYSIS_H
#define LS_AUDIO_ANALYSIS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ls_audio_analysis ls_audio_analysis;
typedef enum { LS_AA_HANN, LS_AA_BLACKMAN_HARRIS } ls_aa_window;
typedef struct {
  unsigned fft_size, hop;
  ls_aa_window window;
  bool log_axis;
  float lo_hz, hi_hz, floor_db, range_db, gain_db;
} ls_aa_config;
typedef struct {
  uint32_t time_ms, frames, onsets;
  float peak_hz, spectral_db, pitch_hz, pitch_confidence;
  int note;
  float cents;
  float rms_db, peak_db, crest_db, estimated_peak_db;
  float momentary_lufs, short_lufs, correlation;
  float chroma[12], key_confidence, bpm, onset_strength;
  int key_root;
  bool key_minor;
} ls_aa_result;
typedef struct {
  uint32_t time_ms;
  float hz, db;
  int note;
  float cents;
} ls_aa_cursor;
typedef struct {
  uint32_t time_ms;
  float hz;
} ls_aa_track;
ls_aa_config ls_audio_analysis_defaults(void);
ls_audio_analysis *ls_audio_analysis_new(void);
void ls_audio_analysis_free(ls_audio_analysis *a);
size_t ls_audio_analysis_bytes(void);
bool ls_audio_analysis_configure(ls_audio_analysis *a,
                                 const ls_aa_config *config);
ls_aa_config ls_audio_analysis_config(const ls_audio_analysis *a);
/* Single producer: audio task only packs stereo frames into the PSRAM ring.
   Reset is also producer-owned. It never clears the UI's working arrays. */
void ls_audio_analysis_reset(ls_audio_analysis *a);
void ls_audio_analysis_reset_at(ls_audio_analysis *a,uint32_t source_ms);
uint32_t ls_audio_analysis_origin_ms(const ls_audio_analysis *a);
void ls_audio_analysis_append(ls_audio_analysis *a, const int16_t *pcm,
                              size_t frames, unsigned channels, unsigned rate);
/* UI/consumer owned. Bounded PCM metrics plus at most max_windows FFTs.
   No allocations, file IO or waits. Old windows are skipped on overrun. */
unsigned ls_audio_analysis_process(ls_audio_analysis *a, unsigned max_windows);
const ls_aa_result *ls_audio_analysis_result(const ls_audio_analysis *a);
unsigned ls_audio_analysis_bins(const ls_audio_analysis *a);
float ls_audio_analysis_bin_db(const ls_audio_analysis *a, unsigned bin,
                               bool hold);
float ls_audio_analysis_axis(const ls_audio_analysis *a, unsigned pixel,
                             unsigned pixels, bool melodic);
bool ls_audio_analysis_cursor(const ls_audio_analysis *a, uint32_t time_ms,
                              float hz, ls_aa_cursor *out);
unsigned ls_audio_analysis_track(const ls_audio_analysis *a, ls_aa_track *out,
                                 unsigned capacity);
unsigned ls_audio_analysis_pitch_track(const ls_audio_analysis *a,
                                       ls_aa_track *out, unsigned capacity);
/* Independent consumer of the same source PCM, for radio decoders. */
size_t ls_audio_analysis_read(const ls_audio_analysis *a, uint32_t *next,
                              uint32_t *epoch, int16_t *stereo, size_t frames,
                              unsigned *rate);
bool ls_audio_analysis_scope(const ls_audio_analysis *a, unsigned index,
                             float *left, float *right);
/* Last window's source waveform, not a generated visual. */
float ls_audio_analysis_wave(const ls_audio_analysis *a, unsigned pixel,
                             unsigned pixels);
#ifdef __cplusplus
}
#endif
#endif
