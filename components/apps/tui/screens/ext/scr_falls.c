/* FALLS: the waterfall as a tool in its own right. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_tui_ui.h"
#include "../../ls_waterfall.h"
#include "../../ls_wf_source.h"
#include "../../ls_picker.h"
#include "../../ls_quick.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/fm_mode_label.h"

static void enter(void) { ls_wf_source_select(LS_WF_SRC_AUTO); }
static void leave(void) { ls_wf_source_release(); }

/* WHICH RADIO, as one button that opens a list. */

static const fm_mode_t MODES[] = {
    FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_AM, FM_MODE_POCSAG, FM_MODE_FLEX, FM_MODE_ACARS,
};

static void mode_picked(int index)
{
    if (index < 0 || index >= (int)(sizeof(MODES) / sizeof(MODES[0]))) return;
    ls_args_t args = {.n = 1};
    args.v[0].kind = LS_VAL_TEXT;
    args.v[0].s = fm_mode_command_name(MODES[index]);
    ls_val_t result;
    ls_action_call("fm.submode", &args, &result, ls_quick_grant_builtin());
}

static void open_mode_picker(void)
{
    ls_picker_open("RECEIVER MODE", mode_picked);
    for (int i = 0; i < (int)(sizeof(MODES) / sizeof(MODES[0])); ++i)
        ls_picker_add(fm_mode_label(MODES[i]), FM.mode == MODES[i] ? "selected" : "");
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

/* What each source IS, as opposed to what it is called. One line each, and
   the reason the picker carries a detail column at all. */
static const char *source_detail(ls_wf_src_t src)
{

    switch (src) {
    case LS_WF_SRC_P25:  return "RTL dongle";
    case LS_WF_SRC_FM:   return "RTL dongle";
    case LS_WF_SRC_LORA: return "SX1262";
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
    ls_wf_source_start(src);
    snprintf(s_flash, sizeof(s_flash), "%s - %s",
             ls_wf_source_label(src), source_detail(src));
    s_flash_ttl = 40;
}

static void picked(int index)
{
    if (index >= 0 && index < (int)LS_WF_SRC__COUNT) pick((ls_wf_src_t)index);
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

static void open_radio_picker(void)
{
    ls_picker_open("RADIO", picked);
    for (int i = 0; i < (int)LS_WF_SRC__COUNT; i++) {
        const ls_wf_src_t src = (ls_wf_src_t)i;
        const char *no = ls_wf_source_blocked(src);
        char label[LS_PICKER_TEXT];

        snprintf(label, sizeof(label), "%s%s",
                 ls_wf_source_label(src),
                 src == ls_wf_source_get() ? " *" : "");
        ls_picker_add(label, no ? no : source_detail(src));
    }
}

static void draw_buttons(tui_surface *sf, tui_rect r)
{
    const ls_wf_src_t src = ls_wf_source_get();
    ls_btn_t buttons[] = {
        {"RADIO", ls_wf_source_label(src), 'v', false, false},
        {"BAND", ls_wf_preset_current(src), 'n', false, ls_wf_preset_count(src) == 0},
        {"MODE", fm_mode_label(FM.mode), 'e', false, false},
        {"TUNE", "MHz", 't', false, false},
        {"SWEEP", FM.mode == FM_MODE_SCAN ? "ON" : "OFF", 'w', FM.mode == FM_MODE_SCAN, false},
    };
    ls_btn_bar_raised(sf, r, buttons, src == LS_WF_SRC_FM ? 5 : 2, -1);
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_wf_source_pump();

    int want = ls_btn_raised_height(area, ls_wf_source_get() == LS_WF_SRC_FM ? 5 : 2);
    draw_buttons(sf, tui_rect_make(area.x, area.y, area.w, want));
    tui_rect body = tui_rect_make(area.x, area.y + want, area.w, area.h - want);

    if (s_flash[0] && s_flash_ttl > 0) {
        s_flash_ttl--;
        tui_put_str(sf, body, body.x + 1, body.y, s_flash, LS_ATTR_DIM);
        body = tui_rect_make(body.x, body.y + 1, body.w, body.h - 1);
    } else {
        s_flash[0] = 0;
    }

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
    if (k == LS_TK_CHAR && ls_wf_source_get() == LS_WF_SRC_FM) {
        if (ch == 'e' || ch == 'E') { open_mode_picker(); return true; }
        if (ch == 'w' || ch == 'W') {
            ls_wf_fm_sweep(FM.mode != FM_MODE_SCAN);
            return true;
        }
    }
    if (k == LS_TK_CHAR && (ch == 't' || ch == 'T') &&
        ls_wf_source_get() == LS_WF_SRC_FM) {
        tune_fm();
        return true;
    }
    /* The source selector is this screen's, not the widget's: the widget
       draws whatever it is given and has no opinion about radios. */
    if (k == LS_TK_CHAR && (ch == 'v' || ch == 'V')) {
        open_radio_picker();
        return true;
    }
    /* N for the band list. Not B, which the waterfall already uses
       to hide its own buttons, and not P, which is its palette. */
    if (k == LS_TK_CHAR && (ch == 'n' || ch == 'N')) {
        open_preset_picker();
        return true;
    }
    return ls_wf_key(k, ch);
}

static bool touch(int col, int row)
{
    int i = ls_btn_hit(col, row);
    if (i >= 0) return key(LS_TK_CHAR, "vnetw"[i]);
    return ls_wf_touch(col, row);
}

const ls_tui_screen_t ls_scr_falls = {
    /* No `radio`, deliberately. This screen is a view of whatever
       receiver is already running, not a receiver of its own. Starting one
       here would mean that opening the waterfall silently tunes and un-mutes
       a radio nobody asked to start - and on a handheld with a speaker that
       is a surprise worth avoiding. It says so instead, and says which
       screen to open. */
    .name = "FALLS",
    .hint = "V radio  E mode  T tune  N band  W sweep",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
