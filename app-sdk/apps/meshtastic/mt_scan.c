/* See mt_scan.h. Preset table, region profiles and slot rule ported from
   Meshtastic's firmware (MeshRadio.h modemPresetToParams, DisplayFormatters.cpp,
   RadioInterface.cpp; Copyright (C) Meshtastic LLC and contributors, GPL-3.0).
   GPL-3.0 as part of LakeShark. */
#include "mt_scan.h"
#include <math.h>
#include <string.h>

/* PRESETS_STD in a reading order: long to short. bw is the narrow-band
   bandwidth; cr is 4/cr. eu868 marks PRESETS_EU_868. */
const mt_scan_preset_t MT_SCAN_PRESETS[] = {
    { "LongFast",    "L.FST", 250000, 11, 5, 1 },
    { "LongMod",     "L.MOD", 125000, 11, 8, 1 },
    { "LongSlow",    "L.SLO", 125000, 12, 8, 1 },
    { "LongTurbo",   "L.TRB", 500000, 11, 8, 0 },
    { "MediumFast",  "M.FST", 250000,  9, 5, 1 },
    { "MediumSlow",  "M.SLO", 250000, 10, 5, 1 },
    { "MediumTurbo", "M.TRB", 500000,  9, 5, 0 },
    { "ShortFast",   "S.FST", 250000,  7, 5, 1 },
    { "ShortSlow",   "S.SLO", 250000,  8, 5, 1 },
    { "ShortTurbo",  "S.TRB", 500000,  7, 5, 0 },
};
const int MT_SCAN_PRESET_COUNT = (int)(sizeof(MT_SCAN_PRESETS) / sizeof(MT_SCAN_PRESETS[0]));

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33u + (uint8_t)*s++;
    return h;
}

static int region_ok(int region) { return region >= 0 && region < MT_REGION_COUNT ? region : 0; }
/* EU_868 is the one region here on PROFILE_EU868; the rest are PROFILE_STD. */
static bool eu868(int region) { return !strcmp(MT_REGIONS[region_ok(region)].name, "EU_868"); }

uint32_t mt_scan_bw_hz(int region, int preset)
{
    if (preset < 0 || preset >= MT_SCAN_PRESET_COUNT) preset = 0;
    const uint32_t bw = MT_SCAN_PRESETS[preset].bw_hz;
    return MT_REGIONS[region_ok(region)].wide ? bw / 4u * 13u : bw;   /* 2.4 GHz: x3.25 */
}

int mt_scan_slots(int region, uint32_t bw_hz)
{
    const mt_region_t *r = &MT_REGIONS[region_ok(region)];
    if (!bw_hz) return 0;
    const uint64_t band = (uint64_t)(r->hi_khz - r->lo_khz) * 1000u;
    return (int)((band + bw_hz / 2u) / bw_hz);            /* round(), as upstream */
}

uint64_t mt_scan_freq_hz(int region, int preset, int *slot, int *slots)
{
    if (preset < 0 || preset >= MT_SCAN_PRESET_COUNT) preset = 0;
    const mt_region_t *r = &MT_REGIONS[region_ok(region)];
    const uint32_t bw = mt_scan_bw_hz(region, preset);
    const int n = mt_scan_slots(region, bw);
    if (slot) *slot = 0;
    if (slots) *slots = n;
    if (n <= 0) return 0;
    const uint32_t s = djb2(MT_SCAN_PRESETS[preset].name) % (uint32_t)n;
    if (slot) *slot = (int)s + 1;
    return (uint64_t)r->lo_khz * 1000u + bw / 2u + (uint64_t)s * bw;
}

