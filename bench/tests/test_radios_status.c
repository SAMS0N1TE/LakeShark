#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_radios.c"

static ls_radio_endpoint_info_t rtl, hackrf;
static ls_mixrf_status_t mix;
static bool external, acknowledged, running;
static esp_err_t switch_error, stop_error, gps_error;
static ls_mesh_socket_t socket_state;
static ls_wireless_snapshot_t wireless;
static bool wide;
static unsigned pauses;
static unsigned switches, saved;
static bool remembered;
static tui_rect rendered_button;

ls_radio_err_t ls_radio_endpoint_get(const char *id, ls_radio_endpoint_info_t *out)
{ *out = !strcmp(id, LS_RADIO_ENDPOINT_RTL_USB) ? rtl : hackrf; return LS_RADIO_OK; }
void ls_tui_radio_want(const char *mode) { (void)mode; }
bool ls_gps_running(void) { return false; }
void ls_gps_get(ls_gps_state_t *out) { memset(out, 0, sizeof(*out)); }
esp_err_t ls_gps_start(void) { return gps_error; }
void ls_gps_stop(void) { }
bool ls_mesh_running(void) { return running; }
void ls_mesh_socket_get(ls_mesh_socket_t *out) { *out = socket_state; out->present |= running; }
void ls_wireless_observe(bool active) { (void)active; }
void ls_wireless_get(ls_wireless_snapshot_t *out) { *out = wireless; }
ls_radio_err_t ls_radio_endpoint_pause(const char *id, const char *owner, bool pause)
{ ls_radio_endpoint_info_t *p = !strcmp(id, LS_RADIO_ENDPOINT_RTL_USB) ? &rtl : &hackrf;
  ++pauses; p->paused = pause; if (pause) p->streaming = false; return LS_RADIO_OK; }
const char *ls_radio_err_name(ls_radio_err_t err) { return "error"; }
bool ls_mesh_tx_enabled(void) { return false; }
esp_err_t ls_mesh_start(void) { running = true; return ESP_OK; }
esp_err_t ls_mesh_stop(void) { if (!stop_error) running = false; return stop_error; }
void ls_mixrf_snapshot(ls_mixrf_status_t *out) { *out = mix; }
bool ls_nfc_suite_busy(void) { return false; }
int ls_app_count(void) { return 0; }
const ls_app_t *ls_app_at(int i) { return NULL; }
void ls_app_open(int i) { }
bool ls_board_hw_antenna_is_external(void) { return external; }
bool ls_board_hw_antenna_tx_allowed(void) { return !external || acknowledged; }
esp_err_t ls_board_hw_antenna_external(bool ext)
{ if (switch_error) return switch_error; ++switches; external = ext; acknowledged = false; return ESP_OK; }
esp_err_t ls_board_hw_antenna_confirm_external(void)
{ if (switch_error) return switch_error; ++switches; external = acknowledged = true; return ESP_OK; }
void settings_set_antenna_external(bool ext) { ++saved; }
bool settings_get_antenna_remember(void) { return remembered; }
void settings_set_antenna_remember(bool v) { remembered = v; }
int ls_motion_phase(int steps, int ms) { return 0; }
ls_btn_t ls_opt_button(const ls_opt_ctx_t *ctx) { return (ls_btn_t){ .label = "OPTIONS" }; }
void ls_opt_open(const ls_opt_ctx_t *ctx) { }
void ls_opt_close(void) { }
void ls_btn_bar_raised(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n, int focus)
{ rendered_button = bar; }
void ls_btn_bar_transport(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n, int focus)
{ rendered_button = bar; }
bool ls_btn_rect_slot(int slot, int i, int *x, int *y, int *w, int *h)
{ *x = rendered_button.x; *y = rendered_button.y; *w = rendered_button.w; *h = rendered_button.h; return true; }
bool settings_get_ble_at_boot(void) { return false; }
bool settings_get_wifi_at_boot(void) { return false; }
void settings_set_ble_at_boot(bool on) { }
void settings_set_wifi_at_boot(bool on) { }
bool ls_tui_is_wide(void) { return wide; }
char ls_motion_pip(bool on) { return '*'; }
void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue) { }

LS_CASE(sdr_missing_failed_and_parked_do_not_count_as_powered)
{
    memset(&rtl, 0, sizeof(rtl)); memset(&hackrf, 0, sizeof(hackrf));
    int base = count_live();
    LS_EQ_INT(rtl_read(), RS_ABSENT);
    rtl_set(true);
    LS_EQ_INT(rtl_read(), RS_ABSENT);
    rtl.present = true;
    LS_CHECK(!strcmp(rtl_state(), "parked"));
    LS_EQ_INT(count_live(), base);
    rtl.leased = true;
    LS_EQ_INT(rtl_read(), RS_STARTING);
    rtl.last_error = LS_RADIO_ERR_IO;
    rtl.streaming = true;
    LS_CHECK(!strcmp(rtl_state(), "failed"));
    LS_EQ_INT(count_live(), base);
    /* A stale stop, timeout or busy is an event, not a failed receiver:
       streaming shows busy and a turned-off one shows parked. */
    rtl.last_error = LS_RADIO_ERR_STOPPED;
    LS_EQ_INT(rtl_read(), RS_BUSY);
    rtl.last_error = LS_RADIO_ERR_TIMEOUT;
    LS_EQ_INT(rtl_read(), RS_BUSY);
    rtl.streaming = false; rtl.leased = false;
    rtl.last_error = LS_RADIO_ERR_STOPPED;
    LS_EQ_INT(rtl_read(), RS_OFF);
    LS_CHECK(!strcmp(rtl_state(), "parked"));
    rtl.leased = true; rtl.streaming = true;
    rtl.last_error = LS_RADIO_OK;
    LS_EQ_INT(rtl_read(), RS_BUSY);
    LS_EQ_INT(count_live(), base + 1);
    LS_EQ_INT(hackrf_read(), RS_ABSENT);
}

