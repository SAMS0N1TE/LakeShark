/* RADIOS: what is transmitting, receiving, or drawing current, and a way to stop it. */

#include "../../ls_tui_screen.h"
#include "board/ls_board_hw.h"
#include "core/settings.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"

#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "../../ls_deskui.h"
#include "../../ls_box.h"
#include "../../ls_i18n.h"
#include "../../ls_quick.h"
#include "../../ls_options.h"
#include "../../ls_rffe_ui.h"
#include "../../ls_app.h"
#include "../../ls_wireless.h"
#include "../../ls_lap_service.h"

/* ls_board.h and not ls_caps.h: the caps header has an #error in it saying
   exactly that, which is the right way for a header to be private. */
#include "ls_board.h"
#include "ls_gps.h"
#ifdef LS_BOARD_MIX_CC_CS
#include "ls_mixrf.h"
#include "ls_nfc_suite.h"
#endif

/* Presence and activity come from the endpoint that owns each USB radio. */
#include "radio_endpoint.h"

#if LS_HAS_LORA
#include "ls_mesh.h"
#include "../../ls_radio_select.h"
#endif

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

typedef enum {
    RS_ABSENT = 0,   /* the part is not on this board, or not driven yet */
    RS_OFF,
    RS_ON,
    RS_BUSY,         /* on and actually moving data                     */
    RS_STARTING,
    RS_FAILED,
} radio_state_t;

typedef struct {
    const char *name;
    const char *what;                 /* one line: what it costs you     */
    radio_state_t (*read)(void);
    void (*set)(bool on);             /* NULL when it cannot be switched */

    const char *(*state_text)(void);  /* NULL: the on/off words          */
    const char *(*action_text)(void); /* NULL: TURN ON / TURN OFF        */
    bool        no_power;             /* selects a path, draws nothing   */
    bool        at_boot;              /* a preference for the next boot,
                                         not a load running now          */
} radio_row_t;

/* ------------------------------------------------------------------ rows -- */

static const char *s_error;
/* LORA on/off in progress on its worker: 1 turning on, 2 turning off. */
static volatile int s_mesh_busy;
static char s_error_text[80];
static tui_rect s_options_hit;

static int mmcx_use_get(const ls_opt_t *o)
{ (void)o; return ls_board_hw_antenna_mmcx_tx(); }
static void mmcx_use_set(const ls_opt_t *o, int v)
{ (void)o; ls_board_hw_antenna_set_mmcx_tx(v != 0); settings_set_antenna_mmcx_tx(v != 0); }
static const char *const MMCX_USE_NAMES[] = { "RX ONLY", "RX+TX" };
static void options_back(const ls_opt_t *o)
{ (void)o; ls_opt_close(); }
static const char *lr_only(const ls_opt_t *o)
{
    (void)o;
#if LS_HAS_LORA
    ls_rsel_hw_t hw; ls_rsel_hw(&hw); return hw.lora_lr20xx ? NULL : "LR2021 boards";
#else
    return "LR2021 boards";
#endif
}
#if LS_HAS_C6
static int ble_boot_get(const ls_opt_t *o) { (void)o; return settings_get_ble_at_boot(); }
static void ble_boot_set(const ls_opt_t *o, int v) { (void)o; settings_set_ble_at_boot(v != 0); }
static int wifi_boot_get(const ls_opt_t *o) { (void)o; return settings_get_wifi_at_boot(); }
static void wifi_boot_set(const ls_opt_t *o, int v) { (void)o; settings_set_wifi_at_boot(v != 0); }
#endif
#if LS_HAS_C6
static const char *const WARNING_NAMES[] = { "OFF", "ON" };
#endif
static const ls_opt_t RADIO_OPTIONS[] = {
    { .label = "MMCX1 USE", .kind = LS_OPT_TOGGLE,
      .names = MMCX_USE_NAMES, .get = mmcx_use_get, .set = mmcx_use_set },
#if LS_HAS_C6
    { .label = "BLE AT BOOT", .kind = LS_OPT_TOGGLE,
      .names = WARNING_NAMES, .get = ble_boot_get, .set = ble_boot_set },
    { .label = "WI-FI AT BOOT", .kind = LS_OPT_TOGGLE,
      .names = WARNING_NAMES, .get = wifi_boot_get, .set = wifi_boot_set },
#endif
    /* The SDRs' front ends, the same lists every app on them opens. */
    { .label = "RTL-SDR FRONT END", .kind = LS_OPT_MENU, .sub = &ls_rffe_ctx_rtl },
    { .label = "HACKRF FRONT END", .kind = LS_OPT_MENU, .sub = &ls_rffe_ctx_hackrf },
    { .label = "LR2021 FRONT END", .kind = LS_OPT_MENU, .sub = &ls_rffe_ctx_lr, .why_not = lr_only },
    { .label = "BACK", .kind = LS_OPT_ACTION, .act = options_back, .leaves = true },
};
static const ls_opt_ctx_t RADIO_CONTEXT = {
    .name = "RADIOS", .job = -1, .radio = LS_RSEL_NONE,
    LS_OPT_ROWS(RADIO_OPTIONS),
};

static void options_open(void)
{
    ls_opt_open(&RADIO_CONTEXT);
}

/* A parked endpoint does not establish that its hardware is drawing power. */
static radio_state_t sdr_endpoint_read(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present)
        return RS_ABSENT;
    if (info.paused) return RS_OFF;
    /* Success clears current faults; ordinary stream events are not faults. */
    if (info.last_error != LS_RADIO_OK && info.last_error != LS_RADIO_ERR_STOPPED &&
        info.last_error != LS_RADIO_ERR_TIMEOUT && info.last_error != LS_RADIO_ERR_BUSY)
        return RS_FAILED;
    if (info.streaming) return RS_BUSY;
    return info.leased && !info.configured ? RS_STARTING : RS_OFF;
}

