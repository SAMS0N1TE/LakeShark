/* FALLS: the waterfall as a tool in its own right. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../../ls_tui_ui.h"
#include "../../ls_waterfall.h"
#include "../../ls_wf_source.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_field.h"
#include "../../ls_skyview.h"
#include "ls_gps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "../../ls_quick.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/fm_mode_label.h"

/* The SDRs and the LoRa chip draw a spectrum; the other radios use honest
   passive data views until their spectrum adapters are available. */
typedef struct { ls_rsel_radio_t radio; const char *detail; ls_field_source_t field; } data_source_t;
static const data_source_t data_sources[] = {
    {LS_RSEL_CC1101, "Receiver data; spectrum pending", LS_FIELD_CC1101},
    {LS_RSEL_GPS, "Satellite signal levels", LS_FIELD_NONE},
    {LS_RSEL_NRF24, "Survey data; spectrum pending", LS_FIELD_NRF24},
    {LS_RSEL_NFC, "Field data", LS_FIELD_NFC},
    {LS_RSEL_WIFI, "Link data", LS_FIELD_WIFI},
    {LS_RSEL_BLE, "Link data", LS_FIELD_BLE},
};
#define DATA_COUNT ((int)(sizeof(data_sources)/sizeof(data_sources[0])))
static int s_data = -1;
static int s_satellite;
static EXT_RAM_BSS_ATTR ls_gps_state_t s_gps;
/* The SDR last chosen here, for the button while no receiver holds one. */
static ls_rsel_radio_t s_sdr = LS_RSEL_SDR_RTL;

static int data_of(ls_rsel_radio_t r)
{
    for (int i = 0; i < DATA_COUNT; i++) if (data_sources[i].radio == r) return i;
    return -1;
}

/* What the screen is showing, as a radio: a data view's radio, the LoRa
   chip's sweep, or the SDR a receiver holds. */
static ls_rsel_radio_t in_use(void)
{
    if (s_data >= 0) return data_sources[s_data].radio;
    if (ls_wf_source_get() == LS_WF_SRC_LORA) return LS_RSEL_LORA;
    ls_rsel_radio_t r = ls_rsel_sdr_held_by("fm");
    if (r == LS_RSEL_NONE) r = ls_rsel_sdr_held_by("p25");
    return r != LS_RSEL_NONE ? r : s_sdr;
}

static bool show_data(int data)
{
    if (!ls_field_start() || !ls_field_source(data_sources[data].field)) return false;
    ls_wf_source_release();
    ls_field_watch(true);
    s_data = data; s_satellite = 0;
    return true;
}

static void enter(void)
{
    s_data = -1;
    ls_rsel_track(LS_RSEL_WATERFALL, in_use);
    /* The radio chosen last time, without starting a receiver: an SDR
       follows whichever receiver is running, and starting one stays a
       choice made here. */
    const ls_rsel_radio_t saved = ls_rsel_saved(LS_RSEL_WATERFALL);
    if (saved == LS_RSEL_SDR_RTL || saved == LS_RSEL_SDR_HACKRF) s_sdr = saved;
    const int data = data_of(saved);
    if (saved == LS_RSEL_LORA && !ls_wf_source_blocked(LS_WF_SRC_LORA)) ls_wf_source_select(LS_WF_SRC_LORA);
    else ls_wf_source_select(LS_WF_SRC_AUTO);
    if (data >= 0) show_data(data);
}
static void leave(void) { if(s_data>=0) ls_field_watch(false); s_data=-1; ls_wf_source_release(); }


/* WHICH RADIO, as one button that opens a list. */

static const fm_mode_t MODES[] = {
    FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_AM, FM_MODE_POCSAG, FM_MODE_FLEX, FM_MODE_ACARS,
};

static void pick(ls_wf_src_t src);

/* The FM receiver's modes, and P25 after them: both are what an SDR here
   can be listening as. */
#define MODE_COUNT ((int)(sizeof(MODES) / sizeof(MODES[0])))
static void mode_picked(int index)
{
    if (index == MODE_COUNT) { pick(LS_WF_SRC_P25); return; }
    if (index < 0 || index >= MODE_COUNT) return;
    if (ls_wf_source_get() != LS_WF_SRC_FM) pick(LS_WF_SRC_FM);
    ls_args_t args = {.n = 1};
    args.v[0].kind = LS_VAL_TEXT;
    args.v[0].s = fm_mode_command_name(MODES[index]);
    ls_val_t result;
    ls_action_call("fm.submode", &args, &result, ls_quick_grant_builtin());
}

