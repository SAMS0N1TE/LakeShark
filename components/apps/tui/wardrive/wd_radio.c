/* WARDRIVE's extra radios (wd_radio.h): the scheduler and the decoders'
   sightings. Nothing here touches a radio; the port does (wd_rport_*).

   The scheduler, per radio, every pass:
   - A radio not fitted (an SDR with no 5 V on battery) is ABSENT and runs
     nothing. One another app has is YIELD: its dwell clock stops until it is
     back. Nothing here ever asks for a radio someone else holds; the port
     only asks the way a background service does.
   - A pinned radio (SWAP) runs its pinned job for the drive.
   - Otherwise, when a dwell ends, every job the radio can run is scored:
       base (what the job is worth at this speed: tyre sensors while driving,
             aircraft and mesh nodes while standing)
     + 0.25 per find a minute it has made (smoothed over its dwells)
     x 0.6 for each dwell in a row that found nothing (at most five)
     + 0.04 a minute since it last ran (so a dry job is tried again)
     x 0.4 when the other radio is already on that job
     + 0.15 for the job running now (a receiver switch costs seconds)
     and the best runs next. A dwell that is finding things is stretched by
     half its length, up to three times: TPMS bursts in traffic hold the
     radio while they last.
   - Region from the fix: 315 MHz TPMS in North America, 433.92 elsewhere;
     915 or 868 for the ISM band and the meshes. */
#include "wd_radio.h"
#include "wd_feed.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define UNIQ_N      128
#define UNIQ_MS     60000u      /* the same ID again within a minute is no new find */
#define REFUSE_MS   60000u      /* a job the port refused is not asked again for a minute */

typedef struct {
    uint16_t hits, runs;
    uint8_t dry;
    float rate, score;
    uint32_t last_hit_ms, last_run_ms, refused_until;
} job_t;

typedef struct {
    bool present;
    uint8_t state, can;
    int8_t job, pin;
    char name[12], why[32];
    uint32_t freq_khz, since_ms, until_ms;
    uint16_t hits, found;       /* this session; this dwell */
    uint16_t found_ext;         /* since the last stretch */
    uint8_t stretched;
    int8_t region;              /* the region the job was started in */
    job_t jobs[WD_JOBS];
    uint8_t trace[WD_TRACE];
    int8_t trace_job[WD_TRACE];
    char last[WD_LAST][24];     /* the newest finds, newest first */
    int8_t last_job[WD_LAST];
} radio_t;

static int8_t s_pin[WD_RJ_N] = { -1, -1 };

static EXT_RAM_BSS_ATTR struct {
    bool on;
    bool src[WD_KINDS];
    uint8_t region, motion;
    bool region_fix;
    float speed;
    uint32_t now, last_tick, start_ms, trace_b, passive;
    radio_t r[WD_RJ_N];
    struct { uint32_t key, ms; } uniq[UNIQ_N];
    int uniq_at;
} W;

/* ------------------------------------------------------------ the tables */

static const char *const JOB_NAME[WD_JOBS] = { "TPMS", "ISM", "ADS-B", "MESH" };
static const char *const JOB_WHAT[WD_JOBS] = { "tyre sensors", "sensors, remotes", "aircraft", "mesh nodes" };
static const char *const REGION_NAME[WD_REGIONS] = { "N.AMERICA", "EUROPE", "AU/S.AMER" };

/* kHz a job listens on, by region; 0 ends a list. */
static const uint32_t FREQ[WD_JOBS][WD_REGIONS][4] = {
    { { 315000 }, { 433920 }, { 433920 } },
    { { 433920, 915000 }, { 433920, 868300 }, { 433920, 915000 } },
    { { 1090000 }, { 1090000 }, { 1090000 } },
    { { 906875, 910525 }, { 869525, 869618 }, { 919875, 915800 } },
};

/* What a job is worth at a speed: still, slow, driving, unknown. */
static const float BASE[WD_JOBS][4] = {
    { 0.15f, 0.60f, 1.00f, 0.45f },     /* TPMS: sensors send while the wheels turn */
    { 0.60f, 0.45f, 0.35f, 0.50f },     /* ISM: stations every half minute, short range */
    { 0.90f, 0.70f, 0.60f, 0.75f },     /* ADS-B: the sky comes to you */
    { 0.80f, 0.60f, 0.50f, 0.65f },     /* MESH: nodes are slow talkers */
};
/* A dwell's length, seconds: still, slow, driving, unknown. */
static const uint16_t DWELL_S[WD_JOBS][4] = {
    { 25, 35, 45, 35 },
    { 60, 50, 40, 50 },
    { 90, 75, 60, 75 },
    { 120, 90, 75, 90 },
};

