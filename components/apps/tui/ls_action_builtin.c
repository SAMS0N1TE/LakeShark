/* The built-in action set. */

#include "ls_action.h"
#include "ls_tui_screen.h"
#include "ls_waterfall.h"
#include "ls_map.h"
#include "ls_numpad.h"
#include "ls_app.h"
#include "ls_gps.h"
#include "ls_track_log.h"
/* The saved home, the fallback under a live fix. */
#include "core/settings.h"

#include <string.h>

#include "apps/fm/fm_state.h"
#include "lakeshark_backend.h"
#include "p25_state.h"
#include "audio/audio_out.h"
#include "ls_mesh.h"

void lakeshark_fm_set_freq(uint32_t hz);
void lakeshark_p25_set_freq(uint32_t hz);
void lakeshark_fm_set_mode(int mode);
int  lakeshark_fm_get_mode(void);

/* ---- gps --------------------------------------------------------------- */

/* Start or stop recording the track. */

static ls_act_status_t a_track_rec(const ls_args_t *in, ls_val_t *out)
{
    const bool want = (in->v[0].i != 0);
    if (want) {
        const esp_err_t e = ls_track_rec_start();
        if (e != ESP_OK) return LS_ACT_UNAVAILABLE;
    } else {
        ls_track_rec_stop();
    }
    out->kind = LS_VAL_BOOL;
    out->i = ls_track_rec_running();
    return LS_ACT_OK;
}

static ls_act_status_t a_gps_on(const ls_args_t *in, ls_val_t *out)
{
    const bool want = (in->v[0].i != 0);
    if (want) {
        if (ls_gps_start() != ESP_OK) return LS_ACT_UNAVAILABLE;
    } else {
        ls_gps_stop();
    }
    /* Report what was actually achieved - powered or not - rather
       than whether it has started talking yet, which it will not have done
       for a second or two after a start. */
    out->kind = LS_VAL_BOOL;
    out->i = ls_gps_running();
    return LS_ACT_OK;
}

/* ---- map --------------------------------------------------------------- */

static ls_act_status_t a_map_zoom(const ls_args_t *in, ls_val_t *out)
{
    const int dz = (int)in->v[0].i;
    if (dz < -8 || dz > 8) return LS_ACT_BADARG;
    ls_map_zoom_by(dz);
    out->kind = LS_VAL_INT;
    out->i = ls_map_zoom();
    return LS_ACT_OK;
}

void ls_scr_map_reload(void);

static ls_act_status_t a_map_reload(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_scr_map_reload();
    out->kind = LS_VAL_TEXT;
    out->s = ls_map_status() ? ls_map_status() : "archive loaded";
    return ls_map_status() ? LS_ACT_UNAVAILABLE : LS_ACT_OK;
}

/* Open the map, and centre it if there is anything to centre on. */

static ls_act_status_t a_map_here(const ls_args_t *in, ls_val_t *out)
{
    (void)in;

    /* Three answers, in order, and the last one always exists. */

    const char *how = NULL;

    ls_gps_state_t g;
    ls_gps_get(&g);
    if (g.fix) {
        ls_map_center(g.lat_deg, g.lon_deg);
        how = "centred on the fix";
    }
    if (!how) {
        float hlat, hlon;
        if (settings_get_home(&hlat, &hlon)) {
            ls_map_center((double)hlat, (double)hlon);
            how = "no fix - centred on the saved home";
        }
    }
    if (!how && ls_map_go_last_good())
        how = "no fix - back to the last view that had tiles under it";
    if (!how) how = "no fix and nowhere known to go - the map has not drawn yet";

    const ls_app_t *app = ls_app_by_id("map");
    const int idx = (app && app->screen) ? ls_tui_screen_index_of(app->screen)
                                         : -1;
    if (idx >= 0) ls_tui_screen_show(idx);

    out->kind = LS_VAL_TEXT;
    out->s = idx < 0 ? "this build has no map screen" : how;
    return LS_ACT_OK;
}

/* ---- ui ---------------------------------------------------------------- */

static ls_act_status_t a_ui_screen(const ls_args_t *in, ls_val_t *out)
{
    int n = (int)in->v[0].i;
    if (n < 0 || n >= ls_tui_screen_count()) return LS_ACT_BADARG;
    ls_tui_screen_show(n);
    out->kind = LS_VAL_TEXT;
    out->s = ls_tui_screen_name(n);
    return LS_ACT_OK;
}

static ls_act_status_t a_ui_next(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_tui_screen_next();
    out->kind = LS_VAL_TEXT;
    out->s = ls_tui_screen_name(ls_tui_screen_current());
    return LS_ACT_OK;
}

