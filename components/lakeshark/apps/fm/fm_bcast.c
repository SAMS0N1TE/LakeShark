/* FM broadcast extras: options and published snapshots. See fm_bcast.h. */
#include "fm_bcast.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "settings.h"

static uint32_t s_opts;

uint32_t fm_bcast_options(void) { return __atomic_load_n(&s_opts, __ATOMIC_ACQUIRE); }
bool fm_bcast_opt(uint32_t bit) { return (fm_bcast_options() & bit) != 0; }

void fm_bcast_set_opt(uint32_t bit, bool on)
{
    uint32_t v = fm_bcast_options();
    v = on ? (v | bit) : (v & ~bit);
    __atomic_store_n(&s_opts, v, __ATOMIC_RELEASE);
    settings_set_fm_bcast(v);
}

int fm_bcast_wx_audio(void)
{
    const int v = (int)((fm_bcast_options() & FM_BC_WX_MASK) >> FM_BC_WX_SHIFT);
    return v > 2 ? 0 : v;
}

void fm_bcast_set_wx_audio(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    const uint32_t v = (fm_bcast_options() & ~FM_BC_WX_MASK) | ((uint32_t)mode << FM_BC_WX_SHIFT);
    __atomic_store_n(&s_opts, v, __ATOMIC_RELEASE);
    settings_set_fm_bcast(v);
}

/* The weather card is small and changes rarely: a plain copy under the
   same kind of sequence lock. */
static fm_wx_status_t s_wx;
static uint32_t s_wx_lock;

void fm_wx_publish(const fm_wx_status_t *s)
{
    __atomic_add_fetch(&s_wx_lock, 1, __ATOMIC_ACQ_REL);
    const uint32_t pubs = s_wx.pubs + 1;
    memcpy(&s_wx, s, sizeof(s_wx));
    s_wx.pubs = pubs;
    __atomic_add_fetch(&s_wx_lock, 1, __ATOMIC_ACQ_REL);
}

void fm_wx_status(fm_wx_status_t *out)
{
    for (int tries = 0; tries < 16; ++tries) {
        const uint32_t a = __atomic_load_n(&s_wx_lock, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        memcpy(out, &s_wx, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_wx_lock, __ATOMIC_ACQUIRE) == a) return;
    }
    memcpy(out, &s_wx, sizeof(*out));
}

void fm_bcast_load(void)
{
    __atomic_store_n(&s_opts, settings_get_fm_bcast(0), __ATOMIC_RELEASE);
}

/* One writer (the receive task), any number of readers: a sequence lock.
   Odd while a publish is under way; a reader that saw it move copies again. */
static EXT_RAM_BSS_ATTR fm_wfm_status_t s_wfm;
static uint32_t s_wfm_lock;
static uint32_t s_wfm_seq;

static EXT_RAM_BSS_ATTR uint8_t s_hl[FM_WFM_HIST], s_hr[FM_WFM_HIST], s_hq[FM_WFM_HIST];
static EXT_RAM_BSS_ATTR bool s_hm[FM_WFM_HIST];
static int s_hn, s_hpos;
static uint32_t s_good_seen, s_bad_seen;

uint8_t fm_wfm_level_byte(float level)
{
    float v = level <= 1e-4f ? 0.0f : 1.0f + 0.5f * log10f(level);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint8_t)(v * 255.0f + 0.5f);
}

static void history_push(const fm_wfm_status_t *s)
{
    if (!s->running) { s_hn = 0; s_good_seen = s_bad_seen = 0; return; }
    const uint32_t good = s->rds.blocks_ok + s->rds.blocks_fixed, bad = s->rds.blocks_bad;
    if (good < s_good_seen || bad < s_bad_seen) s_good_seen = s_bad_seen = 0;
    const uint32_t dg = good - s_good_seen, db = bad - s_bad_seen;
    s_good_seen = good;
    s_bad_seen = bad;
    s_hl[s_hpos] = fm_wfm_level_byte(s->lvl_l);
    s_hr[s_hpos] = fm_wfm_level_byte(s->lvl_r);
    s_hq[s_hpos] = !(dg || db) ? 0 : db == 0 ? 1 : dg > db ? 2 : 3;
    s_hm[s_hpos] = !s->stereo;
    s_hpos = (s_hpos + 1) % FM_WFM_HIST;
    if (s_hn < FM_WFM_HIST) s_hn++;
}

int fm_wfm_history(uint8_t *l, uint8_t *r, uint8_t *q, bool *mono, int max)
{
    const int n = s_hn < max ? s_hn : max;
    for (int i = 0; i < n; ++i) {
        const int at = (s_hpos - n + i + 2 * FM_WFM_HIST) % FM_WFM_HIST;
        l[i] = s_hl[at]; r[i] = s_hr[at]; q[i] = s_hq[at]; mono[i] = s_hm[at];
    }
    return n;
}

void fm_wfm_publish(const fm_wfm_status_t *s)
{
    history_push(s);
    __atomic_add_fetch(&s_wfm_lock, 1, __ATOMIC_ACQ_REL);
    if (s_wfm.rds.seq != s->rds.seq || s_wfm.stereo != s->stereo ||
        s_wfm.running != s->running)
        __atomic_add_fetch(&s_wfm_seq, 1, __ATOMIC_RELAXED);
    const uint32_t pubs = s_wfm.pubs + 1;
    memcpy(&s_wfm, s, sizeof(s_wfm));
    s_wfm.pubs = pubs;
    __atomic_add_fetch(&s_wfm_lock, 1, __ATOMIC_ACQ_REL);
}