LS_CASE(cc1101_replay_is_visible_without_receive)
{
    memset(&mix, 0, sizeof(mix)); mix.cc = true;
    LS_EQ_INT(cc_read(), RS_ON);
    mix.transmitting = true;
    LS_EQ_INT(cc_read(), RS_BUSY);
    LS_CHECK(!strcmp(cc_state(), "TX replay"));
    mix.transmitting = false;
    LS_EQ_INT(cc_read(), RS_ON);
}

LS_CASE(antenna_confirmation_defaults_to_cancel_and_reports_switch_failure)
{
    remembered = false; external = acknowledged = false; switches = saved = 0; switch_error = ESP_OK;
    ant_set(true);
    LS_CHECK(s_ant_confirm);
    LS_EQ_INT(switches, 0);
    key(LS_TK_ENTER, 0);
    LS_EQ_INT(switches, 0);
    ant_set(true); key(LS_TK_DOWN, 0); key(LS_TK_ENTER, 0);
    LS_CHECK(external && acknowledged);
    LS_EQ_INT(saved, 1);
    ant_set(false);
    switch_error = ESP_ERR_INVALID_STATE;
    ant_set(true); key(LS_TK_DOWN, 0); key(LS_TK_ENTER, 0);
    LS_CHECK(!external && s_error);
    LS_EQ_INT(saved, 2);
    switch_error = ESP_OK;
    ls_board_hw_antenna_external(true);
    LS_CHECK(!acknowledged);
    int ant = 0;
    while (ROWS[ant].set != ant_set) ++ant;
    toggle(ant);
    LS_CHECK(s_ant_confirm);
    leave(); LS_CHECK(!s_ant_confirm);
}

LS_CASE(lora_stop_failure_keeps_the_radio_on_and_reports_error)
{
    running = true; stop_error = ESP_FAIL;
    mesh_set(false);
    LS_CHECK(mesh_read() != RS_OFF && s_error);
    stop_error = ESP_OK; mesh_set(false);
    LS_EQ_INT(mesh_read(), RS_OFF);
    LS_CHECK(!s_error);
}

LS_CASE(antenna_warning_and_touch_confirmation_are_rendered)
{
    tui_cell back[40 * 24], front[40 * 24];
    tui_surface sf;
    tui_surface_setup(&sf, back, front, 40, 24);
    tui_frame_begin(&sf);
    external = acknowledged = false; switch_error = ESP_OK;
    ant_set(true);
    touch(3, 14); /* Confirmation is unavailable until the warning is drawn. */
    LS_CHECK(!external);
    draw(&sf, tui_surface_rect(&sf));
    const char *warning = "can damage the radio.";
    const int wx = 2 + (36 - (int)strlen(warning)) / 2;
    for (int i = 0; warning[i]; ++i)
        LS_EQ_INT(back[6 * 40 + wx + i].ch, warning[i]);
    LS_CHECK(s_ant_no.h >= 3 && s_ant_yes.h >= 3 && s_ant_toggle.h >= 3);
    touch(s_ant_toggle.x, s_ant_toggle.y);
    LS_CHECK(s_ant_remember && !remembered);
    touch(s_ant_no.x, s_ant_no.y);
    LS_CHECK(!external && !s_ant_confirm && !remembered);
    ant_set(true); draw(&sf, tui_surface_rect(&sf));
    LS_CHECK(!s_ant_remember);
    touch(s_ant_yes.x, s_ant_yes.y);
    LS_CHECK(external && acknowledged && !s_ant_confirm && !remembered);
}

LS_CASE(remember_requires_successful_accept_and_warning_can_be_restored)
{
    remembered = false; external = acknowledged = false; switch_error = ESP_OK;
    ant_set(true); key(LS_TK_TAB, 0); key(LS_TK_TAB, 0); key(LS_TK_ENTER, 0);
    LS_CHECK(s_ant_remember && !remembered);
    key(LS_TK_ESC, 0); LS_CHECK(!remembered && !external);
    ant_set(true); LS_CHECK(!s_ant_remember);
    s_ant_remember = true; switch_error = ESP_FAIL; ant_confirm(true);
    LS_CHECK(!remembered);
    switch_error = ESP_OK; ant_set(true); s_ant_remember = true; ant_confirm(true);
    LS_CHECK(remembered && acknowledged);
    ant_set(false); unsigned before = switches; ant_set(true);
    LS_CHECK(!s_ant_confirm && acknowledged); LS_EQ_INT(switches, before + 1);
    warning_set(NULL, 1); LS_EQ_INT(warning_get(NULL), 1);
    ant_set(false); ant_set(true); LS_CHECK(s_ant_confirm);
    ant_confirm(false);
}

