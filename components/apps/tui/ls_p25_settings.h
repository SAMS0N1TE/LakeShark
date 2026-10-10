#ifndef LS_P25_SETTINGS_H
#define LS_P25_SETTINGS_H
#include "lakeshark_backend.h"
#include "p25_demod_control.h"
#include "p25_p2_runtime.h"
#include "ls_numpad.h"
#include "p25_program.h"
static bool ps_lr_receiver(void)
{
    return P25.lora_rx || ls_rsel_effective(LS_RSEL_P25) == LS_RSEL_LORA;
}
static int ps_lr_gain(void)
{
    ls_val_t v;
    return ls_value_read("p25.lrgain", &v, NULL) ? (int)v.i : 0;
}
static int ps_volume(void)
{
    ls_val_t v;
    return ls_value_read("sys.volume", &v, NULL) ? (int)v.i : 0;
}
static bool ps_set_action(const char *name, double value, bool integer)
{
    ls_args_t args = {.n = 1};
    ls_val_t result;
    args.v[0].kind = integer ? LS_VAL_INT : LS_VAL_FLOAT;
    if (integer)
        args.v[0].i = (int)value;
    else
        args.v[0].f = (float)value;
    return ls_action_call(name, &args, &result, ls_quick_grant_builtin()) == LS_ACT_OK;
}

static int ps_selected;
static char ps_status[64];
static const char *const ps_names[] = {"Demodulation",
                                       "Polarity",
                                       "Automatic gain",
                                       "Receiver gain (dB)",
                                       "Volume",
                                       "Voice gate",
                                       "Sync beep",
                                       "Trunking auto-follow",
                                       "Leave encrypted calls",
                                       "Encrypted skip (seconds)",
                                       "CQPSK timing gain",
                                       "CQPSK carrier gain",
                                       "Reset CQPSK tuning",
                                       "Scan threshold (%)",
                                       "Scan hang (ms)",
                                       "Phase II (experimental)",
                                       "Profile from card",
                                       "Phase II follow (exp)",
                                       "Hold current talkgroup",
                                       "Lockout current talkgroup",
                                       "Remove lockout TG",
                                       "Allow list mode",
                                       "Add allowed TG",
                                       "Remove allowed TG",
                                       "Control survey",
                                       "Lockout list",
                                       "Allowed list"};
