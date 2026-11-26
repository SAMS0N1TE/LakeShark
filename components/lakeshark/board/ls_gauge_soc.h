/* State of charge from the pack voltage, for a gauge that cannot learn.

   The T-Display-P4's BQ27220 runs on its factory defaults (3000 mAh design
   capacity), not a profile for the cell in the bay, and it restarts
   whenever the cell is disconnected. Its own estimate read 2 % on a cell
   at 4124 mV. The voltage, corrected for the drop across the cell and its
   protection, is the better number, once it is filtered: one loaded sample
   is not a state of charge, and a plugged-in board says nothing about the
   cell from its voltage alone.

   Pure arithmetic, no clock and no bus, so the bench tests all of it. */

#ifndef LS_GAUGE_SOC_H
#define LS_GAUGE_SOC_H

#include <stdbool.h>
#include <stdint.h>

/* Cell and protection in series while discharging. Estimated, not
   measured: a wrong value shows as the raw percentage moving with the load,
   which the filter below then has to absorb. */
#define LS_GAUGE_PACK_MOHM 100
/* Charging adds the cell's polarisation to that drop: a cell taking 600 mA
   reads well over its resting voltage, so a charging board read as full
   long before it was. */
#define LS_GAUGE_CHARGE_MOHM 200

/* Rest voltage from a loaded one. Current is positive into the cell. */
static inline int ls_gauge_rest_mv(int millivolts, int milliamps)
{
    return millivolts - milliamps * LS_GAUGE_PACK_MOHM / 1000;
}

/* Rest voltage to 0..100 % for an NMC 18650/21700 cell. 0 % is where the
   board stops, about 3.4 V at rest, not where the cell is empty: the last
   few percent of an NMC cell are below what the regulator can use, and a
   gauge that says 20 % when the board browns out is the worst kind of wrong.
   The mid points follow published NMC open-circuit curves, set 10-20 mV
   high so an aged or cold cell does not read better than it is. */
static inline int ls_gauge_soc_from_rest_mv(int mv)
{
    static const int16_t v[] = {3400, 3480, 3550, 3600, 3650, 3700, 3760, 3830, 3910, 4000, 4090, 4170};
    static const uint8_t p[] = {   0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100};
    const int n = (int)(sizeof(v) / sizeof(v[0]));
    if (mv <= v[0]) return 0;
    if (mv >= v[n - 1]) return 100;
    int i = 1;
    while (mv > v[i]) i++;
    return p[i - 1] + (p[i] - p[i - 1]) * (mv - v[i - 1]) / (v[i] - v[i - 1]);
}

/* ---- what the cell is doing ---------------------------------------------

   The board always draws well over 100 mA from its cell, so a current near
   zero means something else is powering it: USB, with the charger either
   finished or not charging. */
typedef enum {
    LS_BATT_UNKNOWN = 0,
    LS_BATT_DISCHARGING,    /* on the cell                                  */
    LS_BATT_CHARGING,       /* current into the cell                        */
    LS_BATT_PLUGGED,        /* external power, cell idle, not full          */
    LS_BATT_FULL,           /* external power, charger done                 */
} ls_batt_state_t;

#define LS_BATT_IDLE_MA     25     /* |current| under this: the cell is idle */
/* With USB in and the cell idle, a charger like the board's LGS4056H only
   stops at 4.2 V and starts again under about 4.05 V: an idle cell above
   this is one it has finished. */
#define LS_BATT_FULL_MV     4050
#define LS_BATT_SETTLE      2      /* samples a new state must hold        */
#define LS_BATT_WARMUP_S    60     /* free to move either way at first     */
#define LS_BATT_TAU_S       120    /* smoothing time constant              */
#define LS_BATT_DROP_S      20     /* at most one point down per this      */
#define LS_BATT_RISE_S      30     /* at most one point up per this, charging */

static inline ls_batt_state_t ls_batt_classify(int mv, int ma)
{
    if (ma >= LS_BATT_IDLE_MA) return LS_BATT_CHARGING;
    if (ma <= -LS_BATT_IDLE_MA) return LS_BATT_DISCHARGING;
    return mv >= LS_BATT_FULL_MV ? LS_BATT_FULL : LS_BATT_PLUGGED;
}

