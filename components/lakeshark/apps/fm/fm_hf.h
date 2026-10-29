/* HF (shortwave) listening inside the FM app: the settings the HF
   demodulators run with, the band plan with each band's mode and step, and
   when the AM mode hands its audio to them (fm_hf_chain).

   HF lives in FM_MODE_AM rather than a mode of its own: AM below 30 MHz, and
   USB, LSB, CW or synchronous AM at any frequency, all go through hf_demod.c.
   Plain AM above 30 MHz (airband) keeps the existing fm_demod_am path. So the
   mode list, the RADIO picker, OPTIONS, the keypad and the console keep
   working exactly as they do for AM, and HF adds its rows to AM's OPTIONS.

   Pure state, no RTOS: the screen writes it on the UI task, the receive task
   reads it and watches fm_hf_generation() for changes. app_fm.c installs the
   hooks that persist it and retune. */
#ifndef FM_HF_H
#define FM_HF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same numbering as hf_demod_mode_t (hf_demod.h). */
typedef enum {
    FM_HF_AM = 0, FM_HF_SAM, FM_HF_USB, FM_HF_LSB, FM_HF_CW, FM_HF_DEMOD_COUNT
} fm_hf_demod_t;

#define FM_HF_RIT_MAX_HZ   2000    /* clarifier reach, either way          */
#define FM_HF_RIT_STEP_HZ    10
#define FM_HF_PITCH_MIN_HZ  400
#define FM_HF_PITCH_MAX_HZ 1000

typedef struct {
    uint8_t  demod;      /* fm_hf_demod_t                                  */
    uint8_t  narrow;     /* 1: the narrow filter of the demod's pair       */
    uint8_t  agc;        /* hf_agc_t: 0 fast, 1 slow, 2 off                */
    uint8_t  nb;         /* hf_nb_t: 0 off, 1 low, 2 high                  */
    int16_t  rit_hz;     /* clarifier, 10 Hz steps                         */
    uint16_t pitch_hz;   /* CW BFO pitch                                   */
    uint32_t step_hz;    /* VFO step while HF is on; 0 follows the band    */
} fm_hf_settings_t;

typedef struct {
    void (*changed)(void);         /* a setting moved: persist, reconfigure */
    void (*tune)(uint32_t hz);     /* go to HF on this dial frequency       */
} fm_hf_hooks_t;

void fm_hf_install(const fm_hf_hooks_t *hooks);

const fm_hf_settings_t *fm_hf_get(void);
/* Moves on every change; the receive task compares it. */
uint32_t fm_hf_generation(void);

void fm_hf_set_demod(int demod);
void fm_hf_set_narrow(bool narrow);
void fm_hf_set_agc(int agc);
void fm_hf_set_nb(int nb);
/* Rounded to 10 Hz and held to +-FM_HF_RIT_MAX_HZ. */
void fm_hf_set_rit(int hz);
void fm_hf_rit_step(int direction);
void fm_hf_set_pitch(int hz);
void fm_hf_set_step(uint32_t hz);
/* The next of the HF raster: 10, 50, 100, 500 Hz, 1, 5, 9, 10 kHz. */
uint32_t fm_hf_step_next(uint32_t hz);
/* The VFO step in force at `hz`: the chosen one, or the band's. */
uint32_t fm_hf_step_at(uint32_t hz);

/* The filter width the demod runs: AM/SAM 8 or 6 kHz, SSB 2.8 or 2.4 kHz,
   CW 500 or 250 Hz (wide, narrow). */
uint32_t fm_hf_filter_hz(const fm_hf_settings_t *s);

const char *fm_hf_demod_label(int demod);       /* "AM" "SAM" "USB" ... */
/* am sam usb lsb cw, and "ssb" for the sideband the band plan expects. */
bool fm_hf_demod_parse(const char *text, uint32_t hz, int *demod);
const char *fm_hf_agc_label(int agc);
const char *fm_hf_nb_label(int nb);

/* Whether AM mode at `hz` with `demod` runs the HF demodulators. */
static inline bool fm_hf_chain(bool am_mode, uint32_t hz, int demod)
{
    return am_mode && (hz < 30000000u || demod != FM_HF_AM);
}

/* The HF signal meter: channel power from FM_HF_METER_FLOOR_DB (empty) over
   FM_HF_METER_SPAN_DB (full), 0..1. HF levels span far more than AM's linear
   carrier scale can show (8-bit direct sampling sits near -75 dBFS in a
   2.4 kHz channel), so the meter is in dB. AM's squelch setting, 0..100,
   is a point on this same travel, so its gate mark lands where it opens. */
#define FM_HF_METER_FLOOR_DB (-110.0f)
#define FM_HF_METER_SPAN_DB   (90.0f)
static inline float fm_hf_meter(float dbfs)
{
    const float m = (dbfs - FM_HF_METER_FLOOR_DB) / FM_HF_METER_SPAN_DB;
    return m < 0.0f ? 0.0f : m > 1.0f ? 1.0f : m;
}
/* The level a squelch setting opens at, in dBFS. */
static inline float fm_hf_squelch_dbfs(int squelch)
{
    return FM_HF_METER_FLOOR_DB + FM_HF_METER_SPAN_DB * (float)squelch / 100.0f;
}

/* Settings to and from one versioned word for NVS. Unpack rejects anything
   out of range and leaves *s alone. */
uint32_t fm_hf_pack(const fm_hf_settings_t *s);
bool fm_hf_unpack(uint32_t word, fm_hf_settings_t *s);
/* Load without notifying (boot). */
void fm_hf_load(const fm_hf_settings_t *s);

/* The band plan. */
typedef enum {
    FM_HF_KIND_BROADCAST = 0, FM_HF_KIND_HAM, FM_HF_KIND_CB, FM_HF_KIND_TIME,
} fm_hf_kind_t;

typedef struct {
    const char *name;
    uint32_t lo_hz, hi_hz;   /* equal for a single station */
    uint32_t dial_hz;        /* where choosing the band goes */
    uint8_t  demod;          /* fm_hf_demod_t the band is used in */
    uint8_t  kind;           /* fm_hf_kind_t */
    uint32_t step_hz;
} fm_hf_band_t;

int fm_hf_band_count(void);
const fm_hf_band_t *fm_hf_band(int i);
/* The band `hz` is in, -1 for none. A single station matches within 5 kHz. */
int fm_hf_band_find(uint32_t hz);
/* Point the receiver at band `i`: its demod and step, then its dial through
   the tune hook. False for no such band. */
bool fm_hf_band_apply(int i);
/* Go to HF on `hz` in `demod` (FM_HF_DEMOD_COUNT: the band plan's choice). */
void fm_hf_tune(uint32_t hz, int demod);

/* The demod the band plan uses at `hz`: the band's own, else AM. */
int fm_hf_auto_demod(uint32_t hz);
/* The sideband amateurs use at `hz`: LSB below 10 MHz except 60 m. */
int fm_hf_auto_sideband(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif
