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
    uint8_t  percent;       /* the gauge's learned estimate, 0..100       */
    int16_t  temp_c10;      /* tenths of a degree C                       */
    uint16_t remaining_mah;
    uint16_t full_mah;
    bool     charging;      /* current is positive                        */
} ls_gauge_t;

/* Probe the part. Safe to call more than once. */
esp_err_t ls_gauge_start(void);
bool      ls_gauge_present(void);

/* One read of everything. Cheap enough to call once a second; do NOT call
   it per frame - it is seven I2C transactions on a bus the touch controller
   also wants. */
esp_err_t ls_gauge_read(ls_gauge_t *out);

/* Cached, refreshed at most once a second by the reader itself. This is what
   a drawing path should call. Returns false when there is no gauge. */
bool ls_gauge_get(ls_gauge_t *out);

void ls_gauge_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_GAUGE_H */
