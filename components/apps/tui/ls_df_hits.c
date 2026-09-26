#include "ls_df_hits.h"

#include <math.h>
#include <string.h>

/* The floor settles over a few readings; before that nothing is a hit. */
#define SETTLE 4

void ls_df_log_clear(ls_df_log_t *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
    for (int t = 0; t < LS_DF_TRACKS; t++) { g->open[t] = -1; g->noise[t] = NAN; }
}

void ls_df_log_reset_track(ls_df_log_t *g, int track)
{
    if (!g || track < 0 || track >= LS_DF_TRACKS) return;
    g->open[track] = -1; g->noise[track] = NAN; g->heard[track] = 0;
    g->buckets[track] = 0; g->bucket[track] = 0; g->bucket_us[track] = 0;
}

/* The middle of the buckets' lowest readings, plus the 2 dB the lowest
   of many noise readings sits under their average. */
static float floor_of(const ls_df_log_t *g, int t)
{
    const int n = g->buckets[t];
    if (!n) return NAN;
    float v[LS_DF_NOISE_BUCKETS];
    for (int i = 0; i < n; i++) v[i] = g->quiet[t][i];
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { const float x = v[j]; v[j] = v[j - 1]; v[j - 1] = x; }
    const float mid = n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    return mid + 2.0f;
}

static void note_quiet(ls_df_log_t *g, int t, float level, int64_t now)
{
    if (!g->buckets[t] || now - g->bucket_us[t] >= LS_DF_NOISE_BUCKET_US) {
        if (g->buckets[t]) g->bucket[t] = (uint8_t)((g->bucket[t] + 1) % LS_DF_NOISE_BUCKETS);
        if (g->buckets[t] < LS_DF_NOISE_BUCKETS) g->buckets[t]++;
        g->quiet[t][g->bucket[t]] = level;
        g->bucket_us[t] = now;
    } else if (level < g->quiet[t][g->bucket[t]]) g->quiet[t][g->bucket[t]] = level;
}

float ls_df_log_noise(const ls_df_log_t *g, int track)
{
    return g && track >= 0 && track < LS_DF_TRACKS ? g->noise[track] : NAN;
}

int ls_df_log_count(const ls_df_log_t *g) { return g ? g->n : 0; }

const ls_df_hit_t *ls_df_log_at(const ls_df_log_t *g, int i)
{
    if (!g || i < 0 || i >= g->n) return NULL;
    return &g->hit[(g->head - 1 - i + 2 * LS_DF_HIT_MAX) % LS_DF_HIT_MAX];
}

bool ls_df_hit_open(const ls_df_hit_t *h, const ls_df_hit_cfg_t *cfg, int64_t now_us)
{
    return h && cfg && now_us - h->last_us <= (int64_t)(cfg->gap_s * 1e6f);
}

const ls_df_hit_t *ls_df_log_feed(ls_df_log_t *g, const ls_df_hit_cfg_t *cfg, int track,
                                  uint32_t freq_hz, float level, float heading,
                                  uint8_t flags, int64_t now_us)
{
    if (!g || !cfg || track < 0 || track >= LS_DF_TRACKS || !isfinite(level)) return NULL;
    const bool settled = g->heard[track] >= SETTLE;
    if (g->heard[track] < UINT16_MAX) g->heard[track]++;
    note_quiet(g, track, level, now_us);
    g->noise[track] = floor_of(g, track);
    const float over = level - g->noise[track];
    if (!settled || !isfinite(over) || over < cfg->threshold_db) return NULL;

    ls_df_hit_t *h = NULL;
    const int o = g->open[track];
    if (o >= 0 && ls_df_hit_open(&g->hit[o], cfg, now_us) && g->hit[o].track == track &&
        g->hit[o].freq_hz == freq_hz) h = &g->hit[o];
    if (!h) {
        const int at = g->head;
        g->head = (g->head + 1) % LS_DF_HIT_MAX;
        if (g->n < LS_DF_HIT_MAX) g->n++;
        /* The slot may have been another track's open hit. */
        for (int t = 0; t < LS_DF_TRACKS; t++) if (g->open[t] == at) g->open[t] = -1;
        h = &g->hit[at];
        memset(h, 0, sizeof(*h));
        h->start_us = now_us; h->track = (uint8_t)track; h->freq_hz = freq_hz;
        h->peak = -INFINITY; h->heading = NAN;
        g->open[track] = at;
        g->total++;
    }
    h->last_us = now_us;
    if (h->count < UINT16_MAX) h->count++;
    h->flags |= flags;
    if (level > h->peak) {
        h->peak = level; h->peak_us = now_us; h->snr = over;
        h->heading = heading;
    }
    return h;
}
