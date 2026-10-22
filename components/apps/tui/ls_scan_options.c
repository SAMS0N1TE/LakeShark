#include "ls_scan_options.h"

#include "scan_channels.h"
#include "scan_engine.h"

#include <stdio.h>

/* Which channel the per-channel rows act on, and whether the NFM-only rows
   are in the list. Set when the list is opened; the tables themselves are
   const, which is what keeps this allocation-free. */
static int s_channel = -1;

int ls_scan_options_channel(void) { return s_channel; }

/* What is scanned has changed, so where the scan had got to is meaningless. */
static void retarget(void) { scan_engine_stop(); }

static const scan_channel_t *selected(void)
{
    return s_channel >= 0 ? scan_channel_get(s_channel) : NULL;
}

static const char *need_channel(const ls_opt_t *o)
{
    (void)o;
    return selected() ? NULL : "Pick a channel in LISTS first";
}

/* ------------------------------------------------------------- the source */

static const char *const SOURCE[] = { "CHANNELS", "BAND" };
static int  o_source(const ls_opt_t *o)
{
    (void)o;
    return scan_engine_get_source() == SCAN_SRC_BAND ? 1 : 0;
}
static void o_set_source(const ls_opt_t *o, int v)
{
    (void)o;
    retarget();
    scan_engine_set_source(v ? SCAN_SRC_BAND : SCAN_SRC_CHANNELS);
}

static const char *const ZONES[] = { "ALL", "0", "1", "2", "3", "4", "5", "6", "7" };
static int  o_zone(const ls_opt_t *o)
{
    (void)o;
    const int z = scan_engine_get_zone();
    return z < 0 ? 0 : z + 1 <= 8 ? z + 1 : 8;
}
static void o_set_zone(const ls_opt_t *o, int v)
{
    (void)o;
    retarget();
    scan_engine_set_zone(v <= 0 ? -1 : v - 1);
}

/* ------------------------------------------------------- how the scan runs */

static double o_hang(const ls_opt_t *o)
{
    (void)o;
    return scan_engine_get_hang_ms() / 1000.0;
}
static void o_set_hang(const ls_opt_t *o, double s)
{
    (void)o;
    scan_engine_set_hang_ms((int)(s * 1000.0 + 0.5));
}
static void o_show_hang(const ls_opt_t *o, char *out, size_t n)
{
    snprintf(out, n, "%.1f s", o_hang(o));
}

static double o_thresh(const ls_opt_t *o)
{
    (void)o;
    return scan_engine_get_threshold_pct();
}
static void o_set_thresh(const ls_opt_t *o, double pct)
{
    (void)o;
    scan_engine_set_threshold_pct((int)(pct + 0.5));
}
static void o_show_thresh(const ls_opt_t *o, char *out, size_t n)
{
    snprintf(out, n, "%d %%", (int)(o_thresh(o) + 0.5));
}

static double o_priority(const ls_opt_t *o)
{
    (void)o;
    return scan_engine_get_priority_ms() / 1000.0;
}
static void o_set_priority(const ls_opt_t *o, double s)
{
    (void)o;
    scan_engine_set_priority_ms((int)(s * 1000.0 + 0.5));
}
static void o_show_priority(const ls_opt_t *o, char *out, size_t n)
{
    const double s = o_priority(o);
    if (s <= 0.0) snprintf(out, n, "OFF");
    else          snprintf(out, n, "%.1f s", s);
}

/* --------------------------------------------------------------- the band */

static double o_band_start(const ls_opt_t *o)
{
    (void)o;
    uint32_t lo = 0;
    scan_engine_get_band(&lo, NULL, NULL);
    return lo / 1e6;
}
static double o_band_stop(const ls_opt_t *o)
{
    (void)o;
    uint32_t hi = 0;
    scan_engine_get_band(NULL, &hi, NULL);
    return hi / 1e6;
}

/* A band is a raster: start, start+step, start+2*step. Keep the pair ordered
   with at least eight steps between them, so an edge typed past the other one
   leaves a span that can still be scanned instead of being refused. */
static void o_set_band_start(const ls_opt_t *o, double mhz)
{
    (void)o;
    uint32_t hi, step;
    scan_engine_get_band(NULL, &hi, &step);
    const uint32_t want = (uint32_t)(mhz * 1e6 + 0.5);
    const uint32_t span = step ? step * 8u : 100000u;
    if (want + span > hi) hi = want + span;
    retarget();
    scan_engine_set_band(want, hi, step);
}
static void o_set_band_stop(const ls_opt_t *o, double mhz)
{
    (void)o;
    uint32_t lo, step;
    scan_engine_get_band(&lo, NULL, &step);
    const uint32_t want = (uint32_t)(mhz * 1e6 + 0.5);
    const uint32_t span = step ? step * 8u : 100000u;
    if (want < lo + span) lo = want > span ? want - span : 0;
    retarget();
    scan_engine_set_band(lo, want, step);
}
static void o_show_start(const ls_opt_t *o, char *out, size_t n)
{
    snprintf(out, n, "%.4f MHz", o_band_start(o));
}
static void o_show_stop(const ls_opt_t *o, char *out, size_t n)
{
    snprintf(out, n, "%.4f MHz", o_band_stop(o));
}

