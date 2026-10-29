/* Channelised airband / CB / weather scanner. See air_scan.h.

   Channel plans: airband 118-137 MHz on 25 kHz and on the 8.33 kHz raster
   (25/3 kHz, EUROCONTROL), 121.5 MHz the international emergency channel;
   CB 26.965-27.405 MHz in FCC part 95 channel order (23 sits above 24 and
   25); NOAA Weather Radio 162.400-162.550 MHz numbered WX1-WX7 as NWS does. */
#include "air_scan.h"

#include <string.h>
#include "ls_cold.h"

static const uint32_t CB_HZ[40] = {
    26965000, 26975000, 26985000, 27005000, 27015000, 27025000, 27035000, 27055000,
    27065000, 27075000, 27085000, 27105000, 27115000, 27125000, 27135000, 27155000,
    27165000, 27175000, 27185000, 27205000, 27215000, 27225000, 27255000, 27235000,
    27245000, 27265000, 27275000, 27285000, 27295000, 27305000, 27315000, 27325000,
    27335000, 27345000, 27355000, 27365000, 27375000, 27385000, 27395000, 27405000,
};
static const uint32_t WX_HZ[7] = {
    162550000, 162400000, 162475000, 162425000, 162450000, 162500000, 162525000,
};

static const air_preset_t PRESETS[AIR_PRESET_COUNT] = {
    [AIR_PRESET_OFF]    = { "OFF", "", AIR_DEMOD_AM, 0, 0, 0, NULL, 0, 0, 0 },
    [AIR_PRESET_AIR25]  = { "AIR 25k", "AIRBAND", AIR_DEMOD_AM, 118000000, 25000, 760, NULL, 0, 121500000, 2500 },
    [AIR_PRESET_AIR833] = { "AIR 8.33k", "AIRBAND", AIR_DEMOD_AM, 118000000, 0, 2280, NULL, 0, 121500000, 2000 },
    [AIR_PRESET_CB_AM]  = { "CB AM", "CB", AIR_DEMOD_AM, 0, 0, 0, CB_HZ, 40, 0, 2500 },
    [AIR_PRESET_CB_USB] = { "CB USB", "CB", AIR_DEMOD_USB, 0, 0, 0, CB_HZ, 40, 0, 2500 },
    [AIR_PRESET_CB_LSB] = { "CB LSB", "CB", AIR_DEMOD_LSB, 0, 0, 0, CB_HZ, 40, 0, 2500 },
    [AIR_PRESET_WX]     = { "WEATHER", "WEATHER", AIR_DEMOD_NFM, 0, 0, 0, WX_HZ, 7, 0, 4000 },
};

const air_preset_t *air_preset(int id)
{
    return id > AIR_PRESET_OFF && id < AIR_PRESET_COUNT ? &PRESETS[id] : NULL;
}

int air_channel_count(const air_preset_t *p)
{
    return !p ? 0 : p->list ? p->list_n : p->raster_n;
}

uint32_t air_channel_hz(const air_preset_t *p, int i)
{
    if (!p || i < 0 || i >= air_channel_count(p)) return 0;
    if (p->list) return p->list[i];
    /* 8.33 kHz is 25/3 kHz exactly: kept in thirds so it never drifts. */
    if (!p->step_hz) return p->lo_hz + (uint32_t)(((uint64_t)i * 25000u + 1u) / 3u);
    return p->lo_hz + (uint32_t)i * p->step_hz;
}

int air_channel_number(const air_preset_t *p, int i)
{
    return p && p->list ? i + 1 : 0;
}

int air_channel_find(const air_preset_t *p, uint32_t hz)
{
    const int n = air_channel_count(p);
    int best = -1;
    uint32_t best_d = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t f = air_channel_hz(p, i);
        const uint32_t d = f > hz ? f - hz : hz - f;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    const uint32_t half = p && p->step_hz ? p->step_hz / 2 : p && !p->list ? 4166 : 5000;
    return best >= 0 && best_d <= half ? best : -1;
}