const char *wd_radio_job_name(int j) { return j >= 0 && j < WD_JOBS ? JOB_NAME[j] : "--"; }
const char *wd_radio_job_what(int j) { return j >= 0 && j < WD_JOBS ? JOB_WHAT[j] : "nothing"; }
const char *wd_radio_region_name(int g) { return g >= 0 && g < WD_REGIONS ? REGION_NAME[g] : "?"; }

uint32_t wd_radio_job_khz(int j, int g)
{
    if (j < 0 || j >= WD_JOBS || g < 0 || g >= WD_REGIONS) return 0;
    return FREQ[j][g][0];
}

int wd_radio_job_freqs(int j, int g, uint32_t khz[4])
{
    if (j < 0 || j >= WD_JOBS || g < 0 || g >= WD_REGIONS) return 0;
    int n = 0;
    while (n < 4 && FREQ[j][g][n]) { khz[n] = FREQ[j][g][n]; n++; }
    return n;
}

int wd_radio_region_of(double lat, double lon)
{
    if (!isfinite(lat) || !isfinite(lon)) return WD_REGION_NA;
    if (lon < -30) return lat > 7 ? WD_REGION_NA : WD_REGION_AU;   /* Panama and north; South America */
    if (lon > 110 && lat < 0) return WD_REGION_AU;                 /* Australia, New Zealand */
    return WD_REGION_EU;
}

int wd_radio_motion(float speed)
{
    if (!(speed >= 0)) return WD_MOVE_UNKNOWN;
    return speed < 1.5f ? WD_MOVE_STILL : speed < 6.0f ? WD_MOVE_SLOW : WD_MOVE_DRIVE;
}

int wd_radio_job_of(const wd_obs_t *o)
{
    switch (o->kind) {
    case WD_AIR: return WD_JOB_AIR;
    case WD_LORA: return WD_JOB_MESH;
    case WD_RF: return (o->tags & WD_T_VEHICLE) ? WD_JOB_TPMS : WD_JOB_ISM;
    default: return -1;
    }
}

/* The session source a job's finds belong to. */
static int job_kind(int j) { return j == WD_JOB_AIR ? WD_AIR : j == WD_JOB_MESH ? WD_LORA : WD_RF; }
static int obs_radio(int r) { return r == WD_RJ_LORA ? WD_RADIO_LORA : WD_RADIO_SDR; }

/* ------------------------------------------------------------ scheduler */

void wd_radio_reset(void)
{
    /* pins outlive a session: set before the start, kept for the next drive */
    memset(&W, 0, sizeof(W));
    for (int r = 0; r < WD_RJ_N; r++) {
        W.r[r].job = -1;
        W.r[r].region = -1;
        memset(W.r[r].trace_job, -1, sizeof(W.r[r].trace_job));
    }
    W.r[0].pin = s_pin[0]; W.r[1].pin = s_pin[1];
    W.speed = -1;
    W.motion = WD_MOVE_UNKNOWN;
}

static bool can_run(const radio_t *R, int j, uint32_t now)
{
    return R->present && (R->can >> j & 1) && W.src[job_kind(j)] &&
           (!R->jobs[j].refused_until || (int32_t)(now - R->jobs[j].refused_until) >= 0);
}

uint32_t wd_radio_dwell_ms(int r, int j, int motion)
{
    (void)r;
    if (j < 0 || j >= WD_JOBS) return 30000;
    if (motion < 0 || motion > WD_MOVE_UNKNOWN) motion = WD_MOVE_UNKNOWN;
    return DWELL_S[j][motion] * 1000u;
}

static float score_of(int r, int j, int motion, uint32_t now, const job_t *jt, int other_job)
{
    const radio_t *R = &W.r[r];
    if (!can_run(R, j, now)) return -1000;
    if (motion < 0 || motion > WD_MOVE_UNKNOWN) motion = WD_MOVE_UNKNOWN;
    float s = BASE[j][motion];
    s += 0.25f * (jt->rate < 8 ? jt->rate : 8);
    for (int k = 0; k < (jt->dry < 5 ? jt->dry : 5); k++) s *= 0.6f;
    const uint32_t since = jt->last_run_ms ? now - jt->last_run_ms : now - W.start_ms;
    const float min = since / 60000.0f;
    s += 0.04f * (min < 10 ? min : 10);
    if (other_job == j) s *= 0.4f;
    if (R->job == j) s += 0.15f;
    return s;
}

