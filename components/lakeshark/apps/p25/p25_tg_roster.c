/* see p25_tg_roster.h.  Pure C, allocation-free and IDF-free. */

#include "p25_tg_roster.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const p25_profile_talkgroup_t *selected(
    const p25_tg_roster_t *roster, const p25_profile_t *profile)
{
    if (!roster || !profile || !roster->selected_tg) return NULL;
    for (size_t i = 0; i < profile->talkgroup_count; ++i) {
        if (profile->talkgroups[i].id == roster->selected_tg)
            return &profile->talkgroups[i];
    }
    return NULL;
}

static p25_tg_edit_result_t selected_result(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    const p25_profile_talkgroup_t **out)
{
    if (!roster || !out) return P25_TG_EDIT_ARGUMENT;
    if (!profile) return P25_TG_EDIT_NO_PROFILE;
    if (profile->talkgroup_count == 0) return P25_TG_EDIT_EMPTY;
    *out = selected(roster, profile);
    return *out ? P25_TG_EDIT_OK : P25_TG_EDIT_NO_SELECTION;
}

void p25_tg_roster_init(p25_tg_roster_t *roster)
{
    if (roster) roster->selected_tg = 0;
}

bool p25_tg_roster_bind(p25_tg_roster_t *roster,
                        const p25_profile_t *profile)
{
    if (!roster) return false;
    if (!profile || profile->talkgroup_count == 0) {
        roster->selected_tg = 0;
        return false;
    }
    if (!selected(roster, profile))
        roster->selected_tg = profile->talkgroups[0].id;
    return true;
}

bool p25_tg_roster_select(p25_tg_roster_t *roster,
                          const p25_profile_t *profile, size_t index)
{
    if (!roster || !profile || index >= profile->talkgroup_count) return false;
    roster->selected_tg = profile->talkgroups[index].id;
    return true;
}

size_t p25_tg_roster_selected_index(const p25_tg_roster_t *roster,
                                    const p25_profile_t *profile)
{
    if (!roster || !profile || !roster->selected_tg) return SIZE_MAX;
    for (size_t i = 0; i < profile->talkgroup_count; ++i)
        if (profile->talkgroups[i].id == roster->selected_tg) return i;
    return SIZE_MAX;
}

uint16_t p25_tg_roster_selected_id(const p25_tg_roster_t *roster,
                                   const p25_profile_t *profile)
{
    return selected(roster, profile) ? roster->selected_tg : 0;
}

bool p25_tg_roster_row(const p25_profile_t *profile,
                       const p25_scan_ctrl_t *scan, size_t index,
                       p25_tg_roster_row_t *out)
{
    if (!profile || !scan || !out || index >= profile->talkgroup_count)
        return false;
    const p25_profile_talkgroup_t *tg = &profile->talkgroups[index];
    memset(out, 0, sizeof(*out));
    out->id = tg->id;
    const p25_scan_name_t *name = p25_scan_name_lookup(scan, tg->id);
    const char *alias = (name && name->name[0]) ? name->name : tg->alias;
    size_t alias_len = 0;
    while (alias[alias_len] && alias_len + 1U < sizeof(out->alias)) {
        out->alias[alias_len] = alias[alias_len];
        ++alias_len;
    }
    out->alias[alias_len] = '\0';
    out->profile_enabled = tg->enabled;
    out->list_member = p25_scan_allow_contains(scan, tg->id);
    out->held = p25_scan_hold_get(scan) == tg->id;
    out->locked_out = p25_scan_is_locked_out(scan, tg->id);
    out->priority = p25_scan_priority_rank(scan, tg->id);
    return true;
}

p25_tg_edit_result_t p25_tg_roster_toggle_allow(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan)
{
    const p25_profile_talkgroup_t *tg = NULL;
    p25_tg_edit_result_t r = selected_result(roster, profile, &tg);
    if (r != P25_TG_EDIT_OK) return r;
    if (!scan) return P25_TG_EDIT_ARGUMENT;
    if (p25_scan_allow_contains(scan, tg->id)) {
        return p25_scan_allow_remove(scan, tg->id) ? P25_TG_EDIT_OK
                                                   : P25_TG_EDIT_ARGUMENT;
    }
    return p25_scan_allow_add(scan, tg->id) ? P25_TG_EDIT_OK
                                             : P25_TG_EDIT_ALLOW_FULL;
}

p25_tg_edit_result_t p25_tg_roster_toggle_hold(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan)
{
    const p25_profile_talkgroup_t *tg = NULL;
    p25_tg_edit_result_t r = selected_result(roster, profile, &tg);
    if (r != P25_TG_EDIT_OK) return r;
    if (!scan) return P25_TG_EDIT_ARGUMENT;
    p25_scan_hold_set(scan, p25_scan_hold_get(scan) == tg->id ? 0 : tg->id);
    return P25_TG_EDIT_OK;
}

