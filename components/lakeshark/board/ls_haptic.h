

#ifndef LS_HAPTIC_H
#define LS_HAPTIC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ls_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_HAPTIC_TICK = 0,   /* a key, a step of a tuning dial: barely there */
    LS_HAPTIC_CLICK,      /* a button took the press                      */
    LS_HAPTIC_CONFIRM,    /* something committed - armed, saved, sent     */
    LS_HAPTIC_ALERT,      /* a message arrived and you are not looking    */
    LS_HAPTIC_WARN,       /* something is wrong and needs a hand          */
    LS_HAPTIC_EFFECT_COUNT
} ls_haptic_effect_t;

/* Bring the part up: probe, identify, reset, configure for the board's LRA,
   and start the worker. ESP_ERR_NOT_FOUND when nothing answers, which on a
   board that may be assembled without a motor is not a fault. Safe to call
   twice. */
esp_err_t ls_haptic_start(void);

/* True once the part has answered and been configured. Nothing below does
   anything until it has. */
bool ls_haptic_present(void);

/* Play one. Returns false when the part is absent or the queue is full -
   and a full queue is a dropped buzz on purpose: a notice that arrives while
   the motor is still running the last one is not worth blocking a draw for.

   Callable from any task INCLUDING the draw path: it is a queue send and
   nothing else. */
bool ls_haptic_play(ls_haptic_effect_t effect);

/* Run the motor for an arbitrary time at an arbitrary strength. `ms` is
   clamped to LS_HAPTIC_MAX_MS and `strength` to 0..100. For the console and
   for anything tuning the part; the named effects above are what the
   firmware should be asking for. */
bool ls_haptic_buzz(int ms, int strength);

/* Stop whatever is playing and drop anything queued behind it. */
void ls_haptic_stop(void);

/* The worker's whole body, as one call. */

bool ls_haptic_service(int wait_ms);

#define LS_HAPTIC_MAX_MS 2000

/* What the part can measure about the motor on the other side of it.

   This is the difference between "the register writes were accepted" and
   "there is a motor there". The chip drives a diagnostic current through the
   coil and reports its resistance; an open circuit or a missing motor does
   not read 10 to 30 ohms. The resonance is the part's own tracking loop
   reporting what it locked onto during the last CONT playback, so it is
   only meaningful after something has played. */
typedef struct {
    float lra_ohms;   /* coil resistance, measured. 0 when the read failed  */
    float vdd_v;      /* the part's own supply, measured                    */
    float f0_hz;      /* tracked resonance, or 0 if nothing has played yet  */
    bool  over_current;
    bool  over_temp;
    bool  under_voltage;
} ls_haptic_diag_t;

/* Measure the above. Takes about 10 ms and talks to the part, so it is not
   for the draw path. Returns false when the part is absent. */
bool ls_haptic_diag(ls_haptic_diag_t *out);

/* CONTCFG2 F_PRE: the part derives F0 = 24000 / code. */
uint8_t ls_haptic_f_pre_code(int f0_hz);

/* CONTCFG3 DRV_WIDTH: the datasheet's recommendation is
   (24000/F0) - 8 - TRACK_MARGIN - BRK_GAIN, and it must stay under the half
   cycle time of F0 or the brake fires into the drive. */
uint8_t ls_haptic_drv_width_code(int f0_hz, int track_margin, int brk_gain);

/* CONTCFG6/7 drive level: 0..127, from a percentage. */
uint8_t ls_haptic_level_code(int strength);

int ls_haptic_clamp_ms(int ms);

/* DET_RL + DET_LO -> ohms, per the datasheet's 10-bit form. */
float ls_haptic_rl_ohms(uint8_t rl, uint8_t rl_lo, int d2s_gain);

/* DET_VBAT + DET_LO -> volts. */
float ls_haptic_vbat_volts(uint8_t vbat, uint8_t vbat_lo);

/* CONTRD16/17 -> Hz. Zero when the part has not tracked anything. */
float ls_haptic_f0_hz(uint8_t f0_h, uint8_t f0_l);

#ifdef __cplusplus
}
#endif

#endif /* LS_HAPTIC_H */
