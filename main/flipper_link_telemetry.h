/*
 * Shared telemetry frame formats for the Flipper link.
 *
 * The format string set is the contract between firmware and the Flipper app.
 * Keep this file as the single place to update all modes when that contract
 * changes.
 */
#ifndef FLIPPER_LINK_TELEMETRY_H
#define FLIPPER_LINK_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#include "lakeshark_backend.h"
#include "rec_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      volume;
    int      muted;
    int      rtl_ready;
    uint32_t uptime_s;
    uint32_t free_internal;
    uint32_t free_dma;
    int      sdr_stall_s;
    const char *rhs_state;
} ls_telemetry_common_t;

int ls_telemetry_build_fm(char *buf, size_t len,
                          const lakeshark_fm_tel_t *t,
                          const ls_telemetry_common_t *common);

int ls_telemetry_build_adsb(char *buf, size_t len,
                            const lakeshark_adsb_tel_t *t,
                            const ls_telemetry_common_t *common);

int ls_telemetry_build_p25(char *buf, size_t len,
                            const lakeshark_p25_tel_t *t,
                            const char *mode_name,
                            const char *demod_name,
                            const ls_telemetry_common_t *common);

int ls_telemetry_build_rec(char *buf, size_t len,
                           const rec_status_t *s,
                           const ls_telemetry_common_t *common);

#ifdef __cplusplus
}
#endif

#endif
