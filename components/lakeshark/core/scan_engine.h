#ifndef SCAN_ENGINE_H
#define SCAN_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scan_feedback.h"

#ifdef __cplusplus
extern "C" {
#endif

void scan_engine_init(void);
void scan_engine_start(void);
void scan_engine_stop(void);
bool scan_engine_active(void);
/**/
/* True while the engine is driving the tuner fast - scanning OR calibrating.
   This, not scan_engine_active(), is what a retune fast-path should test. */
bool scan_engine_sweeping(void);

void scan_engine_skip(void);
void scan_engine_set_hang_ms(int ms);
/**/
void scan_engine_set_threshold_pct(int pct);
/**/
void scan_engine_set_zone(int zone);
/**/
void scan_engine_set_priority_ms(int ms);

int   scan_engine_current(void);
int   scan_engine_get_hang_ms(void);
/**/
int   scan_engine_get_threshold_pct(void);
/**/
int   scan_engine_get_zone(void);
/**/
int   scan_engine_get_priority_ms(void);

void scan_engine_status(char *buf, size_t n);
scan_phase_t scan_engine_phase(void);

/**/
/* Two ways to scan, the way a handheld does it.
   CHANNELS - the stored list, filtered by zone. Presets.
   BAND     - a bare frequency grid, start/stop/step, no stored channels at
              all. This is the "scan every 12.5 kHz and stop on anything"
              behaviour of a Baofeng, and it is NOT the FM app's SWEEP:
              SWEEP measures a band and plots it, this one STOPS and lets
              you listen, reusing the same hold/hang/squelch path as the
              preset scanner. */
typedef enum {
    SCAN_SRC_CHANNELS = 0,
    SCAN_SRC_BAND     = 1,
} scan_src_t;

void       scan_engine_set_source(scan_src_t src);
scan_src_t scan_engine_get_source(void);
/* step_hz 0 keeps the current step. Returns false if the range is unusable. */
bool       scan_engine_set_band(uint32_t start_hz, uint32_t stop_hz, uint32_t step_hz);
void       scan_engine_get_band(uint32_t *start_hz, uint32_t *stop_hz, uint32_t *step_hz);
int        scan_engine_band_steps(void);

/**/
/* Measure the noise floor and set the NFM squelch a margin above it. Runs
   asynchronously on the scan task (it tunes and blocks), so this returns
   immediately - watch scan_engine_status() for the result. margin < 0 keeps
   the current margin. NFM only: squelch is not a P25 concept. */
void       scan_engine_autosquelch(int margin);
int        scan_engine_autosquelch_floor(void);

#ifdef __cplusplus
}
#endif

#endif
