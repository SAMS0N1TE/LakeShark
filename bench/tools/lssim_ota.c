/* The ota.* values main/ls_ota.c publishes on the board, faked so lssim can
   draw UPDATE in every stage. LSSIM_OTA=<stage> (the ls_ota_stage_t number)
   holds one, at LSSIM_OTA_PCT percent for a download; without it the stages
   go round on the shim clock, a few seconds each. LSSIM_OTA_FAILED=1 makes
   the offered build the one that last did not start. LSSIM_OTA_NOTIFY=1 says a
   newer verified build is known (the HOME dots), and LSSIM_OTA_NOTICE=<version>
   raises the one UPDATE pop-up for it (lssim_ota_notice, called after setup).
   LSSIM_OTA_NOTHING=1 makes
   the up-to-date stage the one where the server has no manifest. In stage 7 (written,
   waiting to restart) LSSIM_OTA_PCT is how far through the ten-second wait:
   100 is the restart itself. */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "ls_action.h"
#include "ls_value.h"
#include "ls_notify.h"
#include "ls_tui_screen.h"
#include "ls_app.h"
#include "ls_ota_core.h"

#define SIZE 3787600L

static const struct { long st; int secs; } SEQ[] = {
    { LS_OTA_ST_WIFI, 2 }, { LS_OTA_ST_CHECKING, 3 }, { LS_OTA_ST_AVAILABLE, 3 },
    { LS_OTA_ST_DOWNLOADING, 10 }, { LS_OTA_ST_VERIFYING, 3 }, { LS_OTA_ST_RESTARTING, 8 },
    { LS_OTA_ST_UPDATED, 6 }, { LS_OTA_ST_CURRENT, 3 }, { LS_OTA_ST_FAILED, 3 },
};
#define NSEQ ((int)(sizeof(SEQ) / sizeof(SEQ[0])))

static long  s_stage;
static float s_frac;          /* how far through the stage, 0..1 */

static void now(void)
{
    const char *pin = getenv("LSSIM_OTA");
    if (pin) {
        const char *pct = getenv("LSSIM_OTA_PCT");
        s_stage = atol(pin);
        s_frac = pct ? (float)atof(pct) / 100.0f : 0.6f;
        return;
    }
    int total = 0;
    for (int i = 0; i < NSEQ; i++) total += SEQ[i].secs;
    const double t = (double)(esp_timer_get_time() % ((int64_t)total * 1000000)) / 1e6;
    double at = 0;
    for (int i = 0; i < NSEQ; i++) {
        if (t < at + SEQ[i].secs) { s_stage = SEQ[i].st; s_frac = (float)((t - at) / SEQ[i].secs); return; }
        at += SEQ[i].secs;
    }
}

static bool known(void)
{
    return s_stage != LS_OTA_ST_IDLE && s_stage != LS_OTA_ST_WIFI && s_stage != LS_OTA_ST_CHECKING
           && !(s_stage == LS_OTA_ST_CURRENT && getenv("LSSIM_OTA_NOTHING"));
}

