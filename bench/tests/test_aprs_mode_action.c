#include "ls_test.h"
#include "../../components/apps/tui/ls_action_builtin.c"
static int selected_mode, stops;
static bool mixed;
void lakeshark_fm_set_mode(int mode) { selected_mode = mode; }
void scan_engine_stop(void) { ++stops; }
void scan_engine_set_mixed(bool on) { mixed = on; }
LS_CASE(production_fm_action_selects_receive_modes_and_keeps_id_six_reserved)
{
    ls_args_t in = {0}; ls_val_t out = {0};
    in.n = 1; in.v[0].kind = LS_VAL_TEXT; in.v[0].s = "APRS";
    selected_mode = FM_MODE_LISTEN; stops = 0;
    LS_EQ_INT(LS_ACT_OK, a_fm_submode(&in, &out));
    LS_EQ_INT(FM_MODE_APRS, selected_mode); LS_EQ_STR("aprs", out.s); LS_EQ_INT(1, stops);
    in.v[0].s = "same"; LS_EQ_INT(LS_ACT_OK, a_fm_submode(&in, &out));
    LS_EQ_INT(FM_MODE_SAME, selected_mode);
    in.v[0].s = "ais"; LS_EQ_INT(LS_ACT_OK, a_fm_submode(&in, &out));
    LS_EQ_INT(FM_MODE_AIS, selected_mode); LS_EQ_STR("ais", out.s);
    in.v[0].s = "6"; LS_EQ_INT(LS_ACT_BADARG, a_fm_submode(&in, &out));
    LS_EQ_INT(FM_MODE_AIS, selected_mode);
    mixed = true; in.v[0].s = "listen"; LS_EQ_INT(LS_ACT_OK, a_fm_submode(&in, &out)); LS_CHECK(!mixed);
}
