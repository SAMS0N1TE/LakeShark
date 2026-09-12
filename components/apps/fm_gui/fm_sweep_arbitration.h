#ifndef FM_SWEEP_ARBITRATION_H
#define FM_SWEEP_ARBITRATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**/
/* The FM app has TWO owners of the tuner in a sweep context: the band-power SWEEP mode (FM_MODE_SCAN) and scan_engine's stored/band channel scanner. */

typedef struct {
    /* True when the stored/band channel scanner is running. */
    bool (*scanner_active)(void *ctx);
    /* Ask scan_engine to stop. */
    void (*scanner_stop)(void *ctx);
    /* Put the FM app into its band-power SWEEP submode (FM_MODE_SCAN). */
    void (*enter_sweep_mode)(void *ctx);
    /* Reset the sweep bins and start a fresh sweep across the current band. */
    void (*restart_sweep)(void *ctx);
    void *ctx;
} fm_sweep_hooks_t;

/* Install hooks. Pass NULL to clear them (used by the bench). */
void fm_sweep_configure(const fm_sweep_hooks_t *hooks);

/* The one entry point every UI path that starts a band sweep must use.
   Order is fixed and single-sourced:
     1. stop the channel scanner if it is running (otherwise it drags the app
        back to FM_MODE_LISTEN on its next pass and the sweep looks dead);
     2. set FM_MODE_SCAN so the sweep loop and its UI wake up;
     3. restart the sweep so bins are reset and the trace redraws.
   Returns nothing - a hook that is not installed makes its step a no-op, which
   is what the bench relies on. */
void fm_sweep_start_arbitrated(void);

#ifdef __cplusplus
}
#endif

#endif
