#include "ls_compass_live.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_field.h"
#include "ls_gps.h"
#include "ls_wmm.h"
#include "core/ls_time.h"
#include "core/settings.h"

#define RAD 0.017453292519943295
#define DEG 57.29577951308232

typedef struct { float x, y, z; } v3;
static v3 cross(v3 a, v3 b) { return (v3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float norm(v3 a) { return sqrtf(dot(a, a)); }
static v3 scale(v3 a, float k) { return (v3){ a.x * k, a.y * k, a.z * k }; }

bool ls_compass_solve(const ls_imu_sample_t *s, const ls_compass_cal_t *cal,
                      bool *back, ls_compass_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magnetic = out->true_deg = out->declination = NAN;
    out->expected_ut = out->expected_dip = out->cal_ut = out->bend = NAN;
    out->strength_off = out->dip_off = NAN;
    if (!s) return false;
    /* Screen axes: x right, y up, z out of the glass, which ls_imu's
       accelerometer is not (ls_compass_gravity). At rest it reads the
       reaction to gravity, so down is its opposite. */
    float gv[3];
    ls_compass_gravity(s, gv);
    v3 a = { gv[0], gv[1], gv[2] };
    const float g = norm(a);
    if (!isfinite(g) || g < 0.5f || g > 1.5f) return false;
    const v3 up = scale(a, 1 / g), down = scale(up, -1);
    out->tilt = acosf(fmaxf(-1, fminf(1, up.z))) * (float)DEG;
    out->pitch = asinf(fmaxf(-1, fminf(1, up.y))) * (float)DEG;
    out->roll = asinf(fmaxf(-1, fminf(1, up.x))) * (float)DEG;
    /* Offset, soft iron and the magnetic basis into screen axes, as the
       calibration found them (ls_compass_field). */
    float f[3];
    if (!ls_compass_field(s, cal, f)) return false;
    out->calibrated = cal != NULL;
    out->cal_ut = cal ? cal->radius : NAN;
    const v3 m = { f[0], f[1], f[2] };
    const float mn = norm(m);
    if (!isfinite(mn) || mn < 1) return false;
    out->field_ut = mn;
    out->dip = asinf(fmaxf(-1, fminf(1, dot(m, down) / mn))) * (float)DEG;
    v3 east = cross(down, m);
    const float en = norm(east);
    if (en < 1e-3f) return false;          /* the field is straight down */
    east = scale(east, 1 / en);
    const v3 north = cross(east, down);
    /* The direction the heading describes: the top of the board while it
       lies near flat, the back of it once held up like a camera. */
    bool upright = back ? *back : false;
    if (fabsf(up.y) > 0.85f) upright = true;
    else if (fabsf(up.y) < 0.70f) upright = false;
    if (back) *back = upright;
    out->back_axis = upright;
    const v3 axis = upright ? (v3){ 0, 0, -1 } : (v3){ 0, 1, 0 };
    const v3 h = { axis.x - dot(axis, down) * down.x, axis.y - dot(axis, down) * down.y,
                   axis.z - dot(axis, down) * down.z };
    if (norm(h) < 0.1f) return false;      /* pointing straight up or down */
    float deg = atan2f(dot(h, east), dot(h, north)) * (float)DEG;
    if (deg < 0) deg += 360;
    out->magnetic = deg;
    out->valid = true;
    return true;
}

void ls_compass_apply_model(ls_compass_reading_t *r, double declination,
                            double expected_ut, double expected_dip)
{
    if (!r) return;
    r->model = isfinite(expected_ut) && expected_ut > 0;
    r->expected_ut = (float)expected_ut;
    r->expected_dip = (float)expected_dip;
    if (isfinite(declination)) {
        r->declination = (float)declination;
        r->true_valid = r->valid;
        if (r->valid) r->true_deg = fmodf(r->magnetic + (float)declination + 720, 360);
    }
    /* A calibrated sensor in a clean field reads the model within a few
       microtesla and a few degrees of dip. Steel, a car, a speaker magnet
       or rebar pushes one or both well past that. */
    if (r->model && r->valid && r->calibrated) {
        /* Strength against what the calibration measured: taking out soft
           iron keeps the field's shape, not its scale, and on the board the
           fit's radius came out at 67 uT where the model says 52. */
        const float ref = isfinite(r->cal_ut) && r->cal_ut > 0 ? r->cal_ut : r->expected_ut;
        r->strength_off = (r->field_ut - ref) / ref;
        r->dip_off = r->dip - r->expected_dip;
        const float strength = fabsf(r->strength_off);
        const float dip = fabsf(r->dip_off);
        r->interference = strength > 0.15f || dip > 8.0f;
        r->bend = fmaxf(strength / 0.15f, dip / 8.0f);
    }
}

float ls_compass_steady_step(ls_compass_steady_t *s, float heading, float pitch, float roll,
                             float rate_dps, float dt)
{
    if (!s) return heading;
    if (!isfinite(heading)) return s->started ? s->heading : NAN;
    if (!s->started || !isfinite(s->heading)) {
        s->heading = heading; s->pitch = pitch; s->roll = roll; s->started = true;
        return heading;
    }
    if (dt <= 0) return s->heading;
    if (dt > 0.5f) dt = 0.5f;
    const float d = fmodf(heading - s->heading + 540.0f, 360.0f) - 180.0f;
    const float a = fabsf(d);
    const float t = a <= 1.5f ? 1.0f : a >= 4.0f ? 0.0f : (4.0f - a) / 2.5f;
    /* Turning, by the gyro: all but the sensor's own lag, so a sweep is
       filed where it was heard. */
    const float tau = isfinite(rate_dps) && rate_dps > 4.0f ? 0.03f : 0.08f + t * (1.5f - 0.08f);
    const float k = 1.0f - expf(-dt / tau);
    s->heading = fmodf(s->heading + d * k + 720.0f, 360.0f);
    const float kt = 1.0f - expf(-dt / 0.5f);
    if (isfinite(pitch)) s->pitch += (pitch - s->pitch) * kt;
    if (isfinite(roll)) s->roll += (roll - s->roll) * kt;
    return s->heading;
}

bool ls_compass_bend_step(ls_compass_bend_t *b, float bend, int64_t now)
{
    if (!isfinite(bend)) { memset(b, 0, sizeof(*b)); return false; }
    if (!b->primed) { b->primed = true; b->score = bend; b->last_us = b->edge_us = now; }
    const float dt = (float)(now - b->last_us) / 1e6f;
    b->last_us = now;
    if (dt > 0) b->score += (1 - expf(-dt)) * (bend - b->score);
    const bool want = b->on ? b->score > 0.75f : b->score > 1.0f;
    if (want == b->on) b->edge_us = now;
    else if (now - b->edge_us >= (b->on ? 3000000 : 2000000)) { b->on = want; b->edge_us = now; }
    return b->on;
}

void ls_compass_nav(double lat1, double lon1, double lat2, double lon2,
                    double *bearing, double *metres)
{
    const double p1 = lat1 * RAD, p2 = lat2 * RAD, dl = (lon2 - lon1) * RAD;
    const double y = sin(dl) * cos(p2);
    const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * DEG;
    if (b < 0) b += 360;
    const double dp = p2 - p1;
    const double h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (bearing) *bearing = b;
    if (metres) *metres = 2 * 6371008.8 * asin(fmin(1, sqrt(h)));
}

/* ----------------------------------------------------------------- live -- */

EXT_RAM_BSS_ATTR static struct { bool set; double lat, lon; char name[48]; } s_target;
static bool s_back;
static double s_last_lat = NAN, s_last_lon = NAN, s_last_alt;
EXT_RAM_BSS_ATTR static struct { bool read, ok; double lat, lon; } s_saved;
EXT_RAM_BSS_ATTR static struct { bool ok; double lat, lon, year; ls_wmm_field_t f; } s_model;
EXT_RAM_BSS_ATTR static ls_compass_bend_t s_bend;

static double decimal_year(void)
{
    if (ls_time_is_synced()) {
        /* Days since 1970 to a civil date (Hinnant's algorithm), which needs
           no gmtime_r and so builds on the bench too. */
        const long z = (long)(time(NULL) / 86400) + 719468;
        const long era = z / 146097, doe = z - era * 146097;
        const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        const int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
        return ls_wmm_year((int)(yoe + era * 400 + (m <= 2)), m, d);
    }
    EXT_RAM_BSS_ATTR static ls_gps_state_t g;
    ls_gps_get(&g);
    if (g.year >= 2025) return ls_wmm_year(g.year, g.month, g.day);
    /* No date at all: the middle of the model's span. Declination moves by
       a tenth of a degree a year or less almost everywhere. */
    return 2027.5;
}

void ls_compass_live(ls_compass_reading_t *out)
{
    ls_field_sample_t p;
    ls_field_sample_snapshot(&p);
    ls_compass_cal_t cal;
    const bool has_cal = ls_field_compass_cal(&cal);
    if (!p.imu_valid || !ls_compass_solve(&p.imu, has_cal ? &cal : NULL, &s_back, out)) {
        if (!p.imu_valid) memset(out, 0, sizeof(*out));
        out->magnetic = out->true_deg = NAN;
        out->valid = false;
    }
    if (!s_saved.read) {
        float lat, lon;
        s_saved.ok = settings_get_last_fix(&lat, &lon);
        s_saved.lat = lat; s_saved.lon = lon; s_saved.read = true;
    }
    if (p.gps_valid) {
        s_last_lat = p.lat; s_last_lon = p.lon; s_last_alt = p.alt_m;
        /* Declination moves about a degree per hundred kilometres, so a
           tenth of a degree of travel is worth one flash write. */
        if ((!s_saved.ok || fabs(p.lat - s_saved.lat) > 0.1 || fabs(p.lon - s_saved.lon) > 0.1) &&
            settings_set_last_fix((float)p.lat, (float)p.lon)) { s_saved.ok = true; s_saved.lat = p.lat; s_saved.lon = p.lon; }
    }
    double lat = s_last_lat, lon = s_last_lon, alt = s_last_alt;
    if (!isfinite(lat)) {
        /* No fix since boot, which is every boot indoors. Where the GPS last
           was, or HOME, puts true north within a fraction of a degree. */
        float hl, ho;
        if (s_saved.ok) { lat = s_saved.lat; lon = s_saved.lon; }
        else if (settings_get_home(&hl, &ho)) { lat = hl; lon = ho; }
        else return;
        alt = 0;
        out->position_saved = true;
    }
    const double year = decimal_year();
    /* The field changes over kilometres and years, not frames. */
    if (!s_model.ok || fabs(s_model.lat - lat) > 0.01 || fabs(s_model.lon - lon) > 0.01 ||
        fabs(s_model.year - year) > 0.05) {
        const double y = year < 2025.0 ? 2025.0 : year > 2030.0 ? 2030.0 : year;
        s_model.ok = ls_wmm_field(lat, lon, alt, y, &s_model.f);
        s_model.lat = lat; s_model.lon = lon; s_model.year = year;
    }
    if (s_model.ok)
        ls_compass_apply_model(out, s_model.f.declination, s_model.f.total_nt / 1000.0, s_model.f.inclination);
    out->interference = ls_compass_bend_step(&s_bend, out->bend, esp_timer_get_time());
}

void ls_compass_set_target(double lat, double lon, const char *name)
{
    s_target.set = isfinite(lat) && isfinite(lon);
    s_target.lat = lat; s_target.lon = lon;
    snprintf(s_target.name, sizeof(s_target.name), "%s", name ? name : "TARGET");
}

void ls_compass_clear_target(void) { s_target.set = false; }

bool ls_compass_target(double *lat, double *lon, char *name, size_t cap)
{
    if (!s_target.set) return false;
    if (lat) *lat = s_target.lat;
    if (lon) *lon = s_target.lon;
    if (name && cap) snprintf(name, cap, "%s", s_target.name);
    return true;
}
