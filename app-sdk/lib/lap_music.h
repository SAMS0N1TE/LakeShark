/* LTM1 tracker, SDK 2.5. Four tone voices mixed to Phase 2's MUSIC PCM
 * channel (16kHz). Same LAP_WAVE_* shapes as lap_tone, with per-instrument
 * attack/release. No allocation, callbacks, waits or per-note tone queue.
 * Call pump each visible frame; pause/stop on focus loss. No speaker? Skip
 * mixer calls after lap_caps; render can still generate PCM offline. */
#ifndef LAP_MUSIC_H
#define LAP_MUSIC_H
#include "lakeshark_app.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LAP_MUSIC_CHANNELS 4
#define LAP_MUSIC_PATTERNS 8
#define LAP_MUSIC_ROWS 64
#define LAP_MUSIC_ORDERS 32
typedef struct { uint8_t note, instrument; } lap_music_note_t; /* 0 hold, 255 stop, MIDI 12..119 */
typedef struct { uint8_t wave,volume; uint16_t attack_ms,release_ms; } lap_music_instrument_t;
typedef struct {
    uint16_t bpm;
    uint8_t rows_per_beat,order_count,loop_order; /* 255 = no loop */
    uint8_t order[LAP_MUSIC_ORDERS],rows[LAP_MUSIC_PATTERNS];
    lap_music_instrument_t instruments[8];
    lap_music_note_t patterns[LAP_MUSIC_PATTERNS][LAP_MUSIC_ROWS][LAP_MUSIC_CHANNELS];
} lap_music_song_t;
/* Bounded text, no NUL needed; # to end of line. Returns 0 or EFORMAT (-200),
 * error_line receives the 1-based line. On failure out is zeroed.
 * Grammar, one command per line:
 * LTM1
 * tempo 120 4                    # BPM 30..300, rows/beat 1..16
 * inst 0 2 40 5 60              # id 0..7, LAP_WAVE_*, volume 0..100,
 *                               # attack/release milliseconds 0..1000
 * pattern 0 16                  # id 0..7, row count 1..64
 * row 0 C4/0 ... --- G#3/1       # selected pattern, row 0..count-1,
 *                               # 4 channels, hold/rest, stop, note/instrument
 * order 0 0                     # pattern sequence, max 32 entries
 * loop 0                        # order index, or 255 for one-shot
 * Missing rows hold all channels. C4 is MIDI 60. Tempo, pattern and instrument
 * declarations precede rows; order/loop required. No external references. */
int lap_music_parse(lap_music_song_t *out,const char *text,size_t bytes,int *error_line);
typedef struct {
    uint32_t phase,increment,age,gate,release,attack,noise;
    uint8_t wave,volume;
} lap_music_voice_t;
typedef struct {
    const lap_music_song_t *song; /* caller retains song through stop */
    lap_music_voice_t voices[4];
    uint32_t row_left,row_fraction;
    unsigned order,row;
    bool playing;
    int16_t pending[1024]; /* app-owned PCM; partial mixer writes retained */
    unsigned offset,count;
} lap_music_player_t;
/* start resets player; song must be validated by parse. No mixer access. */
void lap_music_start(lap_music_player_t *p,const lap_music_song_t *song);
/* Exactly samples generated, no allocation. Silence after end of song. */
void lap_music_render(lap_music_player_t *p,int16_t *pcm,unsigned samples);
/* One bounded render/write, at most 1024 samples per call, no busy polling.
 * Returns samples accepted, 0 on backpressure or negative mixer error.
 * Queue drain determines musical time; no catch-up after stalls. */
int lap_music_pump(lap_music_player_t *p);
void lap_music_stop(lap_music_player_t *p); /* clears MUSIC queue, effects untouched */
#ifdef __cplusplus
}
#endif
#endif
