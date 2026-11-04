/* See ls_power.h. */

#include "ls_power.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_gauge.h"
#include "core/settings.h"
#include "core/display_ctl.h"
#include "core/ls_track_log.h"
#include "ls_board.h"
#include "ls_gps.h"
#include "ls_tui.h"
#include "ls_tui_screen.h"
#if LS_HAS_LORA
#include "ls_mesh.h"
#endif

static const char *TAG = "power";

extern const ls_tui_screen_t ls_scr_home;
#if LS_HAS_LORA
extern const ls_tui_screen_t ls_scr_mesh;
#endif

static ls_pwr_mode_t s_mode = LS_PWR_FULL;
static ls_pwr_keep_t s_keep = LS_KEEP_ALL;

/* What the dark stopped or started, so the light can put it back. */
static bool s_dark;
static bool s_gps_stopped, s_gps_started, s_track_started;
static bool s_mesh_stopped, s_mesh_started, s_mesh_tx;
static int  s_left_screen = -1;
static const char *s_radio_parked;   /* a receiver claimed with no screen of its own */
static char s_parked[48];

/* Asked from the console task, done on the UI task: the dark and the policy
   are the UI task's, and a change between its reads of them lit a panel over
   parked services. -1 is nothing asked. */
static volatile int s_req_mode = -1, s_req_keep = -1;

static const char *const MODE_NAMES[LS_PWR_MODES] = { "FULL", "SAVER", "WATCH" };
static const char *const MODE_WHAT[LS_PWR_MODES] = {
    "everything at full rate",
    "half light, dim at 30 s",
    "dark at 20 s, one job",
};
static const char *const KEEP_NAMES[LS_KEEPS] = { "EVERYTHING", "MESH", "P25", "GPS LOG" };

const char *ls_power_mode_name(int m) { return m >= 0 && m < LS_PWR_MODES ? MODE_NAMES[m] : "?"; }
const char *ls_power_mode_what(int m) { return m >= 0 && m < LS_PWR_MODES ? MODE_WHAT[m] : ""; }
const char *ls_power_keep_name(int k) { return k >= 0 && k < LS_KEEPS ? KEEP_NAMES[k] : "?"; }

static void apply(void)
{
    switch (s_mode) {
    case LS_PWR_SAVER: display_ctl_set_power_policy(50, 30, 0);  break;
    case LS_PWR_WATCH: display_ctl_set_power_policy(50, 10, 20); break;
    default:           display_ctl_set_power_policy(100, 0, 0);  break;
    }
    /* Leaving WATCH in the dark: light it, or nothing would. */
    if (s_mode != LS_PWR_WATCH && display_ctl_off()) display_ctl_set_off(false);
}

void ls_power_init(void)
{
    const int m = settings_get_power_mode(), k = settings_get_power_keep();
    s_mode = m >= 0 && m < LS_PWR_MODES ? (ls_pwr_mode_t)m : LS_PWR_FULL;
    s_keep = k >= 0 && k < LS_KEEPS ? (ls_pwr_keep_t)k : LS_KEEP_ALL;
    apply();
    ESP_LOGI(TAG, "mode %s, watch keeps %s", MODE_NAMES[s_mode], KEEP_NAMES[s_keep]);
}

ls_pwr_mode_t ls_power_mode(void) { return s_mode; }
ls_pwr_keep_t ls_power_keep(void) { return s_keep; }

void ls_power_set_mode(ls_pwr_mode_t m)
{
    if (m < 0 || m >= LS_PWR_MODES) return;
    s_mode = m;
    settings_set_power_mode((int)m);
    apply();
}

void ls_power_set_keep(ls_pwr_keep_t k)
{
    if (k < 0 || k >= LS_KEEPS) return;
    s_keep = k;
    settings_set_power_keep((int)k);
}

static void add_parked(const char *what)
{
    const size_t n = strlen(s_parked);
    snprintf(s_parked + n, sizeof(s_parked) - n, "%s%s", n ? ", " : "", what);
}

