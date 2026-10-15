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
    if (norm(h) < 0.173648178f) /* cos(80 degrees) */ return NAN;
    const float deg = atan2f(dot(h, east), dot(h, north)) * (float)DEG;
    return deg < 0 ? deg + 360 : deg;
}

ls_compass_quality_t ls_compass_quality(bool calibrated, float field_ut, float cal_ut,
                                       float dip, float expected_dip)
{
    if (!calibrated || !isfinite(cal_ut) || cal_ut <= 0) return LS_COMPASS_UNCALIBRATED;
    if (!isfinite(field_ut) || field_ut <= 0 || !isfinite(dip)) return LS_COMPASS_DISTURBED;
    if (fabsf(field_ut - cal_ut) > cal_ut * 0.20f ||
        (isfinite(expected_dip) && fabsf(dip - expected_dip) > 15.0f))
        return LS_COMPASS_DISTURBED;
    return LS_COMPASS_GOOD;
}

const char *ls_compass_quality_name(ls_compass_quality_t quality)
{
    switch (quality) {
    case LS_COMPASS_GOOD: return "GOOD";
    case LS_COMPASS_DISTURBED: return "DISTURBED";
    default: return "UNCALIBRATED";
    }
}

static void field_quality(ls_compass_reading_t *r)
{
    r->quality = ls_compass_quality(r->calibrated, r->field_ut, r->cal_ut, r->dip, r->expected_dip);
    r->strength_off = isfinite(r->cal_ut) && r->cal_ut > 0 ? (r->field_ut-r->cal_ut)/r->cal_ut : NAN;
    r->dip_off = isfinite(r->expected_dip) ? r->dip-r->expected_dip : NAN;
    r->bend = r->calibrated ? fmaxf(fabsf(r->strength_off)/0.20f,
                                   isfinite(r->dip_off) ? fabsf(r->dip_off)/15.0f : 0) : NAN;
    r->interference = r->quality == LS_COMPASS_DISTURBED;
}

bool ls_compass_solve(const ls_imu_sample_t *s, const ls_compass_cal_t *cal,
                      bool *back, ls_compass_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magnetic = out->true_deg = out->declination = NAN;
    out->expected_ut = out->expected_dip = out->cal_ut = out->bend = NAN;
    out->strength_off = out->dip_off = NAN;
    out->calibrated = cal != NULL;
    out->cal_ut = cal ? cal->radius : NAN;
    out->field_ut = out->dip = NAN;
    field_quality(out);
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
    const v3 m = { f[0], f[1], f[2] };
    const float mn = norm(m);
    if (!isfinite(mn) || mn < 1) return false;
    out->field_ut = mn;
    out->dip = asinf(fmaxf(-1, fminf(1, dot(m, down) / mn))) * (float)DEG;
    field_quality(out);
    v3 east = cross(down, m);
    const float en = norm(east);
    if (en < 1e-3f) return false;          /* the field is straight down */
    east = scale(east, 1 / en);
    const v3 north = cross(east, down);
    /* One levelled view in screen coordinates: cos(pitch) * top -
       sin(pitch) * out. Signed up.z carries it smoothly past upright.
       Keep its length: normalizing would hide the right-edge-vertical
       singularity where pitch (and thus the view) is indeterminate. */
    bool upright = back ? *back : false;
    if (fabsf(up.y) > 0.85f) upright = true;
    else if (fabsf(up.y) < 0.70f) upright = false;
    if (back) *back = upright;
    out->back_axis = upright;             /* label only, never a heading switch */
    const v3 view = { 0, up.z, -up.y };
    const float deg = azimuth(view, down, north, east);
    if (!isfinite(deg)) { out->low_confidence = true; return false; }
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
    field_quality(r);
}

