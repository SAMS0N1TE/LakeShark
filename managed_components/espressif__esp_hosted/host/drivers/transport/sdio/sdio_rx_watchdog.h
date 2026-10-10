#ifndef SDIO_RX_WATCHDOG_H
#define SDIO_RX_WATCHDOG_H
#include <stdbool.h>
#include <stdint.h>

/* Read-task-owned state. Progress counts completed double-buffer transfers;
 * a busy link that keeps making progress must never be called stalled. */
typedef struct {
    uint32_t progress, dropped;
    int64_t since_us, last_log_us;
    bool active, reported;
} sdio_rx_watchdog_t;
enum { SDIO_RX_QUIET, SDIO_RX_BUSY, SDIO_RX_STALLED };
static inline int sdio_rx_watchdog_poll(sdio_rx_watchdog_t *w, bool busy,
                                      uint32_t progress, int64_t now)
{
    if (!busy || progress != w->progress) {
        w->active = w->reported = false;
        w->progress = progress;
        w->dropped = 0;
    }
    if (!busy) return SDIO_RX_QUIET;
    if (w->dropped != UINT32_MAX) ++w->dropped;
    if (!w->active) {
        w->active = true;
        w->since_us = now;
    }
    if (!w->reported && now - w->since_us >= 5000000) {
        w->reported = true;
        w->last_log_us = now;
        return SDIO_RX_STALLED;
    }
    /* Report a persistent stall once; preserve the console until progress. */
    if (!w->reported && now - w->last_log_us >= 1000000) {
        w->last_log_us = now;
        return SDIO_RX_BUSY;
    }
    return SDIO_RX_QUIET;
}
#endif