p25_tg_edit_result_t p25_tg_roster_toggle_lockout(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan)
{
    const p25_profile_talkgroup_t *tg = NULL;
    p25_tg_edit_result_t r = selected_result(roster, profile, &tg);
    if (r != P25_TG_EDIT_OK) return r;
    if (!scan) return P25_TG_EDIT_ARGUMENT;
    if (p25_scan_is_locked_out(scan, tg->id)) {
        return p25_scan_lockout_remove(scan, tg->id) ? P25_TG_EDIT_OK
                                                     : P25_TG_EDIT_ARGUMENT;
    }
    return p25_scan_lockout_add(scan, tg->id) ? P25_TG_EDIT_OK
                                               : P25_TG_EDIT_LOCKOUT_FULL;
}

p25_tg_edit_result_t p25_tg_roster_priority_delta(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan, int delta)
{
    const p25_profile_talkgroup_t *tg = NULL;
    p25_tg_edit_result_t r = selected_result(roster, profile, &tg);
    if (r != P25_TG_EDIT_OK) return r;
    if (!scan || delta == 0) return P25_TG_EDIT_ARGUMENT;
    int rank = (int)p25_scan_priority_rank(scan, tg->id);
    if ((delta < 0 && rank == 0) || (delta > 0 && rank == UCHAR_MAX))
        return P25_TG_EDIT_PRIORITY_LIMIT;
    int next = rank + delta;
    if (next < 0) next = 0;
    if (next > UCHAR_MAX) next = UCHAR_MAX;
    return p25_scan_priority_set(scan, tg->id, (uint8_t)next)
               ? P25_TG_EDIT_OK : P25_TG_EDIT_PRIORITY_FULL;
}

p25_tg_edit_result_t p25_tg_roster_set_mode(p25_scan_ctrl_t *scan,
                                             p25_scan_list_mode_t mode)
{
    if (!scan || (mode != P25_SCAN_LIST_OFF &&
                  mode != P25_SCAN_LIST_ALLOW))
        return P25_TG_EDIT_ARGUMENT;
    p25_scan_list_set_mode(scan, mode);
    return P25_TG_EDIT_OK;
}

const char *p25_tg_edit_result_text(p25_tg_edit_result_t result)
{
    switch (result) {
    case P25_TG_EDIT_OK:             return "UPDATED";
    case P25_TG_EDIT_NO_PROFILE:     return "NO PROFILE LOADED";
    case P25_TG_EDIT_EMPTY:          return "PROFILE HAS NO TALK GROUPS";
    case P25_TG_EDIT_NO_SELECTION:   return "SELECT A TALK GROUP";
    case P25_TG_EDIT_OUT_OF_RANGE:   return "TALK GROUP OUT OF RANGE";
    case P25_TG_EDIT_ALLOW_FULL:     return "ALLOW LIST FULL - NO CHANGE";
    case P25_TG_EDIT_LOCKOUT_FULL:   return "LOCKOUT LIST FULL - NO CHANGE";
    case P25_TG_EDIT_PRIORITY_FULL:  return "PRIORITY LIST FULL - NO CHANGE";
    case P25_TG_EDIT_PRIORITY_LIMIT: return "PRIORITY AT LIMIT - NO CHANGE";
    case P25_TG_EDIT_ARGUMENT:
    default:                         return "EDIT FAILED - NO CHANGE";
    }
}

void p25_tg_roster_format_summary(const p25_profile_t *profile,
                                  const p25_scan_ctrl_t *scan,
                                  char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!profile) {
        snprintf(out, out_size, "NO PROFILE LOADED");
        return;
    }
    if (profile->talkgroup_count == 0) {
        snprintf(out, out_size, "PROFILE HAS NO TALK GROUPS");
        return;
    }
    if (!scan) {
        snprintf(out, out_size, "POLICY UNAVAILABLE");
        return;
    }
    const char *mode = p25_scan_list_get_mode(scan) == P25_SCAN_LIST_ALLOW
                           ? "ALLOW LIST" : "OPEN MONITOR";
    snprintf(out, out_size, "%s  LIST %u/%u%s  LOCK %u/%u%s  PRI %u/%u%s",
             mode,
             (unsigned)p25_scan_allow_count(scan), (unsigned)P25_SCAN_ALLOW_MAX,
             p25_scan_allow_count(scan) == P25_SCAN_ALLOW_MAX ? " FULL" : "",
             (unsigned)p25_scan_lockout_count(scan),
             (unsigned)P25_SCAN_LOCKOUT_MAX,
             p25_scan_lockout_count(scan) == P25_SCAN_LOCKOUT_MAX ? " FULL" : "",
             (unsigned)p25_scan_priority_count(scan),
             (unsigned)P25_SCAN_PRIORITY_MAX,
             p25_scan_priority_count(scan) == P25_SCAN_PRIORITY_MAX ? " FULL" : "");
}