static int other_job(int r)
{
    const radio_t *O = &W.r[r ^ 1];
    return O->state == WD_RS_RUN || O->state == WD_RS_WAIT ? O->job : -1;
}

float wd_radio_score(int r, int j, const wd_rctx_t *c)
{
    if (r < 0 || r >= WD_RJ_N || j < 0 || j >= WD_JOBS) return -1000;
    return score_of(r, j, wd_radio_motion(c->speed), c->now, &W.r[r].jobs[j], other_job(r));
}

static int choose(int r, const wd_rctx_t *c)
{
    radio_t *R = &W.r[r];
    int best = -1;
    float top = -100;
    const int motion = wd_radio_motion(c->speed), oj = other_job(r);
    for (int j = 0; j < WD_JOBS; j++) {
        const float s = score_of(r, j, motion, c->now, &R->jobs[j], oj);
        R->jobs[j].score = s;
        if (s > top) { top = s; best = j; }
    }
    if (R->pin >= 0) return can_run(R, R->pin, c->now) ? R->pin : -1;
    return best;
}

/* A dwell is over: what it found goes into the job's record. */
static void dwell_end(radio_t *R, uint32_t now)
{
    if (R->job < 0) return;
    job_t *J = &R->jobs[R->job];
    const uint32_t ran = now - R->since_ms;
    if (ran >= 5000) {
        const float r = R->found * 60000.0f / (float)ran;
        J->rate = J->runs <= 1 ? r : 0.6f * J->rate + 0.4f * r;
        J->dry = R->found ? 0 : (uint8_t)(J->dry < 250 ? J->dry + 1 : J->dry);
    }
    J->last_run_ms = now ? now : 1;
}

static void start_job(int r, int j, const wd_rctx_t *c)
{
    radio_t *R = &W.r[r];
    char why[32] = "";
    if (j >= WD_JOBS) j = -1;          /* never past the table (and the compiler can see it) */
    if (j >= 0 && !wd_rport_run(r, j, c->region, why, sizeof(why))) {
        R->jobs[j].refused_until = c->now + REFUSE_MS;
        snprintf(R->why, sizeof(R->why), "%s", why[0] ? why : "refused");
        j = -1;
        wd_rport_run(r, -1, c->region, NULL, 0);
    } else if (j < 0) {
        wd_rport_run(r, -1, c->region, NULL, 0);
    }
    const bool same = j == R->job && j >= 0;
    R->job = (int8_t)j;
    R->region = (int8_t)c->region;
    R->since_ms = c->now;
    R->found = R->found_ext = 0;
    R->stretched = 0;
    R->until_ms = c->now + wd_radio_dwell_ms(r, j, wd_radio_motion(c->speed));
    (void)same;
    if (j >= 0) R->jobs[j].runs++;
    if (j >= 0) R->freq_khz = wd_radio_job_khz(j, c->region);
    else R->freq_khz = 0;
}

static void trace_roll(uint32_t now)
{
    const uint32_t b = now / WD_TRACE_MS;
    if (!W.trace_b) W.trace_b = b;
    if (b <= W.trace_b) return;
    uint32_t k = b - W.trace_b;
    if (k > WD_TRACE) k = WD_TRACE;
    for (int r = 0; r < WD_RJ_N; r++) {
        radio_t *R = &W.r[r];
        memmove(R->trace, R->trace + k, WD_TRACE - k);
        memmove(R->trace_job, R->trace_job + k, WD_TRACE - k);
        memset(R->trace + WD_TRACE - k, 0, k);
        for (uint32_t i = WD_TRACE - k; i < WD_TRACE; i++)
            R->trace_job[i] = R->state == WD_RS_RUN ? R->job : -1;
    }
    W.trace_b = b;
}