static radio_state_t rtl_read(void) { return sdr_endpoint_read(LS_RADIO_ENDPOINT_RTL_USB); }
static radio_state_t hackrf_read(void) { return sdr_endpoint_read(LS_RADIO_ENDPOINT_HACKRF_USB); }
static const char *sdr_word(radio_state_t st);
/* An idle SDR the watchdog has powered down reads "standby", not "parked":
   the USB-A port's 5 V cannot be switched, so this is the dongle's off. */
static const char *sdr_state(const char *id, radio_state_t st)
{
    ls_radio_endpoint_info_t info;
    if (st == RS_OFF && ls_radio_endpoint_get(id, &info) == LS_RADIO_OK &&
        info.present && info.standby)
        return "standby";
    return sdr_word(st);
}
static const char *rtl_state(void) { return sdr_state(LS_RADIO_ENDPOINT_RTL_USB, rtl_read()); }
static const char *hackrf_state(void) { return sdr_state(LS_RADIO_ENDPOINT_HACKRF_USB, hackrf_read()); }

/* STOP turns off the app using the radio. It used to pause the receiver
   (SDR) or block the chip (LoRa) under an app that kept wanting it, so the
   app took it straight back at ALLOW: STOP never released anything. Now the
   app itself is switched off - its mode parked, its watch ended - and the
   radio is free with nothing to allow. An owner not listed here still gets
   the old block. Weak, so a build without one of these apps still links. */
static ls_lap_service_info_t s_svc;
static bool svc_get(void);
bool rec_watch_enable(bool on) __attribute__((weak));
bool rec_watch_enabled(void) __attribute__((weak));
void rec_disarm(void) __attribute__((weak));
void rec_catch_stop(void) __attribute__((weak));
bool ls_drone_watch_enable(bool on) __attribute__((weak));
void cell_monitor_stop(void) __attribute__((weak));
void cell_iq_stop(void) __attribute__((weak));

typedef enum { ST_NONE, ST_MODE, ST_WATCH, ST_REC, ST_CATCH, ST_DRONES, ST_CELL, ST_CELL_IQ, ST_SERVICE } stop_kind_t;
static stop_kind_t stop_kind(const char *owner, const char **app)
{
    static const struct { const char *prefix, *app; stop_kind_t kind; } OWNERS[] = {
        { "subghz", "SUB-GHZ", ST_WATCH }, { "wf_", "SUB-GHZ", ST_WATCH },
        { "tui_rec", "REC", ST_REC }, { "rec", "REC", ST_REC },
        { "adsb", "ADS-B", ST_MODE }, { "p25", "P25", ST_MODE }, { "pager", "P25", ST_MODE },
        { "fm", "FM", ST_MODE }, { "CATCH", "CATCH", ST_CATCH }, { "catch", "CATCH", ST_CATCH },
        /* CELL WATCH's I/Q capture is its own loop that retakes the radio;
           stopping the monitor leaves it running. Before "cell". */
        { "drone", "DRONES", ST_DRONES }, { "cell-iq", "CELL", ST_CELL_IQ }, { "cell", "CELL", ST_CELL }, { "lap", "APP", ST_SERVICE },
    };
    if (!owner || !owner[0]) return ST_NONE;
    for (size_t i = 0; i < sizeof(OWNERS) / sizeof(OWNERS[0]); i++) {
        if (strncmp(owner, OWNERS[i].prefix, strlen(OWNERS[i].prefix))) continue;
        stop_kind_t k = OWNERS[i].kind;
        /* The LR2021's I/Q session is WATCH's only while WATCH is on. */
        if (k == ST_WATCH && !(rec_watch_enabled && rec_watch_enabled())) return ST_NONE;
        if (k == ST_SERVICE && !svc_get()) return ST_NONE;
        if (app) *app = k == ST_SERVICE ? s_svc.name : OWNERS[i].app;
        return k;
    }
    return ST_NONE;
}
static bool stop_owner(const char *owner, const char **app)
{
    switch (stop_kind(owner, app)) {
    case ST_WATCH:
    case ST_REC:
        if (rec_watch_enabled && rec_watch_enabled() && rec_watch_enable) (void)rec_watch_enable(false);
        if (rec_disarm) rec_disarm();
        ls_tui_radio_want(NULL);
        return true;
    case ST_MODE:    ls_tui_radio_want(NULL); return true;
    case ST_CATCH:   if (!rec_catch_stop) return false; rec_catch_stop(); return true;
    case ST_DRONES:  return ls_drone_watch_enable && ls_drone_watch_enable(false);
    case ST_CELL:    if (!cell_monitor_stop) return false; cell_monitor_stop(); return true;
    case ST_CELL_IQ: if (!cell_iq_stop) return false; cell_iq_stop(); return true;
    case ST_SERVICE: return ls_lap_service_request_stop(s_svc.id);
    default:         return false;
    }
}
static const char *stop_label(const char *owner)
{
    static char t[24];
    const char *app = NULL;
    if (stop_kind(owner, &app) == ST_NONE) return NULL;
    snprintf(t, sizeof(t), "STOP %.18s", app);
    return t;
}

