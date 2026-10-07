#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_radios.c"

static ls_radio_endpoint_info_t rtl, hackrf;
static ls_mixrf_status_t mix;
static bool external, acknowledged, running;
static esp_err_t switch_error, stop_error;
static unsigned switches, saved;

ls_radio_err_t ls_radio_endpoint_get(const char *id, ls_radio_endpoint_info_t *out)
{ *out = !strcmp(id, LS_RADIO_ENDPOINT_RTL_USB) ? rtl : hackrf; return LS_RADIO_OK; }
void ls_tui_radio_want(const char *mode) { (void)mode; }
bool ls_gps_running(void) { return false; }
void ls_gps_get(ls_gps_state_t *out) { memset(out, 0, sizeof(*out)); }
esp_err_t ls_gps_start(void) { return ESP_OK; }
void ls_gps_stop(void) { }
bool ls_mesh_running(void) { return running; }
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
bool settings_get_ble_at_boot(void) { return false; }
bool settings_get_wifi_at_boot(void) { return false; }
void settings_set_ble_at_boot(bool on) { }
void settings_set_wifi_at_boot(bool on) { }
bool ls_tui_is_wide(void) { return false; }
char ls_motion_pip(bool on) { return '*'; }
void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue) { }

LS_CASE(sdr_missing_failed_and_parked_do_not_count_as_powered)
{
    memset(&rtl, 0, sizeof(rtl)); memset(&hackrf, 0, sizeof(hackrf));
    int base = count_live();
    LS_EQ_INT(rtl_read(), RS_ABSENT);
    sdr_set(true);
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
    external = acknowledged = false; switches = saved = 0; switch_error = ESP_OK;
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
    for (int i = 0; warning[i]; ++i)
        LS_EQ_INT(back[5 * 40 + 2 + i].ch, warning[i]);
    touch(s_ant_yes.x, s_ant_yes.y);
    LS_CHECK(external && acknowledged && !s_ant_confirm);
}
