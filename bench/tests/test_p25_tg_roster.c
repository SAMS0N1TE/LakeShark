/* LS_TEST_SOURCES: ${APP}/p25/p25_tg_roster.c ${APP}/p25/scan_ctrl.c
 *                  ${APP}/p25/grant_follower.c ${APP}/p25/p25_controls.c
 *                  ${APP}/p25/p25_ess.c */
/* pure selection/editing coverage for the TALK GROUPS view.  These
 * cases drive the same scan_ctrl object the grant path consults; there is no
 * GUI-owned shadow policy to pass a visual smoke build while doing nothing. */
#include "ls_test.h"

#include "p25_tg_roster.h"
#include "grant_follower.h"

#include <string.h>

static p25_profile_t profile3(void)
{
    p25_profile_t p;
    memset(&p, 0, sizeof(p));
    p.talkgroup_count = 3;
    p.talkgroups[0] = (p25_profile_talkgroup_t){1201, "Operations", true, 7};
    p.talkgroups[1] = (p25_profile_talkgroup_t){1202, "Facilities", false, 0};
    p.talkgroups[2] = (p25_profile_talkgroup_t){1203, "Training", true, 1};
    return p;
}

LS_CASE(selection_is_explicit_bounded_and_survives_a_roster_reorder)
{
    p25_profile_t p = profile3();
    p25_tg_roster_t roster;
    p25_tg_roster_init(&roster);

    LS_CHECK(p25_tg_roster_bind(&roster, &p));
    LS_EQ_UINT(p25_tg_roster_selected_id(&roster, &p), 1201);
    LS_CHECK(p25_tg_roster_select(&roster, &p, 2));
    LS_EQ_UINT(p25_tg_roster_selected_id(&roster, &p), 1203);

    /* A bad touch row preserves the explicit selection. */
    LS_CHECK(!p25_tg_roster_select(&roster, &p, p.talkgroup_count));
    LS_EQ_UINT(p25_tg_roster_selected_id(&roster, &p), 1203);

    p25_profile_talkgroup_t swap = p.talkgroups[0];
    p.talkgroups[0] = p.talkgroups[2];
    p.talkgroups[2] = swap;
    LS_CHECK(p25_tg_roster_bind(&roster, &p));
    LS_EQ_UINT(p25_tg_roster_selected_index(&roster, &p), 0);

    p.talkgroup_count = 1;
    p.talkgroups[0] = swap;
    LS_CHECK(p25_tg_roster_bind(&roster, &p));
    LS_EQ_UINT(roster.selected_tg, 1201);
}

LS_CASE(rows_report_profile_defaults_and_the_effective_scan_policy)
{
    p25_profile_t p = profile3();
    p25_scan_ctrl_t scan;
    p25_scan_init(&scan);
    LS_CHECK(p25_scan_allow_add(&scan, 1202));
    LS_CHECK(p25_scan_lockout_add(&scan, 1202));
    p25_scan_hold_set(&scan, 1202);
    LS_CHECK(p25_scan_priority_set(&scan, 1202, 9));

    scan.names_count = 1;
    scan.names[0].number = 1202;
    strcpy(scan.names[0].name, "Plant Ops");

    p25_tg_roster_row_t row;
    LS_CHECK(p25_tg_roster_row(&p, &scan, 1, &row));
    LS_EQ_UINT(row.id, 1202);
    LS_EQ_STR(row.alias, "Plant Ops");
    LS_CHECK(!row.profile_enabled);
    LS_CHECK(row.list_member);
    LS_CHECK(row.held);
    LS_CHECK(row.locked_out);
    LS_EQ_UINT(row.priority, 9);
    LS_CHECK(!p25_tg_roster_row(&p, &scan, 3, &row));
}

