/*
 * diag_emit.c — central dispatcher for typed telemetry.
 *
 * Responsibilities:
 *   - Initialise both sinks via diag_emit_init()
 *   - Fan out diag_emit() calls to both kv and JSON sinks
 *   - Maintain shared counter state (sync attempts, BCH ec hist,
 *     frame DUID counts) and emit a typed 1-Hz rollup
 *   - Provide diag_uptime_s() helper
 *
 * Owning the counter state here (rather than in diag.c as before)
 * means the periodic emit can use the typed diag_emit() path directly,
 * giving us proper int_array fields for ec_hist instead of a stringly
 * baked "[0:%d,1:%d,...]" line.
 */
#include "diag_emit.h"
#include "diag_kv.h"
#include "diag_json.h"

#include "sdkconfig.h"

#ifdef CONFIG_APP_P25
#include "p25_state.h"
#include "dsp_pipeline.h"
#endif

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdint.h>

/* ── shared counter state ───────────────────────────────── */
static SemaphoreHandle_t s_lock = NULL;
static int64_t           s_start_us       = 0;
static int64_t           s_last_periodic  = 0;

typedef struct {
    int sync_attempts;
    int sync_exact;
    int sync_hd1;
    int sync_hd2;
    int sync_hd3;
    int best_hd_this_period;

    int bch_ok;
    int bch_fail;
    int bch_ec_hist[12];

    int frm_hdu;
    int frm_ldu1;
    int frm_ldu2;
    int frm_tdu;
    int frm_tdulc;
    int frm_tsdu;
    int frm_pdu;
    int frm_other;
} counters_t;

static counters_t s_cnt;

/* Unconditional entry counters. Incremented on EVERY call, before
 * any mutex attempt. If these are nonzero but s_cnt fields stay zero,
 * the mutex is dropping increments. If both are zero, the function
 * is genuinely never called. Reported in the periodic emit as
 * SYN.dbg_calls / BCH.dbg_calls / FRM.dbg_calls.
 *
 * volatile because incremented from one task and read from another.
 * Atomic on RV32 for naturally-aligned uint32 — good enough for
 * diagnostics. */
static volatile uint32_t s_dbg_sync_calls  = 0;
static volatile uint32_t s_dbg_bch_calls   = 0;
static volatile uint32_t s_dbg_frame_calls = 0;

/* ── lifecycle ──────────────────────────────────────────── */
void diag_emit_init(void)
{
    if (s_lock) return;

    s_start_us       = esp_timer_get_time();
    s_last_periodic  = s_start_us;
    memset(&s_cnt, 0, sizeof(s_cnt));
    s_cnt.best_hd_this_period = 99;

    s_lock = xSemaphoreCreateMutex();

    diag_kv_init();
    diag_json_init();

    /* Boot banner — both sinks get it. */
    diag_emit("BOOT", 4, (diag_field_t[]){
        DF_STR ("fw",     "p25-tool-0.5"),
        DF_STR ("target", "esp32p4"),
        DF_INT ("freq",   154785000),
        DF_STR ("mod",    "C4FM"),
    });
}

/* ── core emit ──────────────────────────────────────────── */
void diag_emit(const char *tag, int n_fields, const diag_field_t *fields)
{
    /* Both sinks consume the same typed list. They format
     * independently — kv quotes by rule, json quotes always — so the
     * outputs differ in quoting but never in data.
     */
    diag_kv_record  (tag, n_fields, fields);
    diag_json_record(tag, n_fields, fields);
}

/* ── helpers ────────────────────────────────────────────── */
float diag_uptime_s(void)
{
    return (float)((esp_timer_get_time() - s_start_us) / 1000) / 1000.0f;
}

uint32_t diag_kv_dropped_pub  (void) { return diag_kv_dropped(); }
uint32_t diag_json_dropped_pub(void) { return diag_json_dropped(); }

/* ── counter updates ────────────────────────────────────── */
void diag_count_sync_attempt(int matched_exact, int best_hd)
{
    s_dbg_sync_calls++;
    if (!s_lock) return;
    /* 1ms wait — long enough to ride past a periodic-emit pass,
     * short enough that the hot path doesn't perceptibly stall.
     * The previous 0-tick timeout silently dropped increments
     * whenever the periodic emit happened to be holding the lock. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1)) != pdTRUE) return;
    s_cnt.sync_attempts++;
    if      (matched_exact)   s_cnt.sync_exact++;
    else if (best_hd == 1)    s_cnt.sync_hd1++;
    else if (best_hd == 2)    s_cnt.sync_hd2++;
    else if (best_hd == 3)    s_cnt.sync_hd3++;
    if (best_hd < s_cnt.best_hd_this_period) s_cnt.best_hd_this_period = best_hd;
    xSemaphoreGive(s_lock);
}

void diag_count_bch_result(int ok, int ec)
{
    s_dbg_bch_calls++;
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1)) != pdTRUE) return;
    if (ok) {
        s_cnt.bch_ok++;
        if (ec >= 0 && ec < 12) s_cnt.bch_ec_hist[ec]++;
    } else {
        s_cnt.bch_fail++;
    }
    xSemaphoreGive(s_lock);
}

void diag_count_frame(const char *duid2)
{
    s_dbg_frame_calls++;
    if (!s_lock || !duid2) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1)) != pdTRUE) return;
    if      (!strcmp(duid2, "00")) s_cnt.frm_hdu++;
    else if (!strcmp(duid2, "11")) s_cnt.frm_ldu1++;
    else if (!strcmp(duid2, "22")) s_cnt.frm_ldu2++;
    else if (!strcmp(duid2, "03")) s_cnt.frm_tdu++;
    else if (!strcmp(duid2, "33")) s_cnt.frm_tdulc++;
    else if (!strcmp(duid2, "13")) s_cnt.frm_tsdu++;
    else if (!strcmp(duid2, "30")) s_cnt.frm_pdu++;
    else                           s_cnt.frm_other++;
    xSemaphoreGive(s_lock);
}

/* ── 1-Hz periodic emit ─────────────────────────────────── */
void diag_emit_periodic(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_periodic < 1000000) return;
    s_last_periodic = now;

    counters_t snap;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) == pdTRUE) {
        snap = s_cnt;
        memset(&s_cnt, 0, sizeof(s_cnt));
        s_cnt.best_hd_this_period = 99;
        xSemaphoreGive(s_lock);
    } else {
        return;
    }

    /* RF and DSP records read live state from the P25 app. When the
     * P25 app isn't compiled in, those externs don't exist, so we
     * skip them entirely. The sync/BCH/frame counter records below
     * use only state owned by this file and remain.
     */