/* A screen with a radio of its own is left for HOME, which parks it. HOME,
   MESH and the settings screens hold no radio. */
static void leave_screen(bool mesh_ok)
{
    const int cur = ls_tui_screen_current();
    const int home = ls_tui_screen_index_of(&ls_scr_home);
#if LS_HAS_LORA
    const int mesh = ls_tui_screen_index_of(&ls_scr_mesh);
#else
    const int mesh = -1;
#endif
    if (cur < 0 || home < 0 || cur == home || (mesh_ok && cur == mesh)) return;
    const char *name = ls_tui_screen_name(cur);
    if (name && (!strcmp(name, "SET") || !strcmp(name, "RADIOS"))) return;
    s_left_screen = cur;
    add_parked(name ? name : "app");
    ls_tui_screen_show(home);
}

static void park(void)
{
    s_parked[0] = 0;
    s_gps_stopped = s_gps_started = s_track_started = false;
    s_mesh_stopped = s_mesh_started = s_mesh_tx = false;
    s_left_screen = -1;
    s_radio_parked = NULL;
    if (s_keep == LS_KEEP_ALL) return;

    if (s_keep == LS_KEEP_GPS) {
        /* the firmware's own demand, so the light can give back exactly it */
        if (!ls_gps_firmware_wanted() && ls_gps_start() == ESP_OK) s_gps_started = true;
        if (!ls_track_rec_requested()) {
            if (ls_track_rec_start() == ESP_OK) s_track_started = true;
            /* a start that failed (no card) still saved the intent: take it back */
            else ls_track_rec_stop();
        }
    } else if (ls_gps_firmware_wanted() && !ls_track_rec_requested()) {
        /* A walk being recorded keeps its GPS whatever WATCH keeps; an app's
           own demand is the app's, and ls_gps_stop leaves it alone. */
        ls_gps_stop();
        s_gps_stopped = true;
        add_parked("GPS");
    }
#if LS_HAS_LORA
    if (s_keep == LS_KEEP_MESH) {
        if (!ls_mesh_running() && ls_mesh_start() == ESP_OK) s_mesh_started = true;
    } else if (ls_mesh_running()) {
        s_mesh_tx = ls_mesh_tx_enabled();
        /* restored whatever the stop answered: a stop that timed out part way
           is still a mesh the owner had running */
        (void)ls_mesh_stop();
        s_mesh_stopped = true;
        add_parked("MESH");
    }
#endif
    if (s_keep == LS_KEEP_MESH || s_keep == LS_KEEP_GPS) {
        leave_screen(s_keep == LS_KEEP_MESH);
        /* HOME claims no receiver but does not release one either: a radio
           still claimed is let go explicitly, and claimed again on light. */
        const char *claimed = ls_tui_radio_claimed();
        if (claimed) {
            s_radio_parked = claimed;
            ls_tui_radio_want(NULL);
            if (s_left_screen < 0) add_parked(claimed);
        }
    }
    ESP_LOGW(TAG, "dark: keeping %s, stopped %s", KEEP_NAMES[s_keep], s_parked[0] ? s_parked : "nothing");
}

static void restore(void)
{
    if (s_gps_stopped) (void)ls_gps_start();
    if (s_gps_started) ls_gps_stop();
    if (s_track_started) ls_track_rec_stop();
#if LS_HAS_LORA
    if (s_mesh_stopped && ls_mesh_start() == ESP_OK && s_mesh_tx) ls_mesh_set_tx(true);
    if (s_mesh_started) (void)ls_mesh_stop();
#endif
    if (s_left_screen >= 0 && s_left_screen < ls_tui_screen_count()) ls_tui_screen_show(s_left_screen);
    else if (s_radio_parked) ls_tui_radio_want(s_radio_parked);
    if (s_parked[0]) ESP_LOGW(TAG, "lit: back %s", s_parked);
    s_gps_stopped = s_gps_started = s_track_started = false;
    s_mesh_stopped = s_mesh_started = s_mesh_tx = false;
    s_left_screen = -1;
    s_radio_parked = NULL;
}

