/* Host fixtures for the P25 settings controls. */
#include <stdbool.h>
#include <stdio.h>
static bool phase2;
bool p25_p2_enabled(void) { return phase2; }
void p25_p2_enable(bool on) { phase2=on; }
void p25_p2_describe(char *text,unsigned capacity) { snprintf(text,capacity,"%s",phase2?"EXP: need WACN/SYS/NAC":"OFF (experimental)"); }
#include "../../components/lakeshark/apps/p25/p25_cqpsk_controls.h"
static int mode, gate = 30;
static bool polarity, agc, beep, follow, encrypted;
static unsigned skip = 30000;
static p25_cqpsk_config_t config = {P25_CQPSK_TIMING_GAIN_DEFAULT, P25_CQPSK_CARRIER_GAIN_DEFAULT};
const char *lakeshark_p25_mode_name(void)
{
    return mode == 0 ? "AUTO" : mode == 1 ? "C4FM" : "CQPSK";
}
const char *lakeshark_p25_cycle_mode(void)
{
    mode = (mode + 1) % 3;
    return lakeshark_p25_mode_name();
}
bool lakeshark_p25_polarity_inverted(void)
{
    return polarity;
}
void lakeshark_p25_toggle_polarity(void)
{
    polarity = !polarity;
}
bool lakeshark_p25_agc_enabled(void)
{
    return agc;
}
void lakeshark_p25_agc(void)
{
    agc = !agc;
}
int lakeshark_p25_gain_tenths(void)
{
    return 280;
}
int lakeshark_p25_voice_gate(void)
{
    return gate;
}
void lakeshark_p25_set_voice_gate(int v)
{
    gate = v;
}
bool lakeshark_p25_beep_enabled(void)
{
    return beep;
}
void lakeshark_p25_beep_toggle(void)
{
    beep = !beep;
}
bool p25_get_auto_follow(void)
{
    return follow;
}
bool p25_set_auto_follow(bool v)
{
    follow = v;
    return true;
}
bool p25_get_leave_on_encrypted(void)
{
    return encrypted;
}
bool p25_set_leave_on_encrypted(bool v)
{
    encrypted = v;
    return true;
}
static bool phase2_follow;
bool p25_get_phase2_follow(void)
{
    return phase2_follow;
}
void p25_set_phase2_follow(bool v)
{
    phase2_follow = v;
}
unsigned p25_get_encrypted_skip_ms(void)
{
    return skip;
}
bool p25_set_encrypted_skip_ms(unsigned v)
{
    skip = v;
    return true;
}
void p25_get_cqpsk_config(p25_cqpsk_config_t *out, bool *pending)
{
    *out = config;
    *pending = false;
}
bool p25_reset_cqpsk_config(void)
{
    config.timing_gain = P25_CQPSK_TIMING_GAIN_DEFAULT;
    config.carrier_gain = P25_CQPSK_CARRIER_GAIN_DEFAULT;
    return true;
}
bool p25_step_cqpsk_timing(int d)
{
    config.timing_gain *= d > 0 ? 2 : 0.5f;
    return true;
}
bool p25_step_cqpsk_carrier(int d)
{
    config.carrier_gain *= d > 0 ? 2 : 0.5f;
    return true;
}

int p25_demod_get_preference(void)
{
    return mode;
}
void p25_demod_set_preference(int v)
{
    mode = v;
}

#include "p25_state.h"
p25_state_t P25 __attribute__((weak));
#include "p25_program.h"
static bool survey;
bool p25_program_survey_active_now(void) { return survey; }
bool p25_program_survey_start_now(void) { survey = true; return true; }
bool p25_program_survey_cancel_now(p25_survey_cancel_t reason)
{ (void)reason; survey = false; return true; }
bool p25_ui_hold_toggle(void)
{
    P25.hold_tg = P25.hold_tg ? 0 : (uint16_t)P25.dsd_tg;
    return true;
}
bool p25_ui_lockout_current(void)
{
    if (!P25.dsd_tg || P25.lockout_count == 64) return false;
    P25.lockout[P25.lockout_count++] = (uint16_t)P25.dsd_tg;
    return true;
}
bool p25_ui_lockout_remove(uint16_t tg)
{
    for (unsigned i = 0; i < P25.lockout_count; i++) {
        if (P25.lockout[i] != tg) continue;
        P25.lockout[i] = P25.lockout[--P25.lockout_count]; return true;
    }
    return false;
}
bool p25_ui_policy(p25_ui_policy_t kind, uint16_t value)
{
    if (kind == P25_UI_LIST_MODE) { P25.list_mode = value; return true; }
    if (kind == P25_UI_HOLD_SET) { P25.hold_tg = value; return true; }
    if (kind == P25_UI_LOCKOUT_ADD) {
        if (!value || P25.lockout_count == 64) return false;
        P25.lockout[P25.lockout_count++] = value; return true;
    }
    if (kind == P25_UI_ALLOW_ADD && P25.allow_count < 64) {
        P25.allow[P25.allow_count++] = value; return true;
    }
    for (unsigned i = 0; i < P25.allow_count; i++) {
        if (P25.allow[i] != value) continue;
        P25.allow[i] = P25.allow[--P25.allow_count]; return true;
    }
    return false;
}

/* Names for the CALLS fixture, as a names file on the card would give them.
   Generic service names; no real system is described. */
const char *p25_calls_host_alias(uint16_t tg)
{
    static const struct { uint16_t tg; const char *name; } NAMES[] = {
        { 1041, "FIRE DISP" }, { 1043, "FIRE TAC 2" }, { 2051, "EMS NORTH" },
        { 2104, "PD CENTRAL" }, { 2110, "PD SOUTH" }, { 3301, "PUBLIC WORKS" },
        { 4005, "SCHOOL BUSES" }, { 5120, "COUNTY INTEROP" },
    };
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (NAMES[i].tg == tg) return NAMES[i].name;
    return NULL;
}