static ls_act_status_t a_ui_prev(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_tui_screen_prev();
    out->kind = LS_VAL_TEXT;
    out->s = ls_tui_screen_name(ls_tui_screen_current());
    return LS_ACT_OK;
}

/* ---- waterfall --------------------------------------------------------- */

static ls_act_status_t wf_apply(ls_wf_cfg_t *c, ls_val_t *out, long shown)
{
    ls_wf_cfg_set(c);
    out->kind = LS_VAL_INT;
    out->i = shown;
    return LS_ACT_OK;
}

static ls_act_status_t a_wf_palette(const ls_args_t *in, ls_val_t *out)
{
    long v = in->v[0].i;
    if (v < 0 || v >= LS_WF_PAL__COUNT) return LS_ACT_BADARG;
    ls_wf_cfg_t c = *ls_wf_cfg();
    c.palette = (uint8_t)v;
    return wf_apply(&c, out, v);
}

static ls_act_status_t a_wf_split(const ls_args_t *in, ls_val_t *out)
{
    long v = in->v[0].i;
    if (v != 0 && v != 25 && v != 50 && v != 75 && v != 100) return LS_ACT_BADARG;
    ls_wf_cfg_t c = *ls_wf_cfg();
    c.split_pct = (uint8_t)v;
    return wf_apply(&c, out, v);
}

static ls_act_status_t a_wf_range(const ls_args_t *in, ls_val_t *out)
{
    long v = in->v[0].i;
    if (v < 4 || v > 16) return LS_ACT_BADARG;
    ls_wf_cfg_t c = *ls_wf_cfg();
    c.range = (uint8_t)v;
    return wf_apply(&c, out, v);
}

static ls_act_status_t a_wf_ref(const ls_args_t *in, ls_val_t *out)
{
    long v = in->v[0].i;
    if (v < -8 || v > 8) return LS_ACT_BADARG;
    ls_wf_cfg_t c = *ls_wf_cfg();
    c.ref = (int8_t)v;
    return wf_apply(&c, out, v);
}

static ls_act_status_t a_wf_pause(const ls_args_t *in, ls_val_t *out)
{
    ls_wf_cfg_t c = *ls_wf_cfg();
    c.paused = in->v[0].i != 0;
    return wf_apply(&c, out, c.paused);
}

static ls_act_status_t a_wf_peak(const ls_args_t *in, ls_val_t *out)
{
    ls_wf_cfg_t c = *ls_wf_cfg();
    c.peak_hold = in->v[0].i != 0;
    return wf_apply(&c, out, c.peak_hold);
}

/* ---- tune -------------------------------------------------------------- */

static ls_act_status_t a_fm_freq(const ls_args_t *in, ls_val_t *out)
{
    float mhz = in->v[0].kind == LS_VAL_INT ? (float)in->v[0].i : in->v[0].f;

    if (mhz < 24.0f || mhz > 1766.0f) return LS_ACT_BADARG;
    lakeshark_fm_set_freq((uint32_t)(mhz * 1e6f + 0.5f));
    out->kind = LS_VAL_FLOAT; out->f = mhz;
    return LS_ACT_OK;
}

static ls_act_status_t a_fm_freq_hz(const ls_args_t *in, ls_val_t *out)
{
    const int32_t hz = in->v[0].i;
    if (hz < 24000000 || hz > 1766000000) return LS_ACT_BADARG;
    lakeshark_fm_set_freq((uint32_t)hz);
    out->kind = LS_VAL_INT;
    out->i = hz;
    return LS_ACT_OK;
}

static ls_act_status_t a_p25_freq(const ls_args_t *in, ls_val_t *out)
{
    float mhz = in->v[0].kind == LS_VAL_INT ? (float)in->v[0].i : in->v[0].f;
    if (mhz < 24.0f || mhz > 1766.0f) return LS_ACT_BADARG;
    lakeshark_p25_set_freq((uint32_t)(mhz * 1e6f + 0.5f));
    out->kind = LS_VAL_FLOAT; out->f = mhz;
    return LS_ACT_OK;
}

/* Open the keypad on a frequency, and tune to whatever comes back. */

static void tuned_p25(double mhz)
{
    ls_args_t a = { .n = 1 };
    a.v[0].kind = LS_VAL_FLOAT;
    a.v[0].f = (float)mhz;
    (void)ls_action_call("p25.freq", &a, NULL, LS_CAP_TUNE);
}

static void tuned_fm(double mhz)
{
    ls_args_t a = { .n = 1 };
    a.v[0].kind = LS_VAL_FLOAT;
    a.v[0].f = (float)mhz;
    (void)ls_action_call("fm.freq", &a, NULL, LS_CAP_TUNE);
}

