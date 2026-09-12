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

#define STRIP_ROWS_PORTRAIT 3
#define STRIP_ROWS_WIDE     1

static tui_rect s_button;
static tui_rect s_preset_button;
static tui_rect s_tune_button;
static tui_rect s_sweep_button;
static tui_rect s_mode_button;

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
    snprintf(title, sizeof(title), "%s BAND", ls_wf_source_label(src));
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

static void draw_one(tui_surface *sf, tui_rect r, const char *text,
                     uint8_t hue, bool live)
{
    const uint8_t at = live ? TUI_ATTR(TUI_BLACK, hue) : LS_ATTR_DIM;
    tui_fill(sf, r, ' ', at);
    int len = (int)strlen(text);
    if (len > r.w - 2) len = r.w - 2;
    tui_put_str(sf, r, r.x + (r.w - len) / 2, r.y + r.h / 2, text, at);
}

static void draw_buttons(tui_surface *sf, tui_rect r)
{
    s_tune_button = tui_rect_make(0, 0, 0, 0);
    s_sweep_button = tui_rect_make(0, 0, 0, 0);
    s_mode_button = tui_rect_make(0, 0, 0, 0);
    const int half = r.w / 2;
    s_button        = tui_rect_make(r.x, r.y, half - 1, r.h);
    s_preset_button = tui_rect_make(r.x + half, r.y, r.w - half, r.h);

    char text[40];
    const ls_wf_src_t src = ls_wf_source_get();
    if (src == LS_WF_SRC_FM) {
        const int radio_w = r.w / 6;
        const int part = (r.w - radio_w) / 4;
        s_button = tui_rect_make(r.x, r.y, radio_w - 1, r.h);
        s_mode_button = tui_rect_make(r.x + radio_w, r.y, part - 1, r.h);
        s_tune_button = tui_rect_make(r.x + radio_w + part, r.y, part - 1, r.h);
        s_preset_button = tui_rect_make(r.x + radio_w + part * 2, r.y, part - 1, r.h);
        s_sweep_button = tui_rect_make(r.x + radio_w + part * 3, r.y,
                                      r.w - radio_w - part * 3, r.h);
        draw_one(sf, s_button, "FM", TUI_CYAN, true);
        draw_one(sf, s_mode_button, FM.mode == FM_MODE_SCAN ? "MODE" : fm_mode_label(FM.mode),
                 TUI_CYAN, true);
        draw_one(sf, s_tune_button, "TUNE", TUI_GREEN, true);
        draw_one(sf, s_preset_button, "BAND", TUI_GREEN, true);
        draw_one(sf, s_sweep_button, FM.mode == FM_MODE_SCAN ? "LIVE" : "SWEEP",
                 TUI_YELLOW, true);
        return;
    }
    const char *want = ls_wf_source_label(src);
    const char *now  = ls_wf_source_name();
    if (strcmp(want, now) == 0) snprintf(text, sizeof(text), "%s", want);
    else snprintf(text, sizeof(text), "%s (%s)", want, now);
    draw_one(sf, s_button, text, TUI_CYAN, true);

    /* Dim when there is nothing behind it. A lit control that does nothing
       is the fault this project already has a rule about; this one is lit
       exactly when tapping it will offer something. */
    const bool any = ls_wf_preset_count(src) > 0;
    draw_one(sf, s_preset_button, ls_wf_preset_current(src),
             TUI_GREEN, any);
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_wf_source_pump();

    const int want = ls_tui_is_wide() ? STRIP_ROWS_WIDE : STRIP_ROWS_PORTRAIT;
    tui_rect body = area;
    if (area.h > want + 2) {
        draw_buttons(sf, tui_rect_make(area.x, area.y, area.w, want));
        body = tui_rect_make(area.x, area.y + want, area.w, area.h - want);
    } else {
        s_button = tui_rect_make(0, -1, 0, 0);
        s_preset_button = tui_rect_make(0, -1, 0, 0);
        s_mode_button = s_tune_button = s_sweep_button = tui_rect_make(0, -1, 0, 0);
    }

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
    if (tui_rect_contains(s_mode_button, col, row)) {
        open_mode_picker();
        return true;
    }
    if (tui_rect_contains(s_sweep_button, col, row)) {
        ls_wf_fm_sweep(FM.mode != FM_MODE_SCAN);
        return true;
    }
    if (tui_rect_contains(s_tune_button, col, row)) {
        tune_fm();
        return true;
    }
    if (s_button.h > 0 && row >= s_button.y &&
        row < s_button.y + s_button.h) {
        if (col < s_button.x + s_button.w) open_radio_picker();
        else                               open_preset_picker();
        return true;
    }
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
