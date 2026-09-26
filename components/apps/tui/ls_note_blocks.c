#include "ls_note_blocks.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_value.h"
#include "core/ls_time.h"

static size_t done(char *out, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return 0; }
    return (size_t)n;
}

size_t ls_note_fmt_time(char *out, size_t cap, const char *stamp)
{
    if (!stamp || !stamp[0]) { if (cap) out[0] = 0; return 0; }
    return done(out, cap, snprintf(out, cap, "> TIME %s\n", stamp));
}

size_t ls_note_fmt_gps(char *out, size_t cap, const ls_gps_state_t *g, bool fresh)
{
    if (!g || !g->fix || !fresh) {
        return done(out, cap, snprintf(out, cap, "> GPS no fix%s\n",
                    g && g->alive ? " (receiver running)" : ""));
    }
    char when[16] = "";
    if (g->year) snprintf(when, sizeof(when), "  %02u:%02u:%02uZ", g->hour, g->minute, g->second);
    return done(out, cap, snprintf(out, cap,
        "> GPS %.6f, %.6f  alt %.0f m  hdop %.1f  %u sats%s\n",
        g->lat_deg, g->lon_deg, g->alt_m, g->hdop, g->sats_used, when));
}

size_t ls_note_fmt_heading(char *out, size_t cap, float magnetic, float true_deg,
                           float tilt_deg, bool calibrated)
{
    if (!isfinite(magnetic)) return done(out, cap, snprintf(out, cap, "> HEADING unavailable\n"));
    char t[24] = "";
    if (isfinite(true_deg)) snprintf(t, sizeof(t), "  %03.0f T", fmodf(true_deg + 360, 360));
    return done(out, cap, snprintf(out, cap, "> HEADING %03.0f M%s  tilt %.0f  %s\n",
        fmodf(magnetic + 360, 360), t, isfinite(tilt_deg) ? tilt_deg : 0,
        calibrated ? "calibrated" : "UNCALIBRATED"));
}

size_t ls_note_fmt_map(char *out, size_t cap, double lat, double lon, int zoom,
                       const char *picture)
{
    return done(out, cap, snprintf(out, cap, "> MAP %.6f, %.6f  z%d%s%s\n",
        lat, lon, zoom, picture && picture[0] ? "  " : "", picture ? picture : ""));
}

size_t ls_note_fmt_bearing(char *out, size_t cap, float true_deg, float spread_deg,
                           const char *source, float level, const char *unit,
                           double lat, double lon, bool placed)
{
    if (!isfinite(true_deg)) { if (cap) out[0] = 0; return 0; }
    char where[48] = "";
    if (placed) snprintf(where, sizeof(where), "  from %.6f, %.6f", lat, lon);
    char spread[16] = "";
    if (isfinite(spread_deg)) snprintf(spread, sizeof(spread), " +/-%.0f", spread_deg);
    char lvl[24] = "";
    if (isfinite(level)) snprintf(lvl, sizeof(lvl), "  %.0f %s", level, unit ? unit : "");
    return done(out, cap, snprintf(out, cap, "> BEARING %03.0f T%s  %s%s%s\n",
        fmodf(true_deg + 360, 360), spread, source ? source : "", lvl, where));
}

size_t ls_note_fmt_sensors(char *out, size_t cap, float ax, float ay, float az,
                           float field_ut, float temp_c, float battery_v)
{
    char bat[20] = "";
    if (isfinite(battery_v) && battery_v > 0) snprintf(bat, sizeof(bat), "  battery %.2f V", battery_v);
    return done(out, cap, snprintf(out, cap,
        "> SENSORS accel %+.2f %+.2f %+.2f g  field %.0f uT  %.0f C%s\n",
        ax, ay, az, field_ut, temp_c, bat));
}

/* ------------------------------------------------------------- live -- */

static const char *const GROUPS[] = { "p25", "fm", "adsb", "mesh", "cell", "rf" };
static const char *const LABELS[] = { "P25", "FM", "ADS-B", "MESH", "CELL", "RTL-SDR" };

int ls_note_radio_groups(const char **groups, const char **labels, int cap)
{
    int n = 0;
    for (unsigned i = 0; i < sizeof(GROUPS) / sizeof(GROUPS[0]) && n < cap; i++) {
        char probe[160];
        if (!ls_note_live_radio(probe, sizeof(probe), GROUPS[i])) continue;
        groups[n] = GROUPS[i]; labels[n] = LABELS[i]; n++;
    }
    return n;
}

size_t ls_note_live_radio(char *out, size_t cap, const char *group)
{
    if (!out || cap < 16 || !group) return 0;
    const char *label = group;
    for (unsigned i = 0; i < sizeof(GROUPS) / sizeof(GROUPS[0]); i++)
        if (!strcmp(GROUPS[i], group)) label = LABELS[i];
    size_t n = (size_t)snprintf(out, cap, "> RADIO %s", label);
    const size_t prefix = strlen(group);
    bool any = false;
    for (int i = 0; i < ls_value_count() && n < cap; i++) {
        const char *name = ls_value_name(i);
        if (!name || strncmp(name, group, prefix) || name[prefix] != '.') continue;
        ls_val_t v; const char *unit = NULL;
        if (!ls_value_read(name, &v, &unit)) continue;
        char text[48];
        switch (v.kind) {
        case LS_VAL_INT:   if (!v.i) continue; snprintf(text, sizeof(text), "%ld", v.i); break;
        case LS_VAL_FLOAT: if (!isfinite(v.f) || v.f == 0) continue;
                           snprintf(text, sizeof(text), fabsf(v.f) >= 100 ? "%.4f" : "%.2f", v.f); break;
        case LS_VAL_TEXT:  if (!v.s || !v.s[0] || !strcmp(v.s, "-")) continue;
                           snprintf(text, sizeof(text), "%s", v.s); break;
        case LS_VAL_BOOL:  if (!v.i) continue; snprintf(text, sizeof(text), "yes"); break;
        default: continue;
        }
        int k = snprintf(out + n, cap - n, "  %s %s%s%s", name + prefix + 1, text,
                         unit && unit[0] ? " " : "", unit ? unit : "");
        if (k < 0 || (size_t)k >= cap - n) break;
        n += (size_t)k; any = true;
    }
    if (!any || n + 2 > cap) { out[0] = 0; return 0; }
    out[n++] = '\n'; out[n] = 0;
    return n;
}

size_t ls_note_live_gps(char *out, size_t cap)
{
    EXT_RAM_BSS_ATTR static ls_gps_state_t g;
    ls_gps_get(&g);
    const bool ok = g.alive;
    const int64_t now = esp_timer_get_time();
    const bool fresh = ok && g.last_fix_us > 0 && now - g.last_fix_us < 3000000;
    return ls_note_fmt_gps(out, cap, ok ? &g : NULL, fresh);
}

size_t ls_note_live_time(char *out, size_t cap)
{
    char stamp[LS_TIME_STAMP_MAX];
    ls_time_render_stamp(stamp, sizeof(stamp));
    return ls_note_fmt_time(out, cap, stamp);
}
