/* The values the firmware publishes about itself. See ls_value.h.

   Split from ls_value.c, which is now the registry alone: this is the half
   that knows what a P25 receiver is, and it is the half a host test of the
   registry has no use for. Same arrangement as ls_action_builtin.c. */
#include "ls_value.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "p25_state.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/fm_mode_label.h"
#include "apps/adsb/adsb_state.h"
#include "core/perf.h"
#include "radio/radio_health.h"
#include "radio/radio_endpoint.h"
#include "audio/audio_out.h"
#include "ls_mesh.h"
#include "ls_gps.h"
#include "ls_track_log.h"

/* ---------------------------------------------------------------- readers */

/* Each of these reads one scalar and returns. See the header: several of them
   are written by the RX task with no lock, so reading a set of them here and
   calling it a snapshot would be exactly the lie the screens avoid. */

#define RD_INT(name, expr)  static bool name(ls_val_t *v) \
    { v->kind = LS_VAL_INT;   v->i = (long)(expr); return true; }
#define RD_FLT(name, expr)  static bool name(ls_val_t *v) \
    { v->kind = LS_VAL_FLOAT; v->f = (float)(expr); return true; }
#define RD_BOOL(name, expr) static bool name(ls_val_t *v) \
    { v->kind = LS_VAL_BOOL;  v->i = (expr) ? 1 : 0; return true; }

/* P25 */
static bool v_p25_freq(ls_val_t *v)
{
    if (!s_tune_freq_hz) return false;
    v->kind = LS_VAL_FLOAT;
    v->f = (float)s_tune_freq_hz / 1000000.0f;
    return true;
}
static bool v_p25_nac(ls_val_t *v)
{
    if (P25.dsd_nac <= 0) return false;
    v->kind = LS_VAL_INT; v->i = P25.dsd_nac; return true;
}
static bool v_p25_tg(ls_val_t *v)
{
    if (P25.dsd_tg <= 0) return false;
    v->kind = LS_VAL_INT; v->i = P25.dsd_tg; return true;
}
static bool v_p25_src(ls_val_t *v)
{
    if (P25.dsd_src <= 0) return false;
    v->kind = LS_VAL_INT; v->i = P25.dsd_src; return true;
}
static bool v_p25_mod(ls_val_t *v)
{
    static char buf[8];
    snprintf(buf, sizeof(buf), "%.7s", P25.dsd_modulation);
    if (!buf[0]) return false;
    v->kind = LS_VAL_TEXT; v->s = buf; return true;
}
RD_BOOL(v_p25_sync,  P25.dsd_has_sync)
RD_FLT (v_p25_level, P25.iq_level)

/* FM */
static bool v_fm_freq(ls_val_t *v)
{
    if (!FM.freq_hz) return false;
    v->kind = LS_VAL_FLOAT; v->f = (float)FM.freq_hz / 1000000.0f; return true;
}
static bool v_fm_mode(ls_val_t *v)
{
    v->kind = LS_VAL_TEXT; v->s = fm_mode_label(FM.mode); return v->s != NULL;
}
RD_FLT (v_fm_level,   FM.iq_level)
RD_FLT (v_fm_gain,    FM.gain_tenths / 10.0f)
/* The squelch THRESHOLD, which is not the same question as
   fm.squelch - that one is whether the squelch is currently open. A control
   that steps a setting has to read the setting, and reading the bool would
   have stepped from 0 or 1 dB every time. */
RD_FLT (v_fm_sql,     FM.squelch_tenths / 10.0f)
RD_FLT (v_p25_gain,   P25.rtl_gain_tenths / 10.0f)

/* Whether the track recorder is running, and how much it has. */
static bool v_track_on(ls_val_t *o)
{
    o->kind = LS_VAL_BOOL; o->i = ls_track_rec_running(); return true;
}
static bool v_track_points(ls_val_t *o)
{
    o->kind = LS_VAL_INT; o->i = ls_track_points(); return true;
}

static bool v_gps_on(ls_val_t *o)
{

    o->kind = LS_VAL_BOOL; o->i = ls_gps_running(); return true;
}
static bool v_gps_fix(ls_val_t *o)
{
    ls_gps_state_t g; ls_gps_get(&g);
    o->kind = LS_VAL_BOOL; o->i = g.fix; return true;
}
static bool v_gps_sats(ls_val_t *o)
{
    ls_gps_state_t g; ls_gps_get(&g);
    o->kind = LS_VAL_INT; o->i = g.sats_used; return true;
}
static bool v_gps_lat(ls_val_t *o)
{
    ls_gps_state_t g; ls_gps_get(&g);
    if (!g.fix) return false;
    o->kind = LS_VAL_FLOAT; o->f = (float)g.lat_deg; return true;
}
static bool v_gps_lon(ls_val_t *o)
{
    ls_gps_state_t g; ls_gps_get(&g);
    if (!g.fix) return false;
    o->kind = LS_VAL_FLOAT; o->f = (float)g.lon_deg; return true;
}
RD_BOOL(v_fm_squelch, FM.squelch_open)
RD_INT (v_fm_pages,   FM.pocsag_pages)
RD_BOOL(v_fm_psync,   FM.pocsag_sync)

/* ADS-B and the shared decode counters */
RD_INT(v_adsb_active, adsb_state_active_count())
RD_INT(v_msg_rate,    perf_get_msgs_per_sec())
RD_INT(v_crc_good,    perf_get_crc_good())
RD_INT(v_crc_err,     perf_get_crc_err())

