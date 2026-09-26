/* Direction finding without radios: made-up transmitters whose level
   depends on which way the board faces, so FIND can be looked at. Each
   channel has its own transmitter; with LSSIM_TURN the board turns and the
   levels follow. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "ls_df_sources.h"

extern float lssim_turn_deg(void);

static struct {
    bool active;
    ls_dfs_t src;
    uint32_t freq[LS_DFS_CHANNELS];
    int nch;
    uint32_t updates;
} s_slot[LS_DFS_SLOTS] = { { .src = LS_DFS_MESH, .freq = { 915000000 }, .nch = 1 },
                           { .src = LS_DFS_COUNT, .freq = { 433920000 }, .nch = 1 } };
static int64_t s_last_us;

const char *ls_dfs_name(ls_dfs_t s)
{
    static const char *const N[LS_DFS_COUNT] = { "MESH", "LORA", "RTL-SDR", "HACKRF", "CC1101",
                                                 "NRF24", "WI-FI", "BLUETOOTH", "NFC", "GPS" };
    return s >= 0 && s < LS_DFS_COUNT ? N[s] : "?";
}
const char *ls_dfs_unavailable(ls_dfs_t s)
{
    if (s == LS_DFS_NFC) return "NFC reaches a few centimetres: hold the board to the tag";
    if (s == LS_DFS_GPS) return "Satellites are overhead: GPS places each bearing instead";
    if (s == LS_DFS_HACKRF) return "Plug a HackRF into the USB-A port";
    return NULL;
}
static bool tunable(ls_dfs_t s) { return s == LS_DFS_LORA || s == LS_DFS_RTL || s == LS_DFS_CC1101 || s == LS_DFS_NRF24; }
const char *ls_dfs_conflict(int slot, ls_dfs_t s)
{
    const int o = 1 - slot;
    if (s_slot[o].active && (s_slot[o].src == s || (s <= LS_DFS_LORA && s_slot[o].src <= LS_DFS_LORA)))
        return "shares hardware with the other slot";
    return NULL;
}
bool ls_dfs_in_range(ls_dfs_t s, uint32_t hz) { (void)s; return hz >= 24000000u && hz <= 2500000000u; }
bool ls_dfs_select_slot(int k, ls_dfs_t s, uint32_t f)
{
    if (k < 0 || k >= LS_DFS_SLOTS) return false;
    s_slot[k].src = s; s_slot[k].active = true;
    if (f) s_slot[k].freq[0] = f;
    if (!s_slot[k].nch) s_slot[k].nch = 1;
    return true;
}
bool ls_dfs_select(ls_dfs_t s, uint32_t f) { return ls_dfs_select_slot(0, s, f); }
int ls_dfs_set_channels(int k, const uint32_t *hz, int n, uint32_t dwell)
{
    (void)dwell;
    if (k < 0 || k >= LS_DFS_SLOTS || n < 1) return 0;
    if (n > LS_DFS_CHANNELS) n = LS_DFS_CHANNELS;
    memcpy(s_slot[k].freq, hz, sizeof(uint32_t) * n); s_slot[k].nch = n;
    return n;
}
bool ls_dfs_tune(uint32_t f) { return ls_dfs_set_channels(0, &f, 1, 400) == 1; }
void ls_dfs_stop_slot(int k) { if (k >= 0 && k < LS_DFS_SLOTS) s_slot[k].active = false; }
void ls_dfs_stop(void) { ls_dfs_stop_slot(0); }
void ls_dfs_poll_slot(int k, ls_dfs_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (k < 0 || k >= LS_DFS_SLOTS) { out->source = LS_DFS_COUNT; return; }
    out->source = s_slot[k].src; out->active = s_slot[k].active; out->fresh = s_slot[k].active;
    out->level = -71.0f; out->unit = "dBm"; out->freq_hz = s_slot[k].freq[0]; out->updates = ++s_slot[k].updates;
    out->tunable = tunable(s_slot[k].src);
    out->targets = s_slot[k].src == LS_DFS_MESH || s_slot[k].src == LS_DFS_WIFI || s_slot[k].src == LS_DFS_BLE;
    out->channels = out->tunable ? s_slot[k].nch : 1;
    memcpy(out->freqs, s_slot[k].freq, sizeof(out->freqs));
    snprintf(out->target, sizeof(out->target), "everything heard");
    snprintf(out->status, sizeof(out->status), "Simulated signal");
}
void ls_dfs_poll(ls_dfs_status_t *out) { ls_dfs_poll_slot(0, out); }

/* Where each channel's transmitter is, relative to the turn. */
static float level_for(int k, int ch, float facing)
{
    static const float TRUTH[2][LS_DFS_CHANNELS] = { { 40, 160, 250, 300, 100, 200, 20, 330 },
                                                     { 210, 80, 130, 280, 10, 190, 60, 240 } };
    const float off = fabsf(fmodf(facing - TRUTH[k][ch] + 540.0f, 360.0f) - 180.0f) * 0.0174533f;
    /* A main lobe, a weaker back lobe, and a little fading. */
    const float fade = 1.5f * sinf(facing * 0.21f + ch);
    return -96.0f + 18.0f * powf(cosf(off * 0.5f), 4.0f) + 5.0f * powf(sinf(off * 0.5f), 8.0f) + fade - ch * 2.0f;
}

int ls_dfs_take(ls_dfs_reading_t *out, int max)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_last_us < 100000) return 0;
    s_last_us = now;
    const float facing = lssim_turn_deg() + 72.0f;
    int n = 0;
    for (int k = 0; k < LS_DFS_SLOTS; k++) {
        if (!s_slot[k].active) continue;
        const int nch = tunable(s_slot[k].src) ? s_slot[k].nch : 1;
        for (int ch = 0; ch < nch && n < max; ch++)
            out[n++] = (ls_dfs_reading_t){ .us = now, .freq_hz = s_slot[k].freq[ch],
                                           .level = level_for(k, ch, facing), .slot = (uint8_t)k, .channel = (uint8_t)ch };
    }
    return n;
}
int ls_dfs_target_count(void) { return 2; }
bool ls_dfs_target_label(int i, char *l, size_t lc, char *d, size_t dc)
{
    snprintf(l, lc, i ? "Ridge repeater" : "Base station"); snprintf(d, dc, "%d dBm", i ? -88 : -71); return i < 2;
}
bool ls_dfs_target_pick(int i) { (void)i; return true; }
void ls_dfs_step(void) {}