static const char *const STEPS[] = { "5 k",  "6.25 k", "7.5 k", "10 k", "12.5 k",
                                     "15 k", "20 k",   "25 k",  "50 k" };
static const uint32_t STEP_HZ[] = { 5000,  6250,  7500,  10000, 12500,
                                    15000, 20000, 25000, 50000 };
#define N_STEPS ((int)(sizeof(STEP_HZ) / sizeof(STEP_HZ[0])))

static int o_step(const ls_opt_t *o)
{
    (void)o;
    uint32_t step = 0;
    scan_engine_get_band(NULL, NULL, &step);
    for (int i = 0; i < N_STEPS; ++i)
        if (STEP_HZ[i] == step) return i;
    return 0;
}
static void o_set_step(const ls_opt_t *o, int i)
{
    (void)o;
    if (i < 0 || i >= N_STEPS) return;
    uint32_t lo, hi;
    scan_engine_get_band(&lo, &hi, NULL);
    retarget();
    scan_engine_set_band(lo, hi, STEP_HZ[i]);
}

/* ------------------------------------------------------------ the squelch */

static void o_autosquelch(const ls_opt_t *o)
{
    (void)o;
    scan_engine_autosquelch(-1);
}
static void o_show_autosquelch(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    const int floor_pct = scan_engine_autosquelch_floor();
    if (floor_pct > 0) snprintf(out, n, "FLOOR %d %%", floor_pct);
    else               snprintf(out, n, "MEASURE");
}

/* ------------------------------------------------------- the channel's own */

static int o_lockout(const ls_opt_t *o)
{
    (void)o;
    const scan_channel_t *c = selected();
    return c && (c->flags & SCAN_FLAG_LOCKOUT) ? 1 : 0;
}
static void o_set_lockout(const ls_opt_t *o, int v)
{
    (void)o;
    if (!selected()) return;
    retarget();
    scan_channel_set_lockout(s_channel, v != 0);
}

static int o_prio_flag(const ls_opt_t *o)
{
    (void)o;
    const scan_channel_t *c = selected();
    return c && (c->flags & SCAN_FLAG_PRIORITY) ? 1 : 0;
}
static void o_set_prio_flag(const ls_opt_t *o, int v)
{
    (void)o;
    if (!selected()) return;
    retarget();
    scan_channel_set_priority(s_channel, v != 0);
}

static int o_enabled(const ls_opt_t *o)
{
    (void)o;
    const scan_channel_t *c = selected();
    return c && (c->flags & SCAN_FLAG_ENABLED) ? 1 : 0;
}
static void o_set_enabled(const ls_opt_t *o, int v)
{
    (void)o;
    if (!selected()) return;
    retarget();
    scan_channel_set_enabled(s_channel, v != 0);
}

static void o_delete(const ls_opt_t *o)
{
    (void)o;
    if (!selected()) return;
    retarget();
    if (scan_channel_remove(s_channel)) s_channel = -1;
}
static void o_show_channel(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    const scan_channel_t *c = selected();
    if (!c) snprintf(out, n, "--");
    else snprintf(out, n, "%lu.%04lu", (unsigned long)(c->freq_hz / 1000000),
                  (unsigned long)((c->freq_hz % 1000000) / 100));
}

/* ------------------------------------------------------------- the tables */

#define ROW_SOURCE                                                                  \
    { .label = "Scan source", .kind = LS_OPT_CYCLE, .names = SOURCE, .n = 2,        \
      .get = o_source, .set = o_set_source, .step = 1 }
#define ROW_ZONE                                                                    \
    { .label = "Zone", .kind = LS_OPT_CYCLE, .names = ZONES, .n = 9,                \
      .get = o_zone, .set = o_set_zone, .step = 1 }
#define ROW_HANG                                                                    \
    { .label = "Hang time", .kind = LS_OPT_LEVEL, .num = o_hang,                    \
      .set_num = o_set_hang, .lo = 0, .hi = 30, .step = 0.5,                        \
      .unit = "seconds held after a signal drops", .show = o_show_hang }
#define ROW_THRESH                                                                  \
    { .label = "Open at", .kind = LS_OPT_LEVEL, .num = o_thresh,                    \
      .set_num = o_set_thresh, .lo = 1, .hi = 100, .step = 2,                       \
      .unit = "percent of quieting to stop on", .show = o_show_thresh }