static void open_mode_picker(void)
{
    const ls_wf_src_t src = ls_wf_source_get();
    ls_picker_open("RECEIVER MODE", mode_picked);
    for (int i = 0; i < MODE_COUNT; ++i)
        ls_picker_add(fm_mode_label(MODES[i]), src == LS_WF_SRC_FM && FM.mode == MODES[i] ? "selected" : "");
    ls_picker_add("P25", src == LS_WF_SRC_P25 ? "selected" : "");
}

/* MODE's face: what the SDR is listening as. */
static const char *mode_face(void)
{
    switch (ls_wf_source_get()) {
    case LS_WF_SRC_FM:  return fm_mode_label(FM.mode);
    case LS_WF_SRC_P25: return "P25";
    default:            return "ANY";
    }
}

static bool sdr_view(void)
{
    const ls_wf_src_t src = ls_wf_source_get();
    return s_data < 0 && src != LS_WF_SRC_LORA;
}

static void tune_fm(void)
{
    if (FM.mode == FM_MODE_SCAN) {
        ls_wf_fm_sweep(false);
    }
    const ls_quick_t tune = {.kind = LS_QUICK_ACTION, .action = "fm.tune"};
    ls_quick_fire(&tune, ls_quick_grant_builtin());
}
static char     s_flash[64];
/* Frames, not milliseconds - the same shape scr_mesh uses, and a screen that
   only exists while it is being drawn has no business reading a clock. */
static int      s_flash_ttl;

/* What each source IS, as opposed to what it is called. */
static const char *source_detail(ls_wf_src_t src)
{
    switch (src) {
    case LS_WF_SRC_P25:
    case LS_WF_SRC_FM:   return ls_rsel_name(in_use());
    case LS_WF_SRC_LORA: return ls_rsel_name(LS_RSEL_LORA);
    default:             return "any running";
    }
}

static void pick(ls_wf_src_t src)
{
    const char *no = ls_wf_source_blocked(src);
    if (no) {
        snprintf(s_flash, sizeof(s_flash), "%s: %s",
                 ls_wf_source_label(src), no);
        s_flash_ttl = 90;
        return;
    }
    /* Choosing a radio starts it. The screen still starts nothing on
       its own - see the note on ls_wf_source_start. */
    if (!ls_wf_source_start(src)) {
        snprintf(s_flash,sizeof(s_flash),"Source could not start"); s_flash_ttl=90; return;
    }
    if(s_data>=0) ls_field_watch(false);
    s_data=-1;
    snprintf(s_flash, sizeof(s_flash), "%s - %s",
             ls_wf_source_label(src), source_detail(src));
    s_flash_ttl = 40;
}

/* The receiver an SDR's spectrum comes from: P25 when it is the one
   running, otherwise FM. The chosen dongle becomes that receiver's own
   choice too - this spectrum is that receiver's - and a receiver holding
   the other one starts again on this one. */
static void pick_sdr(ls_rsel_radio_t radio)
{
    const char *claimed = ls_tui_radio_claimed();
    const bool p25 = claimed && !strcmp(claimed, "P25");
    const ls_rsel_job_t job = p25 ? LS_RSEL_P25 :
        FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_FLEX ? LS_RSEL_PAGER :
        FM.mode == FM_MODE_ACARS ? LS_RSEL_ACARS : LS_RSEL_FM;
    s_sdr = radio;
    ls_rsel_set(job, radio);
    const ls_rsel_radio_t held = ls_rsel_sdr_held_by(p25 ? "p25" : "fm");
    pick(p25 ? LS_WF_SRC_P25 : LS_WF_SRC_FM);
    if (held != LS_RSEL_NONE && held != radio) ls_rsel_restart_sdr();
}

static void picked(ls_rsel_radio_t radio)
{
    if (radio == LS_RSEL_SDR_RTL || radio == LS_RSEL_SDR_HACKRF) { pick_sdr(radio); return; }
    if (radio == LS_RSEL_LORA) { pick(LS_WF_SRC_LORA); return; }
    const int data = data_of(radio);
    if (data < 0) return;
    if (!show_data(data)) {
        ls_rsel_set(LS_RSEL_WATERFALL, in_use());
        snprintf(s_flash,sizeof(s_flash),"Stop recording before changing its source");
        s_flash_ttl=90; return;
    }
    s_flash[0]=0; s_flash_ttl=0;
}

static void preset_picked(int index)
{
    const ls_wf_src_t src = ls_wf_source_get();
    if (!ls_wf_preset_apply(src, index)) return;
    snprintf(s_flash, sizeof(s_flash), "%s - %s",
             ls_wf_source_label(src), ls_wf_preset_current(src));
    s_flash_ttl = 40;
}

