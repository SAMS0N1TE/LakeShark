/* BQ27220 battery fuel gauge. */

#ifndef LS_GAUGE_H
#define LS_GAUGE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     present;
    uint16_t millivolts;    /* pack voltage, measured                     */
    int16_t  milliamps;     /* positive charging, negative discharging    */
    /* What to show: from ls_gauge_get, the filtered figure (ls_gauge_soc.h:
       follows discharge down only, charge up only, 100 only when the charger
       is done); from ls_gauge_read, that one sample's estimate. */
    uint8_t  percent;
    uint8_t  learned_percent; /* the gauge's own estimate, 0..100         */
    int16_t  temp_c10;      /* tenths of a degree C                       */
    uint16_t remaining_mah;
    uint16_t full_mah;
    bool     charging;      /* current into the cell                      */
    bool     plugged;       /* external power: charging, full or idle     */
    bool     full;          /* external power and the charger is done    */
    uint8_t  state;         /* ls_batt_state_t                            */
} ls_gauge_t;

/* Probe and start a low-priority sampler: voltage and current every
   5 seconds into the filter, the rest every 30. Idempotent; call at boot. */
esp_err_t ls_gauge_start(void);
bool      ls_gauge_present(void);

/* One read of everything. Cheap enough to call once a second; do NOT call
   it per frame - it is six I2C transactions on a bus the touch controller
   also wants. */
esp_err_t ls_gauge_read(ls_gauge_t *out);

/* Cache-only, no I2C and no waits. Updated by the background sampler every
   5 seconds, independent of UI/charger activity. False before first sample.
   Every screen reads its percent from here, so they all agree. */
bool ls_gauge_get(ls_gauge_t *out);

/* The cached current and how old it is, with no bus traffic, so a task
   other than the one refreshing it can read it. False before the first
   read or with no gauge. */
bool ls_gauge_peek(int16_t *milliamps, int64_t *age_us);

void ls_gauge_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_GAUGE_H */