#ifdef CONFIG_APP_P25
    /* Mode → string. */
    const char *mode_str = "?";
    switch (s_dsp.mode) {
        case DEMOD_C4FM:          mode_str = "C4FM";     break;
        case DEMOD_CQPSK:         mode_str = "CQPSK";    break;
        case DEMOD_DIFF_4FSK:     mode_str = "DIFF4FSK"; break;
        case DEMOD_FSK4_TRACKING: mode_str = "FSK4T";    break;
        default: break;
    }

    /* RF (renamed from old STAT/DSP merge). */
    diag_emit("RF", 12, (diag_field_t[]){
        DF_UINT ("freq",     (unsigned long)s_tune_freq_hz),
        DF_FLT  ("gain_db",  (double)P25.rtl_gain_tenths / 10.0),
        DF_FLT  ("agc_gain", (double)s_dsp.agc_gain),
        DF_FLT  ("mag_in",   (double)s_dsp.agc_mag_in_avg),
        DF_FLT  ("mag_out",  (double)s_dsp.agc_mag_out_avg),
        DF_FLT  ("mag_peak", (double)s_dsp.agc_mag_in_peak),
        DF_UINT ("iq_Bs",    (unsigned long)P25.iq_bytes_sec),
        DF_INT  ("ring",     P25.ring_fill),
        DF_INT  ("ring_max", P25.ring_size),
        DF_INT  ("read_err", P25.read_errors),
        DF_FLT  ("dc_i",     (double)s_dsp.dc_avg_i),
        DF_FLT  ("dc_q",     (double)s_dsp.dc_avg_q),
    });
    /* Reset peak after read so each 1Hz record reflects the past second
     * only, not cumulative. Avg fields are running IIR — no reset. */
    s_dsp.agc_mag_in_peak = 0.0f;

    /* DSP state. */
    diag_emit("DSP", 3, (diag_field_t[]){
        DF_STR  ("mode",       mode_str),
        DF_FLT  ("demod_gain", (double)P25.demod_gain),
        DF_FLT  ("c4fm_dc",    (double)s_dsp.c4fm_dc_avg),
    });
#endif /* CONFIG_APP_P25 */

    /* Sync stats. dbg_calls is incremented unconditionally on entry
     * to diag_count_sync_attempt; if dbg_calls>0 but att+hd0+...=0,
     * the mutex is dropping increments. */
    diag_emit("SYN", 7, (diag_field_t[]){
        DF_INT ("att",  snap.sync_attempts),
        DF_INT ("hd0",  snap.sync_exact),
        DF_INT ("hd1",  snap.sync_hd1),
        DF_INT ("hd2",  snap.sync_hd2),
        DF_INT ("hd3",  snap.sync_hd3),
        DF_INT ("best", (snap.best_hd_this_period == 99) ? -1 : snap.best_hd_this_period),
        DF_UINT("dbg_calls", s_dbg_sync_calls),
    });

    /* BCH with ec_hist as a real array. */
    int total = snap.bch_ok + snap.bch_fail;
    int pct_x10 = total ? (snap.bch_ok * 1000) / total : 0;
    diag_emit("BCH", 5, (diag_field_t[]){
        DF_INT ("ok",        snap.bch_ok),
        DF_INT ("fail",      snap.bch_fail),
        DF_INT ("pct_x10",   pct_x10),
        DF_ARR ("ec_hist",   snap.bch_ec_hist, 12),
        DF_UINT("dbg_calls", s_dbg_bch_calls),
    });

    /* Frame type counters. */
    diag_emit("FRM", 9, (diag_field_t[]){
        DF_INT ("hdu",       snap.frm_hdu),
        DF_INT ("ldu1",      snap.frm_ldu1),
        DF_INT ("ldu2",      snap.frm_ldu2),
        DF_INT ("tdu",       snap.frm_tdu),
        DF_INT ("tdulc",     snap.frm_tdulc),
        DF_INT ("tsdu",      snap.frm_tsdu),
        DF_INT ("pdu",       snap.frm_pdu),
        DF_INT ("other",     snap.frm_other),
        DF_UINT("dbg_calls", s_dbg_frame_calls),
    });

    /* Sink-drop self-report. */
    uint32_t kv_drop  = diag_kv_dropped();
    uint32_t jsn_drop = diag_json_dropped();
    if (kv_drop || jsn_drop) {
        diag_emit("ERR", 2, (diag_field_t[]){
            DF_UINT("kv_drops",   kv_drop),
            DF_UINT("json_drops", jsn_drop),
        });
    }
}