static void open_preset_picker(void)
{
    const ls_wf_src_t src = ls_wf_source_get();
    const int n = ls_wf_preset_count(src);

    char title[24];
    snprintf(title, sizeof(title), "%s BAND", src == LS_WF_SRC_FM ? "RECEIVER" : ls_wf_source_label(src));
    ls_picker_open(title, preset_picked);
    for (int i = 0; i < n; i++)
        ls_picker_add(ls_wf_preset_label(src, i), ls_wf_preset_detail(src, i));

    /* An empty list with no sentence reads as a fault. For P25 it is not one
       - it means no profile has been programmed - and that is a different
       thing to be told. */
    const char *why = ls_wf_preset_none(src);
    if (why) ls_picker_empty_reason(why);
}

static void open_radio_picker(void) { ls_rsel_open(LS_RSEL_WATERFALL, picked); }

/* How many of the buttons below apply: RADIO alone for a data view, BAND
   for a spectrum, MODE for an SDR, and FM's TUNE and SWEEP when FM is it. */
static int button_count(void)
{
    if (s_data >= 0) return 1;
    if (ls_wf_source_get() == LS_WF_SRC_FM) return 5;
    return sdr_view() ? 3 : 2;
}

/* The waterfall's display settings, while a spectrum is what is shown; a
   data view has none. */
static const ls_opt_ctx_t *falls_options(void) { return s_data < 0 ? ls_wf_options() : NULL; }

/* The buttons that apply, and OPTIONS after them when there is one. */
static int bar_count(void) { return button_count() + (ls_opt_count(falls_options()) ? 1 : 0); }

static void draw_buttons(tui_surface *sf, tui_rect r)
{
    const ls_wf_src_t src = ls_wf_source_get();
    ls_btn_t buttons[] = {
        ls_rsel_button(LS_RSEL_WATERFALL),
        {"BAND", ls_wf_preset_current(src), 'n', false, ls_wf_preset_count(src) == 0},
        {"MODE", mode_face(), 'e', false, false},
        {"TUNE", "MHz", 't', false, false},
        {"SWEEP", FM.mode == FM_MODE_SCAN ? "ON" : "OFF", 'w', FM.mode == FM_MODE_SCAN, false},
        {0},
    };
    const int n = button_count();
    buttons[n] = ls_opt_button(falls_options());
    ls_btn_bar_raised(sf, r, buttons, bar_count(), -1);
}

/* Current measurement only: no extra history or fabricated spectrum. */
static void signal_meter(tui_surface *sf, tui_rect area, int row, int x, int width, float value)
{
    if(width<=0 || !isfinite(value)) return;
    if(value<0) value=0;
    if(value>1) value=1;
    int filled=(int)(value*width);
    for(int i=0;i<width;i++)
        tui_put_char(sf,area,area.x+x+i,area.y+row,
            i<filled?'#':'.',
            i<filled?TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM);
}

