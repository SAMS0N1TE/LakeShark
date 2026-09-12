#ifndef LS_HUB_PRESENT_H
#define LS_HUB_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_HUB_MODE_MAX   8
#define LS_HUB_DETAIL_MAX 40

typedef struct {
    const char *backend_mode;
    bool        receiver_ready;
    bool        parked;

    uint32_t freq_hz;
    int      signal_pct;
    bool     active;
    uint32_t iq_bytes_sec;

    int         p25_nac;
    int         p25_tg;
    bool        p25_has_sync;
    const char *p25_ftype;

    int fm_submode;
    int fm_squelch_tenths;

    int adsb_tracked;
    int adsb_msgs_sec;

    int rec_phase;
    int rec_edges;
    int rec_mag_now;
    int rec_mag_thresh;
} ls_hub_observation_t;

typedef struct {
    char     mode[LS_HUB_MODE_MAX];
    char     target_app[LS_HUB_MODE_MAX];
    uint32_t freq_hz;
    int      signal_pct;
    bool     active;
    uint32_t iq_bytes_sec;
    int      contacts;
    char     detail[LS_HUB_DETAIL_MAX];
} ls_hub_presentation_t;

void ls_hub_present(const ls_hub_observation_t *in,
                    ls_hub_presentation_t *out);

#ifdef __cplusplus
}
#endif

#endif
