#ifndef LS_LABS_LIMITS_H
#define LS_LABS_LIMITS_H
#include <math.h>
#include "ls_lora.h"
#include "ls_mesh.h"

/* LABS uses a conservative US915 packet budget: +14 dBm conducted power,
   400 ms per packet and 600 ms in any 60 seconds. Charges survive DIRECT
   release, and uncertain sends retain their full planned airtime. */
typedef struct {
    struct { int64_t until_us; uint32_t airtime_ms; } charge[16];
} ls_labs_tx_budget_t;
static inline const char *ls_labs_tx_check(const ls_lora_cfg_t *cfg,
    uint32_t airtime_ms, int64_t now, const ls_labs_tx_budget_t *budget)
{
    const uint32_t half_bw = (cfg->bw_hz + 1) / 2;
    if (cfg->freq_hz < LS_MESH_US915_MIN_HZ + half_bw ||
        cfg->freq_hz > LS_MESH_US915_MAX_HZ - half_bw)
        return "SEND refused: use the US915 band";
    if (cfg->power_dbm > 14 || cfg->power_dbm < -9)
        return "SEND refused: power max +14 dBm";
    if (!airtime_ms || airtime_ms > 400)
        return "SEND refused: max 400 ms airtime";
    unsigned used = 0, free_slots = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (budget->charge[i].until_us > now) used += budget->charge[i].airtime_ms;
        else free_slots++;
    }
    if (!free_slots || used + airtime_ms > 600)
        return "SEND refused: duty limit; wait 60s";
    return NULL;
}
static inline void ls_labs_tx_charge(ls_labs_tx_budget_t *budget,
    uint32_t airtime_ms, int64_t now)
{
    for (unsigned i = 0; i < 16; i++) if (budget->charge[i].until_us <= now) {
        /* Retain the charge for a full window after the packet ends. */
        budget->charge[i].until_us = now + (int64_t)airtime_ms * 1000 + 60000000;
        budget->charge[i].airtime_ms = airtime_ms;
        return;
    }
}
static inline bool ls_labs_fsk_number(double n, unsigned bytes)
{
    return isfinite(n) && n >= 0 && n <= (bytes == 1 ? UINT8_MAX : UINT32_MAX) && floor(n) == n;
}
#endif
