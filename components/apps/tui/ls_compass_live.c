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
static float wrap180f(float a) { return fmodf(a + 540.0f, 360.0f) - 180.0f; }

/* The compass direction `axis` lies in, clockwise from north, or NAN when
   it points too nearly straight up or down to have one. */
static float azimuth(v3 axis, v3 down, v3 north, v3 east)
{
    const v3 h = { axis.x - dot(axis, down) * down.x, axis.y - dot(axis, down) * down.y,
                   axis.z - dot(axis, down) * down.z };
    if (norm(h) < 0.1f) return NAN;
    const float deg = atan2f(dot(h, east), dot(h, north)) * (float)DEG;
    return deg < 0 ? deg + 360 : deg;
}

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
       lies near flat, the back of it once held up like a camera, and between
       44 and 72 degrees of rise a blend of the two that moves with the rise.
       It used to switch in one step, and rolled at all the top and the back
       point different ways: the reading jumped 14 degrees at 10 of roll and
       27 at 20, and a hold near the switch flipped it back and forth, the
       skipping seen going from flat to antenna up. back_axis keeps its
       hysteresis; it only says which the screen should describe. */
    bool upright = back ? *back : false;
    if (fabsf(up.y) > 0.85f) upright = true;
    else if (fabsf(up.y) < 0.70f) upright = false;
    if (back) *back = upright;
    out->back_axis = upright;
    const float t = (fabsf(up.y) - 0.70f) / 0.25f;
    const float w = t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t);
    const float top = w < 1 ? azimuth((v3){ 0, 1, 0 }, down, north, east) : NAN;
    const float rear = w > 0 ? azimuth((v3){ 0, 0, -1 }, down, north, east) : NAN;
    float deg = w <= 0 || !isfinite(rear) ? top
              : w >= 1 || !isfinite(top) ? rear
              : fmodf(top + w * wrap180f(rear - top) + 360, 360);
    if (!isfinite(deg)) return false;      /* pointing straight up or down */
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

float ls_compass_yaw_rate(const float g[3], const float a[3])
{
    const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!isfinite(an) || an < 0.5f || an > 1.5f) return NAN;
    const float w = (g[0] * a[0] + g[1] * a[1] + g[2] * a[2]) / an;
    return isfinite(w) ? -w : NAN;
}

float ls_compass_steady_step(ls_compass_steady_t *s, float heading, float pitch, float roll,
                             float yaw_dps, float dt)
{
    if (!s) return heading;
    /* Lying still, what comes off the gyro is already all it reads
       (ls_compass_steady_imu); the loop's trim is for a board on the move. */
    const bool still = s->still_s >= LS_COMPASS_REST_USE_S;
    const float trim = still ? 0.0f : s->bias;
    if (!isfinite(heading)) {
        /* No magnetic heading this frame (the accelerometer was swung off
           1 g, or the board points straight up): the gyro turns what is
           held, for up to LS_COMPASS_COAST_S, then it simply holds. */
        if (!s->started) return NAN;
        if (dt > 0 && dt < 0.5f && isfinite(yaw_dps) && s->agree > -0.5f && s->coast_s < LS_COMPASS_COAST_S) {
            s->coast_s += dt;
            s->heading = fmodf(s->heading + (yaw_dps - trim) * dt + 720.0f, 360.0f);
        }
        return s->heading;
    }
    s->coast_s = 0;
    if (!s->started || !isfinite(s->heading)) {
        /* The reading as it is. What is known of the gyro holds for any
           heading, so it is kept. */
        const ls_compass_steady_t was = *s;
        memset(s, 0, sizeof(*s));
        s->mag_tau_s = was.mag_tau_s;
        s->agree = was.agree; s->bias = was.bias;
        memcpy(s->rest, was.rest, sizeof(s->rest));
        s->rest_known = was.rest_known; s->yaw = was.yaw;
        s->heading = heading; s->pitch = pitch; s->roll = roll; s->started = true;
        s->prev_mag = heading;
        return heading;
    }
    if (dt <= 0) return s->heading;
    if (dt > 0.5f) dt = 0.5f;
    const float kt = 1.0f - expf(-dt / 0.2f);
    if (isfinite(pitch)) s->pitch += (pitch - s->pitch) * kt;
    if (isfinite(roll)) s->roll += (roll - s->roll) * kt;

    /* Does the gyro turn the way the magnetometer does? Judged over half a
       second, and only when both saw a clear turn, so the magnetometer's
       own jitter cannot cast the vote. */
    const float mag_step = wrap180f(heading - s->prev_mag);
    s->prev_mag = heading;
    if (isfinite(yaw_dps)) {
        /* The raw rate: a gyro turning the wrong way would otherwise be
           half learnt away as bias. A true bias of a few deg/s is a degree
           or two a window, under the 8 needed to vote. */
        s->win_s += dt; s->win_gyro += yaw_dps * dt; s->win_mag += mag_step;
        if (s->win_s >= 0.5f) {
            if (fabsf(s->win_gyro) > 8.0f && fabsf(s->win_mag) > 8.0f) {
                const float vote = (s->win_gyro > 0) == (s->win_mag > 0) ? 1.0f : -1.0f;
                s->agree += (vote - s->agree) * 0.5f;
            }
            s->win_s = s->win_gyro = s->win_mag = 0;
        }
    }
    s->gyro_used = isfinite(yaw_dps) && s->agree > -0.5f;
    const bool trusted = s->gyro_used && s->agree > 0.5f && !(s->sat_s > 0);
    const bool bent = s->mag_tau_s > LS_COMPASS_FUSE_TAU_S;
    const float turn = s->gyro_used ? yaw_dps - trim : 0.0f;
    s->slow_s = fabsf(turn) < LS_COMPASS_LEARN_DPS ? s->slow_s + dt : 0.0f;

    const bool far = fabsf(wrap180f(heading - s->heading)) > 45.0f;
    if (far) {
        s->far_s += dt;
        const bool lasting = s->far_s >= LS_COMPASS_JUMP_S;
        const bool settled = s->still_s >= LS_COMPASS_STILL_S && s->far_s >= LS_COMPASS_STILL_S;
        if (!bent && (trusted ? settled : lasting)) {
            s->heading = heading; s->far_s = 0;
            return s->heading;
        }
        /* A glitch until it lasts: the gyro carries the heading, or with
           no gyro it holds. After that it is pulled in, below. */
        if (s->far_s <= dt) s->glitches++;
        if (!lasting) {
            if (s->gyro_used) s->heading = fmodf(s->heading + turn * dt + 720.0f, 360.0f);
            return s->heading;
        }
    } else {
        s->far_s = 0;
    }
    if (s->gyro_used) {
        s->heading += turn * dt;
        const float d = wrap180f(heading - s->heading);
        const float tau = s->mag_tau_s > 0 ? s->mag_tau_s : LS_COMPASS_FUSE_TAU_S;
        const float kp = 1.0f / tau, ki = kp * kp / 4.0f;   /* critically damped */
        const float most = LS_COMPASS_PULL_DPS * dt;
        float pull = d * (1.0f - expf(-dt * kp));
        const bool capped = fabsf(pull) >= most;
        if (capped) pull = pull > 0 ? most : -most;
        s->heading += pull;
        /* The trim learns from a board moving slowly for a while, not
           lying still (the rest reading has all of it then). Nor while the
           pull is held to its limit: that error is not bias, and learning
           it would wind the trim up past anything the gyro reads. */
        if (!far && !capped && !still && s->slow_s >= LS_COMPASS_SETTLE_S) {
            s->bias -= d * ki * dt;
            if (s->bias > 15.0f) s->bias = 15.0f;
            if (s->bias < -15.0f) s->bias = -15.0f;
        }
    } else {
        s->heading += wrap180f(heading - s->heading) * (1.0f - expf(-dt / 0.25f));
    }
    s->heading = fmodf(s->heading + 720.0f, 360.0f);
    return s->heading;
}