uint32_t mt_scan_airtime_ms(uint32_t bw_hz, uint8_t sf, uint8_t cr, uint8_t preamble, int len)
{
    if (!bw_hz || sf < 5 || sf > 12) return 0;
    const float tsym = (float)(1u << sf) * 1000.0f / (float)bw_hz;
    const int de = tsym > 16.0f;                            /* low data rate optimise */
    int n = (int)ceilf((8.0f * len - 4.0f * sf + 28 + 16) / (4.0f * (sf - 2 * de)));
    if (n < 0) n = 0;
    return (uint32_t)((preamble + 4.25f + 8 + n * (float)(cr < 5 ? 5 : cr)) * tsym);
}

static uint8_t default_hash(const char *name)
{
    static const uint8_t psk = 1;           /* AQ== */
    uint8_t key[32];
    const int kl = mt_channel_key(&psk, 1, key);
    return mt_channel_hash(name, key, kl);
}

static void plan_times(mt_scan_entry_t *e, uint32_t base)
{
    const uint32_t air = mt_scan_airtime_ms(e->bw_hz, e->sf, e->cr, e->preamble, 64);
    e->dwell_ms = base > 3u * air ? base : 3u * air;       /* room for a few long packets */
    e->hang_ms = air + 1000u;                               /* a reply or a relay after one */
}

void mt_scan_init(mt_scan_t *s, int region, uint32_t base_dwell_ms)
{
    memset(s, 0, sizeof(*s));
    s->region = region_ok(region);
    s->cur = s->pin = -1;
    s->base_dwell_ms = base_dwell_ms ? base_dwell_ms : 10000u;
    const bool eu = eu868(s->region), wide = MT_REGIONS[s->region].wide != 0;
    for (int p = 0; p < MT_SCAN_PRESET_COUNT && s->n < MT_SCAN_MAX; p++) {
        if (eu && !MT_SCAN_PRESETS[p].eu868) continue;
        int slot, slots;
        const uint64_t f = mt_scan_freq_hz(s->region, p, &slot, &slots);
        if (!f) continue;                                   /* wider than the band */
        mt_scan_entry_t *e = &s->e[s->n++];
        e->preset = (uint8_t)p;
        e->on = 1;
        e->sf = MT_SCAN_PRESETS[p].sf;
        e->cr = MT_SCAN_PRESETS[p].cr;
        e->preamble = wide ? 12 : MT_PREAMBLE;
        e->bw_hz = mt_scan_bw_hz(s->region, p);
        e->freq_hz = f;
        e->slot = (uint16_t)slot;
        e->slots = (uint16_t)slots;
        e->def_hash = default_hash(MT_SCAN_PRESETS[p].name);
        e->rssi = e->snr = INT16_MIN;
        plan_times(e, s->base_dwell_ms);
    }
}

void mt_scan_set_dwell(mt_scan_t *s, uint32_t base_dwell_ms)
{
    s->base_dwell_ms = base_dwell_ms ? base_dwell_ms : 10000u;
    for (int i = 0; i < s->n; i++) plan_times(&s->e[i], s->base_dwell_ms);
}

void mt_scan_clear(mt_scan_t *s)
{
    for (int i = 0; i < s->n; i++) {
        mt_scan_entry_t *e = &s->e[i];
        e->packets = e->readable = e->listen_ms = e->last_ms = 0;
        e->rssi = e->snr = INT16_MIN;
        e->node_n = e->node_more = e->hash_n = 0;
    }
    s->sweeps = s->total = s->readable = 0;
    s->seen_n = s->log_n = s->log_head = 0;
}

static void account(mt_scan_t *s, uint32_t now)
{
    if (s->running && s->cur >= 0 && s->cur < s->n) s->e[s->cur].listen_ms += now - s->acc_at;
    s->acc_at = now;
}

static int go(mt_scan_t *s, int i, uint32_t now)
{
    account(s, now);
    s->cur = i;
    s->step_at = s->acc_at = now;
    s->rx_step = false;
    return i;
}

/* The next entry in the cycle after `from` (-1: the first); wrapping counts a sweep. */
static int next_on(mt_scan_t *s, int from)
{
    for (int k = 1; k <= s->n; k++) {
        const int raw = from + k, i = raw % s->n;
        if (!s->e[i].on) continue;
        if (from >= 0 && raw >= s->n) s->sweeps++;
        return i;
    }
    return -1;
}