static void ps_value(int i, char *out, size_t n)
{
    p25_cqpsk_config_t cfg;
    bool pending;
    switch (i) {
    case 0:
        snprintf(out, n, "%s", lakeshark_p25_mode_name());
        break;
    case 1:
        snprintf(out, n, "%s", lakeshark_p25_polarity_inverted() ? "INVERTED" : "NORMAL");
        break;
    case 2:
        snprintf(out, n, "%s", (ps_lr_receiver() ? ps_lr_gain() == 0 : lakeshark_p25_agc_enabled()) ? "ON" : "OFF");
        break;
    case 3:
        if (ps_lr_receiver()) snprintf(out, n, "%s", ps_lr_gain() ? "MAX" : "AUTO");
        else snprintf(out, n, "%.1f", lakeshark_p25_gain_tenths() / 10.0);
        break;
    case 4:
        snprintf(out, n, "%d", ps_volume());
        break;
    case 5:
        snprintf(out, n, "%d", lakeshark_p25_voice_gate());
        break;
    case 6:
        snprintf(out, n, "%s", lakeshark_p25_beep_enabled() ? "ON" : "OFF");
        break;
    case 7:
        snprintf(out, n, "%s", p25_get_auto_follow() ? "ON" : "OFF");
        break;
    case 8:
        snprintf(out, n, "%s", p25_get_leave_on_encrypted() ? "ON" : "OFF");
        break;
    case 9:
        snprintf(out, n, "%u", p25_get_encrypted_skip_ms() / 1000);
        break;
    case 10:
    case 11:
        p25_get_cqpsk_config(&cfg, &pending);
        snprintf(out, n, i == 10 ? "%.8f%s" : "%.4f%s",
                 i == 10 ? cfg.timing_gain : cfg.carrier_gain, pending ? " QUEUED" : "");
        break;
    case 12:
        snprintf(out, n, "Restore tested values");
        break;
    case 13:
        snprintf(out, n, "%d", scan_engine_get_threshold_pct());
        break;
    case 14:
        snprintf(out, n, "%d", scan_engine_get_hang_ms());
        break;
    case 15:
        snprintf(out,n,"%s",ps_lr_receiver()?"IQ receiver required":p25_p2_enabled()?"ON / MANUAL VOICE":"OFF");
        break;
    case 17:
        snprintf(out, n, "%s", ps_lr_receiver() ? "IQ receiver required" : p25_get_phase2_follow() ? "ON" : "OFF");
        break;
    case 18: snprintf(out, n, "TG %u%s", P25.hold_tg, P25.hold_tg ? " / HELD" : " / OFF"); break;
    case 19: snprintf(out, n, "TG %u", P25.grant_on_traffic ? P25.grant_talkgroup : (unsigned)P25.dsd_tg); break;
    case 20: case 22: case 23: snprintf(out, n, "ENTER talkgroup number"); break;
    case 21: snprintf(out, n, "%s", P25.list_mode == 1 ? "ALLOW ONLY" : P25.list_mode == 2 ? "FOLLOW NONE" : "ALL"); break;
    case 24: snprintf(out, n, "%s", p25_program_survey_active_now() ? "RUNNING / ENTER cancels" : "ENTER starts"); break;
    case 25: case 26: snprintf(out, n, "%u entries / ENTER to view", i == 25 ? P25.lockout_count : P25.allow_count); break;
    case 16: {
        /* The system name is what the operator recognises; the path is what
           they chose. Neither is useful before a profile has ever loaded, so
           say that instead of showing an empty field. */
        const p25_program_t *ps = p25_program_session();
        if (ps && ps->active_valid && ps->active.system_name[0])
            snprintf(out, n, "%s", ps->active.system_name);
        else if (ps && ps->active_valid)
            snprintf(out, n, "loaded");
        else
            snprintf(out, n, "none loaded - ENTER to choose");
        break;
    }
    }
}
static void ps_number(double v)
{
    bool ok = true;
    switch (ps_selected) {
    case 3:
        if (ps_lr_receiver()) { ok = ps_set_action("p25.lrgain", v > 0 ? 13 : 0, true); break; }
        if (!(v >= 0 && v <= 50))
            ok = false;
        else
            ok = ps_set_action("p25.gain", v, false);
        break;
    case 4:
        if (!(v >= 0 && v <= 100))
            ok = false;
        else
            ok = ps_set_action("audio.volume", v, true);
        break;
    case 5:
        if (!(v >= 0 && v <= 99))
            ok = false;
        else
            lakeshark_p25_set_voice_gate((int)v);
        break;
    case 9:
        if (!(v >= 1 && v <= 3600))
            ok = false;
        else
            ok = p25_set_encrypted_skip_ms((unsigned)(v * 1000));
        break;
    case 20: case 22: case 23:
        if (!(v >= 1 && v <= 65535) || v != (unsigned)v) ok = false;
        else if (ps_selected == 20) ok = p25_ui_lockout_remove((uint16_t)v);
        else ok = p25_ui_policy(ps_selected == 22 ? P25_UI_ALLOW_ADD : P25_UI_ALLOW_REMOVE, (uint16_t)v);
        break;
    case 13:
        if (!(v >= 1 && v <= 100))
            ok = false;
        else
            scan_engine_set_threshold_pct((int)v);
        break;
    case 14:
        if (!(v >= 0 && v <= 30000))
            ok = false;
        else
            scan_engine_set_hang_ms((int)v);
        break;
    }
    snprintf(ps_status, sizeof(ps_status), "%s",
             ok ? "Change requested" : "Invalid value or save failed");
}
static bool ps_list_allow;
static unsigned ps_list_first;
static void ps_list_page(void);
static void ps_list_done(int index)
{
    if (index == LS_PICKER_MAX - 1) {
        ps_list_first = ps_list_first ? 0 : LS_PICKER_MAX - 1;
        ps_list_page();
    }
}
static void ps_list_page(void)
{
    ls_picker_open(ps_list_allow ? "P25 ALLOWED TALKGROUPS" : "P25 LOCKED OUT TALKGROUPS", ps_list_done);
    const uint16_t *list = ps_list_allow ? P25.allow : P25.lockout;
    unsigned count = ps_list_allow ? P25.allow_count : P25.lockout_count;
    unsigned last = ps_list_first + LS_PICKER_MAX - 1;
    for (unsigned i = ps_list_first; i < count && i < last && i < 64; i++) {
        char label[32]; snprintf(label, sizeof(label), "TG %u", list[i]);
        ls_picker_add(label, "Use Remove TG to edit");
    }
    if (!ps_list_first && count > last) ls_picker_add("MORE", "Remaining talkgroups");
    ls_picker_empty_reason("No talkgroups in this list");
}
static void ps_list(bool allow)
{
    ps_list_allow = allow; ps_list_first = 0; ps_list_page();
}
static void ps_change(int direction, bool edit)
{
    char value[48];
    ps_value(ps_selected, value, sizeof(value));
    if (ps_selected == 3 && ps_lr_receiver()) {
        bool max = edit ? ps_lr_gain() == 0 : direction > 0;
        (void)ps_set_action("p25.lrgain", max ? 13 : 0, true);
        return;
    }
    if (ps_selected == 20 || ps_selected == 22 || ps_selected == 23) {
        if (edit) ls_numpad_open(ps_names[ps_selected], "TG 1..65535", 1, ps_number);
        return;
    }
    if (ps_selected == 3 || ps_selected == 4 || ps_selected == 5 || ps_selected == 9 ||
        (ps_selected >= 13 && ps_selected <= 14)) {
        double v = atof(value);
        if (edit)
            ls_numpad_open(ps_names[ps_selected], "", v, ps_number);
        else
            ps_number(v + direction * (ps_selected == 14                      ? 500
                                       : ps_selected == 4 || ps_selected == 9 ? 5
                                                                              : 1));
        return;
    }
    bool ok = true;
    switch (ps_selected) {
    case 0: {
        p25_p2_enable(false);
        int pref = p25_demod_get_preference();
        int next = direction > 0 ? (pref >= DEMOD_FSK4_TRACKING ? P25_DEMOD_AUTO : pref + 1)
                                 : (pref <= P25_DEMOD_AUTO ? DEMOD_FSK4_TRACKING : pref - 1);
        p25_demod_set_preference(next);
        break;
    }
    case 1:
        if(edit || lakeshark_p25_polarity_inverted()!=(direction>0)) lakeshark_p25_toggle_polarity();
        break;
    case 2:
        if (ps_lr_receiver()) {
            bool agc = edit ? ps_lr_gain() != 0 : direction > 0;
            ok = ps_set_action("p25.lrgain", agc ? 0 : 13, true);
        } else if(edit || lakeshark_p25_agc_enabled()!=(direction>0)) lakeshark_p25_agc();
        break;
    case 6:
        if(edit || lakeshark_p25_beep_enabled()!=(direction>0)) lakeshark_p25_beep_toggle();
        break;
    case 7:
        ok = p25_set_auto_follow(edit?!p25_get_auto_follow():direction>0);
        break;
    case 8:
        ok = p25_set_leave_on_encrypted(edit?!p25_get_leave_on_encrypted():direction>0);
        break;
    case 10:
        ok = p25_step_cqpsk_timing(direction);
        break;
    case 11:
        ok = p25_step_cqpsk_carrier(direction);
        break;
    case 12:
        if (edit)
            ok = p25_reset_cqpsk_config();
        else
            return;
        break;
    case 16:
        /* ENTER only, like Reset CQPSK tuning: a list you step through with
           LEFT/RIGHT should not open a modal in passing. */
        if (!edit) return;
        {
            ls_args_t a = {.n = 0};
            ls_val_t r;
            ok = ls_action_call("p25.profile", &a, &r,
                                ls_quick_grant_builtin()) == LS_ACT_OK;
        }
        if (!ok) snprintf(ps_status, sizeof(ps_status), "%s",
                          "No profile chooser on this build");
        return;
    case 18: if (edit) ok = p25_ui_hold_toggle(); break;
    case 19: if (edit) ok = p25_ui_lockout_current(); break;
    case 21: ok = p25_ui_policy(P25_UI_LIST_MODE, (uint16_t)((P25.list_mode + (direction > 0 ? 1 : 2)) % 3)); break;
    case 24:
        if (edit) ok = p25_program_survey_active_now()
            ? p25_program_survey_cancel_now(P25_SURVEY_CANCEL_MANUAL_TUNE)
            : p25_program_survey_start_now();
        break;
    case 25: case 26: if (edit) ps_list(ps_selected == 26); return;
    case 17:
        if (ps_lr_receiver()) { snprintf(ps_status, sizeof(ps_status), "Phase II needs an IQ receiver"); return; }
        p25_set_phase2_follow(edit ? !p25_get_phase2_follow() : direction > 0);
        snprintf(ps_status, sizeof(ps_status), "%s",
                 p25_get_phase2_follow()
                     ? "EXP: TDMA grants followed; needs WACN/SYS"
                     : "TDMA grants observed only");
        return;
    case 15:
        if (ps_lr_receiver()) { snprintf(ps_status, sizeof(ps_status), "Phase II needs an IQ receiver"); return; }
        scan_engine_stop();
        p25_p2_enable(edit?!p25_p2_enabled():direction>0);
        snprintf(ps_status,sizeof(ps_status),"%s",p25_p2_enabled()?"EXP: manual voice; IDs from control or p2 config":"Phase I restored");
        return;
    }
    snprintf(ps_status, sizeof(ps_status), "%s",
             ok ? "Change requested" : "Could not save setting");
}
#endif