void wd_radio_tick(const wd_rctx_t *c)
{
    const uint32_t now = c->now;
    const uint32_t dt = W.last_tick && now > W.last_tick ? now - W.last_tick : 0;
    W.last_tick = now;
    W.now = now;
    W.speed = c->speed;
    W.motion = (uint8_t)wd_radio_motion(c->speed);
    W.region = (uint8_t)c->region;
    trace_roll(now);
    for (int r = 0; r < WD_RJ_N; r++) {
        radio_t *R = &W.r[r];
        wd_rport_t p;
        memset(&p, 0, sizeof(p));
        wd_rport_probe(r, &p);
        R->present = p.present;
        R->can = p.can;
        snprintf(R->name, sizeof(R->name), "%s", p.name[0] ? p.name : r == WD_RJ_LORA ? "LoRa" : "SDR");
        if (!W.on) { R->state = p.present ? WD_RS_OFF : WD_RS_ABSENT; continue; }
        if (!p.present) {
            if (R->job >= 0) { dwell_end(R, now); start_job(r, -1, c); }
            R->state = WD_RS_ABSENT;
            snprintf(R->why, sizeof(R->why), "%s", r == WD_RJ_SDR ? "not plugged in" : "no LoRa chip");
            continue;
        }
        /* Someone else's: the dwell waits for the radio to come back. */
        if (p.busy[0] && R->job >= 0) {
            R->state = WD_RS_YIELD;
            snprintf(R->why, sizeof(R->why), "%.24s has it", p.busy);
            R->until_ms += dt;
            R->since_ms += dt;
            continue;
        }
        const bool over = R->job < 0 || (int32_t)(now - R->until_ms) >= 0;
        const bool lost = R->job >= 0 && !can_run(R, R->job, now);
        const bool pinned_else = R->pin >= 0 && R->job != R->pin;
        const bool moved = R->job >= 0 && R->region != c->region;
        if (over || lost || pinned_else || moved) {
            /* Stretch a productive dwell rather than leave it. */
            if (over && !lost && !pinned_else && !moved && R->job >= 0 && R->found_ext && R->stretched < 4 &&
                R->pin < 0) {
                R->until_ms = now + wd_radio_dwell_ms(r, R->job, W.motion) / 2;
                R->stretched++;
                R->found_ext = 0;
            } else if (R->pin >= 0 && !pinned_else && !lost && !moved) {
                dwell_end(R, now);                  /* pinned: the record goes on, the job stays */
                (void)choose(r, c);                 /* ...and the scores, for the page */
                R->since_ms = now;
                R->found = R->found_ext = 0;
                R->until_ms = now + wd_radio_dwell_ms(r, R->job, W.motion);
            } else {
                if (R->job >= 0) dwell_end(R, now);
                const int j = choose(r, c);
                if (j < 0 && R->job >= 0) start_job(r, -1, c);
                else if (j >= 0) start_job(r, j, c);
                if (j < 0) snprintf(R->why, sizeof(R->why), "%s", R->pin >= 0 ? "pinned job not possible" : "no job it can run");
            }
        }
        if (R->job < 0) { R->state = WD_RS_OFF; continue; }
        R->state = p.waiting ? WD_RS_WAIT : WD_RS_RUN;
        if (p.waiting) { R->until_ms += dt; R->since_ms += dt; snprintf(R->why, sizeof(R->why), "asking for it"); }
        else R->why[0] = 0;
        if (p.freq_khz) R->freq_khz = p.freq_khz;
        if (R->state == WD_RS_RUN) R->trace_job[WD_TRACE - 1] = R->job;
    }
}

static uint32_t key_of(const wd_obs_t *o)
{
    uint32_t h = 2166136261u ^ o->kind;
    for (int i = 0; i < 6; i++) h = (h ^ o->addr[i]) * 16777619u;
    return h ? h : 1;
}

/* True the first time an ID is heard in a minute. */
static bool fresh(const wd_obs_t *o, uint32_t now)
{
    const uint32_t k = key_of(o);
    for (int i = 0; i < UNIQ_N; i++)
        if (W.uniq[i].key == k) {
            const bool again = now - W.uniq[i].ms < UNIQ_MS;
            W.uniq[i].ms = now;
            return !again;
        }
    W.uniq[W.uniq_at].key = k;
    W.uniq[W.uniq_at].ms = now;
    W.uniq_at = (W.uniq_at + 1) % UNIQ_N;
    return true;
}