int mt_scan_start(mt_scan_t *s, uint32_t now)
{
    if (s->running) return s->cur;
    s->running = true;
    s->acc_at = now;
    if (s->pin >= 0 && s->pin < s->n) return go(s, s->pin, now);
    s->cur = -1;
    const int i = next_on(s, -1);
    if (i < 0) { s->running = false; return -1; }
    return go(s, i, now);
}

void mt_scan_stop(mt_scan_t *s, uint32_t now)
{
    account(s, now);
    s->running = false;
    s->cur = -1;
}

int mt_scan_tick(mt_scan_t *s, uint32_t now)
{
    if (!s->running || s->n <= 0) return -1;
    account(s, now);
    if (s->pin >= 0 && s->pin < s->n) return s->cur == s->pin ? -1 : go(s, s->pin, now);
    if (s->cur < 0 || s->cur >= s->n || !s->e[s->cur].on) {
        const int i = next_on(s, s->cur < s->n ? s->cur : -1);
        return i < 0 || i == s->cur ? -1 : go(s, i, now);
    }
    const mt_scan_entry_t *e = &s->e[s->cur];
    const uint32_t el = now - s->step_at;
    if (el < e->dwell_ms) return -1;
    /* traffic just now: wait for what follows it, up to two dwells */
    if (s->rx_step && now - s->last_rx_at < e->hang_ms && el < 2u * e->dwell_ms) return -1;
    const int i = next_on(s, s->cur);
    if (i < 0) return -1;
    if (i == s->cur) { s->step_at = now; s->rx_step = false; return -1; }   /* the only one on */
    return go(s, i, now);
}

int mt_scan_pin(mt_scan_t *s, int entry, uint32_t now)
{
    if (entry < 0 || entry >= s->n) {
        s->pin = -1;
        if (s->running) s->step_at = now;                  /* a full dwell before moving on */
        return -1;
    }
    s->pin = entry;
    s->e[entry].on = 1;
    if (!s->running) return -1;
    return s->cur == entry ? -1 : go(s, entry, now);
}

bool mt_scan_enable(mt_scan_t *s, int entry, bool on)
{
    if (entry < 0 || entry >= s->n) return false;
    if (!on) {
        int left = 0;
        for (int i = 0; i < s->n; i++) left += i != entry && s->e[i].on;
        if (!left) return false;
        if (s->pin == entry) s->pin = -1;
    }
    s->e[entry].on = on;
    return true;
}

bool mt_scan_decode(const mt_scan_entry_t *e, const uint8_t *buf, int n, mt_scan_pkt_t *out)
{
    memset(out, 0, sizeof(*out));
    mt_header_t h;
    if (n < MT_HEADER_LEN || !mt_header_unpack(buf, (size_t)n, &h)) return false;
    out->from = h.from;
    out->to = h.dest;
    out->id = h.id;
    out->hop_limit = h.hop_limit;
    out->hop_start = h.hop_start;
    out->hash = h.channel;
    out->via_mqtt = h.via_mqtt;
    out->len = (uint8_t)(n > 255 ? 255 : n);
    out->is_default = h.channel == e->def_hash;
    const int pl = n - MT_HEADER_LEN;
    if (!out->is_default || pl <= 0 || pl > MT_MAX_PAYLOAD) return true;
    static const uint8_t psk = 1;
    uint8_t key[32], data[MT_MAX_PAYLOAD];
    const int kl = mt_channel_key(&psk, 1, key);
    memcpy(data, buf + MT_HEADER_LEN, (size_t)pl);
    mt_crypt(key, kl, h.from, h.id, data, (size_t)pl);
    mt_data_t d;
    if (!mt_data_decode(data, (size_t)pl, &d) || d.portnum == 0 || d.portnum > 511) return true;
    out->readable = 1;
    out->portnum = d.portnum;
    if (d.portnum == MT_PORT_TEXT && !d.emoji) {
        size_t k = 0;
        for (size_t i = 0; i < d.payload_len && k + 1 < sizeof(out->text); i++) {
            const uint8_t c = d.payload[i];
            if (c >= 0x20 && c < 0x7F) out->text[k++] = (char)c;
            else if (c == '\n') out->text[k++] = ' ';
            else if (c >= 0xC0) out->text[k++] = '?';          /* one mark a multibyte letter */
        }
        out->text[k] = 0;
    }
    return true;
}

