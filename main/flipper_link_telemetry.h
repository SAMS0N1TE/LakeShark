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
    int      tts_volume;
    int      rtl_ready;
    uint32_t uptime_s;
    uint32_t free_internal;
    uint32_t free_dma;
    int      sdr_stall_s;
    const char *rhs_state;
    /* The receiver the active mode is using, as the RADIO picker names it
       ("RTL-SDR", "HackRF", "LR2021", "SX1262"), and why it cannot be used
       now when it cannot. NULL name: the board does not know, no field. */
    const char *rx_name;
    const char *rx_why;
} ls_telemetry_common_t;

/* Longest receiver name and reason sent, in characters. */
#define LS_TEL_RX_NAME_MAX 18
#define LS_TEL_RX_WHY_MAX  47

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

/* The md= value of a built line, replaced in place: "PARKED" while the TUI
   has the receiver parked, so a head shows no mode as live. A head reads an
   md it does not know as no mode. Returns the new length, or -1 when the
   line has no md= or the result would not fit. */
int ls_telemetry_set_mode(char *buf, size_t len, const char *md);

#ifdef __cplusplus
}
#endif

#endif