/* ---- windows ------------------------------------------------------------- */

static int sorted_ch(const air_scan_t *s, int k) { return s->p->list ? s->order[k] : k; }

static bool clear_of_dc(const air_scan_t *s, int first, int n, uint32_t centre)
{
    for (int k = first; k < first + n; ++k) {
        const uint32_t f = air_channel_hz(s->p, sorted_ch(s, k));
        const uint32_t d = f > centre ? f - centre : centre - f;
        if (d < 3000) return false;
    }
    return true;
}

LS_COLD static void plan_windows(air_scan_t *s)
{
    if (s->p->list) {
        const int n = s->p->list_n < 64 ? s->p->list_n : 64;
        for (int i = 0; i < n; ++i) s->order[i] = (uint16_t)i;
        for (int i = 1; i < n; ++i)           /* insertion sort, 40 at most */
            for (int j = i; j > 0 && s->p->list[s->order[j]] < s->p->list[s->order[j - 1]]; --j) {
                const uint16_t t = s->order[j]; s->order[j] = s->order[j - 1]; s->order[j - 1] = t;
            }
    }
    const uint32_t spacing = s->p->step_hz ? s->p->step_hz : s->p->list ? 10000u : 8333u;
    s->nwin = 0;
    for (int k = 0; k < s->nch && s->nwin < AIR_MAX_WIN;) {
        const uint32_t f0 = air_channel_hz(s->p, sorted_ch(s, k));
        int n = 1;
        while (k + n < s->nch && air_channel_hz(s->p, sorted_ch(s, k + n)) - f0 <= AIR_SPAN_HZ) ++n;
        const uint32_t f1 = air_channel_hz(s->p, sorted_ch(s, k + n - 1));
        uint32_t c = f0 + (f1 - f0) / 2;
        /* The LO's own spike sits at the centre: keep every channel 3 kHz
           clear of it. */
        static const int8_t nudge[] = { 0, 2, -2, 4, -4, 1, -1 };
        for (unsigned t = 0; t < sizeof(nudge); ++t) {
            const uint32_t tryc = (uint32_t)((int64_t)f0 + (f1 - f0) / 2 + nudge[t] * (int64_t)spacing / 4);
            if (clear_of_dc(s, k, n, tryc)) { c = tryc; break; }
        }
        s->win_hz[s->nwin] = c;
        s->win_first[s->nwin] = (uint16_t)k;
        s->win_n[s->nwin] = (uint16_t)n;
        s->nwin++;
        k += n;
    }
}

/* ---- measuring ------------------------------------------------------------- */

static int bin_of(int32_t off_hz, int n, uint32_t rate)
{
    const int64_t b = (int64_t)n / 2 + ((int64_t)off_hz * n + (off_hz >= 0 ? (int64_t)rate / 2 : -(int64_t)rate / 2)) / (int64_t)rate;
    return (int)b;
}

/* Median of the usable bins, by a 1 dB histogram. */
static float window_floor(const float *db, int n, uint32_t rate)
{
    uint16_t hist[160];
    memset(hist, 0, sizeof(hist));
    const int lo = bin_of(-(int32_t)(AIR_SPAN_HZ / 2 + 8000), n, rate);
    const int hi = bin_of((int32_t)(AIR_SPAN_HZ / 2 + 8000), n, rate);
    int count = 0;
    for (int i = lo < 0 ? 0 : lo; i <= hi && i < n; ++i) {
        if (i >= n / 2 - 3 && i <= n / 2 + 3) continue;          /* the DC spike */
        int v = (int)(db[i] + 150.0f);
        if (v < 0) v = 0;
        if (v > 159) v = 159;
        hist[v]++;
        count++;
    }
    int acc = 0;
    for (int v = 0; v < 160; ++v) {
        acc += hist[v];
        if (acc * 2 >= count) return (float)v - 150.0f + 0.5f;
    }
    return -150.0f;
}