void wd_radio_take(const wd_obs_t *o, int n, uint32_t now)
{
    trace_roll(now);
    for (int i = 0; i < n; i++) {
        const int j = wd_radio_job_of(&o[i]);
        if (j < 0 || !fresh(&o[i], now)) continue;
        const int r = o[i].radio == WD_RADIO_LORA ? WD_RJ_LORA : o[i].radio == WD_RADIO_SDR ? WD_RJ_SDR : -1;
        radio_t *R = r >= 0 ? &W.r[r] : NULL;
        if (!R || R->state != WD_RS_RUN || R->job != j) { W.passive++; continue; }
        R->hits++;
        R->found++;
        R->found_ext++;
        R->jobs[j].hits++;
        R->jobs[j].last_hit_ms = now ? now : 1;
        if (R->trace[WD_TRACE - 1] < 255) R->trace[WD_TRACE - 1]++;
        R->trace_job[WD_TRACE - 1] = (int8_t)j;
        /* the newest finds: one heard again comes to the front */
        char name[24];
        snprintf(name, sizeof(name), "%.23s", o[i].name);
        int at = WD_LAST - 1;
        for (int k = 0; k < WD_LAST; k++) if (!strcmp(R->last[k], name)) { at = k; break; }
        memmove(R->last[1], R->last[0], sizeof(R->last[0]) * (size_t)at);
        memmove(R->last_job + 1, R->last_job, (size_t)at);
        memcpy(R->last[0], name, sizeof(name));
        R->last_job[0] = (int8_t)j;
    }
}

/* ------------------------------------------------------------ the feed */

bool wd_radio_has(int kind)
{
    for (int r = 0; r < WD_RJ_N; r++) {
        wd_rport_t p;
        memset(&p, 0, sizeof(p));
        wd_rport_probe(r, &p);
        if (!p.present) continue;
        for (int j = 0; j < WD_JOBS; j++)
            if ((p.can >> j & 1) && job_kind(j) == kind) return true;
    }
    return false;
}

void wd_radio_start(const bool src[WD_KINDS])
{
    wd_lock();
    wd_radio_reset();
    memcpy(W.src, src, sizeof(W.src));
    W.on = src[WD_RF] || src[WD_AIR] || src[WD_LORA];
    W.start_ms = W.last_tick = 0;
    wd_unlock();
    if (W.on) wd_rport_start();
}

void wd_radio_stop(void)
{
    wd_lock();
    const bool was = W.on;
    W.on = false;
    for (int r = 0; r < WD_RJ_N; r++) {
        if (W.r[r].job >= 0) dwell_end(&W.r[r], W.now);
        W.r[r].job = -1;
        W.r[r].state = W.r[r].present ? WD_RS_OFF : WD_RS_ABSENT;
    }
    wd_unlock();
    if (was) {
        for (int r = 0; r < WD_RJ_N; r++) wd_rport_run(r, -1, W.region, NULL, 0);
        wd_rport_stop();
    }
}

int wd_radio_poll(wd_obs_t *out, int cap, uint32_t now)
{
    if (!W.on) return 0;
    wd_rctx_t c = { .region = W.region, .speed = -1, .now = now };
    wd_lock();
    if (!W.start_ms) W.start_ms = now ? now : 1;
    const wd_stats_t *s = wd_stats();
    if (s->fix_ms) {
        c.region = wd_radio_region_of(s->fix.lat, s->fix.lon);
        W.region_fix = true;
        if (now - s->fix_ms < 10000) c.speed = s->speed;
    }
    wd_radio_tick(&c);
    wd_unlock();
    int n = 0;
    for (int r = 0; r < WD_RJ_N && n < cap; r++) {
        if (W.r[r].job < 0) continue;
        const int k = wd_rport_poll(r, out + n, cap - n, now);
        for (int i = 0; i < k; i++) out[n + i].radio = (uint8_t)obs_radio(r);
        n += k;
    }
    if (n < cap) n += wd_rport_passive(out + n, cap - n, now);
    /* Only the sources the session listens to. */
    int m = 0;
    for (int i = 0; i < n; i++)
        if (out[i].kind < WD_KINDS && W.src[out[i].kind]) out[m++] = out[i];
    wd_lock();
    wd_radio_take(out, m, now);
    wd_unlock();
    return m;
}

void wd_radio_pin(int r, int job)
{
    if (r < 0 || r >= WD_RJ_N) return;
    wd_lock();
    W.r[r].pin = s_pin[r] = (int8_t)(job >= 0 && job < WD_JOBS ? job : -1);
    if (W.r[r].pin >= 0) W.r[r].jobs[W.r[r].pin].refused_until = 0;
    wd_unlock();
}