void fm_wfm_status(fm_wfm_status_t *out)
{
    for (int tries = 0; tries < 16; ++tries) {
        const uint32_t a = __atomic_load_n(&s_wfm_lock, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        memcpy(out, &s_wfm, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_wfm_lock, __ATOMIC_ACQUIRE) == a) return;
    }
    memcpy(out, &s_wfm, sizeof(*out));
}

uint32_t fm_wfm_seq(void) { return __atomic_load_n(&s_wfm_seq, __ATOMIC_RELAXED); }

/* ---- AIR ----------------------------------------------------------------- */

static EXT_RAM_BSS_ATTR fm_air_status_t s_air;
static EXT_RAM_BSS_ATTR uint8_t s_air_levels[AIR_MAX_CH];
static int s_air_nlevels;
static uint32_t s_air_lock;

void fm_air_publish(const fm_air_status_t *s, const uint8_t *levels, int n)
{
    __atomic_add_fetch(&s_air_lock, 1, __ATOMIC_ACQ_REL);
    const uint32_t pubs = s_air.pubs + 1;
    memcpy(&s_air, s, sizeof(s_air));
    s_air.pubs = pubs;
    __atomic_add_fetch(&s_air_lock, 1, __ATOMIC_ACQ_REL);
    /* The band picture tolerates a torn read: it is one byte a channel. */
    if (n > AIR_MAX_CH) n = AIR_MAX_CH;
    if (levels && n > 0) memcpy(s_air_levels, levels, (size_t)n);
    s_air_nlevels = n > 0 ? n : 0;
}

void fm_air_status(fm_air_status_t *out)
{
    for (int tries = 0; tries < 16; ++tries) {
        const uint32_t a = __atomic_load_n(&s_air_lock, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        memcpy(out, &s_air, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_air_lock, __ATOMIC_ACQUIRE) == a) return;
    }
    memcpy(out, &s_air, sizeof(*out));
}

int fm_air_levels(uint8_t *out, int max)
{
    const int n = s_air_nlevels < max ? s_air_nlevels : max;
    memcpy(out, s_air_levels, (size_t)n);
    return n;
}

/* Four requests in flight, one writer (UI) and one reader (receive task). */
static uint32_t s_req[4];
static uint32_t s_req_head, s_req_tail;

void fm_air_request(int req, int arg)
{
    const uint32_t h = __atomic_load_n(&s_req_head, __ATOMIC_RELAXED);
    if (h - __atomic_load_n(&s_req_tail, __ATOMIC_ACQUIRE) >= 4) return;
    s_req[h & 3] = ((uint32_t)req << 24) | ((uint32_t)arg & 0xFFFFFFu);
    __atomic_store_n(&s_req_head, h + 1, __ATOMIC_RELEASE);
}

bool fm_air_take(int *req, int *arg)
{
    const uint32_t t = __atomic_load_n(&s_req_tail, __ATOMIC_RELAXED);
    if (t == __atomic_load_n(&s_req_head, __ATOMIC_ACQUIRE)) return false;
    const uint32_t v = s_req[t & 3];
    __atomic_store_n(&s_req_tail, t + 1, __ATOMIC_RELEASE);
    *req = (int)(v >> 24);
    *arg = (int)(v & 0xFFFFFFu);
    return true;
}

/* ---- SSTV ---------------------------------------------------------------- */

static EXT_RAM_BSS_ATTR fm_sstv_status_t s_sstv;
static uint32_t s_sstv_lock;
static const uint16_t *s_sstv_pix;
static int s_sstv_w, s_sstv_h;

void fm_sstv_publish(const fm_sstv_status_t *s)
{
    __atomic_add_fetch(&s_sstv_lock, 1, __ATOMIC_ACQ_REL);
    const uint32_t pubs = s_sstv.pubs + 1;
    memcpy(&s_sstv, s, sizeof(s_sstv));
    s_sstv.pubs = pubs;
    __atomic_add_fetch(&s_sstv_lock, 1, __ATOMIC_ACQ_REL);
}

void fm_sstv_status(fm_sstv_status_t *out)
{
    for (int tries = 0; tries < 16; ++tries) {
        const uint32_t a = __atomic_load_n(&s_sstv_lock, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        memcpy(out, &s_sstv, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_sstv_lock, __ATOMIC_ACQUIRE) == a) return;
    }
    memcpy(out, &s_sstv, sizeof(*out));
}

void fm_sstv_set_image(const uint16_t *pix, int w, int h)
{
    __atomic_store_n(&s_sstv_pix, NULL, __ATOMIC_RELEASE);
    s_sstv_w = w;
    s_sstv_h = h;
    __atomic_store_n(&s_sstv_pix, pix, __ATOMIC_RELEASE);
}

const uint16_t *fm_sstv_image(int *w, int *h)
{
    const uint16_t *p = __atomic_load_n(&s_sstv_pix, __ATOMIC_ACQUIRE);
    *w = s_sstv_w;
    *h = s_sstv_h;
    return p;
}

bool fm_ct_trust(fm_ct_trust_t *t, int64_t ct_unix, int64_t uptime_us)
{
    if (ct_unix == t->last_ct) return t->agree > 0;
    const int64_t gap = uptime_us - t->last_uptime_us;
    /* The next minute's group, about a minute of uptime later. */
    if (t->last_uptime_us && ct_unix == t->last_ct + 60 &&
        llabs(gap - 60000000LL) < 3000000LL) t->agree++;
    else t->agree = 0;
    t->last_ct = ct_unix;
    t->last_uptime_us = uptime_us;
    return t->agree > 0;
}