LS_CASE(editing_changes_only_the_explicitly_selected_talkgroup)
{
    p25_profile_t p = profile3();
    p25_scan_ctrl_t scan;
    p25_tg_roster_t roster;
    p25_scan_init(&scan);
    p25_tg_roster_init(&roster);
    LS_CHECK(p25_tg_roster_bind(&roster, &p));
    LS_CHECK(p25_tg_roster_select(&roster, &p, 1));

    LS_EQ_INT(p25_tg_roster_toggle_allow(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_hold(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_lockout(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_priority_delta(&roster, &p, &scan, +1),
              P25_TG_EDIT_OK);

    LS_CHECK(p25_scan_allow_contains(&scan, 1202));
    LS_CHECK(!p25_scan_allow_contains(&scan, 1201));
    LS_EQ_UINT(p25_scan_hold_get(&scan), 1202);
    LS_CHECK(p25_scan_is_locked_out(&scan, 1202));
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1202), 1);

    LS_EQ_INT(p25_tg_roster_toggle_allow(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_hold(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_lockout(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_priority_delta(&roster, &p, &scan, -1),
              P25_TG_EDIT_OK);
    LS_CHECK(!p25_scan_allow_contains(&scan, 1202));
    LS_EQ_UINT(p25_scan_hold_get(&scan), 0);
    LS_CHECK(!p25_scan_is_locked_out(&scan, 1202));
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 1202), 0);
}

LS_CASE(full_policy_tables_refuse_the_edit_without_evicting_an_entry)
{
    p25_profile_t p = profile3();
    p25_tg_roster_t roster;
    p25_tg_roster_init(&roster);
    LS_CHECK(p25_tg_roster_bind(&roster, &p));

    p25_scan_ctrl_t scan;
    p25_scan_init(&scan);
    for (size_t i = 0; i < P25_SCAN_ALLOW_MAX; ++i)
        LS_CHECK(p25_scan_allow_add(&scan, (uint16_t)(2000 + i)));
    LS_EQ_INT(p25_tg_roster_toggle_allow(&roster, &p, &scan),
              P25_TG_EDIT_ALLOW_FULL);
    LS_EQ_UINT(p25_scan_allow_count(&scan), P25_SCAN_ALLOW_MAX);
    LS_CHECK(p25_scan_allow_contains(&scan, 2000));

    for (size_t i = 0; i < P25_SCAN_LOCKOUT_MAX; ++i)
        LS_CHECK(p25_scan_lockout_add(&scan, (uint16_t)(3000 + i)));
    LS_EQ_INT(p25_tg_roster_toggle_lockout(&roster, &p, &scan),
              P25_TG_EDIT_LOCKOUT_FULL);
    LS_EQ_UINT(p25_scan_lockout_count(&scan), P25_SCAN_LOCKOUT_MAX);
    LS_CHECK(p25_scan_is_locked_out(&scan, 3000));

    for (size_t i = 0; i < P25_SCAN_PRIORITY_MAX; ++i)
        LS_CHECK(p25_scan_priority_set(&scan, (uint16_t)(4000 + i), 1));
    LS_EQ_INT(p25_tg_roster_priority_delta(&roster, &p, &scan, +1),
              P25_TG_EDIT_PRIORITY_FULL);
    LS_EQ_UINT(p25_scan_priority_count(&scan), P25_SCAN_PRIORITY_MAX);
    LS_EQ_UINT(p25_scan_priority_rank(&scan, 4000), 1);

    char summary[160];
    p25_tg_roster_format_summary(&p, &scan, summary, sizeof(summary));
    LS_CHECK(strstr(summary, "LIST 64/64 FULL") != NULL);
    LS_CHECK(strstr(summary, "LOCK 64/64 FULL") != NULL);
    LS_CHECK(strstr(summary, "PRI 16/16 FULL") != NULL);
}

LS_CASE(open_monitor_allow_mode_and_empty_states_are_unambiguous)
{
    p25_profile_t p = profile3();
    p25_scan_ctrl_t scan;
    p25_scan_init(&scan);
    char summary[160];

    p25_tg_roster_format_summary(NULL, &scan, summary, sizeof(summary));
    LS_EQ_STR(summary, "NO PROFILE LOADED");
    p.talkgroup_count = 0;
    p25_tg_roster_format_summary(&p, &scan, summary, sizeof(summary));
    LS_EQ_STR(summary, "PROFILE HAS NO TALK GROUPS");

    p = profile3();
    p25_tg_roster_format_summary(&p, &scan, summary, sizeof(summary));
    LS_CHECK(strncmp(summary, "OPEN MONITOR", 12) == 0);
    LS_EQ_INT(p25_tg_roster_set_mode(&scan, P25_SCAN_LIST_ALLOW),
              P25_TG_EDIT_OK);
    p25_tg_roster_format_summary(&p, &scan, summary, sizeof(summary));
    LS_CHECK(strncmp(summary, "ALLOW LIST", 10) == 0);
}

LS_CASE(persistent_edits_restore_and_runtime_priority_does_not_pretend_to)
{
    p25_profile_t p = profile3();
    p25_tg_roster_t roster;
    p25_scan_ctrl_t scan;
    p25_tg_roster_init(&roster);
    p25_scan_init(&scan);
    LS_CHECK(p25_tg_roster_bind(&roster, &p));
    LS_CHECK(p25_tg_roster_select(&roster, &p, 2));
    LS_EQ_INT(p25_tg_roster_set_mode(&scan, P25_SCAN_LIST_ALLOW),
              P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_allow(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_hold(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_lockout(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_priority_delta(&roster, &p, &scan, +1),
              P25_TG_EDIT_OK);

    uint8_t blob[512];
    size_t n = p25_scan_persist_save(&scan, blob, sizeof(blob));
    LS_CHECK(n > 0);
    p25_scan_ctrl_t restored;
    p25_scan_init(&restored);
    LS_CHECK(p25_scan_persist_load(&restored, blob, n));
    LS_EQ_INT(p25_scan_list_get_mode(&restored), P25_SCAN_LIST_ALLOW);
    LS_CHECK(p25_scan_allow_contains(&restored, 1203));
    LS_EQ_UINT(p25_scan_hold_get(&restored), 1203);
    LS_CHECK(p25_scan_is_locked_out(&restored, 1203));
    LS_EQ_UINT(p25_scan_priority_rank(&restored, 1203), 0);
}

LS_CASE(conflicting_edits_still_use_scan_ctrl_precedence)
{
    p25_profile_t p = profile3();
    p25_tg_roster_t roster;
    p25_scan_ctrl_t scan;
    p25_grant_follower_t follower;
    p25_tg_roster_init(&roster);
    p25_scan_init(&scan);
    p25_grant_init(&follower, 851012500, NULL, NULL);
    LS_CHECK(p25_tg_roster_bind(&roster, &p));

    LS_EQ_INT(p25_tg_roster_set_mode(&scan, P25_SCAN_LIST_ALLOW),
              P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_allow(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_hold(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_toggle_lockout(&roster, &p, &scan), P25_TG_EDIT_OK);
    LS_EQ_INT(p25_tg_roster_priority_delta(&roster, &p, &scan, +1),
              P25_TG_EDIT_OK);

    p25_scan_decision_t d = p25_scan_decide(&scan, &follower, 1201, 42,
                                             852000000, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_LOCKED_OUT);
}
