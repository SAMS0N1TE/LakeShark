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