static ls_act_status_t a_p25_tune(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_numpad_open("P25 FREQUENCY", "MHz",
                   (double)lakeshark_p25_get_freq() / 1e6, tuned_p25);
    out->kind = LS_VAL_TEXT;
    out->s = "type it";
    return LS_ACT_OK;
}

static ls_act_status_t a_fm_tune(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_numpad_open("FM FREQUENCY", "MHz",
                   (double)lakeshark_fm_get_freq() / 1e6, tuned_fm);
    out->kind = LS_VAL_TEXT;
    out->s = "type it";
    return LS_ACT_OK;
}

static ls_act_status_t a_fm_submode(const ls_args_t *in, ls_val_t *out)
{
    static const struct { const char *name; int mode; } modes[] = {
        { "listen", FM_MODE_LISTEN }, { "scan", FM_MODE_SCAN },
        { "pocsag", FM_MODE_POCSAG }, { "acars", FM_MODE_ACARS },
        { "flex",   FM_MODE_FLEX   }, { "wfm",  FM_MODE_WFM   },
    };
    const char *want = in->v[0].s;
    if (!want) return LS_ACT_BADARG;
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcasecmp(want, modes[i].name)) continue;
        lakeshark_fm_set_mode(modes[i].mode);
        out->kind = LS_VAL_TEXT; out->s = modes[i].name;
        return LS_ACT_OK;
    }
    return LS_ACT_BADARG;
}

static ls_act_status_t a_fm_gain(const ls_args_t *in, ls_val_t *out)
{
    const float db = in->v[0].kind == LS_VAL_INT ? (float)in->v[0].i
                                                 : in->v[0].f;
    if (db < 0.0f || db > 50.0f) return LS_ACT_BADARG;
    lakeshark_fm_set_gain((int)(db * 10.0f + 0.5f));
    out->kind = LS_VAL_FLOAT;
    out->f = lakeshark_fm_gain_tenths() / 10.0f;
    return LS_ACT_OK;
}

static ls_act_status_t a_fm_sql(const ls_args_t *in, ls_val_t *out)
{
    const float v = in->v[0].kind == LS_VAL_INT ? (float)in->v[0].i
                                                : in->v[0].f;
    if (v < 0.0f || v > 100.0f) return LS_ACT_BADARG;
    lakeshark_fm_set_squelch((int)(v * 10.0f + 0.5f));
    out->kind = LS_VAL_FLOAT;
    out->f = lakeshark_fm_squelch_get() / 10.0f;
    return LS_ACT_OK;
}

static ls_act_status_t a_p25_gain(const ls_args_t *in, ls_val_t *out)
{
    const float db = in->v[0].kind == LS_VAL_INT ? (float)in->v[0].i
                                                 : in->v[0].f;
    if (db < 0.0f || db > 50.0f) return LS_ACT_BADARG;
    p25_request_gain((int)(db * 10.0f + 0.5f));
    out->kind = LS_VAL_FLOAT;
    out->f = db;
    return LS_ACT_OK;
}

static ls_act_status_t a_audio_volume(const ls_args_t *in, ls_val_t *out)
{
    int v = (int)in->v[0].i;
    if (v < 0 || v > 100) return LS_ACT_BADARG;
    audio_volume_set(v);
    out->kind = LS_VAL_INT; out->i = audio_volume_get();
    return LS_ACT_OK;
}

/* ---- mesh -------------------------------------------------------------- */

static ls_act_status_t a_mesh_start(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    if (ls_mesh_running()) { out->kind = LS_VAL_BOOL; out->i = 1; return LS_ACT_OK; }
    esp_err_t err = ls_mesh_start();
    if (err == ESP_ERR_NOT_SUPPORTED) return LS_ACT_UNAVAILABLE;
    if (err != ESP_OK) return LS_ACT_FAILED;
    out->kind = LS_VAL_BOOL; out->i = 1;
    return LS_ACT_OK;
}

static ls_act_status_t a_mesh_send(const ls_args_t *in, ls_val_t *out)
{
    if (!ls_mesh_running()) return LS_ACT_UNAVAILABLE;
    const char *text = in->v[0].s;
    if (!text || !*text) return LS_ACT_BADARG;
    esp_err_t err = ls_mesh_send_text(text);
    if (err == ESP_ERR_NOT_ALLOWED) return LS_ACT_DENIED;
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) return LS_ACT_BADARG;
    if (err != ESP_OK) return LS_ACT_FAILED;
    out->kind = LS_VAL_BOOL; out->i = 1;
    return LS_ACT_OK;
}

static ls_act_status_t a_mesh_name(const ls_args_t *in, ls_val_t *out)
{
    esp_err_t err = ls_mesh_set_name(in->v[0].s);
    if (err == ESP_ERR_INVALID_ARG) return LS_ACT_BADARG;
    if (err != ESP_OK) return LS_ACT_FAILED;
    out->kind = LS_VAL_TEXT; out->s = ls_mesh_name();
    return LS_ACT_OK;
}