static void sdr_set_endpoint(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present) return;
    const char *app = NULL;
    if (!info.paused && info.leased && stop_owner(info.owner, &app)) {
        snprintf(s_error_text, sizeof(s_error_text), "%s off: %s is free", app, info.name);
        s_error = s_error_text;
        return;
    }
    const ls_radio_err_t err = ls_radio_endpoint_pause(id, info.owner, !info.paused);
    if (err != LS_RADIO_OK) {
        snprintf(s_error_text, sizeof(s_error_text), "%s: %s (%s)",
                 info.name, ls_radio_err_name(err), info.owner);
        s_error = s_error_text;
    }
}
static void rtl_set(bool on) { (void)on; sdr_set_endpoint(LS_RADIO_ENDPOINT_RTL_USB); }
static void hackrf_set(bool on) { (void)on; sdr_set_endpoint(LS_RADIO_ENDPOINT_HACKRF_USB); }
static const char *sdr_action(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present) return "unavailable";
    const char *stop = !info.paused && info.leased ? stop_label(info.owner) : NULL;
    return stop ? stop : info.paused ? "ALLOW RX" : "STOP / BLOCK RX";
}
static const char *rtl_action(void) { return sdr_action(LS_RADIO_ENDPOINT_RTL_USB); }
static const char *hackrf_action(void) { return sdr_action(LS_RADIO_ENDPOINT_HACKRF_USB); }

static radio_state_t gps_read(void)
{

    if (!ls_gps_running()) return RS_OFF;
    ls_gps_state_t g;
    ls_gps_get(&g);
    return g.fix ? RS_BUSY : RS_ON;
}

static const char *gps_state(void)
{
    if (!ls_gps_running()) return "off";
    ls_gps_state_t g;
    ls_gps_get(&g);
    return g.fix ? "fix" : "searching";
}

static void gps_set(bool on)
{
    if (on) {
        const esp_err_t err = ls_gps_start();
        s_error = err == ESP_OK ? NULL : "GPS start failed (memory / UART)";
    }
    else    ls_gps_stop();
}

#if LS_HAS_LORA
static radio_state_t mesh_read(void)
{
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (!s.present && !ls_mesh_running()) return RS_OFF;
    return s.receiving || s.transmitting ? RS_BUSY : s.present ? RS_ON : RS_OFF;
}

/* The task holding the chip, as the app it belongs to. */
static const char *holder_name(const char *task)
{
    static const struct { const char *prefix, *name; } NAMES[] = {
        { "adsb", "ADSB" }, { "fm", "FM/P25" }, { "p25", "P25" }, { "catch", "CATCH" },
        { "drone", "DRONES" }, { "subghz", "SUB-GHZ" }, { "exp", "EXPERIMENTS" },
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (!strncmp(task, NAMES[i].prefix, strlen(NAMES[i].prefix))) return NAMES[i].name;
    return task;
}

static const char *mesh_state(void)
{
    static char text[64];
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (s.blocked)
        snprintf(text, sizeof(text), "%s %s", ls_tr(s.chip), ls_tr("blocked"));
    else if (s.held && strcmp(s.owner, "MeshCore") && strcmp(s.owner, "none"))
        snprintf(text, sizeof(text), "%s %s %s", ls_tr(s.chip), ls_tr("held by"), holder_name(s.owner));
    else
        snprintf(text, sizeof(text), "%s %s", ls_tr(s.chip),
                 ls_tr(s.transmitting ? "TX" : s.receiving ? "RX" : s.held ? "held" :
                       s.armed ? "armed" : "parked"));
    return text;
}

static const char *mesh_action(void)
{
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (s_mesh_busy) return s_mesh_busy == 1 ? "TURNING ON..." : "TURNING OFF...";
    if (s.blocked) return "ALLOW LORA";
    const char *stop = s.held ? stop_label(s.owner) : NULL;
    if (stop) return stop;
    if (s.held) return "STOP / BLOCK LORA";
    return ls_mesh_running() ? "TURN OFF" : "TURN ON";
}

static void mesh_worker(void *arg)
{
    const bool on = (intptr_t)arg != 0;
    const esp_err_t err = on ? ls_mesh_start() : ls_mesh_stop();
    s_error = err == ESP_OK ? NULL : on ? "LORA start failed" : "LORA stop failed";
    s_mesh_busy = 0;
    /* This used to run on the UI task's stack. A stop with messages not yet
       on the card writes them from here, so say if it ever ran close. */
    const UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
    if (left < 1024) ESP_LOGW("radios", "LORA switch left %u bytes of its stack", (unsigned)left);
    vTaskDelete(NULL);
}

/* Held by an app (ADSB or P25 on the LoRa chip, a sweep, CATCH...), the tap
   takes the chip back and keeps it free until ALLOW: the holder sees it
   wanted and lets go, as it does for any other foreground use. */
static void mesh_set(bool on)
{
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (s.blocked) {
        ls_mesh_radio_block(false);
        s_error = "LORA allowed: apps can take it again";
        return;
    }
    const char *app = NULL;
    if (s.held && stop_owner(s.owner, &app)) {
        snprintf(s_error_text, sizeof(s_error_text), "%s off: LORA %s", app,
                 ls_mesh_running() ? "back to MeshCore" : "free");
        s_error = s_error_text;
        return;
    }
    if (s.held) {
        ls_mesh_radio_block(true);
        snprintf(s_error_text, sizeof(s_error_text), ls_tr("LORA taken back from %s; ALLOW gives it back"),
                 holder_name(s.owner));
        s_error = s_error_text;
        return;
    }
    if (s_mesh_busy) return;
    on = !ls_mesh_running();
    /* Off waits for MeshCore to wind down, park the chip and write its logs
       to the card: 1.5 s on the bench, all of it a frozen screen when it ran
       here. The worker does it; the button says so meanwhile. */
    s_mesh_busy = on ? 1 : 2;
    s_error = on ? "LORA turning on..." : "LORA turning off...";
    if (xTaskCreatePinnedToCore(mesh_worker, "radios_mesh", 6144, (void *)(intptr_t)on, 3,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        s_mesh_busy = 0;
        s_error = "No memory to switch LORA";
    }
}
#endif

#ifdef LS_BOARD_MIX_CC_CS
static radio_state_t cc_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.cc?RS_ABSENT:(s.receiving||s.transmitting)?RS_BUSY:RS_ON;}
static const char *cc_state(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.cc?"absent":s.transmitting?"TX replay":s.receiving?"receiving":"on";}
static radio_state_t nrf_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.nrf?RS_ABSENT:s.scanning?RS_BUSY:RS_ON;}
static radio_state_t nfc_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.nfc?RS_ABSENT:(s.card_scanning||s.nfc_watching||ls_nfc_suite_busy())?RS_BUSY:RS_ON;}
static void open_mix(bool on){(void)on;for(int i=0;i<ls_app_count();i++){const ls_app_t *a=ls_app_at(i);if(a && !strcmp(a->id,"mixrf")){ls_app_open(i);return;}}}
static const char *mix_action(void){return "OPEN MIX-RF";}
#else
static radio_state_t absent_read(void) { return RS_ABSENT; }
#endif

