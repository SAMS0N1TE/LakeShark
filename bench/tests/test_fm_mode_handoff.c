/* LS_TEST_SOURCES: ${APP}/fm/fm_mode_handoff.c */
#include "ls_test.h"

#include "fm_mode_handoff.h"

LS_CASE(pending_command_wins_real_entry_and_is_consumed_once)
{
    fm_mode_handoff_t handoff = FM_MODE_HANDOFF_INITIALIZER;
    fm_mode_handoff_request(&handoff, FM_MODE_ACARS);

    fm_mode_t effective = fm_mode_handoff_resolve_entry(
        &handoff, FM_MODE_POCSAG, FM_MODE_POCSAG);

    /* This is the receiver's effective startup mode, not command reply text. */
    LS_EQ_INT(FM_MODE_ACARS, effective);
    LS_CHECK(!fm_mode_handoff_take(&handoff, &effective));
}

LS_CASE(removed_persisted_mode_falls_back_and_cannot_replace_pending_request)
{
    fm_mode_handoff_t handoff = FM_MODE_HANDOFF_INITIALIZER;
    LS_EQ_INT(FM_MODE_POCSAG, fm_mode_handoff_resolve_entry(
        &handoff, 6, FM_MODE_POCSAG));
    fm_mode_handoff_request(&handoff, FM_MODE_WFM);
    fm_mode_handoff_request(&handoff, (fm_mode_t)6);
    LS_EQ_INT(FM_MODE_WFM, fm_mode_handoff_resolve_entry(
        &handoff, 6, FM_MODE_POCSAG));
    fm_mode_t effective = FM_MODE_LISTEN;
    LS_CHECK(!fm_mode_handoff_take(&handoff, &effective));
    LS_EQ_INT(effective, FM_MODE_LISTEN);
}

LS_CASE(manual_entry_retains_saved_mode_then_uses_default_for_invalid_state)
{
    fm_mode_handoff_t handoff = FM_MODE_HANDOFF_INITIALIZER;

    LS_EQ_INT(FM_MODE_FLEX, fm_mode_handoff_resolve_entry(
        &handoff, FM_MODE_FLEX, FM_MODE_POCSAG));
    LS_EQ_INT(FM_MODE_POCSAG, fm_mode_handoff_resolve_entry(
        &handoff, FM_MODE_COUNT, FM_MODE_POCSAG));
}

LS_CASE(single_slot_is_latest_wins_and_also_serves_live_receiver)
{
    fm_mode_handoff_t handoff = FM_MODE_HANDOFF_INITIALIZER;
    fm_mode_t effective = FM_MODE_LISTEN;

    fm_mode_handoff_request(&handoff, FM_MODE_WFM);
    fm_mode_handoff_request(&handoff, FM_MODE_ACARS);

    LS_CHECK(fm_mode_handoff_take(&handoff, &effective));
    LS_EQ_INT(FM_MODE_ACARS, effective);
    LS_CHECK(!fm_mode_handoff_take(&handoff, &effective));
}