static ls_act_status_t a_mesh_advert(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    if (!ls_mesh_running()) return LS_ACT_UNAVAILABLE;
    esp_err_t err = ls_mesh_advertise();
    if (err == ESP_ERR_NOT_ALLOWED) return LS_ACT_DENIED;
    if (err != ESP_OK) return LS_ACT_FAILED;
    out->kind = LS_VAL_BOOL; out->i = 1;
    return LS_ACT_OK;
}

void ls_action_register_builtin(void)
{
    static bool done;
    if (done) return;
    done = true;

    ls_action_register("gps.on",         "b", LS_CAP_POWER, a_gps_on,
                       "bring the GPS receiver up or take it down");
    ls_action_register("track.rec",      "b", LS_CAP_STORE, a_track_rec,
                       "record where this unit goes, onto the card");

    ls_action_register("map.zoom",       "i", LS_CAP_TUNE, a_map_zoom,
                       "zoom in or out by n steps");
    ls_action_register("map.here",       "",  LS_CAP_TUNE, a_map_here,
                       "centre on the GPS fix");
    ls_action_register("map.reload",     "",  LS_CAP_TUNE, a_map_reload,
                       "look for a .pmtiles archive on the card again");

    ls_action_register("ui.screen",      "i", LS_CAP_UI,   a_ui_screen,
                       "show screen by index");
    ls_action_register("ui.next",        "",  LS_CAP_UI,   a_ui_next,
                       "next screen");
    ls_action_register("ui.prev",        "",  LS_CAP_UI,   a_ui_prev,
                       "previous screen");

    ls_action_register("wf.palette",     "i", LS_CAP_UI,   a_wf_palette,
                       "0 heat, 1 ice, 2 phosphor, 3 neon");
    ls_action_register("wf.split",       "i", LS_CAP_UI,   a_wf_split,
                       "spectrum's share of the pane: 0,25,50,75,100");
    ls_action_register("wf.range",       "i", LS_CAP_UI,   a_wf_range,
                       "contrast window, 4..16 sixteenths");
    ls_action_register("wf.ref",         "i", LS_CAP_UI,   a_wf_ref,
                       "reference shift, -8..8 sixteenths");
    ls_action_register("wf.pause",       "b", LS_CAP_UI,   a_wf_pause,
                       "freeze the history");
    ls_action_register("wf.peak",        "b", LS_CAP_UI,   a_wf_peak,
                       "spectrum keeps a decaying maximum");

    ls_action_register("fm.freq",        "f", LS_CAP_TUNE, a_fm_freq,
                       "tune FM, MHz");
    ls_action_register("fm.freq_hz",     "i", LS_CAP_TUNE, a_fm_freq_hz,
                       "tune FM, Hz");
    ls_action_register("fm.submode",     "s", LS_CAP_TUNE, a_fm_submode,
                       "listen|scan|pocsag|acars|flex|wfm");
    ls_action_register("p25.freq",       "f", LS_CAP_TUNE, a_p25_freq,
                       "tune P25, MHz");
    ls_action_register("p25.tune",       "",  LS_CAP_TUNE, a_p25_tune,
                       "open the keypad to type a P25 frequency");
    ls_action_register("fm.tune",        "",  LS_CAP_TUNE, a_fm_tune,
                       "open the keypad to type an FM frequency");
    ls_action_register("p25.gain",       "f", LS_CAP_TUNE, a_p25_gain,
                       "front end gain, dB");
    ls_action_register("fm.gain",        "f", LS_CAP_TUNE, a_fm_gain,
                       "front end gain, dB");
    ls_action_register("fm.sql",         "f", LS_CAP_TUNE, a_fm_sql,
                       "squelch threshold");
    ls_action_register("audio.volume",   "i", LS_CAP_TUNE, a_audio_volume,
                       "volume 0..100");

    ls_action_register("mesh.start",     "",  LS_CAP_TUNE, a_mesh_start,
                       "bring MeshCore up in the background (receive only)");

    ls_action_register("mesh.advert",    "",  LS_CAP_TX,   a_mesh_advert,
                       "flood a self-advert - requires 'mesh tx on'");
    /* Also LS_CAP_TX: putting text on the air is transmitting, and a user
       app cannot do it for the same reason it cannot advertise. */
    ls_action_register("mesh.send",      "s", LS_CAP_TX,   a_mesh_send,
                       "send text on the public channel - requires 'mesh tx on'");

    ls_action_register("mesh.name",      "s", LS_CAP_STORE, a_mesh_name,
                       "set this node's name, persisted");
}