static radio_state_t ant_read(void)
{
    return ls_board_hw_antenna_is_external() ? RS_ON : RS_OFF;
}
/* One tap, no dialog. MMCX1 receives; whether it transmits too is the
   MMCX1 USE option, and the box's second line says which. */
static void ant_set(bool external)
{
    const esp_err_t err = ls_board_hw_antenna_external(external);
    s_error = err == ESP_OK ? NULL : "Antenna: transmit in progress, try again";
    if (err == ESP_OK) settings_set_antenna_external(external);
}
static const char *ant_state(void)
{
    if (!ls_board_hw_antenna_is_external()) return "internal";
    return ls_board_hw_antenna_mmcx_tx() ? "MMCX1 rx+tx" : "MMCX1 rx";
}
static const char *ant_action(void)
{
    return ls_board_hw_antenna_is_external() ? "USE INTERNAL" : "USE MMCX1";
}

/* A native app's background service (SDK 2.1): it may hold a radio and
   keeps running after its screen closed, so it is listed with the radios,
   with the one thing the owner may want to do to it. */
static ls_lap_service_info_t s_svc;
static bool svc_get(void) { return ls_lap_services(&s_svc, 1) > 0; }
static radio_state_t svc_read(void) { return svc_get() ? RS_BUSY : RS_ABSENT; }
static void svc_set(bool on) { (void)on; if(svc_get()) ls_lap_service_request_stop(s_svc.id); }
static const char *svc_state(void)
{
    static char t[24];
    if (!svc_get()) return "none running";
    snprintf(t, sizeof(t), "%.15s %s", s_svc.name, s_svc.shown ? "open" : "behind");
    return t;
}
static const char *svc_action(void)
{
    static char t[24];
    if (!svc_get()) return "none";
    snprintf(t, sizeof(t), "STOP %.15s", s_svc.name);
    return t;
}

#if LS_HAS_C6
/* The worker owns the C6; keep its snapshot off the TUI stack. */
static EXT_RAM_BSS_ATTR ls_wireless_snapshot_t s_wireless;
static radio_state_t ble_read(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.bt_available ? RS_ABSENT : s_wireless.bt_failed ? RS_FAILED : !s_wireless.bt_running ? RS_OFF : s_wireless.bt_ready ? RS_BUSY : RS_ON; }
static radio_state_t wifi_read(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.wifi_available ? RS_ABSENT : !s_wireless.wifi_running ? RS_OFF : s_wireless.wifi_connected ? RS_BUSY : RS_ON; }
static const char *ble_state(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.bt_available ? "unavailable" : s_wireless.bt_failed ? "failed" : !s_wireless.bt_running ? "off" : s_wireless.bt_status[0] ? s_wireless.bt_status : "starting"; }
static const char *wifi_state(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.wifi_available ? "unavailable" : !s_wireless.wifi_running ? "off" : s_wireless.wifi_connected ? "connected" : s_wireless.busy ? "busy" : "disconnected"; }
#endif
static void open_link(bool on)
{ (void)on; for (int i = 0; i < ls_app_count(); i++) { const ls_app_t *a = ls_app_at(i); if (a && !strcmp(a->id, "link")) { ls_app_open(i); return; } } }
static const char *link_action(void) { return "OPEN LINK"; }

static const radio_row_t ROWS[] = {
    { "RTL-SDR", "STOP turns off the app using it", rtl_read, rtl_set, rtl_state, rtl_action },
    { "HackRF", "STOP turns off the app using it", hackrf_read, hackrf_set, hackrf_state, hackrf_action },
#if LS_HAS_LORA
    { "LORA", "MeshCore from boot; STOP turns off the app using it",
      mesh_read, mesh_set, mesh_state, mesh_action },
#endif
    { "GPS",  "keeps a receiver fed to hold a fix",
      gps_read, gps_set, gps_state },
    { "ANTENNA", "internal, or MMCX1 for receive",
      ant_read, ant_set, ant_state, ant_action, true },
    { "APP SERVICE", "a native app working in the background",
      svc_read, svc_set, svc_state, svc_action, true },
#if LS_HAS_C6
    { "BLE",  "Live C6 / runtime controls in LINK",
      ble_read, open_link, ble_state, link_action },
    { "WI-FI", "Live C6 / runtime controls in LINK",
      wifi_read, open_link, wifi_state, link_action },
#endif
#ifdef LS_BOARD_MIX_CC_CS
    { "CC1101", "Keyboard sub-GHz receive monitor",cc_read,open_mix,cc_state,mix_action },
    { "NRF24", "Keyboard 2.4 GHz energy survey",nrf_read,open_mix,NULL,mix_action },
    { "NFC", "Keyboard card reader / workbench",nfc_read,open_mix,NULL,mix_action },
#else
    { "CC1101", "sub-GHz front end, no driver yet",
      absent_read, NULL },
    { "NFC",  "reader front end, no driver yet",
      absent_read, NULL },
#endif
};
#define N_ROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static int s_sel;

/* ------------------------------------------------------------------ draw -- */

