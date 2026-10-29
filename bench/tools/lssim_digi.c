/* Simulator fixture for DIGI: a 20 m FT8 band a moment after a slot decode,
   or mid-slot with LSSIM_DIGI=mid, or stopped with LSSIM_DIGI=idle. The
   screen and the engine's view are the production ones. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "digi_engine.h"
#include "esp_timer.h"

typedef struct { const char *text, *call, *to, *grid; int snr; float dt, freq; bool cq; } fx_t;

static const fx_t OLD[] = {
    {"CQ EA8XYZ IL18", "EA8XYZ", "CQ", "IL18", -14, 0.2f, 1488, true},
    {"K1ABC JA1XYZ R-17", "JA1XYZ", "K1ABC", "", -19, 0.4f, 2210, false},
    {"W9XYZ DL2ABC JO62", "DL2ABC", "W9XYZ", "JO62", -9, 0.1f, 790, false},
    {"CQ PY2XYZ GG66", "PY2XYZ", "CQ", "GG66", -16, 0.3f, 1925, true},
};
static const fx_t NEW[] = {
    {"CQ K1ABC FN42", "K1ABC", "CQ", "FN42", -4, 0.1f, 620, true},
    {"JA1XYZ K1ABC RR73", "K1ABC", "JA1XYZ", "", -11, 0.2f, 2212, false},
    {"DL2ABC W9XYZ -09", "W9XYZ", "DL2ABC", "", -7, 0.0f, 1210, false},
    {"CQ DX VK2ABC QF56", "VK2ABC", "CQ", "QF56", -20, 0.5f, 2610, true},
    {"G4ABC EA4XYZ IN80", "EA4XYZ", "G4ABC", "IN80", -13, 0.3f, 1540, false},
    {"CQ ZS6ABC KG33", "ZS6ABC", "CQ", "KG33", -17, 0.1f, 345, true},
};

static void put(digi_view_t *v, const fx_t *f, uint32_t slot, uint32_t seq, uint32_t at)
{
    digi_row_t *r = &v->row[v->n++];
    memset(r, 0, sizeof(*r));
    r->seq = seq; r->at_ms = at; r->slot_utc = slot; r->mode = DIGI_MODE_FT8;
    r->snr = (int8_t)f->snr; r->dt = f->dt; r->freq = f->freq; r->cq = f->cq;
    strcpy(r->text, f->text); strcpy(r->call, f->call); strcpy(r->to, f->to); strcpy(r->grid, f->grid);
    double la, lo;
    if (f->grid[0] && digi_grid_to_ll(f->grid, &la, &lo)) {
        /* From EM48 (St. Louis). */
        digi_bearing_range(38.6, -90.2, la, lo, &r->bearing, &r->km);
        r->has_pos = true;
    }
}

void lssim_digi_seed(void)
{
    static digi_view_t v;
    const char *phase = getenv("LSSIM_DIGI");
    const bool mid = phase && !strcmp(phase, "mid"), stopped = phase && !strcmp(phase, "idle");
    memset(&v, 0, sizeof(v));
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const int64_t slot0 = 1793197905LL;          /* 2026-10-28 14:31:45Z */
    v.running = !stopped;
    v.mode = DIGI_MODE_FT8;
    v.source = stopped ? DIGI_SRC_NONE : DIGI_SRC_RADIO;
    strcpy(v.tuner, "HF USB");
    v.band = 5;
    v.dial_hz = 14074000;
    v.tsrc = DIGI_TIME_GPS;
    v.time_ok = true;
    v.slot_ms = 15000;
    v.slot_pos_ms = mid ? 9300 : 900;
    v.utc_ms = (slot0 + 15) * 1000 + v.slot_pos_ms;
    v.capturing = v.running;
    v.level_db = -31;
    v.audio_age_ms = 20;
    v.clock_off_s = 0.18f;
    v.clock_off_n = 14;
    v.trim_s = 0.0f;
    v.have_pos = true;
    strcpy(v.my_grid, "EM48vp");
    v.logging = true;
    v.logged = 37;
    v.total = 37;
    v.slots = 9;
    static const uint8_t HIST[DIGI_SLOT_HIST] = {0, 0, 2, 4, 3, 5, 7, 4, 6, 5, 4, 6};
    memcpy(v.slot_hist, HIST, sizeof(HIST));
    v.last_decodes = 6;
    v.last_cost_ms = 840;
    /* The band: a noise floor with each heard signal standing out at its tone span. */
    srand(7);
    for (int c = 0; c < DIGI_SPEC_CELLS; ++c) v.spec[c] = (uint8_t)(18 + rand() % 22);
    for (unsigned i = 0; i < sizeof(NEW) / sizeof(NEW[0]); ++i) {
        int c = (int)(NEW[i].freq * DIGI_SPEC_CELLS / 3000.0f);
        int lvl = 120 + (NEW[i].snr + 20) * 6;
        for (int k = 0; k < 2 && c + k < DIGI_SPEC_CELLS; ++k) v.spec[c + k] = (uint8_t)(lvl - 25 * k);
    }
    for (int c = 0; c < DIGI_SPEC_CELLS; ++c) v.spec_peak[c] = (uint8_t)(v.spec[c] + (v.spec[c] > 80 ? 30 : 6));
    if (stopped) { v.n = 0; digi_host_set(&v); return; }
    uint32_t seq = 30;
    /* Newest first: this slot's decodes on top, landed under a second ago. */
    for (unsigned i = 0; i < sizeof(NEW) / sizeof(NEW[0]); ++i)
        put(&v, &NEW[i], (uint32_t)slot0, seq--, mid ? now - 8000 : now - 300 - 150 * i);
    for (unsigned i = 0; i < sizeof(OLD) / sizeof(OLD[0]); ++i)
        put(&v, &OLD[i], (uint32_t)(slot0 - 15), seq--, now - 16000);
    digi_host_set(&v);
}