static bool v_stage(ls_val_t *o) { now(); o->kind = LS_VAL_INT; o->i = s_stage; return true; }
static bool v_got(ls_val_t *o)
{
    now();
    o->kind = LS_VAL_INT;
    o->i = s_stage == LS_OTA_ST_DOWNLOADING ? (long)(SIZE * s_frac)
         : s_stage == LS_OTA_ST_FAILED ? SIZE / 3
         : s_stage >= LS_OTA_ST_VERIFYING && s_stage != LS_OTA_ST_CURRENT ? SIZE : 0;
    return true;
}
static bool v_size(ls_val_t *o) { now(); o->kind = LS_VAL_INT; o->i = known() ? SIZE : 0; return true; }
static bool v_build(ls_val_t *o) { now(); o->kind = LS_VAL_TEXT; o->s = known() ? "2.8.4-rc1-g47096733100f" : ""; return true; }
static bool v_notes(ls_val_t *o)
{
    now();
    o->kind = LS_VAL_TEXT;
    o->s = known() ? "Updates over WiFi from the UPDATE app, with a picture of what is happening." : "";
    return true;
}
static bool v_running(ls_val_t *o)
{
    now();
    o->kind = LS_VAL_TEXT;
    o->s = s_stage == LS_OTA_ST_UPDATED ? "2.8.4-rc1-g47096733100f" : "2.8.3-gf6007a1684f0";
    return true;
}
static bool v_trial(ls_val_t *o)
{
    now();
    o->kind = LS_VAL_INT;
    o->i = s_stage == LS_OTA_ST_UPDATED ? (long)(20 * (1.0f - s_frac)) : 0;
    return true;
}
static bool v_restart(ls_val_t *o)
{
    now();
    o->kind = LS_VAL_INT;
    o->i = s_stage == LS_OTA_ST_RESTARTING ? (long)ceilf(10.0f * (1.0f - s_frac)) : 0;
    return true;
}
static bool v_state(ls_val_t *o)
{
    static char text[64];
    now();
    switch (s_stage) {
    case LS_OTA_ST_WIFI:        snprintf(text, sizeof(text), "joining WiFi"); break;
    case LS_OTA_ST_CHECKING:    snprintf(text, sizeof(text), "checking"); break;
    case LS_OTA_ST_CURRENT:     snprintf(text, sizeof(text), getenv("LSSIM_OTA_NOTHING")
                                         ? "2.8.3 is the latest, nothing published yet" : "2.8.3 is the latest"); break;
    case LS_OTA_ST_AVAILABLE:   snprintf(text, sizeof(text), getenv("LSSIM_OTA_FAILED")
                                         ? "2.8.4-rc1 did not start: tap INSTALL to retry"
                                         : "2.8.4-rc1 ready: tap INSTALL"); break;
    case LS_OTA_ST_DOWNLOADING: snprintf(text, sizeof(text), "downloading %d%%", (int)(s_frac * 100)); break;
    case LS_OTA_ST_VERIFYING:   snprintf(text, sizeof(text), "checking the image"); break;
    case LS_OTA_ST_RESTARTING:
    {
        ls_val_t r;
        v_restart(&r);
        if (r.i > 0) snprintf(text, sizeof(text), "restart in %ld s: tap RESTART", r.i);
        else         snprintf(text, sizeof(text), "restarting");
        break;
    }
    case LS_OTA_ST_FAILED:      snprintf(text, sizeof(text), "download cut short"); break;
    case LS_OTA_ST_UPDATED:     snprintf(text, sizeof(text), "updated to 2.8.4-rc1"); break;
    default:                    snprintf(text, sizeof(text), "2.8.3: tap CHECK"); break;
    }
    o->kind = LS_VAL_TEXT;
    o->s = text;
    return true;
}

static bool v_notify(ls_val_t *o)
{
    o->kind = LS_VAL_INT;
    o->i = getenv("LSSIM_OTA_NOTIFY") ? 1 : 0;
    return true;
}

/* The pop-up main/ls_ota.c raises: the same text from the same formatter, in
   the same colour, opening the same screen. */
void lssim_ota_notice(void)
{
    const char *ver = getenv("LSSIM_OTA_NOTICE");
    if (!ver) return;
    ls_notice_t n;
    memset(&n, 0, sizeof(n));
    snprintf(n.title, sizeof(n.title), "UPDATE");
    if (!ls_ota_toast_text(ver, n.body, sizeof(n.body))) return;
    n.hue = TUI_MAGENTA;
    n.accent = TUI_MAGENTA;
    const ls_app_t *a = ls_app_by_id("update");
    n.screen = a && a->screen ? ls_tui_screen_index_of(a->screen) : -1;
    ls_notify_post(&n);
}

static bool v_failed(ls_val_t *o)
{
    o->kind = LS_VAL_TEXT;
    o->s = getenv("LSSIM_OTA_FAILED") ? "2.8.4-rc1-g47096733100f" : "";
    return true;
}

static ls_act_status_t a_step(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    (void)out;
    return LS_ACT_OK;
}

void lssim_ota_publish(void)
{
    ls_value_publish("ota.state", NULL, v_state);
    ls_value_publish("ota.stage", NULL, v_stage);
    ls_value_publish("ota.got", "B", v_got);
    ls_value_publish("ota.size", "B", v_size);
    ls_value_publish("ota.build", NULL, v_build);
    ls_value_publish("ota.notes", NULL, v_notes);
    ls_value_publish("ota.running", NULL, v_running);
    ls_value_publish("ota.failed", NULL, v_failed);
    ls_value_publish("ota.notify", NULL, v_notify);
    ls_value_publish("ota.trial", "s", v_trial);
    ls_value_publish("ota.restart", "s", v_restart);
    ls_action_register("ota.step", "", LS_CAP_STORE | LS_CAP_POWER, a_step, "lssim: nothing to update");
}
