/* See ls_rffe_ui.h. The rows read RAM (radio_frontend's) and write through
   ls_rffe_request, which saves at once and leaves the USB transfers to the
   front-end worker. */
#define LS_RFFE_UI_DEFINES   /* the definitions here are the strong ones */
#include "ls_rffe_ui.h"

#include <stdio.h>
#include <string.h>

#include "ls_notify.h"
#include "ls_wf_source.h"
#include "radio_frontend.h"
#include "settings.h"

/* arg: which SDR the row is for. */
static ls_rffe_dev_t dev_of(const ls_opt_t *o) { return o->arg ? LS_RFFE_HACKRF : LS_RFFE_RTL; }

static const char *plug_in(ls_rffe_dev_t d)
{
    return d == LS_RFFE_RTL ? "plug in the RTL-SDR" : "plug in the HackRF";
}

/* ----------------------------------------------------------- bias-T -- */

static const char *const BIAS_NAMES[] = { "OFF", "ON  (DC on the port)" };
static int o_bias(const ls_opt_t *o) { return ls_rffe_value(dev_of(o), LS_RADIO_FE_BIAS_T); }
static void o_set_bias(const ls_opt_t *o, int v) { (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_BIAS_T, v ? 1 : 0); }
static const char *o_bias_why(const ls_opt_t *o)
{
    return ls_rffe_supported(dev_of(o), LS_RADIO_FE_BIAS_T) ? NULL : plug_in(dev_of(o));
}

static const char *const PIN_NAMES[] = { "NO, OFF EACH TIME", "YES" };
static int o_pin(const ls_opt_t *o) { return ls_radio_bias_t_pinned(ls_rffe_endpoint(dev_of(o))); }
static void o_set_pin(const ls_opt_t *o, int v) { ls_radio_bias_t_pin(ls_rffe_endpoint(dev_of(o)), v != 0); }

/* -------------------------------------------------------------- ppm -- */

static double o_ppm(const ls_opt_t *o) { return ls_rffe_value(dev_of(o), LS_RADIO_FE_PPM); }
static void o_set_ppm(const ls_opt_t *o, double v)
{
    (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_PPM, (int)(v < 0 ? v - 0.5 : v + 0.5));
}
static void o_show_ppm(const ls_opt_t *o, char *out, size_t n)
{
    const int v = ls_rffe_value(dev_of(o), LS_RADIO_FE_PPM);
    snprintf(out, n, "%+d ppm%s", v, dev_of(o) == LS_RFFE_RTL ? "  this dongle" : "");
}
static const char *o_ppm_why(const ls_opt_t *o)
{
    return dev_of(o) == LS_RFFE_RTL && !ls_rffe_rtl_known() ? plug_in(LS_RFFE_RTL) : NULL;
}

/* -------------------------------------------------- the RTL's own -- */

static const char *const DAGC_NAMES[] = { "FOLLOW GAIN", "ON", "OFF" };
static int o_cycle(const ls_opt_t *o, ls_radio_fe_ctl_t ctl) { return ls_rffe_value(dev_of(o), ctl); }
static int o_dagc(const ls_opt_t *o) { return o_cycle(o, LS_RADIO_FE_DIGITAL_AGC); }
static void o_set_dagc(const ls_opt_t *o, int v) { (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_DIGITAL_AGC, v); }

static int o_offset(const ls_opt_t *o) { return o_cycle(o, LS_RADIO_FE_OFFSET_TUNING); }
static void o_set_offset(const ls_opt_t *o, int v) { (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_OFFSET_TUNING, v ? 1 : 0); }
static const char *o_offset_why(const ls_opt_t *o)
{
    ls_radio_endpoint_info_t info;
    /* Saved for later while away; an attached low-IF tuner has nothing to avoid. */
    if (ls_radio_endpoint_try_get(ls_rffe_endpoint(dev_of(o)), &info) != LS_RADIO_OK || !info.present)
        return NULL;
    return ls_rffe_supported(dev_of(o), LS_RADIO_FE_OFFSET_TUNING) ? NULL : "R820T: low IF, no DC spike";
}