static float peak_near(const float *db, int n, uint32_t rate, int32_t off, uint16_t bw)
{
    const int a = bin_of(off - bw, n, rate), b = bin_of(off + bw, n, rate);
    float pk = -200.0f;
    for (int i = a < 0 ? 0 : a; i <= b && i < n; ++i) if (db[i] > pk) pk = db[i];
    return pk;
}

/* A channel's level above the floor; 0 for an image of a stronger carrier
   at the mirror frequency (the dongle's I/Q imbalance puts one 30-40 dB
   down on the other side of the LO). */
static float channel_level(air_scan_t *s, const float *db, int n, uint32_t rate,
                           float floor, int32_t off)
{
    const float pk = peak_near(db, n, rate, off, s->p->bw_hz);
    const float mirror = peak_near(db, n, rate, -off, s->p->bw_hz);
    if (mirror - pk > 18.0f && pk - floor < 40.0f) { s->images_rejected++; return 0.0f; }
    return pk - floor;
}

static void note_level(air_scan_t *s, int ch, float lvl)
{
    int v = (int)(lvl + 0.5f);
    s->level[ch] = (uint8_t)(v < 0 ? 0 : v > 63 ? 63 : v);
}

/* ---- the hit list ---------------------------------------------------------- */

static air_hit_t *hit_for(air_scan_t *s, int ch)
{
    for (int i = 0; i < s->nhits; ++i) if (s->hits[i].channel == ch) return &s->hits[i];
    return NULL;
}

static void hit_open(air_scan_t *s, int ch, float lvl, int64_t now)
{
    air_hit_t *h = hit_for(s, ch);
    if (!h) {
        if (s->nhits < AIR_HITS) h = &s->hits[s->nhits++];
        else {
            h = &s->hits[0];                       /* the longest silent */
            for (int i = 1; i < s->nhits; ++i) if (s->hits[i].last_us < h->last_us) h = &s->hits[i];
        }
        memset(h, 0, sizeof(*h));
        h->channel = (uint16_t)ch;
        h->hz = air_channel_hz(s->p, ch);
        h->first_us = now;
        h->guard = ch == s->guard_ch;
    }
    h->count++;
    h->last_us = now;
    const int v = (int)(lvl + 0.5f);
    if (v > h->peak_db) h->peak_db = (int8_t)(v > 127 ? 127 : v);
}

/* ---- the machine ----------------------------------------------------------- */

static air_action_t retune(air_scan_t *s, uint32_t lo)
{
    s->lo_hz = lo;
    s->drop = s->settle;
    air_action_t a = { .tune_hz = lo };
    if (s->phase == AIR_PH_LISTEN) {
        a.listen = true;
        a.listen_hz = air_channel_hz(s->p, s->listen_ch);
    }
    return a;
}

static air_action_t hold_action(const air_scan_t *s)
{
    air_action_t a = {0};
    if (s->phase == AIR_PH_LISTEN) {
        a.listen = true;
        a.listen_hz = air_channel_hz(s->p, s->listen_ch);
        a.audio = s->carrier || s->held;
    }
    return a;
}

static uint32_t guard_lo(const air_scan_t *s)
{
    return air_channel_hz(s->p, s->guard_ch) + 10000u;
}

static air_action_t next_window(air_scan_t *s, int64_t now)
{
    s->phase = AIR_PH_HOP;
    s->held = false;
    if (++s->win >= s->nwin) {
        s->win = 0;
        s->sweeps++;
        if (s->sweep_t0_us) s->sweep_ms = (uint32_t)((now - s->sweep_t0_us) / 1000);
        s->sweep_t0_us = now;
    }
    s->hops++;
    if (s->guard_on && s->guard_ch >= 0 && s->hops >= s->guard_every) {
        s->hops = 0;
        s->phase = AIR_PH_GUARD;
        s->resume = AIR_PH_HOP;
        return retune(s, guard_lo(s));
    }
    return retune(s, s->win_hz[s->win]);
}