static const char *state_word(radio_state_t s)
{
    switch (s) {
    case RS_BUSY:   return "ACTIVE";
    case RS_ON:     return "on";
    case RS_OFF:    return "off";
    case RS_STARTING: return "starting";
    case RS_FAILED: return "failed";
    default:        return "absent";
    }
}

static const char *sdr_word(radio_state_t st)
{ return st == RS_OFF ? "parked" : st == RS_ABSENT ? "unavailable" : state_word(st); }

/* What pressing it would do, which is not the same as what it is.

   A switch labelled with its own state makes the reader do the inversion,
   and half of them get it wrong - the classic "does ON mean it is on, or
   that pressing it turns it on". The border says the state and the key says
   the action, so neither has to be guessed from the other. */
static const char *action_word(radio_state_t s, bool can)
{
    if (!can) return "no driver";
    return (s == RS_OFF) ? "TURN ON" : "TURN OFF";
}

static int count_live(void)
{
    int n = 0;
    for (int i = 0; i < N_ROWS; i++) {
        if (ROWS[i].no_power) continue;   /* a path, not a load */
        if (ROWS[i].at_boot) continue;    /* next boot, not now */
        const radio_state_t s = ROWS[i].read();
        if (s == RS_ON || s == RS_BUSY) n++;
    }
    return n;
}

/* Each radio is a box you press, not a row you select. */

static tui_rect s_hit[12];
static int s_hit_row[12];
static int s_page_first, s_page_size;
static tui_rect s_page_prev, s_page_next;
static int      s_hit_n;

/* What a row is doing right now, beyond its state word, when there is
   something to say: who holds the receiver, the mesh socket, the service's
   timing, the antenna path. */
static bool row_detail(int i, char *what, size_t n)
{
#if LS_HAS_LORA
    if (ROWS[i].set == mesh_set) {
        ls_mesh_socket_t socket;
        ls_mesh_socket_get(&socket);
        snprintf(what, n, "%s / %s", socket.owner, socket.mode);
        return true;
    }
#endif
    if (ROWS[i].set == svc_set && svc_get()) {
        snprintf(what, n, ls_tr("every %lu ms, %lu ticks, worst %lu us"),
                 (unsigned long)s_svc.period_ms, (unsigned long)s_svc.stats.ticks,
                 (unsigned long)s_svc.stats.worst_us);
        return true;
    }
    if (ROWS[i].set == ant_set && ls_board_hw_antenna_is_external()) {
        snprintf(what, n, "%s", ls_tr(ls_board_hw_antenna_mmcx_tx() ?
                 "TX on MMCX1: band antenna on it" : "TX goes out the internal antenna"));
        return true;
    }
    if (ROWS[i].set == rtl_set || ROWS[i].set == hackrf_set) {
        ls_radio_endpoint_info_t info;
        if (ls_radio_endpoint_get(ROWS[i].set == rtl_set ? LS_RADIO_ENDPOINT_RTL_USB :
                                  LS_RADIO_ENDPOINT_HACKRF_USB, &info) == LS_RADIO_OK) {
            if (info.leased && !info.standby) {
                snprintf(what, n, ls_tr("Owner: %s"), info.owner);
                return true;
            }
            if (info.present && info.standby) {
                snprintf(what, n, "%s", ls_tr("Standby: tuner off, wakes for the next app"));
                return true;
            }
            if (info.present && !info.streaming && info.standby_in_ms) {
                snprintf(what, n, ls_tr("Idle, powered: standby in %lu s"),
                         (unsigned long)((info.standby_in_ms + 999) / 1000));
                return true;
            }
        }
    }
    return false;
}