/* A minute's current, kept an hour: PSRAM, the internal heap is the DMA pool. */
static EXT_RAM_BSS_ATTR short s_hist[LS_POWER_HISTORY];
static int s_hist_n, s_hist_at;
static int64_t s_hist_next_us;

static void history_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_hist_next_us) return;
    s_hist_next_us = now + 60LL * 1000000;
    int16_t ma = 0;
    int64_t age = 0;
    if (!ls_gauge_peek(&ma, &age)) return;
    s_hist[s_hist_at] = ma;
    s_hist_at = (s_hist_at + 1) % LS_POWER_HISTORY;
    if (s_hist_n < LS_POWER_HISTORY) s_hist_n++;
}

int ls_power_history(short *out, int max)
{
    int n = s_hist_n < max ? s_hist_n : max;
    for (int i = 0; i < n; i++)
        out[i] = s_hist[(s_hist_at - n + i + LS_POWER_HISTORY) % LS_POWER_HISTORY];
    return n;
}

void ls_power_tick(void)
{
    history_tick();
    const int rm = s_req_mode, rk = s_req_keep;
    if (rk >= 0) { s_req_keep = -1; ls_power_set_keep((ls_pwr_keep_t)rk); }
    if (rm >= 0) { s_req_mode = -1; ls_power_set_mode((ls_pwr_mode_t)rm); }
    const bool dark = s_mode == LS_PWR_WATCH && display_ctl_off();
    if (dark == s_dark) return;
    s_dark = dark;
    if (dark) park();
    else {
        restore();
        /* Nothing reached the panel while it was dark. */
        ls_tui_invalidate();
    }
}

int ls_power_frame_ms(int normal_ms)
{
    if (s_dark) return 250;
    if (s_mode != LS_PWR_FULL && normal_ms < 66) return 66;
    return normal_ms;
}

bool ls_power_dark(void) { return s_dark; }

/* 'power': the mode, what WATCH keeps, and what the dark parked.
   'power full|saver|watch', 'power keep all|mesh|p25|gps'. */
static int power_cmd(int argc, char **argv)
{
    static const char *const MODE_ARG[LS_PWR_MODES] = { "full", "saver", "watch" };
    static const char *const KEEP_ARG[LS_KEEPS] = { "all", "mesh", "p25", "gps" };
    bool asked = false;
    if (argc == 2)
        for (int m = 0; m < LS_PWR_MODES; m++)
            if (!strcmp(argv[1], MODE_ARG[m])) { s_req_mode = m; asked = true; }
    if (argc == 3 && !strcmp(argv[1], "keep"))
        for (int k = 0; k < LS_KEEPS; k++)
            if (!strcmp(argv[2], KEEP_ARG[k])) { s_req_keep = k; asked = true; }
    /* the UI task takes it on its next frame */
    if (asked) for (int i = 0; i < 20 && (s_req_mode >= 0 || s_req_keep >= 0); i++) vTaskDelay(pdMS_TO_TICKS(25));
    printf("power: mode %s (%s), watch keeps %s, screen %s\n", MODE_NAMES[s_mode], MODE_WHAT[s_mode],
           KEEP_NAMES[s_keep], s_dark ? "dark" : "lit");
    if (s_dark) printf("power: parked for the dark: %s\n", s_parked[0] ? s_parked : "nothing");
    short h[LS_POWER_HISTORY];
    const int n = ls_power_history(h, LS_POWER_HISTORY);
    if (n) printf("power: %d minute samples, last %d mA\n", n, h[n - 1]);
    return 0;
}

void ls_power_console_register(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "power",
        .help = "Power mode: power [full|saver|watch] | power keep all|mesh|p25|gps",
        .hint = NULL, .func = power_cmd, .argtable = NULL,
    };
    esp_console_cmd_register(&cmd);
}

const char *ls_power_parked(void) { return s_dark ? s_parked : ""; }
