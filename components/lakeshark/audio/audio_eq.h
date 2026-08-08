#ifndef AUDIO_EQ_H
#define AUDIO_EQ_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_EQ_FLAT = 0,
    AUDIO_EQ_VOICE,
    AUDIO_EQ_PUNCH,
    AUDIO_EQ_FULL,
    AUDIO_EQ_CUSTOM,
    AUDIO_EQ_PRESET_COUNT
} audio_eq_preset_t;

#define AUDIO_EQ_HP_MIN     8
#define AUDIO_EQ_HP_MAX     40
#define AUDIO_EQ_BASS_MIN  (-6)
#define AUDIO_EQ_BASS_MAX   12
#define AUDIO_EQ_TREB_MIN  (-8)
#define AUDIO_EQ_TREB_MAX   8
#define AUDIO_EQ_LOUD_MAX   3

typedef struct {
    uint8_t preset;
    uint8_t hp10;
    int8_t  bass_db;
    int8_t  treb_db;
    uint8_t punch;
    uint8_t loud;
} audio_eq_cfg_t;

void        audio_eq_init(int rate_hz);
void        audio_eq_get(audio_eq_cfg_t *out);
void        audio_eq_set(const audio_eq_cfg_t *in);
bool        audio_eq_apply_preset(int preset);
const char *audio_eq_preset_name(int preset);
int         audio_eq_preset_from_name(const char *s);
void        audio_eq_reset_state(void);
bool        audio_eq_enabled(void);

void        audio_eq_process(int16_t *pcm, int n);

int         audio_eq_gr_db10(void);

#ifdef __cplusplus
}
#endif

#endif
