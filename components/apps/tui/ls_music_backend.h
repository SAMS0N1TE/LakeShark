#ifndef LS_MUSIC_BACKEND_H
#define LS_MUSIC_BACKEND_H
#include <stdbool.h>
#include <stdint.h>
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
void ls_music_stop(void);
ls_music_state_t ls_music_state(void);
int ls_music_volume(void);
uint32_t ls_music_position_ms(void);
uint32_t ls_music_duration_ms(void);
/* Decoded source peak per channel since the last call, 0..32768, before the
   mono downmix, tone and codec volume. Zero unless playing. */
unsigned ls_music_peak(int channel);
/* Power in `bands` logarithmic bands centred from lo_hz to hi_hz, in dB
   relative to a full-scale sine, from the last LS_MUSIC_FFT_SIZE samples
   sent to the codec (L+R average, after tone, before codec volume).
   Returns how many bands were measured; bands whose upper edge reaches the
   Nyquist frequency are not measured and are left at -INFINITY. Zero unless
   playing with a full window of samples. */
#define LS_MUSIC_FFT_SIZE 2048
int ls_music_spectrum(float lo_hz, float hi_hz, int bands, float *db);
/* Spectrogram columns, gapless: each call that returns true consumes the
   next `hops` (1..4) back-to-back FFT windows after the cursor and gives
   their average power in `rows` equal linear slices from 0 Hz to Nyquist,
   in dB as above. False when those samples have not been decoded yet.
   Zero-initialise the cursor; it restarts on its own after a sample-rate
   change or when the reader falls too far behind. */
typedef struct {
    uint32_t next, rate;
    bool started;
} ls_music_cursor_t;
bool ls_music_spectrogram(ls_music_cursor_t *cursor, int hops, int rows,
                          float *db);
typedef struct {
    unsigned rate_hz, bits, channels;
    bool mono_out; /* the board plays both source channels as one */
} ls_music_format_t;
bool ls_music_format(ls_music_format_t *out);
/* The microphone as a source: live into the meters and plots, and with
   `record` also into a mono WAV in the SD music folder. Starting it stops
   playback; playing a track stops it. */
typedef enum { LS_MIC_OFF, LS_MIC_LIVE, LS_MIC_RECORDING } ls_music_mic_t;
bool ls_music_mic_start(bool record);
void ls_music_mic_stop(void);
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
