#include "AppHome.hpp"
#include "shell/ls_shell.hpp"
#include "sdr_ui/sdr_ui.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "lakeshark_backend.h"
}

/*LS-604*/
static const struct { const char *app; const char *icon; const char *title;
                      const char *sub; uint32_t accent; } TILES[] = {
    { "P25",      "p25",      "P25",   "DIGITAL VOICE", 0xF0B767 },
    { "FM",       "fm",       "FM",    "ANALOG / PAGE", 0x83CFAB },
    { "ADS-B",    "adsb",     "ADS-B", "AIR TRAFFIC",   0x86B8E2 },
    { "Files",    "files",    "FILES", "SD BROWSER",    0x92DADA },
    { "Settings", "settings", "CONFIG","DEVICE",        0xB0A4E4 },
};

AppHome::AppHome() : LsApp("HOME", "home") {}

/*LS-604*/
void AppHome::tileCb(lv_event_t *e)
{
    const char *app = static_cast<const char *>(lv_event_get_user_data(e));
    if (app) LsShell::instance().launchByName(app);
}

/*LS-604*/
void AppHome::faceCb(lv_event_t *)
{
    const ls_hub_state_t *s = ls_hub_state();
    if (s && s->mode[0] && strcmp(s->mode, "--") != 0)
        LsShell::instance().launchByName(s->mode);
}

bool AppHome::run(lv_obj_t *parent)
{
    sdr_style_screen(parent);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    buildFace(parent);
    buildTiles(parent);
    buildSystem(parent);

    /*LS-604*/
    if (!lakeshark_radio_running()) lakeshark_select_p25();

    _sub = ls_hub_subscribe(hubCb, this);
    return true;
}

