#ifndef LS_P25_SETTINGS_H
#define LS_P25_SETTINGS_H
#include "lakeshark_backend.h"
#include "p25_demod_control.h"
#include "p25_p2_runtime.h"
#include "ls_numpad.h"
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

static bool ps_open;
static int ps_selected, ps_first, ps_rows, ps_row_height;
static tui_rect ps_area;
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
                                       "Phase II (experimental)"};
#define PS_COUNT ((int)(sizeof(ps_names) / sizeof(ps_names[0])))
static const ls_btn_t ps_buttons[] = {
    {"UP", NULL, 'U', false, false},     {"DOWN", NULL, 'D', false, false},
    {"LESS", NULL, '[', false, false},   {"MORE", NULL, ']', false, false},
    {"CHANGE", NULL, 'E', false, false}, {"BACK", NULL, 'M', false, false}};
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
        snprintf(out, n, "%s", lakeshark_p25_agc_enabled() ? "ON" : "OFF");
        break;
    case 3:
        snprintf(out, n, "%.1f", lakeshark_p25_gain_tenths() / 10.0);
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
        snprintf(out, n, "CHANGE restores tested values");
        break;
    case 13:
        snprintf(out, n, "%d", scan_engine_get_threshold_pct());
        break;
    case 14:
        snprintf(out, n, "%d", scan_engine_get_hang_ms());
        break;
    case 15:
        snprintf(out,n,"%s",p25_p2_enabled()?"ON / MANUAL VOICE":"OFF");
        break;
    }
}
static void ps_number(double v)
{
    bool ok = true;
    switch (ps_selected) {
    case 3:
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
static void ps_change(int direction, bool edit)
{
    char value[48];
    ps_value(ps_selected, value, sizeof(value));
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
        if(edit || lakeshark_p25_agc_enabled()!=(direction>0)) lakeshark_p25_agc();
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
    case 15:
        scan_engine_stop();
        p25_p2_enable(edit?!p25_p2_enabled():direction>0);
        snprintf(ps_status,sizeof(ps_status),"%s",p25_p2_enabled()?"EXP: manual voice; IDs from control or p2 config":"Phase I restored");
        return;
    }
    snprintf(ps_status, sizeof(ps_status), "%s",
             ok ? "Change requested" : "Could not save setting");
}
static void ps_draw(tui_surface *sf, tui_rect a)
{
    bool wide = a.w > 72;
    if (a.h < 16 || a.w < 30) {
        ls_panel_notice(sf, a, "P25 SETTINGS", "Larger view required", "M: back");
        return;
    }
    a.x++;
    a.w -= 2;
    int bh = wide ? 5 : 10;
    tui_rect b = tui_rect_make(a.x, a.y + a.h - bh - 3, a.w, bh);
    ps_area = tui_rect_make(a.x, a.y + 1, a.w, b.y - a.y - 2);
    ps_row_height = wide ? 2 : 5;
    ps_rows = (ps_area.h - 4) / ps_row_height;
    if (ps_rows < 1)
        ps_rows = 1;
    ps_first = (ps_selected / ps_rows) * ps_rows;
    char title[48];
    int last = ps_first + ps_rows;
    if (last > PS_COUNT) last = PS_COUNT;
    snprintf(title, sizeof(title), "P25 SETTINGS / %d-%d OF %d", ps_first + 1, last, PS_COUNT);
    tui_box(sf, ps_area, title, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    for (int r = 0; r < ps_rows && ps_first + r < PS_COUNT; r++) {
        int i = ps_first + r;
        char value[48], text[100];
        ps_value(i, value, sizeof(value));
        snprintf(text, sizeof(text), "%c %s", i == ps_selected ? '>' : ' ', ps_names[i]);
        int y = ps_area.y + 2 + r * ps_row_height;
        uint8_t attr = TUI_ATTR(i == ps_selected ? TUI_YELLOW | TUI_BRIGHT : TUI_WHITE, TUI_BLACK);
        tui_put_str(sf, ps_area, ps_area.x + 2, y, text, attr);
        tui_put_str(sf, ps_area, wide ? ps_area.x + ps_area.w / 2 : ps_area.x + 4, wide ? y : y + 2,
                    value, attr);
    }
    tui_put_str(sf, ps_area, ps_area.x + 2, ps_area.y + ps_area.h - 2, ps_status, LS_ATTR_DIM);
    ls_btn_t controls[6];memcpy(controls,ps_buttons,sizeof(controls));
    controls[2].dim = controls[3].dim = ps_selected == 12;
    if (ps_selected == 12) controls[4].label = "RESET";
    if(ps_selected==1||ps_selected==2||ps_selected==6||ps_selected==7||ps_selected==8||ps_selected==15) {
        controls[2].label=ps_selected==1?"NORMAL":"OFF";
        controls[3].label=ps_selected==1?"INVERTED":"ON";
    }
    ls_btn_bar_raised(sf, b, controls, 6, -1);
}
static bool ps_key(ls_tk_t k, char c)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
    if (k == LS_TK_ESC || c == 'M' || c == '0') {
        ps_open = false;
        return true;
    }
    if (k == LS_TK_UP || c == 'U')
        ps_selected = (ps_selected + PS_COUNT - 1) % PS_COUNT;
    else if (k == LS_TK_DOWN || c == 'D')
        ps_selected = (ps_selected + 1) % PS_COUNT;
    else if (k == LS_TK_LEFT || c == '[')
        ps_change(-1, false);
    else if (k == LS_TK_RIGHT || c == ']')
        ps_change(1, false);
    else if (k == LS_TK_ENTER || c == 'E')
        ps_change(1, true);
    return true;
}
static bool ps_touch(int x, int y)
{
    if (tui_rect_contains(ps_area, x, y) && y >= ps_area.y + 2) {
        int i = ps_first + (y - ps_area.y - 2) / ps_row_height;
        if (i < PS_COUNT && i < ps_first + ps_rows)
            ps_selected = i;
        return true;
    }
    int hit = ls_btn_hit(x, y);
    if (hit >= 0 && hit < 6)
        return ps_key(LS_TK_CHAR, ps_buttons[hit].key);
    return false;
}
#endif
