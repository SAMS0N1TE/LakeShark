/* bounded talkgroup roster selection and policy editing. */

#ifndef P25_TG_ROSTER_H
#define P25_TG_ROSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "p25_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t selected_tg; /* zero when the active profile has no TG rows */
} p25_tg_roster_t;

typedef struct {
    uint16_t id;
    char     alias[P25_PROFILE_ALIAS_LEN];
    bool     profile_enabled;
    bool     list_member;
    bool     held;
    bool     locked_out;
    uint8_t  priority;
} p25_tg_roster_row_t;

typedef enum {
    P25_TG_EDIT_OK = 0,
    P25_TG_EDIT_NO_PROFILE,
    P25_TG_EDIT_EMPTY,
    P25_TG_EDIT_NO_SELECTION,
    P25_TG_EDIT_OUT_OF_RANGE,
    P25_TG_EDIT_ALLOW_FULL,
    P25_TG_EDIT_LOCKOUT_FULL,
    P25_TG_EDIT_PRIORITY_FULL,
    P25_TG_EDIT_PRIORITY_LIMIT,
    P25_TG_EDIT_ARGUMENT,
} p25_tg_edit_result_t;

void p25_tg_roster_init(p25_tg_roster_t *roster);

/* Reconcile selection with an active profile.  The selected TG ID survives a
 * refresh/reorder; a profile that no longer contains it selects its first row.
 * Returns false for the two explicit empty states: no profile and no rows. */
bool p25_tg_roster_bind(p25_tg_roster_t *roster,
                        const p25_profile_t *profile);
bool p25_tg_roster_select(p25_tg_roster_t *roster,
                          const p25_profile_t *profile, size_t index);
size_t p25_tg_roster_selected_index(const p25_tg_roster_t *roster,
                                    const p25_profile_t *profile);
uint16_t p25_tg_roster_selected_id(const p25_tg_roster_t *roster,
                                   const p25_profile_t *profile);

/* scan_ctrl names override the profile alias when present.  This keeps the
 * existing editable names file useful without copying names into another
 * table. */
bool p25_tg_roster_row(const p25_profile_t *profile,
                       const p25_scan_ctrl_t *scan, size_t index,
                       p25_tg_roster_row_t *out);

p25_tg_edit_result_t p25_tg_roster_toggle_allow(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan);
p25_tg_edit_result_t p25_tg_roster_toggle_hold(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan);
p25_tg_edit_result_t p25_tg_roster_toggle_lockout(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan);
p25_tg_edit_result_t p25_tg_roster_priority_delta(
    const p25_tg_roster_t *roster, const p25_profile_t *profile,
    p25_scan_ctrl_t *scan, int delta);
p25_tg_edit_result_t p25_tg_roster_set_mode(p25_scan_ctrl_t *scan,
                                             p25_scan_list_mode_t mode);

const char *p25_tg_edit_result_text(p25_tg_edit_result_t result);
void p25_tg_roster_format_summary(const p25_profile_t *profile,
                                  const p25_scan_ctrl_t *scan,
                                  char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* P25_TG_ROSTER_H */