/* Lying still (ls_compass_steady_imu): each sample within
   LS_COMPASS_QUIET_DPS and LS_COMPASS_STEADY_G of the means since the board
   came to rest. A sample that is not starts the spell again from itself. */
static void rest_step(ls_compass_steady_t *s, const float g[3], const float a[3], float dt)
{
    bool pinned = false;
    for (int i = 0; i < 3; i++) if (fabsf(g[i]) >= 0.98f * LS_COMPASS_GYRO_FS_DPS) pinned = true;
    if (pinned) s->sat_s = LS_COMPASS_SAT_HOLD_S;
    const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const bool level = !pinned && an > 0.9f && an < 1.1f;
    bool quiet = level && s->resting;
    for (int i = 0; quiet && i < 3; i++)
        if (fabsf(g[i] - s->still_g[i]) > LS_COMPASS_QUIET_DPS || fabsf(a[i] - s->still_a[i]) > LS_COMPASS_STEADY_G)
            quiet = false;
    if (!quiet) {
        s->resting = level;
        s->still_s = 0;
        memcpy(s->still_g, g, sizeof(s->still_g));
        memcpy(s->still_a, a, sizeof(s->still_a));
        return;
    }
    /* The mean over the spell, by time; past 10 s it follows slowly, as
       the gyro warms. */
    s->still_s += dt;
    const float w = dt / fminf(s->still_s + dt, 10.0f);
    for (int i = 0; i < 3; i++) {
        s->still_g[i] += (g[i] - s->still_g[i]) * w;
        s->still_a[i] += (a[i] - s->still_a[i]) * w;
    }
    if (s->still_s >= LS_COMPASS_REST_S) {
        memcpy(s->rest, s->still_g, sizeof(s->rest));
        s->rest_known = true;
        s->bias = 0;
    }
}

float ls_compass_steady_imu(ls_compass_steady_t *s, float heading, float pitch, float roll,
                            const float g[3], const float a[3], float dt)
{
    if (!s) return heading;
    if (dt > 0 && s->sat_s > 0) s->sat_s -= dt;
    bool have = g && a;
    for (int i = 0; have && i < 3; i++) have = isfinite(g[i]) && isfinite(a[i]);
    s->yaw = NAN;
    if (!have) {
        s->resting = false;
        s->still_s = 0;
    } else {
        if (dt > 0) rest_step(s, g, a, dt > 0.5f ? 0.5f : dt);
        const float *r = s->still_s >= LS_COMPASS_REST_USE_S ? s->still_g : s->rest;
        const float off[3] = { g[0] - r[0], g[1] - r[1], g[2] - r[2] };
        s->yaw = ls_compass_yaw_rate(off, a);
    }
    return ls_compass_steady_step(s, heading, pitch, roll, s->yaw, dt);
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
    /* With no sample the reading is empty the way ls_compass_solve leaves
       it: a declination of 0 would say true north is known. */
    if (!ls_compass_solve(p.imu_valid ? &p.imu : NULL, has_cal ? &cal : NULL, &s_back, out)) {
        out->magnetic = out->true_deg = NAN;
        out->valid = false;
    }
    out->sigma_deg = NAN;
    if (p.imu_valid) out->learnt = ls_field_compass_learnt(NULL, &out->sigma_deg);
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
    /* Bent by something near, the learner's figure no longer holds. */
    if (out->interference && isfinite(out->sigma_deg)) out->sigma_deg = fmaxf(out->sigma_deg, 15.0f);
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
