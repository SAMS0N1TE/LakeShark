#ifndef LS_DMR_VIEW_H
#define LS_DMR_VIEW_H
#include "dmr_listen.h"
#include "ls_motion.h"

static dmr_listen_t s_dmr_filter = {.colour_code = -1};
static dmr_watch_t s_dmr_snapshot;
static tui_rect s_dmr_bar;
static const char *const dmr_slots[] = {"BOTH", "1", "2"};
static double dmr_slot_get(const ls_opt_t *o) { (void)o; return s_dmr_filter.slot; }
static void dmr_slot_set(const ls_opt_t *o, double v)
{ (void)o; if (v >= 0 && v <= 2) s_dmr_filter.slot = (unsigned)v; }
static void dmr_slot_show(const ls_opt_t *o, char *out, size_t n)
{ (void)o; snprintf(out, n, "%s", dmr_slots[s_dmr_filter.slot]); }
static void dmr_mute_show(const ls_opt_t *o, char *out, size_t n)
{ (void)o; snprintf(out, n, "ON / ALWAYS"); }
static double dmr_cc_get(const ls_opt_t *o) { (void)o; return s_dmr_filter.colour_code; }
static void dmr_cc_set(const ls_opt_t *o, double v)
{ (void)o; if (v >= -1 && v <= 15) s_dmr_filter.colour_code = (int)v; }
static void dmr_cc_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    if (s_dmr_filter.colour_code < 0) snprintf(out, n, "ANY");
    else snprintf(out, n, "%d", s_dmr_filter.colour_code);
}
static void dmr_allow_show(const ls_opt_t *o, char *out, size_t n)
{ (void)o; snprintf(out, n, "%s", s_dmr_filter.text[0] ? s_dmr_filter.text : "ANY"); }
static const char *dmr_allow_get(const ls_opt_t *o) { (void)o; return s_dmr_filter.text; }
static void dmr_allow_set(const ls_opt_t *o, const char *text)
{
    (void)o;
    if (!dmr_listen_allow(&s_dmr_filter, text)) {
        ls_notice_t note = {.title = "DMR FILTER", .body = "Use up to 16 IDs, 1..16777215, separated by spaces or commas", .hue = TUI_YELLOW};
        ls_notify_post_quiet(&note);
    }
}
static const char *dmr_mute_reason(const ls_opt_t *o)
{ (void)o; return "Encrypted voice is always muted"; }
static const char *dmr_record_reason(const ls_opt_t *o)
{ (void)o; return "Voice codec required for PCM recording"; }
static void dmr_record_show(const ls_opt_t *o, char *out, size_t n)
{ (void)o; snprintf(out, n, "UNAVAILABLE"); }
static const ls_opt_t OPT_DMR_FILTER[] = {
    {.label = "Slot", .kind = LS_OPT_LEVEL, .num = dmr_slot_get,
     .set_num = dmr_slot_set, .lo = 0, .hi = 2, .step = 1,
     .unit = "0 BOTH / 1 / 2", .show = dmr_slot_show},
    {.label = "Colour code", .kind = LS_OPT_LEVEL, .num = dmr_cc_get,
     .set_num = dmr_cc_set, .lo = -1, .hi = 15, .step = 1,
     .unit = "-1 ANY / 0..15", .show = dmr_cc_show},
    {.label = "Talkgroup allow list", .kind = LS_OPT_TEXT, .text = dmr_allow_get,
     .set_text = dmr_allow_set, .max = 127, .show = dmr_allow_show},
};
static const ls_opt_ctx_t CTX_DMR_FILTER = {
    .name = "FILTER", .job = LS_RSEL_P25, LS_OPT_ROWS(OPT_DMR_FILTER)};
static const ls_opt_t OPT_DMR[] = {
    {.label = "Receive filters", .kind = LS_OPT_MENU, .sub = &CTX_DMR_FILTER},
    {.label = "Mute encrypted", .kind = LS_OPT_ACTION, .show = dmr_mute_show,
     .why_not = dmr_mute_reason},
    {.label = "Record calls", .kind = LS_OPT_ACTION, .show = dmr_record_show,
     .why_not = dmr_record_reason},
    LS_CALLS_MENU,
};
static const ls_opt_ctx_t CTX_DMR = {
    .name = "DMR", .job = LS_RSEL_P25, LS_OPT_ROWS(OPT_DMR)};

