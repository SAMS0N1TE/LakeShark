/* LS_TEST_SOURCES: ls_waterfall.c with settings and orientation fakes. */
#include "ls_test.h"
#include "ls_waterfall.h"
#include "ls_options.h"
#include <string.h>
static bool wide = true;
bool ls_tui_is_wide(void) { return wide; }
bool ls_tui_keyboard_mode(void) { return false; }
bool ls_tui_daylight(void) { return false; }
/* Preferences from a previous boot, using non-default values. */
static uint32_t stored = 75u | (12u << 7) | (8u << 12) | (2u << 17)
    | (4u << 19) | (8u << 23) | (1u << 27) | (1u << 28);
uint32_t settings_get_waterfall(uint32_t fallback) { (void)fallback; return stored; }
void settings_set_waterfall(uint32_t value) { stored = value; }
LS_CASE(preferences_load_and_level_rows_stop_at_both_ends)
{
    const ls_wf_cfg_t *cfg = ls_wf_cfg();
    LS_EQ_INT(75, cfg->split_pct); LS_EQ_INT(4, cfg->ref);
    LS_EQ_INT(8, cfg->range); LS_EQ_INT(2, cfg->palette);
    LS_EQ_INT(4, cfg->avg); LS_EQ_INT(8, cfg->decim);
    LS_CHECK(cfg->peak_hold); LS_EQ_INT(1, cfg->grain);
    const ls_opt_ctx_t *ctx = ls_wf_options();
    const int rows[] = {0, 1, 5, 6, 8};
    for (int i = 0; i < 5; i++) {
        const ls_opt_t *o = &ctx->opt[rows[i]];
        LS_EQ_INT(LS_OPT_LEVEL, o->kind);
        if (o->kind != LS_OPT_LEVEL) continue;
        o->set_num(o, o->lo);
        o->set_num(o, o->num(o) - o->step);
        LS_NEAR(o->lo, o->num(o), 0.001);
        o->set_num(o, o->num(o) + o->step);
        LS_NEAR(o->lo + o->step, o->num(o), 0.001);
        o->set_num(o, o->hi);
        o->set_num(o, o->num(o) + o->step);
        LS_NEAR(o->hi, o->num(o), 0.001);
        o->set_num(o, o->num(o) - o->step);
        LS_NEAR(o->hi - o->step, o->num(o), 0.001);
    }
    LS_EQ_INT(LS_OPT_CYCLE, ctx->opt[2].kind);
    LS_EQ_INT(LS_OPT_CYCLE, ctx->opt[3].kind);
    LS_EQ_INT(LS_OPT_TOGGLE, ctx->opt[7].kind);
    if (!ctx->opt[3].set || !ctx->opt[2].set) return;
    ctx->opt[3].set(&ctx->opt[3], 3);
    ctx->opt[2].set(&ctx->opt[2], 2);
    cfg = ls_wf_cfg();
    LS_EQ_INT(cfg->split_pct, stored & 127);
    LS_EQ_INT(cfg->ref, (int)((stored >> 7) & 31) - 8);
    LS_EQ_INT(cfg->range, (stored >> 12) & 31);
    LS_EQ_INT(cfg->palette, (stored >> 17) & 3);
    LS_EQ_INT(cfg->avg, (stored >> 19) & 15);
    LS_EQ_INT(cfg->decim, (stored >> 23) & 15);
}
static tui_cell back[140 * 75], front[140 * 75];
static void readout(int width, bool landscape, ls_wf_owner_t owner, bool marker)
{
    wide = landscape;
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 140, 75);
    ls_wf_cfg_t saved = *ls_wf_cfg();
    ls_wf_cfg_t cfg = saved;
    cfg.avg = cfg.decim = 1; cfg.paused = false;
    ls_wf_cfg_set(&cfg);
    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    ls_wf_claim(owner, owner == LS_WF_OWNER_LORA ? "LORA" : "P25");
    const ls_wf_feed_t f = {
        .center_hz = 851000000, .span_hz = 240000,
        .floor_db = -100, .top_db = 0, .note = "live",
    };
    float bins[64];
    for (int i = 0; i < 64; i++) bins[i] = 0.5f;
    ls_wf_push(owner, bins, 64, &f);
    tui_rect area = tui_rect_make(3, 2, width, landscape ? 28 : 65);
    ls_wf_draw(&sf, area);
    if (marker && owner == LS_WF_OWNER_LORA) ls_wf_key(LS_TK_CHAR, 'm');
    if (!marker && owner != LS_WF_OWNER_LORA) ls_wf_key(LS_TK_CHAR, 'm');
    tui_frame_begin(&sf);
    ls_wf_draw(&sf, area);
    int row = area.y + area.h - (landscape ? 2 : 12) - 1;
    char line[141];
    for (int x = 0; x < 140; x++) line[x] = back[row * 140 + x].ch;
    line[140] = 0;
    const char *unit = owner == LS_WF_OWNER_LORA ? "dBm" : "dBFS";
    LS_CHECK_MSG(strstr(line, unit) != NULL, "missing unit: %s", line);
    if (marker) {
        LS_CHECK_MSG(strstr(line, "MARK 851.") != NULL && strstr(line, " MHz  -50 ") != NULL, "%s", line);
        LS_CHECK_MSG(strstr(line, "SPACE tune") != NULL, "%s", line);
    }
    const char *pk = strstr(line, "PK");
    if (marker && pk) LS_CHECK(pk > strstr(line, "SPACE tune") + 10);
    if (!marker) LS_CHECK(pk != NULL);
    ls_wf_cfg_set(&saved);
}
LS_CASE(readout_units_and_meter_do_not_overwrite_marker_or_stats)
{
    const int widths[] = {52, 70, 84, 118};
    for (int i = 0; i < 4; i++)
        for (int l = 0; l < 2; l++) {
            readout(widths[i], l, LS_WF_OWNER_P25, true);
            readout(widths[i], l, LS_WF_OWNER_LORA, true);
            readout(widths[i], l, LS_WF_OWNER_P25, false);
            readout(widths[i], l, LS_WF_OWNER_LORA, false);
        }
}