static void draw_data(tui_surface *sf, tui_rect area)
{
    char line[96];
    const data_source_t *source=&data_sources[s_data];
    ls_kv(sf,area,1,"VIEW",source->detail,LS_ATTR_DIM);
    if(source->field==LS_FIELD_NONE) {
        ls_gps_get(&s_gps);
        ls_skyview_draw(sf,tui_rect_make(area.x,area.y+2,area.w,area.h-2),&s_gps,s_satellite);
        return;
    }
    ls_field_sample_t sample;
    ls_field_sample_snapshot(&sample);
    const int64_t now=esp_timer_get_time();
    bool fresh=sample.source==source->field && sample.time_us>0 && now>=sample.time_us && now-sample.time_us<1000000;
    if(!fresh || !sample.radio_valid) {
        ls_kv(sf,area,3,"SOURCE","No fresh data; receiver or adapter unavailable",LS_ATTR_DIM);
        return;
    }
    ls_kv(sf,area,3,"SOURCE",ls_rsel_name(source->radio),LS_ATTR_DIM);
    if(sample.frequency) snprintf(line,sizeof(line),"%.4f MHz",sample.frequency/1e6);
    else snprintf(line,sizeof(line),"Not reported");
    ls_kv(sf,area,5,"FREQ",line,LS_ATTR_DIM);
    if(sample.signal_valid) snprintf(line,sizeof(line),"%.1f dBm",sample.rssi);
    else snprintf(line,sizeof(line),"Not reported");
    ls_kv(sf,area,6,"SIGNAL",line,LS_ATTR_DIM);
    snprintf(line,sizeof(line),"%lu",(unsigned long)sample.packets);
    ls_kv(sf,area,7,"COUNT",line,LS_ATTR_DIM);
    if(sample.signal_valid && isfinite(sample.rssi)) {
        signal_meter(sf,area,9,2,area.w-4,(sample.rssi+140.0f)/140.0f);
        ls_kv(sf,area,10,"SCALE","-140 | -70 | 0 dBm",LS_ATTR_DIM);
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    if(s_data<0) ls_wf_source_pump();

    int want = ls_btn_raised_height(area, bar_count());
    draw_buttons(sf, tui_rect_make(area.x, area.y, area.w, want));
    tui_rect body = tui_rect_make(area.x, area.y + want, area.w, area.h - want);

    if (s_flash[0] && s_flash_ttl > 0) {
        s_flash_ttl--;
        tui_put_str(sf, body, body.x + 1, body.y, s_flash, LS_ATTR_DIM);
        body = tui_rect_make(body.x, body.y + 1, body.w, body.h - 1);
    } else {
        s_flash[0] = 0;
    }

    if(s_data>=0) { draw_data(sf,body); return; }

    const char *why = ls_wf_idle_reason();
    /* Keep HOLD reachable while the shared display is paused. */
    if (why && !ls_wf_cfg()->paused) {
        ls_wf_stats_t stats;
        ls_wf_stats(&stats);
        const char *progress = stats.ready ? ls_wf_source_progress() : NULL;
        ls_panel_notice(sf, body, "WATERFALL", progress ? progress : why,
                        progress ? "First row appears after this sweep"
                                 : "tap RADIO above to pick one and start it");
        return;
    }

    ls_wf_draw(sf, body);
}

static bool key(ls_tk_t k, char ch)
{
    if(s_data>=0 && data_sources[s_data].field==LS_FIELD_NONE && k==LS_TK_CHAR &&
       (ch=='j' || ch=='J' || ch=='k' || ch=='K')) {
        int n=s_gps.sat_count;
        if(n>LS_GPS_MAX_SATS)n=LS_GPS_MAX_SATS;
        if(n) s_satellite=(s_satellite+(ch=='j'||ch=='J'?1:n-1))%n;
        return true;
    }
    if (k == LS_TK_CHAR && sdr_view() && (ch == 'e' || ch == 'E')) { open_mode_picker(); return true; }
    if (k == LS_TK_CHAR && s_data<0 && ls_wf_source_get() == LS_WF_SRC_FM) {
        if (ch == 'w' || ch == 'W') {
            ls_wf_fm_sweep(FM.mode != FM_MODE_SCAN);
            return true;
        }
    }
    if (k == LS_TK_CHAR && (ch == 't' || ch == 'T') &&
        s_data<0 && ls_wf_source_get() == LS_WF_SRC_FM) {
        tune_fm();
        return true;
    }
    /* The source selector is this screen's, not the widget's: the widget
       draws whatever it is given and has no opinion about radios. R as in
       every app, and V. */
    if (k == LS_TK_CHAR && (ch == 'r' || ch == 'R' || ch == 'v' || ch == 'V')) {
        open_radio_picker();
        return true;
    }
    /* O before the waterfall, whose strip has no O of its own. */
    if (k == LS_TK_CHAR && ls_opt_key(falls_options(), ch)) return true;
    /* N for the band list. Not B, which the waterfall already uses
       to hide its own buttons, and not P, which is its palette. */
    if (k == LS_TK_CHAR && (ch == 'n' || ch == 'N')) {
        if(s_data>=0) return false;
        open_preset_picker();
        return true;
    }
    return s_data<0 && ls_wf_key(k, ch);
}

static bool touch(int col, int row)
{
    int i = ls_btn_hit(col, row);
    if (i >= 0 && i < button_count()) return key(LS_TK_CHAR, "rnetw"[i]);
    if (i >= 0 && i == button_count()) { ls_opt_open(falls_options()); return true; }
    return s_data<0 && ls_wf_touch(col, row);
}

const ls_tui_screen_t ls_scr_falls = {
    /* No `radio`, deliberately. This screen is a view of whatever
       receiver is already running, not a receiver of its own. Starting one
       here would mean that opening the waterfall silently tunes and un-mutes
       a radio nobody asked to start - and on a handheld with a speaker that
       is a surprise worth avoiding. It says so instead, and says which
       screen to open. */
    .name = "FALLS",
    .hint = "R radio  E mode  T tune  N band  W sweep  O options",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
