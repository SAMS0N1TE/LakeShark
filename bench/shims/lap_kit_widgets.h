/* What the stand-in widgets (lap_kit_widgets.c) were last handed. */
#ifndef LAP_KIT_WIDGETS_H
#define LAP_KIT_WIDGETS_H
#include "tuilib/tui_core.h"
#include <stdio.h>
typedef struct ls_opt_ctx_s ls_opt_ctx_t;
typedef struct {
    int calls;
    char what[32];
    tui_rect last;                       /* the absolute rect the widget got */
    int n;
    char label[12][32];
    const char *label_ptr[12];           /* where the strings the widget read live */
    tui_rect hit[12];
    const ls_opt_ctx_t *opt;
    int opt_closes, picker_rows, picker_closes;
    unsigned long hz;
    int wf_pushes, wf_bins;
    float wf_first, wf_last;
} lap_kit_seen_t;
extern lap_kit_seen_t lap_kit_seen;
void lap_kit_pick(int index);            /* the owner picks a row */
void lap_kit_enter(double v);            /* the owner types a number */
#endif