int wd_radio_pinned(int r) { return r >= 0 && r < WD_RJ_N ? s_pin[r] : -1; }

/* The next few dwells, if nothing more were found: each ends dry. */
static int plan(int r, wd_plan_step_t *out, int max)
{
    const radio_t *R = &W.r[r];
    if (!W.on || !R->present || max <= 0) return 0;
    if (R->pin >= 0) {
        out[0] = (wd_plan_step_t){ R->pin, 0, 0 };
        return 1;
    }
    job_t jobs[WD_JOBS];
    memcpy(jobs, R->jobs, sizeof(jobs));
    int cur = R->job, n = 0;
    uint32_t t = W.now;
    uint32_t end = cur >= 0 && (int32_t)(R->until_ms - W.now) > 0 ? R->until_ms : W.now;
    if (cur >= 0) out[n++] = (wd_plan_step_t){ (int8_t)cur, 0, (end - W.now + 999) / 1000 };
    const int oj = other_job(r);
    while (n < max) {
        if (cur >= 0) {
            jobs[cur].dry = (uint8_t)(jobs[cur].dry + 1);
            jobs[cur].rate *= 0.6f;
            jobs[cur].last_run_ms = end;
        }
        t = end;
        int best = -1;
        float top = -100;
        for (int j = 0; j < WD_JOBS; j++) {
            if (!can_run(R, j, t)) continue;
            float s = BASE[j][W.motion] + 0.25f * (jobs[j].rate < 8 ? jobs[j].rate : 8);
            for (int k = 0; k < (jobs[j].dry < 5 ? jobs[j].dry : 5); k++) s *= 0.6f;
            const uint32_t since = jobs[j].last_run_ms ? t - jobs[j].last_run_ms : t - W.start_ms;
            const float min = since / 60000.0f;
            s += 0.04f * (min < 10 ? min : 10);
            if (oj == j) s *= 0.4f;
            if (cur == j) s += 0.15f;
            if (s > top) { top = s; best = j; }
        }
        if (best < 0) break;
        const uint32_t d = wd_radio_dwell_ms(r, best, W.motion);
        if (n && out[n - 1].job == best) out[n - 1].for_s += d / 1000;
        else out[n++] = (wd_plan_step_t){ (int8_t)best, (t - W.now) / 1000, d / 1000 };
        cur = best;
        end = t + d;
        if (end - W.now > 15 * 60000u) break;
    }
    return n;
}

void wd_radio_view(wd_radio_state_t *o)
{
    memset(o, 0, sizeof(*o));
    wd_lock();
    o->on = W.on;
    o->region = W.region;
    o->region_fix = W.region_fix;
    o->motion = W.motion;
    o->speed = W.speed;
    o->now = W.now;
    o->passive = W.passive;
    for (int r = 0; r < WD_RJ_N; r++) {
        const radio_t *R = &W.r[r];
        wd_radio_view_t *v = &o->r[r];
        v->present = R->present;
        v->state = R->state;
        v->job = R->job;
        v->pin = s_pin[r];
        memcpy(v->name, R->name, sizeof(v->name));
        memcpy(v->why, R->why, sizeof(v->why));
        v->freq_khz = R->freq_khz;
        v->since_ms = R->since_ms;
        v->until_ms = R->until_ms;
        v->hits = R->hits;
        for (int j = 0; j < WD_JOBS; j++) {
            const job_t *J = &R->jobs[j];
            v->jobs[j] = (wd_job_view_t){ can_run(R, j, W.now), J->hits, J->runs, J->dry, J->rate, J->score,
                                          J->last_hit_ms, J->last_run_ms };
        }
        memcpy(v->trace, R->trace, sizeof(v->trace));
        memcpy(v->trace_job, R->trace_job, sizeof(v->trace_job));
        memcpy(v->last, R->last, sizeof(v->last));
        memcpy(v->last_job, R->last_job, sizeof(v->last_job));
        v->plan_n = plan(r, v->plan, WD_PLAN);
    }
    wd_unlock();
    /* No drive: what is fitted and what it could do, so a pin can be set
       before the start. */
    if (!o->on)
        for (int r = 0; r < WD_RJ_N; r++) {
            wd_rport_t p;
            memset(&p, 0, sizeof(p));
            wd_rport_probe(r, &p);
            wd_radio_view_t *v = &o->r[r];
            v->present = p.present;
            v->state = p.present ? WD_RS_OFF : WD_RS_ABSENT;
            v->job = -1;
            v->freq_khz = 0;
            v->why[0] = 0;
            memset(v->trace_job, -1, sizeof(v->trace_job));
            snprintf(v->name, sizeof(v->name), "%s", p.name[0] ? p.name : r == WD_RJ_LORA ? "LoRa" : "SDR");
            for (int j = 0; j < WD_JOBS; j++) v->jobs[j].can = p.present && (p.can >> j & 1);
        }
}