static void draw_one(tui_surface *sf, tui_rect a, int i, int bh)
{
    const uint8_t dim  = A(DIM_FG, TUI_BLACK);
    const radio_state_t st = ROWS[i].read();
    const bool can = ROWS[i].set && (st != RS_ABSENT || ROWS[i].set == open_link);
    const bool sel = (i == s_sel);

    const uint8_t edge = can ? A(TUI_CYAN, TUI_BLACK) : dim;
    const uint8_t name = can ? A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : dim;
    const uint8_t val  = !can              ? dim
                       : st == RS_BUSY     ? A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                       : st == RS_ON       ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                                           : dim;

    ls_panel_box(sf, a, NULL, can ? TUI_CYAN : (TUI_BLACK | TUI_BRIGHT));

    /* Name and state in the top border, the way every panel on this
       interface is labelled. */
    tui_put_char(sf, a, a.x + 1, a.y, ' ', edge);
    const char *row_name = ls_tr(ROWS[i].name);
    tui_put_str(sf, a, a.x + 2, a.y, row_name, name);
    tui_put_char(sf, a, a.x + 2 + (int)strlen(row_name), a.y, ' ', edge);

    const char *w = ls_tr(ROWS[i].state_text ? ROWS[i].state_text() : state_word(st));
    const int wn = (int)strlen(w);
    if (a.w > wn + 8) {
        tui_put_char(sf, a, a.x + a.w - wn - 3, a.y, ' ', edge);
        tui_put_str(sf, a, a.x + a.w - wn - 2, a.y, w, val);
        tui_put_char(sf, a, a.x + a.w - 2, a.y, ' ', edge);

    }

    int y = a.y + 1;
    char what[72];
    const bool detail = row_detail(i, what, sizeof(what));
    if (bh >= 6 || detail) {
        if (!detail) snprintf(what, sizeof(what), "%.*s", a.w - 4, ls_tr(ROWS[i].what));
        else if (a.w - 4 >= 0 && a.w - 4 < (int)sizeof(what)) what[a.w - 4] = '\0';
        tui_put_str(sf, a, a.x + 2, y, what, dim);
        y++;
    }

    /* The key: the interior, in the quarter-block shade the rest of this
       interface uses for a pressable field, with what pressing it does
       written across the middle. Not a solid rectangle - that is the pixel
       toolkit's answer and this has an alphabet. */
    const int kh = a.y + a.h - 1 - y;
    if (kh >= 1) {
        const uint8_t field = !can ? dim
                            : sel  ? A(TUI_BLACK, TUI_CYAN | TUI_BRIGHT)
                                   : A(TUI_CYAN, TUI_BLACK);
        for (int r = 0; r < kh; r++)
            for (int c = 1; c < a.w - 1; c++)
                tui_put_char(sf, a, a.x + c, y + r, LS_TUI_SHADE_25, field);

        const char *act = ls_tr(ROWS[i].action_text ? ROWS[i].action_text()
                                                    : action_word(st, can));
        const int an = (int)strlen(act);
        const int ax = a.x + (a.w - an) / 2;
        const int ay = y + (kh - 1) / 2;
        const uint8_t at = !can ? dim
                         : sel  ? A(TUI_BLACK, TUI_CYAN | TUI_BRIGHT)
                                : A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        if (ax - 1 > a.x) tui_put_char(sf, a, ax - 1, ay, ' ', at);
        if (ax + an < a.x + a.w - 1) tui_put_char(sf, a, ax + an, ay, ' ', at);
        tui_put_str(sf, a, ax, ay, act, at);
    }

    if (s_hit_n < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
    {
        s_hit_row[s_hit_n] = i;
        s_hit[s_hit_n++] = a;
    }
}

/* THE DESK FORM: a list view. One row a radio - a lamp, its name, what it
   is doing, what it is for - and the action as a key at the right end. A
   click on the key does it; a click on the row selects it. */
static tui_rect s_row_hit[12];

static tui_rect s_tool_act;      /* the toolbar's key for the selected row (desk) */

/* A desk window: one toolbar (the selected radio's action, OPTIONS, the
   pages), the radios as a list view in a white well with a header row and a
   selection bar (the action column is words: click it, or the toolbar key,
   to act), the selected radio in a group box under it. */
static void draw_compact(tui_surface *sf, tui_rect area)
{
    const uint8_t text = ls_ink(LS_INK_TEXT, A(TUI_WHITE, TUI_BLACK));
    const uint8_t dim = ls_ink(LS_INK_DIM, A(DIM_FG, TUI_BLACK));
    tui_fill(sf, area, ' ', A(TUI_WHITE, TUI_BLACK));
    /* The toolbar. */
    int tx = area.x;
    s_tool_act = tui_rect_make(0, 0, 0, 0);
    if (s_sel < N_ROWS) {
        const radio_state_t st = ROWS[s_sel].read();
        const bool can = ROWS[s_sel].set && (st != RS_ABSENT || ROWS[s_sel].set == open_link);
        const ls_btn_t key = { .label = ROWS[s_sel].action_text ? ROWS[s_sel].action_text() : action_word(st, can),
                               .dim = !can };
        const int kw = ls_key1_draw(sf, area, tx, area.y, &key, false);
        s_tool_act = tui_rect_make(tx, area.y, kw, 1);
        tx += kw + 1;
    }
    const ls_btn_t opt = ls_opt_button(&RADIO_CONTEXT);
    const int ow = ls_key1_draw(sf, area, tx, area.y, &opt, s_sel == N_ROWS);
    s_options_hit = tui_rect_make(tx, area.y, ow, 1);
    tx += ow + 2;

    /* The list view: a sunken frame, a header of buttons, rows on the well. */
    const int x0 = area.x, right = area.x + area.w;
    const int avail = area.h - 1 - 3 - 7;              /* toolbar, frame + header, the group under it */
    s_page_size = avail >= N_ROWS ? N_ROWS : avail >= 4 ? avail : area.h - 4 < N_ROWS ? (area.h - 4 > 0 ? area.h - 4 : 1) : N_ROWS;
    if (s_sel < N_ROWS) s_page_first = (s_sel / s_page_size) * s_page_size;
    const int shown = N_ROWS - s_page_first < s_page_size ? N_ROWS - s_page_first : s_page_size;
    const int fh = area.h - 1 - 7 > s_page_size + 3 ? area.h - 1 - 7 : s_page_size + 3;   /* the list fills down to the group */
    const tui_rect frame = tui_rect_make(x0, area.y + 1, area.w, fh);
    ls_dui_edge(sf, frame, true);
    const tui_rect head = tui_rect_make(frame.x + 1, frame.y + 1, frame.w - 2, 1);
    const tui_rect body = tui_rect_make(frame.x + 1, frame.y + 2, frame.w - 2, frame.h - 3);
    const uint8_t cap = ls_ink(LS_INK_KEY, A(TUI_CYAN, TUI_BLACK));
    tui_fill(sf, head, ' ', cap);
    tui_fill(sf, body, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_ink_well(body);
    const int x = body.x + 1, act_x = right - 18;
    tui_put_str(sf, head, x + 2, head.y, ls_tr("Radio"), cap);
    tui_put_str(sf, head, x + 16, head.y, ls_tr("State"), cap);
    tui_put_str(sf, head, x + 32, head.y, ls_tr("For"), cap);
    tui_put_str(sf, head, act_x, head.y, ls_tr("Action"), cap);
    for (int slot = 0; slot < shown; slot++) {
        const int i = s_page_first + slot, y = body.y + slot;
        const radio_state_t st = ROWS[i].read();
        const bool can = ROWS[i].set && (st != RS_ABSENT || ROWS[i].set == open_link);
        const bool sel = i == s_sel;
        const tui_rect row = tui_rect_make(body.x, y, body.w, 1);
        const uint8_t band = ls_ink(LS_INK_SELECT, A(TUI_WHITE | TUI_BRIGHT, TUI_BLUE));
        if (sel) tui_fill(sf, row, ' ', band);
        const uint8_t bg = sel ? TUI_ATTR_BG(band) : TUI_BLACK;
        const uint8_t lamp = !can ? LS_FAINT_FG : st == RS_BUSY ? (TUI_GREEN | TUI_BRIGHT)
                           : st == RS_ON ? (TUI_YELLOW | TUI_BRIGHT)
                           : st == RS_FAILED ? (TUI_RED | TUI_BRIGHT) : LS_DIM_FG;
        ls_box_put(sf, row, x, y, LS_SHAPE_DOT, sel ? band : A(lamp, TUI_BLACK));
        tui_put_str(sf, row, x + 2, y, ls_tr(ROWS[i].name), sel ? band : can ? text : dim);
        char w[24];
        snprintf(w, sizeof(w), "%.15s", ls_tr(ROWS[i].state_text ? ROWS[i].state_text() : state_word(st)));
        tui_put_str(sf, row, x + 16, y, w, sel ? band : lamp == LS_FAINT_FG || lamp == LS_DIM_FG ? dim : ls_ink_hue(A(lamp, bg)));
        char what[72];
        const int room = act_x - 2 - (x + 32);
        snprintf(what, sizeof(what), "%.*s", room < 0 ? 0 : room > 70 ? 70 : room, ls_tr(ROWS[i].what));
        tui_put_str(sf, row, x + 32, y, what, sel ? band : dim);
        const char *act = ROWS[i].action_text ? ROWS[i].action_text() : action_word(st, can);
        tui_put_str(sf, row, act_x, y, ls_tr(act), sel ? band : can ? ls_ink(LS_INK_ACCENT, A(TUI_CYAN, TUI_BLACK)) : dim);
        if (s_hit_n < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) {
            s_hit_row[s_hit_n] = i;
            s_row_hit[s_hit_n] = tui_rect_make(body.x, y, act_x - 1 - body.x, 1);
            s_hit[s_hit_n++] = tui_rect_make(act_x, y, right - 1 - act_x, 1);
        }
    }
    /* More radios than rows: the pages on the toolbar's right. */
    char buf[24];
    if (s_page_size < N_ROWS) {
        const ls_btn_t prev = { .label = "< PREV" }, next = { .label = "NEXT >" };
        const int pw = ls_key1_draw(sf, area, tx, area.y, &prev, false);
        s_page_prev = tui_rect_make(tx, area.y, pw, 1);
        snprintf(buf, sizeof(buf), "%d/%d", s_page_first / s_page_size + 1,
                 (N_ROWS + s_page_size - 1) / s_page_size);
        tui_put_str(sf, area, tx + pw + 1, area.y, buf, text);
        const int nx = tx + pw + 2 + (int)strlen(buf);
        s_page_next = tui_rect_make(nx, area.y, ls_key1_draw(sf, area, nx, area.y, &next, false), 1);
    }
    /* The selected radio in full, in a group box under the list. */
    const int dy = frame.y + frame.h;
    const int bh = area.y + area.h - dy;
    if (s_sel < N_ROWS && bh >= 5) {
        const int i = s_sel;
        const radio_state_t st = ROWS[i].read();
        const tui_rect b = tui_rect_make(area.x, dy, area.w, bh > 7 ? 7 : bh);
        const tui_rect in = ls_dui_group(sf, b, ls_tr(ROWS[i].name));
        tui_put_str(sf, in, in.x + 1, in.y, ls_tr(ROWS[i].state_text ? ROWS[i].state_text() : state_word(st)),
                    ls_ink_hue(A(st == RS_BUSY ? TUI_GREEN | TUI_BRIGHT : st == RS_ON ? TUI_YELLOW | TUI_BRIGHT
                      : st == RS_FAILED ? TUI_RED | TUI_BRIGHT : DIM_FG, TUI_BLACK)));
        char line[96];
        snprintf(line, sizeof(line), "%.*s", in.w - 2 < 95 ? in.w - 2 : 95, ls_tr(ROWS[i].what));
        tui_put_str(sf, in, in.x + 1, in.y + 1, line, text);
        char what[72];
        if (row_detail(i, what, sizeof(what)) && in.h >= 3)
            tui_put_str(sf, in, in.x + 1, in.y + 2, what, ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)));
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[64];

    s_hit_n = 0;
    s_options_hit = s_page_prev = s_page_next = tui_rect_make(0, 0, 0, 0);
    if (ls_tui_compact() && area.w >= 72 && area.h >= 12) {
        /* A desk window: the count and any refusal go to the status bar. */
        const int live = count_live();
        int loads = 0;
        for (int i = 0; i < N_ROWS; i++)
            if (!ROWS[i].no_power && !ROWS[i].at_boot) loads++;
        char st[160];
        snprintf(buf, sizeof(buf), ls_tr("%d of %d powered"), live, loads);
        snprintf(st, sizeof(st), "%s%s%s", buf, s_error ? ".  " : "", s_error ? ls_tr(s_error) : "");
        ls_tui_status_set(st, NULL);
        draw_compact(sf, area);
        return;
    }
    ls_panel_box(sf, area, NULL, TUI_CYAN);
    if (area.h < 8 || area.w < 20) return;

    const int live = count_live();
    int loads = 0;
    for (int i = 0; i < N_ROWS; i++)
        if (!ROWS[i].no_power && !ROWS[i].at_boot) loads++;
    snprintf(buf, sizeof(buf), ls_tr("%d of %d powered"), live, loads);
    tui_put_str(sf, area, area.x + 2, area.y + 1, buf,
                live ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : dim);
    if (s_error) tui_put_str(sf, area, area.x + 2, area.y + 2, ls_tr(s_error), A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));

    tui_rect body = tui_rect_make(area.x + 1, area.y + 3,
                                  area.w - 2, area.h - 8);
    if (body.h < 4) return;

    const int ncols = ls_tui_is_wide() && body.w >= 48 ?
        (body.w >= 72 ? 3 : 2) : 1;
    const int total_per = (N_ROWS + ncols - 1) / ncols;
    int bh = (body.h - (total_per - 1)) / total_per;
    if (bh > 9) bh = 9;
    if (bh < 4) bh = 4;
    const int gap = body.h >= total_per * bh + total_per - 1 ? 1 : 0;
    const int per = (body.h + gap) / (bh + gap);
    s_page_size = per * ncols;
    if (s_sel < N_ROWS) s_page_first = (s_sel / s_page_size) * s_page_size;
    const int cw = body.w / ncols;
    for (int slot = 0; slot < s_page_size && s_page_first + slot < N_ROWS; slot++) {
        const int i = s_page_first + slot;
        const int x = body.x + (slot / per) * cw;
        const int y = body.y + (slot % per) * (bh + gap);
        draw_one(sf, tui_rect_make(x, y, cw - (ncols > 1), bh), i, bh);
    }
    s_page_prev = s_page_next = tui_rect_make(0, 0, 0, 0);
    if (s_page_size < N_ROWS) {
        const int y = area.y + area.h - 5;
        snprintf(buf, sizeof(buf), "%d/%d",
                 s_page_first / s_page_size + 1, (N_ROWS + s_page_size - 1) / s_page_size);
        tui_put_str(sf, area, area.x + 2, y, ls_tr("< PREV"), A(TUI_CYAN, TUI_BLACK));
        tui_put_str(sf, area, area.x + (area.w - (int)strlen(buf)) / 2, y, buf, A(TUI_CYAN, TUI_BLACK));
        tui_put_str(sf, area, area.x + area.w - 2 - (int)strlen(ls_tr("NEXT >")), y, ls_tr("NEXT >"), A(TUI_CYAN, TUI_BLACK));
        s_page_prev = tui_rect_make(area.x + 1, y, area.w / 2, 1);
        s_page_next = tui_rect_make(area.x + 1 + area.w / 2, y, area.w / 2 - 1, 1);
    }

    s_options_hit = tui_rect_make(area.x + 2, area.y + area.h - 4, area.w - 4, 3);
    const ls_btn_t opt = ls_opt_button(&RADIO_CONTEXT);
    ls_btn_bar_raised(sf, s_options_hit, &opt, 1, s_sel == N_ROWS ? 0 : -1);

}