static void dmr_hold(void)
{
    if (s_dmr_filter.hold) { s_dmr_filter.hold = 0; return; }
    dmr_watch_get(&s_dmr_snapshot);
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < 2; ++i) {
        const dmr_call_t *c = &s_dmr_snapshot.call[i];
        if (c->active && now >= c->last_us && now - c->last_us <= 1500000 &&
            dmr_listen_matches(&s_dmr_filter, i + 1, c)) {
            s_dmr_filter.hold = c->lc.destination;
            return;
        }
    }
}

static void draw_dmr(tui_surface *sf, tui_rect area)
{
    memset(&s_dmr_snapshot, 0, sizeof(s_dmr_snapshot));
    bool have = dmr_watch_get(&s_dmr_snapshot);
    int64_t now = esp_timer_get_time();
    char line[80], hold[16];
    uint8_t cyan = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    uint8_t yellow = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    snprintf(hold, sizeof(hold), "%u", (unsigned)s_dmr_filter.hold);
    ls_btn_t buttons[] = {
        {.label="P25", .key='1'}, {.label="SLOT", .value=dmr_slots[s_dmr_filter.slot], .key='s'},
        {.label="HOLD", .value=s_dmr_filter.hold ? hold : "OFF", .key='h', .on=s_dmr_filter.hold != 0},
        ls_rsel_button(LS_RSEL_P25), ls_opt_button(&CTX_DMR),
    };
    int bar_h = ls_tui_is_wide() ? 3 : 6;
    if (bar_h > area.h) bar_h = area.h;
    s_dmr_bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    ls_btn_bar_raised_slot(sf, s_dmr_bar, buttons, 5, -1, LS_BTN_SLOT_QUICK);
    tui_rect body = tui_rect_make(area.x, area.y, area.w, area.h - bar_h);
    ls_safe_line(sf, body, body.y, "DMR TIER II / RECEIVE ONLY", cyan);
    ls_safe_line(sf, body, body.y + 1, "Voice unavailable / AMBE+2 required", LS_ATTR_DIM);
    unsigned selected = dmr_listen_select(&s_dmr_filter, &s_dmr_snapshot, now);
    for (unsigned i = 0; i < 2; ++i) {
        const dmr_call_t *c = &s_dmr_snapshot.call[i];
        tui_rect pane = tui_rect_make(body.x, body.y + 3 + i * 6, body.w, 6);
        if (pane.y + pane.h > body.y + body.h) continue;
        bool live = c->active && now >= c->last_us && now - c->last_us <= 1500000;
        snprintf(line, sizeof(line), "SLOT %u %c%s", i + 1, ls_motion_pip(live),
                 selected == i + 1 ? " SELECTED" : "");
        ls_panel_box(sf, pane, line, TUI_CYAN);
        if (!c->have_lc) {
            ls_safe_line(sf, pane, pane.y + 2, "No verified call", LS_ATTR_DIM);
            continue;
        }
        snprintf(line, sizeof(line), "CC %u  %s  %s", c->colour_code,
                 live ? "LIVE" : "ENDED", c->encrypted ? "ENCRYPTED / MUTED" : "CLEAR");
        ls_safe_line(sf, pane, pane.y + 1, line, c->encrypted ? yellow : cyan);
        snprintf(line, sizeof(line), "%s %u", c->lc.flco == 0 ? "TG" : "DST",
                 (unsigned)c->lc.destination);
        ls_safe_line(sf, pane, pane.y + 2, line, cyan);
        snprintf(line, sizeof(line), "SRC %u", (unsigned)c->lc.source);
        ls_safe_line(sf, pane, pane.y + 3, line, LS_ATTR_DIM);
        uint32_t ms = dmr_call_duration_ms(c, now);
        snprintf(line, sizeof(line), "%u.%us  %s", (unsigned)(ms / 1000),
                 (unsigned)(ms % 1000 / 100),
                 dmr_listen_matches(&s_dmr_filter, i + 1, c) ? "MATCH" : "FILTERED");
        ls_safe_line(sf, pane, pane.y + 4, line, LS_ATTR_DIM);
    }
    snprintf(line, sizeof(line), "%s / slot unknown %u", have ? "Watching symbols" : "Start C4FM receiver",
             (unsigned)s_dmr_snapshot.unknown_slot);
    ls_safe_line(sf, body, body.y + 15, line, LS_ATTR_DIM);
    if (body.h > 19) {
        ls_wf_source_select(LS_WF_SRC_P25);
        ls_wf_source_pump();
        ls_wf_draw_mini(sf, tui_rect_make(body.x, body.y + 17, body.w, body.h - 17));
    } else ls_wf_source_release();
}
#endif