static const char *const DS_NAMES[] = { "OFF", "I BRANCH (HF)", "Q BRANCH (HF)" };
static int o_ds(const ls_opt_t *o) { return o_cycle(o, LS_RADIO_FE_DIRECT_SAMPLING); }
static void o_set_ds(const ls_opt_t *o, int v) { (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_DIRECT_SAMPLING, v); }
static const char *o_ds_why(const ls_opt_t *o)
{
    if (ls_rffe_supported(dev_of(o), LS_RADIO_FE_DIRECT_SAMPLING)) return NULL;
    return ls_rffe_supported(dev_of(o), LS_RADIO_FE_BIAS_T) ? "not in this build" : plug_in(dev_of(o));
}

/* ------------------------------------------------ the HackRF's own -- */

static const char *const AMP_NAMES[] = { "BY GAIN", "OFF", "ON  +11 dB" };
static int o_amp(const ls_opt_t *o) { return o_cycle(o, LS_RADIO_FE_AMP); }
static void o_set_amp(const ls_opt_t *o, int v) { (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_AMP, v); }

static const char *const LNA_NAMES[] = { "BY GAIN", "0 dB", "8 dB", "16 dB", "24 dB", "32 dB", "40 dB" };
static int o_lna(const ls_opt_t *o) { const int v = o_cycle(o, LS_RADIO_FE_LNA_DB); return v < 0 ? 0 : v / 8 + 1; }
static void o_set_lna(const ls_opt_t *o, int i) { (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_LNA_DB, i <= 0 ? -1 : (i - 1) * 8); }

static double o_vga(const ls_opt_t *o) { return ls_rffe_value(dev_of(o), LS_RADIO_FE_VGA_DB); }
static void o_set_vga(const ls_opt_t *o, double v)
{
    int db = (int)(v + (v < 0 ? -0.5 : 0.5));
    db = db < 0 ? -1 : db > 62 ? 62 : db & ~1;
    (void)ls_rffe_request(dev_of(o), LS_RADIO_FE_VGA_DB, db);
}
static void o_show_vga(const ls_opt_t *o, char *out, size_t n)
{
    const int v = ls_rffe_value(dev_of(o), LS_RADIO_FE_VGA_DB);
    if (v < 0) snprintf(out, n, "BY GAIN");
    else snprintf(out, n, "%d dB", v);
}

/* ------------------------------------------------------------ tables -- */

#define BIAS_ROWS(a) \
    { .label = "BIAS-T", .kind = LS_OPT_TOGGLE, .arg = (a), .names = BIAS_NAMES, \
      .get = o_bias, .set = o_set_bias, .why_not = o_bias_why }, \
    { .label = "ON AT PLUG-IN", .kind = LS_OPT_TOGGLE, .arg = (a), .names = PIN_NAMES, \
      .get = o_pin, .set = o_set_pin }, \
    { .label = "PPM", .kind = LS_OPT_LEVEL, .arg = (a), .num = o_ppm, .set_num = o_set_ppm, \
      .lo = -LS_RFFE_RTL_PPM_MAX, .hi = LS_RFFE_RTL_PPM_MAX, .step = 1, \
      .unit = "ppm, + when its clock runs fast", .show = o_show_ppm, .why_not = o_ppm_why }