/* ------------------------------------------------------------ sightings */

static int8_t level(float dbm)
{
    if (!isfinite(dbm)) return -90;
    return (int8_t)(dbm < -127 ? -127 : dbm > 0 ? 0 : lroundf(dbm));
}

/* rtl_433 prints IDs in hex for most devices and in decimal for some: hex
   when it has a hex letter or the device is a tyre sensor. */
static uint32_t parse_id(const char *s, bool hex_default)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; hex_default = true; }
    bool letters = false, all_hex = *s != 0;
    for (const char *p = s; *p; p++) {
        if (!isxdigit((unsigned char)*p)) { all_hex = false; break; }
        if (isalpha((unsigned char)*p)) letters = true;
    }
    if (!all_hex) {
        uint32_t h = 2166136261u;
        for (const char *p = s; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        return h;
    }
    return (uint32_t)strtoull(s, NULL, letters || hex_default ? 16 : 10);
}

bool wd_radio_obs_ism(wd_obs_t *o, const lr433_msg_t *m, uint32_t hz, float rssi, int radio)
{
    memset(o, 0, sizeof(*o));
    if (!m->id[0]) return false;
    const bool known = m->proto < LR433_P_COUNT;
    const lr433_class_t cls = known ? lr433_proto_class(m->proto) : LR433_SENSOR;
    const uint32_t id = parse_id(m->id, cls == LR433_TPMS);
    o->kind = WD_RF;
    o->radio = (uint8_t)radio;
    o->sub = (uint8_t)(m->proto + 1);
    o->addr[0] = (uint8_t)(m->proto + 1);
    o->addr[1] = (uint8_t)(m->channel >= 0 ? m->channel + 1 : 0);
    o->addr[2] = (uint8_t)(id >> 24); o->addr[3] = (uint8_t)(id >> 16);
    o->addr[4] = (uint8_t)(id >> 8);  o->addr[5] = (uint8_t)id;
    o->rssi = level(rssi);
    o->freq_khz = hz / 1000;
    const char *label = known ? lr433_proto_label(m->proto) : m->model;
    if (cls == LR433_TPMS) {
        o->tags = WD_T_VEHICLE;
        snprintf(o->name, sizeof(o->name), "TPMS %.9s %.15s", label, m->id);
    } else {
        /* meters are logged, not targets: WD_T_INFRA means gunshot sensors and the like (HUNT alerts on it) */
        snprintf(o->name, sizeof(o->name), "%.12s %.15s", label, m->id);
    }
    return true;
}