float ls_compass_yaw_rate(const float g[3], const float a[3])
{
    const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!isfinite(an) || an < 0.5f || an > 1.5f) return NAN;
    /* Derivative of the SAME levelled view azimuth. In screen axes
       view = right x up, so yaw = -(wy*uy + wz*uz)/(uy^2+uz^2).
       Mounting reverses x/z for both accel and gyro; products survive.
       Unlike -dot(gyro,up), this rejects rotation about the view itself. */
    const float horizontal2 = (a[1]*a[1] + a[2]*a[2]) / (an*an);
    if (horizontal2 < 0.030153690f) return NAN; /* cos(80)^2 */
    const float w = (g[1]*a[1] + g[2]*a[2]) / (an * horizontal2);
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
        s->far_s = s->coast_s = s->slow_s = 0;
        s->win_s = s->win_gyro = s->win_mag = 0;
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
    const bool bounded = s->gyro_used && !(s->sat_s > 0);
    const bool bent = s->mag_tau_s > LS_COMPASS_FUSE_TAU_S;
    const float turn = s->gyro_used ? yaw_dps - trim : 0.0f;
    s->slow_s = fabsf(turn) < LS_COMPASS_LEARN_DPS ? s->slow_s + dt : 0.0f;

    const bool far = fabsf(wrap180f(heading - s->heading)) > 45.0f;
    if (far) {
        s->far_s += dt;
        const bool lasting = s->far_s >= LS_COMPASS_JUMP_S;
        if (!bent && !bounded && lasting) {
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

/* Bias samples are selected by accel/mag observations, never by the
   magnitude or deviation of the gyro that is being estimated. */
static void rest_step(ls_compass_steady_t *s, const float g[3], const float a[3], float heading, float dt)
{
    bool pinned = false;
    for (int i = 0; i < 3; i++) if (fabsf(g[i]) >= 0.98f * LS_COMPASS_GYRO_FS_DPS) pinned = true;
    if (pinned) s->sat_s = LS_COMPASS_SAT_HOLD_S;
    const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const bool level = !pinned && an > 0.9f && an < 1.1f;
    bool quiet = level && s->resting && (s->observed ? s->stationary :
        isfinite(heading) && fabsf(wrap180f(heading-s->rest_heading)) <= 1.0f);
    for (int i = 0; quiet && i < 3; i++)
        if (fabsf(a[i] - s->still_a[i]) > LS_COMPASS_STEADY_G)
            quiet = false;
    if (!quiet) {
        s->resting = level;
        s->still_s = 0; s->rest_heading = heading;
        memset(s->gyro_var, 0, sizeof(s->gyro_var));
        memcpy(s->still_g, g, sizeof(s->still_g));
        memcpy(s->still_a, a, sizeof(s->still_a));
        return;
    }
    /* The mean over the spell, by time; past 10 s it follows slowly, as
       the gyro warms. */
    s->still_s += dt;
    const float w = dt / fminf(s->still_s + dt, 10.0f);
    for (int i = 0; i < 3; i++) {
        const float d = g[i] - s->still_g[i];
        s->gyro_var[i] = (1 - w) * (s->gyro_var[i] + w * d * d);
        s->still_g[i] += d * w;
        s->still_a[i] += (a[i] - s->still_a[i]) * w;
    }
    if (s->still_s >= LS_COMPASS_REST_S) {
        s->gyro_healthy = true;
        for (int i = 0; i < 3; i++)
            if (s->gyro_var[i] > 1.0f) s->gyro_healthy = false;
        s->rest_known = s->gyro_healthy;
        if (!s->gyro_healthy) return;
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
        if (dt > 0) rest_step(s, g, a, heading, dt > 0.5f ? 0.5f : dt);
        const float *r = s->still_s >= LS_COMPASS_REST_USE_S ? s->still_g : s->rest;
        const float off[3] = { g[0] - r[0], g[1] - r[1], g[2] - r[2] };
        if (s->rest_known && s->gyro_healthy && s->sat_s <= 0)
            s->yaw = ls_compass_yaw_rate(off, a);
    }
    return ls_compass_steady_step(s, heading, pitch, roll, s->yaw, dt);
}

/* Rest is observed independently of the gyro. Variance rejects vibration;
   an anchored magnetic vector also rejects a slow turn that has low variance.
   Thresholds include the AK09916 noise and the LR bench accelerometer noise. */
float ls_compass_steady_sample(ls_compass_steady_t *s, float heading, float pitch, float roll,
                               const ls_imu_sample_t *p, float dt)
{
    if (!s) return heading;
    if (!p || !p->mag_valid || !(dt > 0 && dt <= 0.5f) || !isfinite(heading)) {
        s->observed = s->stationary = false;
        s->observe_s = 0;
        return ls_compass_steady_imu(s, heading, pitch, roll, NULL, NULL, dt);
    }
    const float a[3] = {p->ax, p->ay, p->az}, m[3] = {p->mx, p->my, p->mz};
    const float g[3] = {p->gx, p->gy, p->gz};
    bool finite = true;
    for (int i = 0; i < 3; i++) finite &= isfinite(a[i]) && isfinite(m[i]);
    if (!finite) return ls_compass_steady_sample(s, heading, pitch, roll, NULL, dt);
    if (!s->observed) {
        memcpy(s->anchor_m, m, sizeof(m)); memcpy(s->mean_m, m, sizeof(m));
        memcpy(s->mean_a, a, sizeof(a));
        memset(s->var_m, 0, sizeof(s->var_m)); memset(s->var_a, 0, sizeof(s->var_a));
        s->observe_s = 0; s->observed = true; s->check_heading = heading;
    }
    const float w = 1 - expf(-dt / 0.75f);
    float movement = 0, now_move = 0, an = 0;
    bool quiet = true;
    for (int i = 0; i < 3; i++) {
        const float dm = m[i] - s->mean_m[i], da = a[i] - s->mean_a[i];
        s->var_m[i] = (1-w) * (s->var_m[i] + w*dm*dm);
        s->var_a[i] = (1-w) * (s->var_a[i] + w*da*da);
        s->mean_m[i] += w*dm; s->mean_a[i] += w*da;
        movement += (s->mean_m[i]-s->anchor_m[i]) * (s->mean_m[i]-s->anchor_m[i]);
        now_move += (m[i]-s->anchor_m[i]) * (m[i]-s->anchor_m[i]);
        an += a[i]*a[i];
        quiet &= s->var_m[i] < 2.25f && s->var_a[i] < (s->stationary ? 0.01f : 0.0064f);
    }
    /* The mean lags the vector by its 0.75 s window, most of a turn's
       start; the sample itself is the early sign. */
    quiet &= an > 0.81f && an < 1.21f && movement < 9.0f && now_move < LS_COMPASS_MOVE_UT2;
    s->motion_s = quiet ? 0 : s->motion_s + dt;
    /* Entry/exit hysteresis: a brief acceleration spike is not a turn.
       An accumulated magnetic displacement releases immediately. */
    const bool hold = s->stationary && s->motion_s < 0.3f && movement < 9.0f && now_move < LS_COMPASS_MOVE_UT2;
    if (!quiet && !hold) {
        s->observe_s = 0;
        memcpy(s->anchor_m, s->mean_m, sizeof(s->anchor_m));
    } else if (quiet) s->observe_s += dt;
    s->stationary = hold || (quiet && s->observe_s >= 1.0f);

    /* Compare integrated angles over seconds, including mag-still/gyro-moving.
       Do not mistake a single magnetic glitch for a gyro failure. */
    if (s->rest_known) {
        const float off[3] = {g[0]-s->rest[0],g[1]-s->rest[1],g[2]-s->rest[2]};
        const float yaw = ls_compass_yaw_rate(off, a);
        s->check_s += dt;
        if (isfinite(yaw)) s->check_gyro += yaw*dt;
        s->check_mag += wrap180f(heading-s->check_heading);
        if (s->check_s >= 2.0f) {
            if (fabsf(s->check_gyro-s->check_mag) > 6.0f) {
                s->gyro_healthy = s->rest_known = false;
                s->still_s = 0; s->resting = false;
            }
            s->check_s = s->check_gyro = s->check_mag = 0;
        }
    }
    s->check_heading = heading;
    const float held = s->heading;
    const bool had = s->started;
    float result = ls_compass_steady_imu(s, heading, pitch, roll, g, a, dt);
    if (s->stationary && had) {
        /* No synthetic rotation: only independent vector movement releases it. */
        s->heading = held; s->gyro_used = false; s->yaw = 0;
        return held;
    }
    if (!s->gyro_used && had) {
        /* Startup/noisy gyro: a slow acquisition, fast once motion is observed. */
        const float tau = quiet ? 2.0f : 0.25f;
        result = ls_compass_ease(held, heading, 1-expf(-dt/tau));
        s->heading = result;
    }
    return result;
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
    if (s_model.ok) {
        ls_compass_apply_model(out, s_model.f.declination, s_model.f.total_nt / 1000.0, s_model.f.inclination);
        ls_field_compass_reference(out->expected_ut, out->expected_dip);
    }
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