LS_CASE(socket_owner_activity_and_named_refusal)
{
    memset(&socket_state, 0, sizeof(socket_state));
    socket_state.present = true; socket_state.armed = true;
    strcpy(socket_state.chip, "LR2021"); strcpy(socket_state.owner, "MeshCore");
    strcpy(socket_state.mode, "LoRa");
    LS_EQ_INT(mesh_read(), RS_ON);
    LS_CHECK(strstr(mesh_state(), "armed") && !strstr(mesh_state(), "TX"));
    socket_state.transmitting = true;
    LS_EQ_INT(mesh_read(), RS_BUSY); LS_CHECK(strstr(mesh_state(), "TX"));
    socket_state.transmitting = false;
    socket_state.held = true; socket_state.receiving = true;
    strcpy(socket_state.owner, "adsb"); strcpy(socket_state.mode, "Mode S");
    running = false;
    LS_EQ_INT(mesh_read(), RS_BUSY);
    mesh_set(false);
    LS_CHECK(s_error && strstr(s_error, "adsb"));
    LS_CHECK(!running);
    memset(&socket_state, 0, sizeof(socket_state));
}

LS_CASE(sdr_controls_are_scoped_and_absent_controls_do_nothing)
{
    memset(&rtl, 0, sizeof(rtl)); memset(&hackrf, 0, sizeof(hackrf)); pauses = 0;
    toggle(0); LS_EQ_INT(pauses, 0);
    rtl.present = hackrf.present = true;
    rtl.streaming = hackrf.streaming = true;
    toggle(0);
    LS_CHECK(rtl.paused && !rtl.streaming && hackrf.streaming && !hackrf.paused);
    LS_CHECK(!strcmp(rtl_action(), "ALLOW RX"));
    toggle(0); LS_CHECK(!rtl.paused && hackrf.streaming);
}

LS_CASE(large_font_pages_follow_keys_and_touch_maps_real_rows)
{
    static tui_cell back[79 * 46], front[79 * 46];
    tui_surface sf;
    s_ant_confirm = false;
    for (int view = 0; view < 2; view++) {
        wide = view != 0;
        tui_surface_setup(&sf, back, front, wide ? 79 : 34, wide ? 20 : 46);
        for (int i = 0; i < N_ROWS; i++) {
            s_sel = i; draw(&sf, tui_surface_rect(&sf));
            bool seen = false;
            for (int h = 0; h < s_hit_n; h++) {
                LS_CHECK(s_hit[h].y + s_hit[h].h <= s_options_hit.y);
                if (s_hit_row[h] == i) seen = true;
            }
            LS_CHECK(seen);
        }
        s_sel = 0; draw(&sf, tui_surface_rect(&sf));
        LS_CHECK(s_page_size < N_ROWS);
        touch(tui_surface_rect(&sf).x + tui_surface_rect(&sf).w - 7, s_page_next.y);
        draw(&sf, tui_surface_rect(&sf));
        LS_EQ_INT(s_hit_row[0], s_page_first);
        touch(s_hit[0].x, s_hit[0].y);
        LS_EQ_INT(s_sel, s_hit_row[0]);
        s_ant_confirm = false;
    }
    wide = false;
}

LS_CASE(gps_start_error_is_visible)
{
    gps_error = ESP_FAIL; gps_set(true); LS_CHECK(s_error && strstr(s_error, "GPS"));
    gps_error = ESP_OK; gps_set(true); LS_CHECK(!s_error);
}

LS_CASE(c6_live_state_is_separate_from_boot_preference)
{
    memset(&wireless, 0, sizeof(wireless));
    LS_EQ_INT(ble_read(), RS_ABSENT); LS_EQ_INT(wifi_read(), RS_ABSENT);
    wireless.bt_available = wireless.wifi_available = true;
    wireless.bt_ready = wireless.wifi_connected = true;
    wireless.bt_running = wireless.wifi_running = true;
    LS_EQ_INT(ble_read(), RS_BUSY); LS_EQ_INT(wifi_read(), RS_BUSY);
    LS_CHECK(!strcmp(link_action(), "OPEN LINK"));
    wireless.bt_running = wireless.bt_ready = wireless.wifi_running = wireless.wifi_connected = false;
    LS_EQ_INT(ble_read(), RS_OFF); LS_EQ_INT(wifi_read(), RS_OFF);
    wireless.bt_failed = true; LS_EQ_INT(ble_read(), RS_FAILED);
    LS_CHECK(!strcmp(ble_state(), "failed"));
    memset(&wireless, 0, sizeof(wireless));
}