void wd_radio_obs_air(wd_obs_t *o, const wd_air_t *a, int radio)
{
    memset(o, 0, sizeof(*o));
    o->kind = WD_AIR;
    o->radio = (uint8_t)radio;
    o->sub = a->cat;
    o->addr[2] = a->qual;
    o->addr[3] = (uint8_t)(a->icao >> 16); o->addr[4] = (uint8_t)(a->icao >> 8); o->addr[5] = (uint8_t)a->icao;
    o->rssi = -80;                  /* the table keeps no level */
    o->freq_khz = 1090000;
    char cs[9];
    int n = 0;
    for (int i = 0; i < 8 && a->callsign[i]; i++)
        if (a->callsign[i] != ' ' && a->callsign[i] != '_') cs[n++] = a->callsign[i];
    cs[n] = 0;
    if (n) snprintf(o->name, sizeof(o->name), "%s", cs);
    else snprintf(o->name, sizeof(o->name), "ICAO %06X", (unsigned)(a->icao & 0xFFFFFF));
    if (a->pos && isfinite(a->lat) && isfinite(a->lon) && fabsf(a->lat) <= 90 && fabsf(a->lon) <= 180 &&
        (a->lat != 0 || a->lon != 0)) {
        o->own_pos = true;
        o->lat = a->lat;
        o->lon = a->lon;
    }
}

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool wd_radio_obs_lora(wd_obs_t *o, const uint8_t *p, int n, int flavour, float rssi, uint32_t hz, int radio)
{
    memset(o, 0, sizeof(*o));
    o->kind = WD_LORA;
    o->radio = (uint8_t)radio;
    o->sub = (uint8_t)flavour;
    o->rssi = level(rssi);
    o->freq_khz = hz / 1000;
    if (flavour == WD_MESH_MESHTASTIC) {
        /* dest, sender, packet id, flags, channel hash, next hop, relay */
        if (n < 17) return false;
        const uint32_t from = le32(p + 4);
        const uint8_t flags = p[12], limit = flags & 7, start = flags >> 5;
        if (!from || from == 0xFFFFFFFFu || (start && limit > start)) return false;
        o->addr[0] = 'M'; o->addr[1] = 'T';
        o->addr[2] = (uint8_t)(from >> 24); o->addr[3] = (uint8_t)(from >> 16);
        o->addr[4] = (uint8_t)(from >> 8);  o->addr[5] = (uint8_t)from;
        snprintf(o->name, sizeof(o->name), "!%08x", (unsigned)from);
        return true;
    }
    if (flavour == WD_MESH_MESHCORE) {
        /* header: route type, payload type, version; transport codes; path */
        if (n < 2) return false;
        const uint8_t h = p[0], route = h & 3, type = (h >> 2) & 15;
        if (h >> 6) return false;
        int i = 1 + ((route == 0 || route == 3) ? 4 : 0);
        if (i >= n) return false;
        /* The path byte packs the hash size (top 2 bits, +1) and the hop count
           (low 6), as MeshCore's Packet.h; size 4 is not valid. */
        const int pb = p[i++], hops = pb & 63, hsize = (pb >> 6) + 1;
        if (hsize == 4 || hops * hsize > 64) return false;
        i += hops * hsize;
        if (type != 4 || n < i + 32 + 4 + 64 + 1) return false;        /* only an advert names a node */
        const uint8_t *key = p + i;
        memcpy(o->addr, key, 6);
        const uint8_t fl = p[i + 100];
        int k = i + 101;
        if (fl & 0x10) {
            if (n < k + 8) return false;
            const double lat = (int32_t)le32(p + k) / 1e6, lon = (int32_t)le32(p + k + 4) / 1e6;
            if (fabs(lat) <= 90 && fabs(lon) <= 180 && (lat != 0 || lon != 0)) {
                o->own_pos = true; o->lat = lat; o->lon = lon;
            }
            k += 8;
        }
        if (fl & 0x20) k += 2;
        if (fl & 0x40) k += 2;
        int c = 0;
        if (fl & 0x80)
            for (; k < n && c < 32; k++) {
                const uint8_t ch = p[k];
                if (!ch) break;
                o->name[c++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '?');
            }
        o->name[c] = 0;
        if (!c) snprintf(o->name, sizeof(o->name), "MC %02X%02X%02X%02X", key[0], key[1], key[2], key[3]);
        /* a repeater or room server is infrastructure, logged but not a target */
        return true;
    }
    if (flavour == WD_MESH_LORAWAN) {
        const uint8_t mtype = p[0] >> 5;
        if (n < 12 || (p[0] & 3)) return false;
        if (mtype == 0 && n == 23) {                /* join request: the DevEUI */
            for (int b = 0; b < 6; b++) o->addr[b] = p[16 - b];
            snprintf(o->name, sizeof(o->name), "JOIN %02X%02X%02X%02X%02X%02X%02X%02X",
                     p[16], p[15], p[14], p[13], p[12], p[11], p[10], p[9]);
            return true;
        }
        if (mtype != 2 && mtype != 4) return false;  /* uplinks */
        const uint32_t dev = le32(p + 1);
        o->addr[0] = 'L'; o->addr[1] = 'W';
        o->addr[2] = (uint8_t)(dev >> 24); o->addr[3] = (uint8_t)(dev >> 16);
        o->addr[4] = (uint8_t)(dev >> 8);  o->addr[5] = (uint8_t)dev;
        snprintf(o->name, sizeof(o->name), "LoRaWAN %08X", (unsigned)dev);
        return true;
    }
    return false;
}