/*LS-604*/
void AppHome::buildFace(lv_obj_t *parent)
{
    _face = sdr_lcd_panel(parent, SDR_ACCENT_DIM);
    lv_obj_add_flag(_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_face, faceCb, LV_EVENT_CLICKED, this);

    lv_obj_t *head = sdr_row(_face, LV_FLEX_ALIGN_SPACE_BETWEEN);
    sdr_micro(head, "NOW MONITORING");
    _state = sdr_chip(head, "IDLE", SDR_DIM);

    _mode = sdr_value(_face, &lv_font_montserrat_26, SDR_PAS_GOLD);
    lv_obj_set_style_text_letter_space(_mode, 3, 0);
    lv_label_set_text(_mode, "--");

    _freq = sdr_value(_face, &lv_font_montserrat_48, SDR_PAS_AMBER);
    lv_obj_set_width(_freq, lv_pct(100));
    lv_obj_set_style_text_align(_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(_freq, 2, 0);
    lv_label_set_text(_freq, "---.----");

    _detail = sdr_value(_face, sdr_font_mono(), SDR_PAS_CYAN);
    lv_obj_set_width(_detail, lv_pct(100));
    lv_obj_set_style_text_align(_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_detail, "STANDBY");

    _meter = sdr_value(_face, sdr_font_mono_sm(), SDR_DIM);
    lv_obj_set_width(_meter, lv_pct(100));
    lv_label_set_text(_meter, "");
}

/*LS-604*/
void AppHome::buildTiles(lv_obj_t *parent)
{
    sdr_section(parent, "LAUNCH");

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    const int n = (int)(sizeof(TILES) / sizeof(TILES[0]));
    for (int i = 0; i < n; i++) {
        lv_obj_t *t = sdr_tile(grid, TILES[i].icon, TILES[i].title, TILES[i].sub,
                               tileCb, (void *)TILES[i].app);
        lv_obj_set_width(t, lv_pct(32));
        sdr_tile_accent(t, lv_color_hex(TILES[i].accent));
    }
}

/*LS-604*/
static lv_obj_t *sys_cell(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = sdr_row(parent, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_width(row, lv_pct(48));
    sdr_micro(row, name);
    lv_obj_t *v = sdr_value(row, sdr_font_mono_sm(), SDR_TEXT);
    lv_label_set_text(v, "--");
    return v;
}

/*LS-604*/
void AppHome::buildSystem(lv_obj_t *parent)
{
    sdr_section(parent, "SYSTEM");

    lv_obj_t *p = sdr_panel(parent);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(p, 5, 0);
    lv_obj_set_style_pad_column(p, 10, 0);

    _sys_rtl = sys_cell(p, "RTL");
    _sys_sd  = sys_cell(p, "SD");
    _sys_c6  = sys_cell(p, "C6");
    _sys_iq  = sys_cell(p, "IQ");

    lv_obj_t *tick = sdr_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(tick, 2, 0);
    lv_obj_t *caret = sdr_micro(tick, ">");
    lv_obj_set_style_text_color(caret, SDR_ACCENT, 0);
    _ticker = sdr_value(tick, sdr_font_mono_sm(), SDR_DIM);
    lv_obj_set_flex_grow(_ticker, 1);
    lv_label_set_text(_ticker, "READY");
}

void AppHome::hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud)
{
    static_cast<AppHome *>(ud)->apply(s, dirty);
}

/*LS-604*/
void AppHome::apply(const ls_hub_state_t *s, uint32_t dirty)
{
    if (!_visible) return;

    if (dirty & (LS_HUB_RADIO | LS_HUB_SIGNAL)) {
        sdr_text_if_changed(_mode, s->rtl_ready ? s->mode : "--");

        const char *st = !s->rtl_ready ? "NO DONGLE"
                       : s->parked     ? "PARKED"
                       : s->active     ? "ACTIVE" : "MONITOR";
        lv_color_t sc = !s->rtl_ready ? SDR_ERR
                      : s->parked     ? SDR_WARN
                      : s->active     ? SDR_OK : SDR_PAS_CYAN;
        sdr_chip_set(_state, st, sc);
        sdr_chip_live(_state, s->active);

        lv_color_t ec = !s->rtl_ready ? SDR_ERR : s->active ? SDR_OK : SDR_ACCENT_DIM;
        if (lv_obj_get_style_border_color(_face, 0).full != ec.full)
            lv_obj_set_style_border_color(_face, ec, 0);
    }

    if (dirty & LS_HUB_TUNE) {
        char f[24];
        if (s->freq_hz)
            snprintf(f, sizeof(f), "%lu.%04lu",
                     (unsigned long)(s->freq_hz / 1000000UL),
                     (unsigned long)((s->freq_hz / 100UL) % 10000UL));
        else
            snprintf(f, sizeof(f), "---.----");
        sdr_text_if_changed(_freq, f);
        sdr_color_if_changed(_freq, s->rtl_ready ? SDR_PAS_AMBER : SDR_DIM);
    }

    if (dirty & LS_HUB_SIGNAL) {
        sdr_text_if_changed(_detail, s->rtl_ready ? s->detail : "PLUG IN AN RTL-SDR");

        char bar[96];
        sdr_ascii_bar(bar, sizeof(bar), s->sig_pct, sdr_bar_width(_meter, 10));
        char line[112];
        snprintf(line, sizeof(line), "SIG [%s] %3d%%", bar, s->sig_pct);
        sdr_text_if_changed(_meter, line);
        sdr_color_if_changed(_meter, s->sig_pct >= 95 ? SDR_ERR
                                   : s->active        ? SDR_OK
                                   : s->sig_pct < 8   ? SDR_DIM : SDR_PAS_CYAN);

        char iq[16];
        snprintf(iq, sizeof(iq), "%luK/s", (unsigned long)(s->iq_bytes_sec / 1000));
        sdr_text_if_changed(_sys_iq, iq);
        sdr_color_if_changed(_sys_iq, s->iq_bytes_sec ? SDR_TEXT : SDR_DIM);
    }

    if (dirty & LS_HUB_RADIO) {
        sdr_text_if_changed(_sys_rtl, !s->rtl_ready ? "ABSENT"
                                    : s->parked     ? "PARKED" : "STREAMING");
        sdr_color_if_changed(_sys_rtl, !s->rtl_ready ? SDR_ERR
                                     : s->parked     ? SDR_WARN : SDR_OK);

        sdr_text_if_changed(_sys_sd, s->sd_present ? "MOUNTED" : "NONE");
        sdr_color_if_changed(_sys_sd, s->sd_present ? SDR_TEXT : SDR_DIM);

        sdr_text_if_changed(_sys_c6, s->c6_state == 1 ? "LINKED"
                                   : s->c6_state == 0 ? "DOWN" : "--");
        sdr_color_if_changed(_sys_c6, s->c6_state == 1 ? SDR_TEXT
                                    : s->c6_state == 0 ? SDR_WARN : SDR_DIM);
    }

    if (dirty & LS_HUB_EVENT) {
        char l[LS_HUB_LINE_MAX];
        if (ls_hub_last_line(l, sizeof(l))) {
            sdr_text_if_changed(_ticker, l);
            sdr_color_if_changed(_ticker, SDR_TEXT);
        }
    }
}

/*LS-604*/
bool AppHome::pause(void)  { _visible = false; return true; }

/*LS-604*/
bool AppHome::resume(void)
{
    _visible = true;
    apply(ls_hub_state(), LS_HUB_ALL);
    return true;
}

bool AppHome::close(void)
{
    if (_sub >= 0) { ls_hub_unsubscribe(_sub); _sub = -1; }
    _face = _mode = _state = _freq = _detail = _meter = nullptr;
    _sys_rtl = _sys_sd = _sys_c6 = _sys_iq = _ticker = nullptr;
    return true;
}

/*LS-604*/
void AppHome::switchTab(int delta) { LsShell::instance().cycleApp(delta); }