#define ROW_PRIORITY                                                                \
    { .label = "Priority look", .kind = LS_OPT_LEVEL, .num = o_priority,            \
      .set_num = o_set_priority, .lo = 0, .hi = 60, .step = 0.5,                    \
      .unit = "seconds between priority checks, 0 off", .show = o_show_priority }
#define ROW_START                                                                   \
    { .label = "Band start", .kind = LS_OPT_NUMBER, .num = o_band_start,            \
      .set_num = o_set_band_start, .lo = 24, .hi = 1766, .unit = "MHz",             \
      .show = o_show_start }
#define ROW_STOP                                                                    \
    { .label = "Band stop", .kind = LS_OPT_NUMBER, .num = o_band_stop,              \
      .set_num = o_set_band_stop, .lo = 24, .hi = 1766, .unit = "MHz",              \
      .show = o_show_stop }
#define ROW_STEP                                                                    \
    { .label = "Band step", .kind = LS_OPT_CYCLE, .names = STEPS, .n = N_STEPS,     \
      .get = o_step, .set = o_set_step, .step = 1 }
#define ROW_CHANNEL                                                                 \
    { .label = "Channel", .kind = LS_OPT_ACTION, .show = o_show_channel,            \
      .why_not = need_channel }
#define ROW_ENABLED                                                                 \
    { .label = "Channel enabled", .kind = LS_OPT_TOGGLE, .get = o_enabled,          \
      .set = o_set_enabled, .why_not = need_channel }
#define ROW_LOCKOUT                                                                 \
    { .label = "Channel lockout", .kind = LS_OPT_TOGGLE, .get = o_lockout,          \
      .set = o_set_lockout, .why_not = need_channel }
#define ROW_PRIO_FLAG                                                               \
    { .label = "Channel priority", .kind = LS_OPT_TOGGLE, .get = o_prio_flag,       \
      .set = o_set_prio_flag, .why_not = need_channel }
#define ROW_DELETE                                                                  \
    { .label = "Delete channel", .kind = LS_OPT_ACTION, .act = o_delete,            \
      .show = o_show_channel, .why_not = need_channel }

/* The two list switches the old START page carried. The scanner's front now
   starts with one tap, so these live here with the rest of how a scan runs. */
static int  o_mixed(const ls_opt_t *o) { (void)o; return scan_engine_mixed() ? 1 : 0; }
static void o_set_mixed(const ls_opt_t *o, int v) { (void)o; retarget(); scan_engine_set_mixed(v != 0); }
static int  o_gps(const ls_opt_t *o) { (void)o; return scan_engine_location() ? 1 : 0; }
static void o_set_gps(const ls_opt_t *o, int v) { (void)o; retarget(); scan_engine_set_location(v != 0); }
#define ROW_MIXED                                                                   \
    { .label = "Mixed P25 + FM", .kind = LS_OPT_TOGGLE, .get = o_mixed,             \
      .set = o_set_mixed }
#define ROW_GPS                                                                     \
    { .label = "GPS coverage filter", .kind = LS_OPT_TOGGLE, .get = o_gps,          \
      .set = o_set_gps }

/* NFM: the autosquelch measures the noise floor the NFM squelch gates on. */
static const ls_opt_t OPT_SCAN_FM[] = {
    ROW_SOURCE, ROW_ZONE, ROW_GPS, ROW_MIXED, ROW_HANG, ROW_THRESH,
    { .label = "Autosquelch", .kind = LS_OPT_ACTION, .act = o_autosquelch,
      .show = o_show_autosquelch },
    ROW_PRIORITY, ROW_START, ROW_STOP, ROW_STEP,
    ROW_CHANNEL, ROW_ENABLED, ROW_LOCKOUT, ROW_PRIO_FLAG, ROW_DELETE,
};

static const ls_opt_t OPT_SCAN_P25[] = {
    ROW_SOURCE, ROW_ZONE, ROW_GPS, ROW_MIXED, ROW_HANG, ROW_THRESH,
    ROW_PRIORITY, ROW_START, ROW_STOP, ROW_STEP,
    ROW_CHANNEL, ROW_ENABLED, ROW_LOCKOUT, ROW_PRIO_FLAG, ROW_DELETE,
};

static const ls_opt_ctx_t CTX_SCAN_FM = {
    .name = "SCANNER", .job = -1, .radio = LS_RSEL_NONE, .tag = "SCAN",
    .refresh_ms = 250, LS_OPT_ROWS(OPT_SCAN_FM),
};
static const ls_opt_ctx_t CTX_SCAN_P25 = {
    .name = "SCANNER", .job = -1, .radio = LS_RSEL_NONE, .tag = "SCAN",
    .refresh_ms = 250, LS_OPT_ROWS(OPT_SCAN_P25),
};

const ls_opt_ctx_t *ls_scan_options(bool fm, int channel)
{
    s_channel = channel >= 0 && scan_channel_get(channel) ? channel : -1;
    return fm ? &CTX_SCAN_FM : &CTX_SCAN_P25;
}