/* ----------------------------------------------------------------- input -- */

static void toggle(int i)
{
    if (i < 0 || i >= N_ROWS) return;
    if (!ROWS[i].set) return;
    if (ROWS[i].read() == RS_ABSENT && ROWS[i].set != open_link) return;
    s_error = NULL;
    ROWS[i].set(ROWS[i].read() == RS_OFF);
}

static void enter(void)
{
    s_error = NULL; s_page_first = 0; s_sel = 0;
#if LS_HAS_C6
    ls_wireless_observe(true);
#endif
}
static void leave(void)
{
#if LS_HAS_C6
    ls_wireless_observe(false);
#endif
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    if (k == LS_TK_CHAR && (ch == LS_OPT_KEY || ch == 'O')) {
        options_open(); return true;
    }
    switch (k) {
    case LS_TK_UP:    if (s_sel > 0) s_sel--; return true;
    case LS_TK_DOWN:  if (s_sel < N_ROWS) s_sel++; return true;
    case LS_TK_TAB: s_sel = (s_sel + 1) % (N_ROWS + 1); return true;
    case LS_TK_ENTER:
        if (s_sel == N_ROWS) options_open();
        else toggle(s_sel);
        return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    if (col >= s_options_hit.x && col < s_options_hit.x + s_options_hit.w &&
        row >= s_options_hit.y && row < s_options_hit.y + s_options_hit.h) {
        options_open(); return true;
    }
    if (ls_tui_compact()) {
        /* The desk list: the toolbar key or the action column acts, the row selects. */
        if (tui_rect_contains(s_tool_act, col, row) && s_tool_act.w > 0 && s_sel < N_ROWS) {
            toggle(s_sel);
            return true;
        }
        if (tui_rect_contains(s_page_prev, col, row) && s_page_prev.w > 0) {
            s_sel = s_page_first >= s_page_size ? s_page_first - s_page_size : 0;
            return true;
        }
        if (tui_rect_contains(s_page_next, col, row) && s_page_next.w > 0) {
            if (s_page_first + s_page_size < N_ROWS) s_sel = s_page_first + s_page_size;
            return true;
        }
        for (int i = 0; i < s_hit_n && i < N_ROWS; i++) {
            if (tui_rect_contains(s_hit[i], col, row)) { s_sel = s_hit_row[i]; toggle(s_sel); return true; }
            if (tui_rect_contains(s_row_hit[i], col, row)) { s_sel = s_hit_row[i]; return true; }
        }
        return true;
    }
    if (s_page_size > 0 && row == s_page_prev.y && s_page_prev.w > 0) {
        if (col >= s_page_prev.x && col < s_page_prev.x + s_page_prev.w) {
            s_sel = s_page_first >= s_page_size ? s_page_first - s_page_size : 0;
            return true;
        }
        if (col >= s_page_next.x && col < s_page_next.x + s_page_next.w) {
            if (s_page_first + s_page_size < N_ROWS) s_sel = s_page_first + s_page_size;
            return true;
        }
    }
    for (int i = 0; i < s_hit_n && i < N_ROWS; i++) {
        const tui_rect r = s_hit[i];
        if (col >= r.x && col < r.x + r.w &&
            row >= r.y && row < r.y + r.h) {
            s_sel = s_hit_row[i];
            toggle(s_sel);
            return true;
        }
    }
    /* A miss is a miss. */

    return true;
}

const ls_tui_screen_t ls_scr_radios = {
    .name = "RADIOS",
    .hint = "TAP to switch  UP DOWN ENTER",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