static const ls_opt_t RTL_ROWS[] = {
    BIAS_ROWS(0),
    { .label = "DIGITAL AGC", .kind = LS_OPT_CYCLE, .arg = 0, .names = DAGC_NAMES, .n = 3, .step = 1,
      .get = o_dagc, .set = o_set_dagc },
    { .label = "OFFSET TUNING", .kind = LS_OPT_TOGGLE, .arg = 0, .get = o_offset, .set = o_set_offset,
      .why_not = o_offset_why },
    { .label = "DIRECT SAMPLING", .kind = LS_OPT_CYCLE, .arg = 0, .names = DS_NAMES, .n = 3, .step = 1,
      .get = o_ds, .set = o_set_ds, .why_not = o_ds_why },
};
static const ls_opt_t HACKRF_ROWS[] = {
    BIAS_ROWS(1),
    { .label = "RF AMP", .kind = LS_OPT_CYCLE, .arg = 1, .names = AMP_NAMES, .n = 3, .step = 1,
      .get = o_amp, .set = o_set_amp },
    { .label = "LNA", .kind = LS_OPT_CYCLE, .arg = 1, .names = LNA_NAMES, .n = 7, .step = 1,
      .get = o_lna, .set = o_set_lna },
    { .label = "VGA", .kind = LS_OPT_LEVEL, .arg = 1, .num = o_vga, .set_num = o_set_vga,
      .lo = -2, .hi = 62, .step = 2, .unit = "dB in 2s; below 0 is by gain", .show = o_show_vga },
};

const ls_opt_ctx_t ls_rffe_ctx_rtl = {
    .name = "FRONT END", .job = -1, .radio = LS_RSEL_SDR_RTL, LS_OPT_ROWS(RTL_ROWS),
};
const ls_opt_ctx_t ls_rffe_ctx_hackrf = {
    .name = "FRONT END", .job = -1, .radio = LS_RSEL_SDR_HACKRF, LS_OPT_ROWS(HACKRF_ROWS),
};

/* ------------------------------------------------------------ LR2021 -- */

/* WIDE I/Q: the shared LR session's 576k route, LO 144 kHz under the
   channel (lr_wf_session.h). Saved; every LR app's next run takes it. */
static const char *const LR_WIDE_NAMES[] = { "OFF  125k, LO on the channel", "ON  576k, LO 144 kHz below" };
static int o_lr_wide(const ls_opt_t *o) { (void)o; return settings_get_lr_wide(); }
static void o_set_lr_wide(const ls_opt_t *o, int v) { (void)o; (void)settings_set_lr_wide(v != 0); }
static const char *const LR_SPAN_NAMES[] = { "CHANNEL  125 kHz", "FULL SPAN  576 kHz" };
static int o_lr_span(const ls_opt_t *o) { (void)o; return ls_wf_source_lr_span_get(); }
static void o_set_lr_span(const ls_opt_t *o, int v)
{
    (void)o;
    if (settings_set_lr_span(v != 0)) ls_wf_source_lr_span(v != 0);
}
static const char *o_lr_span_why(const ls_opt_t *o)
{
    (void)o;
    return settings_get_lr_wide() ? NULL : "needs WIDE I/Q on";
}
static const ls_opt_t LR_ROWS[] = {
    { .label = "WIDE I/Q", .kind = LS_OPT_TOGGLE, .names = LR_WIDE_NAMES,
      .get = o_lr_wide, .set = o_set_lr_wide },
    { .label = "WATERFALL", .kind = LS_OPT_TOGGLE, .names = LR_SPAN_NAMES,
      .get = o_lr_span, .set = o_set_lr_span, .why_not = o_lr_span_why },
};
const ls_opt_ctx_t ls_rffe_ctx_lr = {
    .name = "FRONT END", .job = -1, .radio = LS_RSEL_LORA, LS_OPT_ROWS(LR_ROWS),
};

/* ------------------------------------------------------------ banner -- */

/* A warning the front end left - from a row here, the console, or a pinned
   bias-T coming on at plug-in - shown once, in red, without the slide. */
static bool rffe_probe(ls_notice_t *n)
{
    if (!ls_rffe_take_notice(n->title, sizeof(n->title), n->body, sizeof(n->body))) return false;
    n->accent = TUI_RED;
    n->steady = true;
    n->notify_visible = true;
    return true;
}

void ls_rffe_ui_init(void)
{
    ls_notify_add_probe(rffe_probe);
    ls_wf_source_lr_span(settings_get_lr_span());
}