/* The instant estimate for one sample, before any filtering. */
static inline int ls_batt_instant(int mv, int ma)
{
    const int mohm = ma > 0 ? LS_GAUGE_CHARGE_MOHM : LS_GAUGE_PACK_MOHM;
    return ls_gauge_soc_from_rest_mv(mv - ma * mohm / 1000);
}

typedef struct {
    float   soc;          /* filtered percent; < 0 before the first sample */
    float   age_s;        /* seconds of samples since the first             */
    uint8_t state;        /* ls_batt_state_t, settled                        */
    uint8_t pending;      /* the state the last samples asked for            */
    uint8_t pending_n;    /* how many in a row                               */
} ls_batt_filter_t;

static inline void ls_batt_filter_init(ls_batt_filter_t *f)
{
    f->soc = -1.0f;
    f->age_s = 0.0f;
    f->state = LS_BATT_UNKNOWN;
    f->pending = LS_BATT_UNKNOWN;
    f->pending_n = 0;
}

static inline int ls_batt_filter_percent(const ls_batt_filter_t *f)
{
    if (f->soc < 0.0f) return 0;
    if (f->state == LS_BATT_FULL) return 100;
    int p = (int)(f->soc + 0.5f);
    /* 100 means the charger finished; a cell on its own says 99 at most
       until it has been seen full on USB. */
    if (p > 99 && f->state != LS_BATT_DISCHARGING) p = 99;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

/* One sample, dt_s seconds after the last. Returns the percent to show.

   Discharging it only goes down, never faster than a point every 20 s
   (a 2500 mAh cell at 1.5 A), and follows the voltage with a two-minute
   time constant, so a radio burst or a waterfall's load moves it by a
   fraction of a point. Charging it only goes up, as fast as a charger can
   fill a cell, and stops at 99 until the charger says it is done. A
   finished charger on USB is FULL, 100. The first minute moves freely: the
   first reading may have been taken under a boot's load. */
static inline int ls_batt_filter_step(ls_batt_filter_t *f, int mv, int ma, float dt_s)
{
    if (dt_s < 0.0f) dt_s = 0.0f;
    if (dt_s > 600.0f) dt_s = 600.0f;
    const ls_batt_state_t now = ls_batt_classify(mv, ma);
    const float target = (float)ls_batt_instant(mv, ma);

    if (f->soc < 0.0f) {
        f->soc = target;
        f->state = f->pending = (uint8_t)now;
        f->pending_n = LS_BATT_SETTLE;
        f->age_s = 0.0f;
    } else {
        f->age_s += dt_s;
        if (now == f->pending) {
            if (f->pending_n < LS_BATT_SETTLE) f->pending_n++;
        } else {
            f->pending = (uint8_t)now;
            f->pending_n = 1;
        }
        if (f->pending_n >= LS_BATT_SETTLE) f->state = f->pending;
    }

    const float k = dt_s / ((float)LS_BATT_TAU_S + dt_s);
    float next = f->soc + (target - f->soc) * k;
    const bool warm = f->age_s < (float)LS_BATT_WARMUP_S;

    switch ((ls_batt_state_t)f->state) {
    case LS_BATT_DISCHARGING:
    default:
        if (!warm && next > f->soc) next = f->soc;
        if (f->soc - next > dt_s / (float)LS_BATT_DROP_S) next = f->soc - dt_s / (float)LS_BATT_DROP_S;
        break;
    case LS_BATT_CHARGING:
        if (!warm && next < f->soc) next = f->soc;
        if (next - f->soc > dt_s / (float)LS_BATT_RISE_S) next = f->soc + dt_s / (float)LS_BATT_RISE_S;
        if (next > 99.0f) next = 99.0f;
        break;
    case LS_BATT_FULL:
        next = 100.0f;
        break;
    case LS_BATT_PLUGGED:
        /* Idle and resting: the voltage is the cell's own, either way. */
        break;
    }
    if (next < 0.0f) next = 0.0f;
    if (next > 100.0f) next = 100.0f;
    f->soc = next;
    return ls_batt_filter_percent(f);
}

#endif /* LS_GAUGE_SOC_H */