static air_action_t start_listen(air_scan_t *s, int ch, float lvl, int64_t now)
{
    s->phase = AIR_PH_LISTEN;
    s->listen_ch = ch;
    s->carrier = true;
    s->carrier_us = s->listen_since_us = s->talk_mark_us = now;
    s->guard_due_us = now + (int64_t)s->guard_ms * 1000;
    hit_open(s, ch, lvl, now);
    return retune(s, air_channel_hz(s->p, ch) + AIR_LISTEN_OFS);
}

void air_scan_stop(air_scan_t *s)
{
    s->phase = AIR_PH_IDLE;
    s->preset = AIR_PRESET_OFF;
    s->p = NULL;
}

bool air_scan_active(const air_scan_t *s) { return s->phase != AIR_PH_IDLE && s->p; }

LS_COLD air_action_t air_scan_start(air_scan_t *s, int preset, int64_t now_us)
{
    air_action_t none = {0};
    const air_preset_t *p = air_preset(preset);
    if (!p) { air_scan_stop(s); return none; }
    if (s->preset != preset) {
        /* A different band: its own lockouts and hits. */
        memset(s->lockout, 0, sizeof(s->lockout));
        memset(s->hits, 0, sizeof(s->hits));
        s->nhits = 0;
    }
    memset(s->level, 0, sizeof(s->level));
    s->p = p;
    s->preset = preset;
    s->nch = air_channel_count(p);
    if (s->nch > AIR_MAX_CH) s->nch = AIR_MAX_CH;
    if (s->thresh_db <= 0.0f) s->thresh_db = 10.0f;
    if (s->settle <= 0) s->settle = 1;
    if (s->hang_ms <= 0) s->hang_ms = 2500;
    if (s->guard_every <= 0) s->guard_every = 12;
    if (s->guard_ms <= 0) s->guard_ms = 3000;
    plan_windows(s);
    s->guard_ch = p->guard_hz ? air_channel_find(p, p->guard_hz) : -1;
    s->guard_on = s->guard_ch >= 0;      /* the caller may turn it off after */
    s->win = 0;
    s->hops = 0;
    s->held = false;
    s->carrier = false;
    s->sweeps = 0;
    s->sweep_t0_us = now_us;
    s->images_rejected = 0;
    s->phase = AIR_PH_HOP;
    return retune(s, s->win_hz[0]);
}