static void add_node(mt_scan_entry_t *e, uint32_t from)
{
    for (int i = 0; i < e->node_n; i++) if (e->nodes[i] == from) return;
    if (e->node_n < MT_SCAN_NODES) e->nodes[e->node_n++] = from;
    else if (e->node_more < 255) e->node_more++;
}

bool mt_scan_heard(mt_scan_t *s, const uint8_t *buf, int n, int16_t rssi, int16_t snr, uint32_t now)
{
    if (s->cur < 0 || s->cur >= s->n) return false;
    mt_scan_entry_t *e = &s->e[s->cur];
    mt_scan_pkt_t p;
    if (!mt_scan_decode(e, buf, n, &p)) return false;
    p.at_ms = now;
    p.entry = (uint8_t)s->cur;
    p.rssi = rssi;
    p.snr = snr;
    e->packets++;
    s->total++;
    e->last_ms = now;
    e->rssi = rssi;
    e->snr = snr;
    if (p.readable) { e->readable++; s->readable++; }
    add_node(e, p.from);
    {
        int i = 0;
        while (i < s->seen_n && s->seen[i] != p.from) i++;
        if (i == s->seen_n && s->seen_n < MT_SCAN_SEEN) s->seen[s->seen_n++] = p.from;
    }
    {
        int i = 0;
        while (i < e->hash_n && e->hash[i] != p.hash) i++;
        if (i < e->hash_n) { if (e->hash_count[i] < 0xFFFF) e->hash_count[i]++; }
        else if (e->hash_n < MT_SCAN_HASHES) { e->hash[e->hash_n] = p.hash; e->hash_count[e->hash_n++] = 1; }
    }
    s->log[s->log_head] = p;
    s->log_head = (s->log_head + 1) % MT_SCAN_LOG;
    if (s->log_n < MT_SCAN_LOG) s->log_n++;
    s->last_rx_at = now;
    s->rx_step = true;
    return true;
}

/* By packets a minute of listening, so a preset that held the radio longer
   (traffic stretches its dwell) does not look bigger for it. */
float mt_scan_share(const mt_scan_t *s, int entry)
{
    if (entry < 0 || entry >= s->n || !s->total) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < s->n; i++) sum += mt_scan_rate(s, i);
    if (sum <= 0.0f) return (float)s->e[entry].packets / (float)s->total;   /* under a second heard yet */
    return mt_scan_rate(s, entry) / sum;
}

float mt_scan_rate(const mt_scan_t *s, int entry)
{
    if (entry < 0 || entry >= s->n || s->e[entry].listen_ms < 1000u) return 0.0f;
    return (float)s->e[entry].packets * 60000.0f / (float)s->e[entry].listen_ms;
}

const mt_scan_pkt_t *mt_scan_log_at(const mt_scan_t *s, int k)
{
    if (k < 0 || k >= s->log_n) return NULL;
    return &s->log[(s->log_head - 1 - k + 2 * MT_SCAN_LOG) % MT_SCAN_LOG];
}

int mt_scan_top_hash(const mt_scan_entry_t *e)
{
    int best = -1;
    for (int i = 0; i < e->hash_n; i++)
        if (best < 0 || e->hash_count[i] > e->hash_count[best]) best = i;
    return best < 0 ? -1 : e->hash[best];
}
