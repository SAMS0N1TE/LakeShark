#include "ls_scan_view.h"

#include "scan_engine.h"

ls_scan_act_t ls_scan_view_change(ls_scan_view_t to, ls_scan_state_t st)
{
    (void)to;
    (void)st;
    return LS_SCAN_KEEP;
}

ls_scan_act_t ls_scan_view_operate(ls_scan_op_t op, ls_scan_state_t st)
{
    if (!st.scanning) return LS_SCAN_KEEP;
    switch (op) {
    case LS_SCAN_OP_TUNE:
    case LS_SCAN_OP_MODE:
    case LS_SCAN_OP_SWEEP:
        return LS_SCAN_STOP;
    case LS_SCAN_OP_SCAN:
    default:
        return LS_SCAN_KEEP;
    }
}

void ls_scan_view_apply(ls_scan_act_t act)
{
    if (act == LS_SCAN_STOP) scan_engine_stop();
}

bool ls_scan_view_ends_sweep(ls_scan_view_t to, ls_scan_state_t st)
{
    return to == LS_SCAN_VIEW_VFO && st.sweeping;
}
