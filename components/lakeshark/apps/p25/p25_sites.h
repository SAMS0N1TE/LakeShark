/* Device GPS site-pack worker and cached, draw-ready status. */
#pragma once
#include "p25_pack.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool enabled;
    char state[16], site[32], system[32], error[80];
    float distance_km;
    unsigned sites, packs;
} p25_sites_status_t;
void p25_sites_init(void);
bool p25_sites_reload(void);
bool p25_sites_set(bool enabled);
bool p25_sites_resume(void);
void p25_sites_status(p25_sites_status_t *out);
/* Owner task, once per GPS poll. Returns -1 or flattened record/control index. */
int p25_sites_poll(bool may_tune);
/* Owner-only profile installation, uses PROGRAM's existing policy/tune seam. */
bool p25_program_site_now(const p25_profile_t *profile, unsigned control);
void p25_program_geo_resume_now(void);
#ifdef __cplusplus
}
#endif
