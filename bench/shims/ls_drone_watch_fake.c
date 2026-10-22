/* Drone Watch for the simulator and UI tests: no radio. While the watch is
   on it reports a fixed set of sample links so the DRONES pages can be drawn
   and checked; off, it reports nothing. */
#include "ls_drone_watch.h"
#include <string.h>
#include <stdio.h>

static bool s_on, s_alerts = true;
static uint32_t s_sources = 63;

unsigned ls_drone_watch_fake_entries(ls_drone_watch_entry_t *out, unsigned cap)
{
    static const struct { ls_dw_source_t src; double mhz; float dbm, slope; ls_dw_confidence_t c; const char *ch; } S[] = {
        { LS_DW_ANALOG58, 5806, -58, 4.5f, LS_DW_LIKELY, "R4 5806" },
        { LS_DW_ELRS24, 2441, -71, 0.4f, LS_DW_LIKELY, "" },
        { LS_DW_HOPPER900, 915, -83, -3.2f, LS_DW_POSSIBLE, "" },
        { LS_DW_RID, 0, -66, 0.0f, LS_DW_CONFIRMED, "" },
    };
    unsigned n = 0;
    for (unsigned i = 0; s_on && i < sizeof(S) / sizeof(S[0]) && n < cap; i++, n++) {
        ls_drone_watch_entry_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        e->id = 100 + i; e->source = S[i].src; e->centre_hz = (uint64_t)(S[i].mhz * 1e6);
        e->band_mhz = (unsigned)S[i].mhz; e->rssi_dbm = S[i].dbm; e->peak_dbm = S[i].dbm + 3;
        e->db_per_10s = S[i].slope; e->calibrated = S[i].src != LS_DW_ANALOG58;
        e->trend = S[i].slope > 3 ? LS_DW_APPROACHING : S[i].slope < -3 ? LS_DW_RECEDING : LS_DW_STEADY;
        e->confidence = S[i].c; e->hits_per_second = 4;
        snprintf(e->channel, sizeof(e->channel), "%s", S[i].ch);
        if (S[i].src == LS_DW_RID) snprintf(e->identity, sizeof(e->identity), "1581F5FJD239C00DL0LE");
        e->history_count = LS_DW_HISTORY;
        for (unsigned k = 0; k < LS_DW_HISTORY; k++) {
            e->history[k].us = (int64_t)k * 500000;
            e->history[k].dbm = S[i].dbm - S[i].slope * (float)(LS_DW_HISTORY - 1 - k) / 4.0f + (float)((k * 7 + i * 3) % 5) - 2.0f;
        }
        e->first_us = 0; e->last_us = 0;
    }
    return n;
}

size_t ls_drone_watch_snapshot(ls_drone_watch_entry_t *out, size_t cap)
{
    return ls_drone_watch_fake_entries(out, (unsigned)cap);
}
bool ls_drone_watch_status(ls_drone_watch_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = s_on; out->alerts = s_alerts; out->sources = s_sources; out->cycle_ms = 9800;
    for (int i = 0; i < LS_DW_SOURCES; i++) {
        out->running[i] = s_on && ((s_sources >> i) & 1) && i != LS_DW_ANALOG58;
        snprintf(out->reason[i], sizeof(out->reason[i]), "%s",
                 !s_on ? "watch off" : i == LS_DW_ANALOG58 ? "needs a HackRF" : "scanning");
    }
    return true;
}
void ls_drone_watch_cursor(ls_dw_cursor_t out[LS_DW_SOURCES])
{
    memset(out, 0, sizeof(ls_dw_cursor_t) * LS_DW_SOURCES);
    if (!s_on) return;
    out[LS_DW_ELRS24] = (ls_dw_cursor_t){ 2425, 2426, 1 };
    out[LS_DW_HOPPER900] = (ls_dw_cursor_t){ 902, 928, 1 };
}
unsigned ls_drone_watch_count(void) { return s_on ? 4 : 0; }
bool ls_drone_watch_enable(bool on) { s_on = on; return true; }
bool ls_drone_watch_alerts(bool on) { s_alerts = on; return true; }
bool ls_drone_watch_source_enable(ls_dw_source_t s, bool on)
{
    if ((unsigned)s >= LS_DW_SOURCES) return false;
    s_sources = on ? (s_sources | (1u << s)) : (s_sources & ~(1u << s));
    return true;
}
void ls_drone_watch_foreground(bool on) { (void)on; }