/* Receiver */
static bool v_rf_state(ls_val_t *v)
{
    radio_health_snapshot_t h;
    if (!radio_health_get(LS_RADIO_ENDPOINT_RTL_USB, &h)) return false;
    v->kind = LS_VAL_TEXT; v->s = radio_health_state_name(h.state); return true;
}
static bool v_rf_bps(ls_val_t *v)
{
    radio_health_snapshot_t h;
    if (!radio_health_get(LS_RADIO_ENDPOINT_RTL_USB, &h)) return false;
    v->kind = LS_VAL_INT; v->i = (long)h.bytes_per_second; return true;
}
static bool v_rf_recover(ls_val_t *v)
{
    radio_health_snapshot_t h;
    if (!radio_health_get(LS_RADIO_ENDPOINT_RTL_USB, &h)) return false;
    v->kind = LS_VAL_INT; v->i = h.recoveries; return true;
}

/* System */
RD_INT(v_sys_internal, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024)
RD_INT(v_sys_psram,    heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)
RD_INT(v_sys_uptime,   esp_timer_get_time() / 1000000)
RD_INT(v_sys_volume,   audio_volume_get())

static bool v_mesh_up(ls_val_t *o)
{
    o->kind = LS_VAL_BOOL; o->i = ls_mesh_running(); return true;
}
static bool v_mesh_id(ls_val_t *o)
{
    static ls_mesh_stats_t st;
    ls_mesh_get_stats(&st);
    if (!st.running) return false;
    o->kind = LS_VAL_TEXT; o->s = st.self_id; return true;
}
static bool v_mesh_rx(ls_val_t *o)
{
    ls_mesh_stats_t st; ls_mesh_get_stats(&st);
    if (!st.running) return false;
    o->kind = LS_VAL_INT; o->i = st.rx_packets; return true;
}
static bool v_mesh_tx(ls_val_t *o)
{
    ls_mesh_stats_t st; ls_mesh_get_stats(&st);
    if (!st.running) return false;
    o->kind = LS_VAL_INT; o->i = st.tx_packets; return true;
}
static bool v_mesh_armed(ls_val_t *o)
{
    ls_mesh_stats_t st; ls_mesh_get_stats(&st);
    if (!st.running) return false;
    o->kind = LS_VAL_BOOL; o->i = st.tx_enabled; return true;
}
static bool v_mesh_rssi(ls_val_t *o)
{
    ls_mesh_stats_t st; ls_mesh_get_stats(&st);
    if (!st.running || !st.rx_packets) return false;
    o->kind = LS_VAL_FLOAT; o->f = st.last_rssi; return true;
}

/* Publishing is idempotent: the TUI task registers on first start and a
   restart must not double the table. */
static bool s_builtin_done;

void ls_value_publish_builtin(void)
{
    if (s_builtin_done) return;
    s_builtin_done = true;

    ls_value_publish("p25.freq",     "MHz",  v_p25_freq);
    ls_value_publish("p25.nac",      NULL,   v_p25_nac);
    ls_value_publish("p25.tg",       NULL,   v_p25_tg);
    ls_value_publish("p25.src",      NULL,   v_p25_src);
    ls_value_publish("p25.mod",      NULL,   v_p25_mod);
    ls_value_publish("p25.sync",     NULL,   v_p25_sync);
    ls_value_publish("p25.level",    NULL,   v_p25_level);
    ls_value_publish("p25.gain",     "dB",   v_p25_gain);

    ls_value_publish("gps.on",       NULL,   v_gps_on);
    /* The track recorder, so a control can show its state rather
       than assume it. `track.points` is what turns "recording" into evidence
       - a recorder that is on and has kept nothing looks identical to a
       working one until you can see the count. */
    ls_value_publish("track.on",     NULL,   v_track_on);
    ls_value_publish("track.points", NULL,   v_track_points);
    ls_value_publish("gps.fix",      NULL,   v_gps_fix);
    ls_value_publish("gps.sats",     NULL,   v_gps_sats);
    ls_value_publish("gps.lat",      "deg",  v_gps_lat);
    ls_value_publish("gps.lon",      "deg",  v_gps_lon);

    ls_value_publish("fm.freq",      "MHz",  v_fm_freq);
    ls_value_publish("fm.mode",      NULL,   v_fm_mode);
    ls_value_publish("fm.level",     NULL,   v_fm_level);
    ls_value_publish("fm.gain",      "dB",   v_fm_gain);
    ls_value_publish("fm.squelch",   NULL,   v_fm_squelch);
    ls_value_publish("fm.sql",       "dB",   v_fm_sql);
    ls_value_publish("fm.pages",     NULL,   v_fm_pages);
    ls_value_publish("fm.pocsag",    NULL,   v_fm_psync);

    ls_value_publish("adsb.aircraft", NULL,  v_adsb_active);
    ls_value_publish("decode.rate",   "/s",  v_msg_rate);
    ls_value_publish("decode.good",   NULL,  v_crc_good);
    ls_value_publish("decode.err",    NULL,  v_crc_err);

    ls_value_publish("rf.state",      NULL,  v_rf_state);
    ls_value_publish("rf.bytes",      "B/s", v_rf_bps);
    ls_value_publish("rf.recoveries", NULL,  v_rf_recover);

    ls_value_publish("mesh.up",       NULL,  v_mesh_up);
    ls_value_publish("mesh.node",     NULL,  v_mesh_id);
    ls_value_publish("mesh.rx",       NULL,  v_mesh_rx);
    ls_value_publish("mesh.tx",       NULL,  v_mesh_tx);
    ls_value_publish("mesh.armed",    NULL,  v_mesh_armed);
    ls_value_publish("mesh.rssi",     "dBm", v_mesh_rssi);

    ls_value_publish("sys.internal",  "KB",  v_sys_internal);
    ls_value_publish("sys.psram",     "KB",  v_sys_psram);
    ls_value_publish("sys.uptime",    "s",   v_sys_uptime);
    ls_value_publish("sys.volume",    "%",   v_sys_volume);
}
