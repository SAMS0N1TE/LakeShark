#include "ls_rfs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"

/* ------------------------------------------------------------- plans -- */

typedef struct { const char *name; uint32_t lo, hi; int cells; const uint32_t *hz; int n; uint32_t bw; } preset_t;

/* Common US paging channels (VHF), the GMRS/FRS interstitials, and the two
   LoRaWAN channel plans' first eight uplinks: frequencies from the FCC
   Part 22 / Part 95 channel tables and the LoRa Alliance regional
   parameters. */
static const uint32_t PAGERS[] = { 152007500, 152240000, 152480000, 152840000, 157740000, 158100000, 158700000, 929612500 };
static const uint32_t FRS[] = { 462562500, 462587500, 462612500, 462637500, 462662500, 462687500, 462712500, 462550000 };
static const uint32_t LORA_US[] = { 903900000, 904100000, 904300000, 904500000, 904700000, 904900000, 905100000, 905300000 };
static const uint32_t LORA_EU[] = { 868100000, 868300000, 868500000, 867100000, 867300000, 867500000, 867700000, 867900000 };

static const preset_t PRESETS[] = {
    { "PAGERS",       0, 0, 0, PAGERS, 8, 12500 },
    { "PUBLIC VHF",   150800000, 174000000, 16, NULL, 0, 0 },
    { "PUBLIC UHF",   450000000, 470000000, 16, NULL, 0, 0 },
    { "FRS / GMRS",   0, 0, 0, FRS, 8, 12500 },
    { "ISM 433",      433050000, 434790000, 8, NULL, 0, 0 },
    { "ISM 868",      863000000, 870000000, 8, NULL, 0, 0 },
    { "ISM 915",      902000000, 928000000, 16, NULL, 0, 0 },
    { "LORA US915",   0, 0, 0, LORA_US, 8, 125000 },
    { "LORA EU868",   0, 0, 0, LORA_EU, 8, 125000 },
    { "CELL 700 DL",  729000000, 746000000, 8, NULL, 0, 0 },
    { "CELL 850 DL",  869000000, 894000000, 8, NULL, 0, 0 },
};
#define N_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

int ls_rfs_preset_count(void) { return N_PRESETS; }

static void mhz_label(char *out, uint32_t hz)
{
    /* Eight characters: 152.0075 shows as 152.007, 2440 MHz as 2440.00. */
    char t[24];
    snprintf(t, sizeof(t), "%.4f", hz / 1e6);
    snprintf(out, LS_RFS_LABEL + 1, "%.*s", LS_RFS_LABEL, t);
    size_t n = strlen(out);
    if (n && out[n - 1] == '.') out[n - 1] = 0;
}

bool ls_rfs_plan_range(ls_rfs_plan_t *out, const char *name, uint32_t lo, uint32_t hi, int cells)
{
    if (!out || cells < 1 || cells > LS_RFS_CH_MAX || hi <= lo || hi - lo < (uint32_t)cells * 1000u) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "RANGE");
    const double w = (double)(hi - lo) / cells;
    for (int i = 0; i < cells; i++) {
        out->ch[i].hz = (uint32_t)(lo + w * (i + 0.5) + 0.5);
        out->ch[i].bw_hz = (uint32_t)(w + 0.5);
        mhz_label(out->ch[i].label, out->ch[i].hz);
    }
    out->n = cells;
    out->range = true;
    return true;
}

bool ls_rfs_preset(int i, ls_rfs_plan_t *out)
{
    if (!out || i < 0 || i >= N_PRESETS) return false;
    const preset_t *p = &PRESETS[i];
    if (!p->hz) return ls_rfs_plan_range(out, p->name, p->lo, p->hi, p->cells);
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", p->name);
    for (int k = 0; k < p->n; k++) {
        out->ch[k].hz = p->hz[k]; out->ch[k].bw_hz = p->bw;
        mhz_label(out->ch[k].label, p->hz[k]);
    }
    out->n = p->n;
    return true;
}

