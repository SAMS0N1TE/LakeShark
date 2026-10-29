/* FM's STATION page in WFM: the RDS station card and the stereo deck. */
#ifndef FM_RDS_VIEW_H
#define FM_RDS_VIEW_H

#include "../../ls_options.h"
#include "../../ls_tui.h"

#ifdef __cplusplus
extern "C" {
#endif

void fm_rds_draw(tui_surface *sf, tui_rect area);
/* The VFO glass's one-line station name and type, "" with no RDS. */
void fm_rds_glass_line(char *out, int n);
/* Stereo pilot locked, for the glass rail. */
bool fm_rds_stereo(void);
bool fm_rds_synced(void);
extern const ls_opt_ctx_t fm_wfm_options;

#ifdef __cplusplus
}
#endif

#endif