air_action_t air_scan_block(air_scan_t *s, const float *db, int n, uint32_t rate,
                            int64_t now)
{
    air_action_t none = {0};
    if (!air_scan_active(s)) return none;
    if (s->drop > 0 || !db || n < 16 || !rate) {
        if (s->drop > 0) s->drop--;
        return hold_action(s);
    }
    const float floor = window_floor(db, n, rate);

    if (s->phase == AIR_PH_HOP) {
        int best = -1;
        float best_lvl = 0.0f;
        for (int k = s->win_first[s->win]; k < s->win_first[s->win] + s->win_n[s->win]; ++k) {
            const int ch = sorted_ch(s, k);
            const int32_t off = (int32_t)((int64_t)air_channel_hz(s->p, ch) - (int64_t)s->lo_hz);
            const float lvl = channel_level(s, db, n, rate, floor, off);
            note_level(s, ch, lvl);
            if (lvl >= s->thresh_db && !air_scan_locked(s, ch) && lvl > best_lvl) {
                best = ch;
                best_lvl = lvl;
            }
        }
        if (best >= 0) return start_listen(s, best, best_lvl, now);
        return next_window(s, now);
    }

    if (s->phase == AIR_PH_GUARD) {
        const int32_t off = (int32_t)((int64_t)air_channel_hz(s->p, s->guard_ch) - (int64_t)s->lo_hz);
        const float lvl = channel_level(s, db, n, rate, floor, off);
        note_level(s, s->guard_ch, lvl);
        if (lvl >= s->thresh_db && !air_scan_locked(s, s->guard_ch) &&
            !(s->resume == AIR_PH_LISTEN && s->listen_ch == s->guard_ch))
            return start_listen(s, s->guard_ch, lvl, now);
        if (s->resume == AIR_PH_LISTEN) {
            s->phase = AIR_PH_LISTEN;
            s->guard_due_us = now + (int64_t)s->guard_ms * 1000;
            /* The gap was the guard's, not the channel's: the hang starts
               again from here. */
            s->carrier_us = now;
            s->talk_mark_us = now;
            return retune(s, air_channel_hz(s->p, s->listen_ch) + AIR_LISTEN_OFS);
        }
        s->phase = AIR_PH_HOP;
        return retune(s, s->win_hz[s->win]);
    }

    /* LISTEN: the channel sits AIR_LISTEN_OFS below the LO. */
    const float lvl = channel_level(s, db, n, rate, floor, -(int32_t)AIR_LISTEN_OFS);
    note_level(s, s->listen_ch, lvl);
    const bool was = s->carrier;
    s->carrier = lvl >= (was ? s->thresh_db - 3.0f : s->thresh_db);
    air_hit_t *h = hit_for(s, s->listen_ch);
    if (s->carrier) {
        if (h && was) h->talk_ms += (uint32_t)((now - s->talk_mark_us) / 1000);
        if (h) { h->last_us = now; if ((int)lvl > h->peak_db) h->peak_db = (int8_t)(lvl > 127 ? 127 : lvl); }
        s->carrier_us = now;
    } else if (h && was) {
        h->talk_ms += (uint32_t)((now - s->talk_mark_us) / 1000);
    }
    s->talk_mark_us = now;
    if (!s->held && now - s->carrier_us > (int64_t)s->hang_ms * 1000)
        return next_window(s, now);
    if (s->guard_on && s->guard_ch >= 0 && s->listen_ch != s->guard_ch && now >= s->guard_due_us) {
        s->phase = AIR_PH_GUARD;
        s->resume = AIR_PH_LISTEN;
        return retune(s, guard_lo(s));
    }
    return hold_action(s);
}

air_action_t air_scan_skip(air_scan_t *s, int64_t now_us)
{
    air_action_t none = {0};
    if (!air_scan_active(s)) return none;
    return next_window(s, now_us);
}

air_action_t air_scan_lockout(air_scan_t *s, int64_t now_us)
{
    air_action_t none = {0};
    if (!air_scan_active(s)) return none;
    if (s->phase == AIR_PH_LISTEN && s->listen_ch >= 0 && s->listen_ch < AIR_MAX_CH)
        s->lockout[s->listen_ch / 8] |= (uint8_t)(1u << (s->listen_ch % 8));
    return next_window(s, now_us);
}

void air_scan_hold(air_scan_t *s, bool hold) { s->held = hold; }

air_action_t air_scan_listen(air_scan_t *s, int ch, int64_t now_us)
{
    air_action_t none = {0};
    if (!air_scan_active(s) || ch < 0 || ch >= s->nch) return none;
    air_action_t a = start_listen(s, ch, 0.0f, now_us);
    s->held = true;
    return a;
}

void air_scan_clear_lockouts(air_scan_t *s) { memset(s->lockout, 0, sizeof(s->lockout)); }

bool air_scan_locked(const air_scan_t *s, int ch)
{
    return ch >= 0 && ch < AIR_MAX_CH && (s->lockout[ch / 8] >> (ch % 8)) & 1;
}

int air_scan_lockout_count(const air_scan_t *s)
{
    int n = 0;
    for (int i = 0; i < s->nch; ++i) n += air_scan_locked(s, i);
    return n;
}