/* ---- the plan file: a strict reader for this one schema --------------

   Objects, arrays, strings without escapes beyond \" and \\, and whole
   numbers. Anything else is a reason to refuse the file. */

typedef struct { const char *p, *end; char *why; size_t cap; bool bad; } rd_t;

static void fail(rd_t *r, const char *msg)
{
    if (!r->bad && r->why && r->cap) snprintf(r->why, r->cap, "%s", msg);
    r->bad = true;
}
static void ws(rd_t *r) { while (r->p < r->end && isspace((unsigned char)*r->p)) r->p++; }
static bool eat(rd_t *r, char c) { ws(r); if (r->p < r->end && *r->p == c) { r->p++; return true; } return false; }
static bool peek(rd_t *r, char c) { ws(r); return r->p < r->end && *r->p == c; }

static bool str(rd_t *r, char *out, size_t cap)
{
    if (!eat(r, '"')) { fail(r, "expected a string"); return false; }
    size_t n = 0;
    while (r->p < r->end && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\') { if (r->p >= r->end || (*r->p != '"' && *r->p != '\\')) { fail(r, "unsupported escape"); return false; } c = *r->p++; }
        if ((unsigned char)c < 32) { fail(r, LS_TEXT(t_ls_rfs_control_character_in_a, "control character in a string")); return false; }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    if (r->p >= r->end) { fail(r, "unterminated string"); return false; }
    r->p++;
    return true;
}

static bool num(rd_t *r, double *v)
{
    ws(r);
    const char *s = r->p;
    if (s < r->end && *s == '-') s++;
    while (s < r->end && isdigit((unsigned char)*s)) s++;
    if (s == r->p || (s - r->p == 1 && *r->p == '-')) { fail(r, "expected a number"); return false; }
    char t[24];
    const size_t n = (size_t)(s - r->p) < sizeof(t) - 1 ? (size_t)(s - r->p) : sizeof(t) - 1;
    memcpy(t, r->p, n); t[n] = 0;
    *v = strtod(t, NULL);
    r->p = s;
    return true;
}

/* Skips a value of a key this reader does not use. */
static void skip(rd_t *r)
{
    ws(r);
    if (peek(r, '"')) { char t[2]; str(r, t, sizeof(t)); return; }
    if (eat(r, '{')) {
        if (eat(r, '}')) return;
        do { char k[2]; str(r, k, sizeof(k)); if (!eat(r, ':')) { fail(r, "expected :"); return; } skip(r); } while (!r->bad && eat(r, ','));
        if (!eat(r, '}')) fail(r, "expected }");
        return;
    }
    if (eat(r, '[')) {
        if (eat(r, ']')) return;
        do skip(r); while (!r->bad && eat(r, ','));
        if (!eat(r, ']')) fail(r, "expected ]");
        return;
    }
    if (r->end - r->p >= 4 && (!strncmp(r->p, "true", 4) || !strncmp(r->p, "null", 4))) { r->p += 4; return; }
    if (r->end - r->p >= 5 && !strncmp(r->p, "false", 5)) { r->p += 5; return; }
    double d; num(r, &d);
}

static bool hz_ok(double v) { return v >= 1e6 && v <= 6e9 && v <= UINT32_MAX; }

static void channel(rd_t *r, ls_rfs_plan_t *pl)
{
    if (!eat(r, '{')) { fail(r, LS_TEXT(t_ls_rfs_a_channel_is_an, "a channel is an object")); return; }
    double hz = 0, bw = 12500; char label[LS_RFS_LABEL + 1] = "";
    if (!eat(r, '}')) {
        do {
            char k[16];
            if (!str(r, k, sizeof(k)) || !eat(r, ':')) { fail(r, "expected a key"); return; }
            if (!strcmp(k, "hz")) num(r, &hz);
            else if (!strcmp(k, "bw")) num(r, &bw);
            else if (!strcmp(k, "label")) str(r, label, sizeof(label));
            else skip(r);
        } while (!r->bad && eat(r, ','));
        if (!eat(r, '}')) fail(r, LS_TEXT(t_ls_rfs_expected_after_a_channel, "expected } after a channel"));
    }
    if (r->bad) return;
    if (!hz_ok(hz)) { fail(r, LS_TEXT(t_ls_rfs_channel_hz_out_of, "channel hz out of range (1 MHz..6 GHz)")); return; }
    if (bw < 1000 || bw > 20e6) { fail(r, LS_TEXT(t_ls_rfs_channel_bw_out_of, "channel bw out of range (1 kHz..20 MHz)")); return; }
    if (pl->n >= LS_RFS_CH_MAX) { fail(r, LS_TEXT(t_ls_rfs_more_than_32_channels, "more than 32 channels in a plan")); return; }
    ls_rfs_ch_t *c = &pl->ch[pl->n++];
    c->hz = (uint32_t)hz; c->bw_hz = (uint32_t)bw;
    if (label[0]) snprintf(c->label, sizeof(c->label), "%s", label); else mhz_label(c->label, c->hz);
}

static void plan(rd_t *r, ls_rfs_plan_t *pl)
{
    memset(pl, 0, sizeof(*pl));
    double lo = 0, hi = 0, cells = 0; bool range = false;
    if (!eat(r, '{')) { fail(r, "a plan is an object"); return; }
    if (!eat(r, '}')) {
        do {
            char k[16];
            if (!str(r, k, sizeof(k)) || !eat(r, ':')) { fail(r, "expected a key"); return; }
            if (!strcmp(k, "name")) str(r, pl->name, sizeof(pl->name));
            else if (!strcmp(k, "channels")) {
                if (!eat(r, '[')) { fail(r, LS_TEXT(t_ls_rfs_channels_is_an_array, "channels is an array")); return; }
                if (!eat(r, ']')) { do channel(r, pl); while (!r->bad && eat(r, ',')); if (!eat(r, ']')) fail(r, "expected ]"); }
            } else if (!strcmp(k, "range")) {
                range = true;
                if (!eat(r, '{')) { fail(r, "range is an object"); return; }
                do {
                    char q[8];
                    if (!str(r, q, sizeof(q)) || !eat(r, ':')) { fail(r, "expected a key"); return; }
                    if (!strcmp(q, "lo")) num(r, &lo); else if (!strcmp(q, "hi")) num(r, &hi);
                    else if (!strcmp(q, "cells")) num(r, &cells); else skip(r);
                } while (!r->bad && eat(r, ','));
                if (!eat(r, '}')) fail(r, LS_TEXT(t_ls_rfs_expected_after_range, "expected } after range"));
            } else skip(r);
        } while (!r->bad && eat(r, ','));
        if (!eat(r, '}')) fail(r, LS_TEXT(t_ls_rfs_expected_after_a_plan, "expected } after a plan"));
    }
    if (r->bad) return;
    if (!pl->name[0]) { fail(r, "a plan has no name"); return; }
    if (range) {
        char name[LS_RFS_NAME + 1]; snprintf(name, sizeof(name), "%s", pl->name);
        if (pl->n || !hz_ok(lo) || !hz_ok(hi) || cells < 1 || cells > LS_RFS_CH_MAX ||
            !ls_rfs_plan_range(pl, name, (uint32_t)lo, (uint32_t)hi, (int)cells))
            fail(r, LS_TEXT(t_ls_rfs_bad_range_lo_hi, "bad range: lo < hi, 1 MHz..6 GHz, 1..32 cells"));
    } else if (!pl->n) fail(r, LS_TEXT(t_ls_rfs_a_plan_needs_channels, "a plan needs channels or a range"));
}

int ls_rfs_plans_parse(const char *json, size_t len, ls_rfs_plan_t *out, int max, char *why, size_t cap)
{
    rd_t r = { json, json + len, why, cap, false };
    if (why && cap) why[0] = 0;
    char schema[48] = ""; double version = 0; int n = 0; bool plans = false;
    if (!json || !out || max < 1 || !eat(&r, '{')) { fail(&r, "not a JSON object"); return -1; }
    if (!eat(&r, '}')) {
        do {
            char k[16];
            if (!str(&r, k, sizeof(k)) || !eat(&r, ':')) { fail(&r, "expected a key"); break; }
            if (!strcmp(k, "schema")) str(&r, schema, sizeof(schema));
            else if (!strcmp(k, "version")) num(&r, &version);
            else if (!strcmp(k, "plans")) {
                plans = true;
                if (!eat(&r, '[')) { fail(&r, "plans is an array"); break; }
                if (!eat(&r, ']')) {
                    do {
                        if (n >= max) { fail(&r, "too many plans"); break; }
                        plan(&r, &out[n]);
                        if (!r.bad) n++;
                    } while (!r.bad && eat(&r, ','));
                    if (!r.bad && !eat(&r, ']')) fail(&r, "expected ]");
                }
            } else skip(&r);
        } while (!r.bad && eat(&r, ','));
        if (!r.bad && !eat(&r, '}')) fail(&r, "expected }");
    }
    if (!r.bad && strcmp(schema, "lakeshark.rfsurvey.plans")) fail(&r, LS_TEXT(t_ls_rfs_schema_is_not_lakeshark, "schema is not lakeshark.rfsurvey.plans"));
    if (!r.bad && version != 1) fail(&r, LS_TEXT(t_ls_rfs_only_version_1_is, "only version 1 is read"));
    if (!r.bad && !plans) fail(&r, "no plans");
    return r.bad ? -1 : n;
}

/* ------------------------------------------------------------- file -- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }
static void putname(uint8_t *p, const char *s, size_t n) { memset(p, 0, n); if (s) memcpy(p, s, strnlen(s, n)); }
static void getname(char *out, const uint8_t *p, size_t n) { memcpy(out, p, n); out[n] = 0; }

size_t ls_rfs_header_bytes(int nch) { return LS_RFS_HEADER + (size_t)LS_RFS_CH_BYTES * (size_t)nch; }
size_t ls_rfs_row_bytes(int nch) { return LS_RFS_ROW_FIXED + 4u * (size_t)nch; }

size_t ls_rfs_header_pack(const ls_rfs_header_t *h, uint8_t *out, size_t cap)
{
    if (!h || h->nch < 1 || h->nch > LS_RFS_CH_MAX) return 0;
    const size_t n = ls_rfs_header_bytes(h->nch);
    if (!out || cap < n) return 0;
    memset(out, 0, n);
    memcpy(out, "LSRFSURV", 8);
    put16(out + 8, LS_RFS_VERSION);
    put16(out + 10, (uint16_t)n);
    put16(out + 12, h->nch);
    put16(out + 14, (uint16_t)ls_rfs_row_bytes(h->nch));
    put32(out + 16, h->start_utc);
    putname(out + 20, h->source, 16);
    putname(out + 36, h->plan, 16);
    put16(out + 52, (uint16_t)h->threshold_cdb);
    put16(out + 54, h->cell_m);
    for (int i = 0; i < h->nch; i++) {
        uint8_t *c = out + LS_RFS_HEADER + LS_RFS_CH_BYTES * i;
        put32(c, h->ch[i].hz); put32(c + 4, h->ch[i].bw_hz);
        putname(c + 8, h->ch[i].label, LS_RFS_LABEL);
    }
    return n;
}

size_t ls_rfs_header_unpack(const uint8_t *in, size_t len, ls_rfs_header_t *h)
{
    if (!in || !h || len < LS_RFS_HEADER || memcmp(in, "LSRFSURV", 8)) return 0;
    memset(h, 0, sizeof(*h));
    h->version = get16(in + 8);
    const uint16_t bytes = get16(in + 10);
    h->nch = get16(in + 12);
    h->row_bytes = get16(in + 14);
    if (h->version != LS_RFS_VERSION || h->nch < 1 || h->nch > LS_RFS_CH_MAX ||
        bytes != ls_rfs_header_bytes(h->nch) || h->row_bytes != ls_rfs_row_bytes(h->nch) || len < bytes) return 0;
    h->start_utc = get32(in + 16);
    getname(h->source, in + 20, 16);
    getname(h->plan, in + 36, 16);
    h->threshold_cdb = (int16_t)get16(in + 52);
    h->cell_m = get16(in + 54);
    for (int i = 0; i < h->nch; i++) {
        const uint8_t *c = in + LS_RFS_HEADER + LS_RFS_CH_BYTES * i;
        h->ch[i].hz = get32(c); h->ch[i].bw_hz = get32(c + 4);
        getname(h->ch[i].label, c + 8, LS_RFS_LABEL);
    }
    return bytes;
}

static int32_t e7(double deg) { return (int32_t)lround(deg * 1e7); }

size_t ls_rfs_row_pack(const ls_rfs_header_t *h, const ls_rfs_row_t *r, uint8_t *out, size_t cap)
{
    const size_t n = h ? ls_rfs_row_bytes(h->nch) : 0;
    if (!n || !r || !out || cap < n) return 0;
    put32(out, r->t_ms); put32(out + 4, r->utc_s);
    put32(out + 8, (uint32_t)e7(r->lat)); put32(out + 12, (uint32_t)e7(r->lon));
    put16(out + 16, r->flags); put16(out + 18, r->pass); put32(out + 20, r->occ);
    for (int i = 0; i < h->nch; i++) {
        put16(out + 24 + 4 * i, (uint16_t)r->level[i]);
        put16(out + 26 + 4 * i, (uint16_t)r->floor[i]);
    }
    return n;
}

bool ls_rfs_row_unpack(const ls_rfs_header_t *h, const uint8_t *in, size_t len, ls_rfs_row_t *r)
{
    if (!h || !in || !r || len < ls_rfs_row_bytes(h->nch)) return false;
    memset(r, 0, sizeof(*r));
    r->t_ms = get32(in); r->utc_s = get32(in + 4);
    r->lat = (int32_t)get32(in + 8) / 1e7; r->lon = (int32_t)get32(in + 12) / 1e7;
    r->flags = get16(in + 16); r->pass = get16(in + 18); r->occ = get32(in + 20);
    for (int i = 0; i < LS_RFS_CH_MAX; i++) r->level[i] = r->floor[i] = LS_RFS_NO_LEVEL;
    for (int i = 0; i < h->nch; i++) {
        r->level[i] = (int16_t)get16(in + 24 + 4 * i);
        r->floor[i] = (int16_t)get16(in + 26 + 4 * i);
    }
    return fabs(r->lat) <= 90 && fabs(r->lon) <= 180;
}

/* ---------------------------------------------------- floor and occupancy -- */

#define FLOOR_RISE_DB 0.05f

void ls_rfs_floor_reset(ls_rfs_floor_t *f) { if (f) memset(f, 0, sizeof(*f)); }

float ls_rfs_floor_update(ls_rfs_floor_t *f, int ch, float level)
{
    if (!f || ch < 0 || ch >= LS_RFS_CH_MAX || !isfinite(level)) return NAN;
    if (!f->seeded[ch] || level < f->floor[ch]) { f->floor[ch] = level; f->seeded[ch] = true; }
    else f->floor[ch] += fminf(FLOOR_RISE_DB, level - f->floor[ch]);
    return f->floor[ch];
}

/* -------------------------------------------------------------- the grid -- */

#define M_PER_DEG_LAT 111132.0

const char *ls_rfs_metric_name(int m)
{
    static const char *const N[] = { "PEAK", "MEAN", "BUSY %", "OVER FLOOR" };
    return m >= 0 && m < LS_RFS_METRICS ? N[m] : "?";
}

void ls_rfs_grid_reset(ls_rfs_grid_t *g, float cell_m, int nch)
{
    if (!g) return;
    const uint32_t serial = g->serial;
    memset(g, 0, offsetof(ls_rfs_grid_t, cell));
    for (int i = 0; i < 2 * LS_RFS_CELLS; i++) g->slot[i] = -1;
    g->cell_m = cell_m >= 1 ? cell_m : 25;
    g->nch = nch < 1 ? 1 : nch > LS_RFS_CH_MAX ? LS_RFS_CH_MAX : nch;
    g->serial = serial + 1;
}

static void origin(ls_rfs_grid_t *g, double lat, double lon)
{
    if (g->origin_set) return;
    g->lat0 = lat; g->lon0 = lon;
    g->m_per_deg_lon = M_PER_DEG_LAT * cos(lat * M_PI / 180.0);
    if (g->m_per_deg_lon < 1) g->m_per_deg_lon = 1;
    g->origin_set = true;
}

void ls_rfs_grid_index(ls_rfs_grid_t *g, double lat, double lon, int *ix, int *iy)
{
    origin(g, lat, lon);
    double dlon = lon - g->lon0;
    if (dlon > 180) dlon -= 360;
    if (dlon < -180) dlon += 360;
    const double x = dlon * g->m_per_deg_lon, y = (lat - g->lat0) * M_PER_DEG_LAT;
    *ix = (int)floor(x / g->cell_m);
    *iy = (int)floor(y / g->cell_m);
}

static unsigned hash(int ix, int iy) { return ((unsigned)ix * 73856093u ^ (unsigned)iy * 19349663u) % (2 * LS_RFS_CELLS); }

int ls_rfs_grid_find(const ls_rfs_grid_t *g, int ix, int iy)
{
    if (!g) return -1;
    for (unsigned k = hash(ix, iy), probes = 0; probes < 2 * LS_RFS_CELLS; probes++, k = (k + 1) % (2 * LS_RFS_CELLS)) {
        const int s = g->slot[k];
        if (s < 0) return -1;
        if (g->cell[s].ix == ix && g->cell[s].iy == iy) return s;
    }
    return -1;
}

int ls_rfs_grid_add(ls_rfs_grid_t *g, const ls_rfs_row_t *r)
{
    if (!g || !r || !isfinite(r->lat) || !isfinite(r->lon)) return -1;
    int ix, iy;
    ls_rfs_grid_index(g, r->lat, r->lon, &ix, &iy);
    if (ix < INT16_MIN || ix > INT16_MAX || iy < INT16_MIN || iy > INT16_MAX) { g->dropped++; return -1; }
    int idx = ls_rfs_grid_find(g, ix, iy);
    if (idx < 0) {
        if (g->count >= LS_RFS_CELLS) { g->dropped++; return -1; }
        unsigned k = hash(ix, iy);
        while (g->slot[k] >= 0) k = (k + 1) % (2 * LS_RFS_CELLS);
        idx = g->count++;
        g->slot[k] = (int16_t)idx;
        ls_rfs_cell_t *c = &g->cell[idx];
        memset(c, 0, sizeof(*c));
        c->ix = (int16_t)ix; c->iy = (int16_t)iy;
        for (int i = 0; i < LS_RFS_CH_MAX; i++) c->max_cdb[i] = LS_RFS_NO_LEVEL;
    }
    ls_rfs_cell_t *c = &g->cell[idx];
    if (c->rows < UINT16_MAX) c->rows++;
    for (int i = 0; i < g->nch; i++) {
        const int16_t v = r->level[i];
        if (v == LS_RFS_NO_LEVEL || c->heard[i] == UINT16_MAX) continue;
        c->heard[i]++;
        c->sum_cdb[i] += v;
        if (v > c->max_cdb[i]) c->max_cdb[i] = v;
        if (r->floor[i] != LS_RFS_NO_LEVEL) c->over_cdb[i] += v - r->floor[i] > 0 ? v - r->floor[i] : 0;
        if (r->occ & (1u << i)) c->occ[i]++;
    }
    g->serial++;
    return idx;
}

void ls_rfs_grid_centre(const ls_rfs_grid_t *g, int idx, double *lat, double *lon)
{
    if (!g || idx < 0 || idx >= g->count) { *lat = *lon = NAN; return; }
    *lat = g->lat0 + (g->cell[idx].iy + 0.5) * g->cell_m / M_PER_DEG_LAT;
    *lon = g->lon0 + (g->cell[idx].ix + 0.5) * g->cell_m / g->m_per_deg_lon;
}

static bool one(const ls_rfs_cell_t *c, int i, ls_rfs_metric_t m, float *out)
{
    if (!c->heard[i]) return false;
    switch (m) {
    case LS_RFS_MAX:  *out = c->max_cdb[i] / 100.0f; break;
    case LS_RFS_MEAN: *out = (float)c->sum_cdb[i] / c->heard[i] / 100.0f; break;
    case LS_RFS_OCC:  *out = 100.0f * c->occ[i] / c->heard[i]; break;
    case LS_RFS_OVER: *out = (float)c->over_cdb[i] / c->heard[i] / 100.0f; break;
    default: return false;
    }
    return true;
}

bool ls_rfs_grid_value(const ls_rfs_grid_t *g, int idx, int ch, ls_rfs_metric_t m, float *out)
{
    if (!g || !out || idx < 0 || idx >= g->count || ch >= g->nch) return false;
    const ls_rfs_cell_t *c = &g->cell[idx];
    if (ch >= 0) return one(c, ch, m, out);
    bool any = false;
    for (int i = 0; i < g->nch; i++) {
        float v;
        if (one(c, i, m, &v) && (!any || v > *out)) { *out = v; any = true; }
    }
    return any;
}

bool ls_rfs_grid_range(const ls_rfs_grid_t *g, int ch, ls_rfs_metric_t m, float *lo, float *hi)
{
    bool any = false;
    for (int i = 0; g && i < g->count; i++) {
        float v;
        if (!ls_rfs_grid_value(g, i, ch, m, &v)) continue;
        if (!any || v < *lo) *lo = v;
        if (!any || v > *hi) *hi = v;
        any = true;
    }
    return any;
}

/* --------------------------------------------------------------- moving -- */

double ls_rfs_distance_m(double lat0, double lon0, double lat1, double lon1)
{
    /* Haversine on a 6371 km sphere: a few metres in a thousand at worst,
       which is far finer than any GPS a survey walks with. */
    const double k = M_PI / 180.0;
    const double a = sin((lat1 - lat0) * k / 2), b = sin((lon1 - lon0) * k / 2);
    const double h = a * a + cos(lat0 * k) * cos(lat1 * k) * b * b;
    return 2 * 6371000.0 * asin(sqrt(h < 1 ? h : 1));
}

bool ls_rfs_moved_enough(bool have_last, double last_lat, double last_lon, double lat, double lon, float step_m)
{
    if (!have_last || step_m <= 0) return true;
    return ls_rfs_distance_m(last_lat, last_lon, lat, lon) >= step_m;
}

/* --------------------------------------------------------------- export -- */

static int clampn(int n, size_t cap) { return n < 0 ? 0 : (size_t)n >= cap ? (int)cap - 1 : n; }

int ls_rfs_csv_header(char *out, size_t cap)
{
    return clampn(LS_SNPUTS(t_ls_rfs_utc_s_t_ms, out, cap, "utc_s,t_ms,lat,lon,fix,channel,label,hz,bw_hz,level_db,floor_db,occupied\n"), cap);
}

int ls_rfs_csv_line(const ls_rfs_header_t *h, const ls_rfs_row_t *r, int ch, bool occupied, char *out, size_t cap)
{
    if (!h || !r || ch < 0 || ch >= h->nch || !out || !cap) return 0;
    char lv[16] = "", fl[16] = "";
    if (r->level[ch] != LS_RFS_NO_LEVEL) snprintf(lv, sizeof(lv), "%.2f", r->level[ch] / 100.0);
    if (r->floor[ch] != LS_RFS_NO_LEVEL) snprintf(fl, sizeof(fl), "%.2f", r->floor[ch] / 100.0);
    return clampn(LS_SNPRINTF(t_ls_rfs_lu_lu_7f_7f, out, cap, "%lu,%lu,%.7f,%.7f,%s,%d,%s,%lu,%lu,%s,%s,%d\n",
                           (unsigned long)r->utc_s, (unsigned long)r->t_ms, r->lat, r->lon,
                           r->flags & LS_RFS_ROW_SIM ? "sim" : r->flags & LS_RFS_ROW_FIX ? "gps" : "none",
                           ch, h->ch[ch].label, (unsigned long)h->ch[ch].hz, (unsigned long)h->ch[ch].bw_hz,
                           lv, fl, occupied ? 1 : 0), cap);
}

int ls_rfs_geojson_begin(const ls_rfs_header_t *h, char *out, size_t cap)
{
    if (!h || !out || !cap) return 0;
    int n = LS_SNPRINTF(t_ls_rfs_type_featurecollection_lakeshark_schema, out, cap, "{\"type\":\"FeatureCollection\",\"lakeshark\":{\"schema\":\"lakeshark.rfsurvey\",\"version\":%u,"
                     "\"source\":\"%s\",\"plan\":\"%s\",\"start_utc\":%lu,\"threshold_db\":%.1f,\"channels\":[",
                     (unsigned)h->version ? h->version : LS_RFS_VERSION, h->source, h->plan,
                     (unsigned long)h->start_utc, h->threshold_cdb / 100.0);
    for (int i = 0; i < h->nch && n > 0 && (size_t)n < cap; i++)
        n += LS_SNPRINTF(t_ls_rfs_s_label_s_hz, out + n, cap - n, "%s{\"label\":\"%s\",\"hz\":%lu,\"bw_hz\":%lu}", i ? "," : "",
                      h->ch[i].label, (unsigned long)h->ch[i].hz, (unsigned long)h->ch[i].bw_hz);
    if (n > 0 && (size_t)n < cap) n += snprintf(out + n, cap - n, "]},\"features\":[\n");
    return clampn(n, cap);
}

int ls_rfs_geojson_feature(const ls_rfs_header_t *h, const ls_rfs_row_t *r, bool first, char *out, size_t cap)
{
    if (!h || !r || !out || !cap) return 0;
    int n = LS_SNPRINTF(t_ls_rfs_s_type_feature_geometry, out, cap, "%s{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[%.7f,%.7f]},"
                     "\"properties\":{\"utc_s\":%lu,\"t_ms\":%lu,\"fix\":\"%s\",\"level_db\":[",
                     first ? "" : ",\n", r->lon, r->lat, (unsigned long)r->utc_s, (unsigned long)r->t_ms,
                     r->flags & LS_RFS_ROW_SIM ? "sim" : r->flags & LS_RFS_ROW_FIX ? "gps" : "none");
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1) n += snprintf(out + n, cap - n, "],\"floor_db\":[");
        if (pass == 2) n += snprintf(out + n, cap - n, "],\"occupied\":[");
        for (int i = 0; i < h->nch && n > 0 && (size_t)n < cap; i++) {
            const int16_t v = pass == 0 ? r->level[i] : r->floor[i];
            if (pass == 2) n += snprintf(out + n, cap - n, "%s%d", i ? "," : "", (int)((r->occ >> i) & 1));
            else if (v == LS_RFS_NO_LEVEL) n += snprintf(out + n, cap - n, "%snull", i ? "," : "");
            else n += snprintf(out + n, cap - n, "%s%.2f", i ? "," : "", v / 100.0);
        }
        if (n <= 0 || (size_t)n >= cap) return clampn(n, cap);
    }
    n += snprintf(out + n, cap - n, "]}}");
    return clampn(n, cap);
}

int ls_rfs_geojson_end(char *out, size_t cap) { return clampn(snprintf(out, cap, "\n]}\n"), cap); }
