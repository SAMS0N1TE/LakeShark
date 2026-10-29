#ifndef LS_MUSIC_BACKEND_H
#define LS_MUSIC_BACKEND_H
#include <stdbool.h>
#include <stdint.h>
#include "ls_audio_analysis.h"
#include "ls_audio_overview.h"
#include "ls_audio_tools.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { LS_MUSIC_STOPPED, LS_MUSIC_PLAYING, LS_MUSIC_PAUSED } ls_music_state_t;
bool ls_music_open(void);
void ls_music_close(void);
bool ls_music_scan(int source);
int ls_music_count(void);
const char *ls_music_name(int index);
const char *ls_music_error(void);
bool ls_music_play(int index);
/* A caller-owned archive path; the decoder owns the file after queueing. */
bool ls_music_play_path(const char *path);
bool ls_music_is_open(void);
bool ls_music_toggle(void);
bool ls_music_seek_ms(uint32_t ms);
bool ls_music_speed_set(unsigned permille); /* 500..2000, varispeed */
unsigned ls_music_speed_get(void);
bool ls_music_loop_set(uint32_t a_ms,uint32_t b_ms,bool enabled);
bool ls_music_loop_get(uint32_t *a_ms,uint32_t *b_ms);
bool ls_music_marker_add(void);
void ls_music_markers_clear(void);
unsigned ls_music_marker_count(void);
bool ls_music_marker_get(unsigned index,uint32_t *at_ms);
void ls_music_stop(void);
ls_music_state_t ls_music_state(void);
int ls_music_volume(void);
bool ls_music_muted(void);
void ls_music_mute_toggle(void);
uint32_t ls_music_position_ms(void);
uint32_t ls_music_duration_ms(void);
/* Decoded source peak per channel since the last call, 0..32768, before the
   mono downmix, tone and codec volume. Zero unless playing. The LEVEL view
   turns it into what is heard with ls_music_volume_db and ls_music_muted. */
unsigned ls_music_peak(int channel);
/* Power in `bands` logarithmic bands centred from lo_hz to hi_hz, in dB
   relative to a full-scale sine, from the last LS_MUSIC_FFT_SIZE samples
   sent to the codec (L+R average, after tone, before codec volume).
   Returns how many bands were measured; bands whose upper edge reaches the
   Nyquist frequency are not measured and are left at -INFINITY. Zero unless
   playing with a full window of samples. */
#define LS_MUSIC_FFT_SIZE 2048
int ls_music_spectrum(float lo_hz, float hi_hz, int bands, float *db);
/* Spectrogram lines at full FFT resolution. Each call that returns true
   transforms the LS_MUSIC_FFT_SIZE-sample Hann window that starts at the
   cursor and writes its one-sided power for bins 0 .. LS_MUSIC_FFT_SIZE/2-1
   into `power`, linear and relative to a full-scale sine (1.0 is 0 dB on
   the spectrum's scale), then moves the cursor on by `hop` samples
   (LS_MUSIC_HOP_MIN .. LS_MUSIC_FFT_SIZE), so a hop under the window size
   overlaps the windows. Bin k is centred on k * rate / LS_MUSIC_FFT_SIZE Hz.
   Same samples as the spectrum: L+R after tone, before codec volume and
   mute, so what a recording carries reads the same at any volume.
   False when that window has not been decoded yet. Zero-initialise the
   cursor; it restarts at the newest window after a source or sample-rate
   change, and skips ahead rather than read samples about to be overwritten
   when the reader falls more than half the history behind. */
#define LS_MUSIC_HOP_MIN 128
typedef struct {
    uint32_t next, rate, epoch;
    bool started;
} ls_music_cursor_t;
bool ls_music_spectrogram_line(ls_music_cursor_t *cursor, unsigned hop,
                               float *power);
typedef struct {
    unsigned rate_hz, bits, channels, epoch;
    bool mono_out; /* the board plays both source channels as one */
} ls_music_format_t;
bool ls_music_format(ls_music_format_t *out);
/* UI-owned consumer; the audio worker only appends source PCM. */
ls_audio_analysis *ls_music_analysis(void);
ls_audio_overview *ls_music_overview(void);
ls_audio_tools *ls_music_tools(void);
bool ls_music_tone_start(ls_gen_kind kind,unsigned start_hz,unsigned end_hz,unsigned duration_ms,int dbfs);
bool ls_music_tone_active(void);
/* Radio WAV capture needs a shared PCM tap before audio_write_mono's
   media/mute checks (P25 reaches it via audio_write_p25_voice). MUSIC
   currently parks RF and owns the exclusive media lease, so it does not
   expose a radio-record action. Existing archived call WAVs are playable. */
/* The microphone as a source: live into the meters and plots, and with
   `record` also into a mono WAV in the SD music folder. Starting it stops
   playback; playing a track stops it. */
typedef enum { LS_MIC_OFF, LS_MIC_LIVE, LS_MIC_RECORDING, LS_MIC_TONE } ls_music_mic_t;
bool ls_music_mic_start(bool record);
void ls_music_mic_stop(void);
bool ls_music_mic_repair(void);
ls_music_mic_t ls_music_mic_state(void);
const char *ls_music_mic_file(void);
uint32_t ls_music_mic_ms(void);
/* One capture block: the worker's loop body, public for the bench. */
bool ls_music_mic_step(void);
/* True while a track plays or the microphone runs: there is audio to show. */
bool ls_music_live(void);
/* Codec attenuation at the current volume setting. */
float ls_music_volume_db(void);
bool ls_music_warm(void);
void ls_music_set_warm(bool enabled);
void ls_music_volume_step(int delta);
const char *ls_music_output(void);
#ifdef __cplusplus
}
#endif
#endif
